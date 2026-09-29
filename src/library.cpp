// The library: see library.h.
//
// Indexing is incremental: a folder is listed (quickly: one large directory read
// at a time), and only files that are new, or whose size or time changed, are
// read for their tags, several at once, and written to the index in batches. A
// file whose folder is left out of the tagged views is not read at all, nor is a
// file only in the cloud (reading it would download it).
//
// While FastPlay runs, the folders are watched: a changed path that is a folder
// is indexed again (incrementally), a file is looked at again, and one that no
// longer exists is dropped, with everything under it.

#include "library.h"
#include "app_ui.h"
#include "audio.h"
#include "folder_watch.h"
#include "globals.h"
#include "paths.h"
#include "playlist_io.h"
#include "sqlite3.h"
#include "utils.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cwctype>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#endif

namespace {

// Paths are the same whatever their case on Windows (and in the index there)
#ifdef _WIN32
const char* const kPathCollate = " COLLATE NOCASE";
#else
const char* const kPathCollate = "";
#endif

// Files read for their tags at a time, and written to the index together
const size_t kBatch = 256;
// A changed path is dealt with once the folder has been quiet this long
const auto kSettle = std::chrono::milliseconds(1500);

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

bool IsSeparator(wchar_t c) { return c == L'\\' || c == L'/'; }

std::wstring WithSeparator(const std::wstring& dir) {
    if (!dir.empty() && IsSeparator(dir.back())) return dir;
    return dir + kPathSeparator;
}

// Without a trailing separator, unless it is a drive's or the file system's root
std::wstring Normalized(std::wstring path) {
    while (path.size() > 1 && IsSeparator(path.back())) {
#ifdef _WIN32
        if (path.size() == 3 && path[1] == L':') break;
#endif
        path.pop_back();
    }
    return path;
}

bool SamePathChar(wchar_t a, wchar_t b) {
#ifdef _WIN32
    if (IsSeparator(a) && IsSeparator(b)) return true;
    return towlower(a) == towlower(b);
#else
    return a == b;
#endif
}

// Whether `path` is `dir` or under it
bool IsUnder(const std::wstring& path, const std::wstring& dir) {
    if (path.size() < dir.size()) return false;
    for (size_t i = 0; i < dir.size(); i++) {
        if (!SamePathChar(path[i], dir[i])) return false;
    }
    return path.size() == dir.size() || IsSeparator(path[dir.size()]) || IsSeparator(dir.back());
}

std::wstring FileNameOf(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring WithoutExtension(const std::wstring& name) {
    size_t dot = name.find_last_of(L'.');
    return dot == std::wstring::npos || dot == 0 ? name : name.substr(0, dot);
}

bool IsAudioName(const std::wstring& name) {
    size_t dot = name.find_last_of(L'.');
    return dot != std::wstring::npos && IsSupportedAudioExt(name.substr(dot));
}

// ---------------------------------------------------------------------------
// Listing folders
// ---------------------------------------------------------------------------

struct Found {
    std::wstring path;
    int64_t size = 0, modified = 0;
    bool offline = false;  // only in the cloud: not to be read
};

struct Listed {
    std::wstring name;
    bool isFolder = false;
    int64_t size = 0, modified = 0;  // modified: in the system's own units
    bool offline = false;
};

// One folder's entries, but "." and "..", links (which could loop) and the
// system's hidden folders. False if it cannot be read.
bool ListDirectory(const std::wstring& dir, std::vector<Listed>& out) {
    out.clear();
#ifdef _WIN32
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileExW((WithSeparator(dir) + L"*").c_str(), FindExInfoBasic, &data, FindExSearchNameMatch,
                                   nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) return false;
    do {
        const wchar_t* name = data.cFileName;
        if (name[0] == L'.' && (name[1] == 0 || (name[1] == L'.' && name[2] == 0))) continue;
        const DWORD attributes = data.dwFileAttributes;
        if ((attributes & FILE_ATTRIBUTE_HIDDEN) && (attributes & FILE_ATTRIBUTE_SYSTEM)) continue;
        Listed entry;
        entry.name = name;
        entry.isFolder = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            // Symbolic links and junctions could lead round in a circle; the cloud's
            // own folders are reparse points too, and are kept
            if (data.dwReserved0 == IO_REPARSE_TAG_SYMLINK || data.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT) continue;
        }
        entry.size = (static_cast<int64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        entry.modified = (static_cast<int64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                         data.ftLastWriteTime.dwLowDateTime;
        const DWORD kRecallOnDataAccess = 0x00400000;
        entry.offline = (attributes & (FILE_ATTRIBUTE_OFFLINE | kRecallOnDataAccess)) != 0;
        out.push_back(std::move(entry));
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return true;
#else
    DIR* d = opendir(WideToUtf8(dir).c_str());
    if (!d) return false;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;  // ".", ".." and hidden files
        struct stat st;
        if (fstatat(dirfd(d), e->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) continue;
        if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode)) continue;  // links too
        Listed entry;
        entry.name = Utf8ToWide(e->d_name);
        entry.isFolder = S_ISDIR(st.st_mode);
        entry.size = static_cast<int64_t>(st.st_size);
#ifdef __APPLE__
        entry.modified = static_cast<int64_t>(st.st_mtimespec.tv_sec) * 1000000000LL + st.st_mtimespec.tv_nsec;
#else
        entry.modified = static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
#endif
        out.push_back(std::move(entry));
    }
    closedir(d);
    return true;
#endif
}

// Seconds since 1970 from ListDirectory's units
int64_t ToSeconds(int64_t modified) {
#ifdef _WIN32
    return modified / 10000000LL - 11644473600LL;
#else
    return modified / 1000000000LL;
#endif
}

// ---------------------------------------------------------------------------
// The index
// ---------------------------------------------------------------------------

void Exec(sqlite3* db, const char* sql) { sqlite3_exec(db, sql, nullptr, nullptr, nullptr); }

void BindText(sqlite3_stmt* s, int i, const std::wstring& text) {
    std::string utf8 = WideToUtf8(text);
    sqlite3_bind_text(s, i, utf8.c_str(), static_cast<int>(utf8.size()), SQLITE_TRANSIENT);
}

std::wstring ColumnText(sqlite3_stmt* s, int i) {
    const unsigned char* text = sqlite3_column_text(s, i);
    return text ? Utf8ToWide(reinterpret_cast<const char*>(text)) : std::wstring();
}

sqlite3* OpenIndex() {
    std::string path = WideToUtf8(GetDataDirectory() + L"library.db");
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return nullptr;
    }
    sqlite3_busy_timeout(db, 5000);
    Exec(db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;");
    std::string schema =
        std::string("CREATE TABLE IF NOT EXISTS tracks ("
                    "  id INTEGER PRIMARY KEY,"
                    "  path TEXT NOT NULL UNIQUE") + kPathCollate + ","
        "  root TEXT NOT NULL,"
        "  size INTEGER, modified INTEGER,"
        "  tagged INTEGER NOT NULL,"
        "  title TEXT, artist TEXT, album TEXT, album_artist TEXT, genre TEXT,"
        "  year INTEGER, track INTEGER, disc INTEGER, duration REAL,"
        "  name TEXT, added INTEGER);"
        "CREATE INDEX IF NOT EXISTS tracks_root ON tracks(root);"
        "CREATE INDEX IF NOT EXISTS tracks_artist ON tracks(artist COLLATE NOCASE);"
        "CREATE INDEX IF NOT EXISTS tracks_album ON tracks(album COLLATE NOCASE);"
        "CREATE INDEX IF NOT EXISTS tracks_genre ON tracks(genre COLLATE NOCASE);";
    Exec(db, schema.c_str());
    return db;
}

// ---------------------------------------------------------------------------
// The indexer
// ---------------------------------------------------------------------------

struct Library {
    sqlite3* reader = nullptr;  // the UI thread's
    sqlite3* writer = nullptr;  // the indexer's

    std::thread thread;
    std::mutex mutex;
    std::condition_variable wake;
    bool stop = false;
    // What the indexer is to do (under mutex)
    std::vector<LibraryFolder> folders;
    bool foldersChanged = false;
    std::set<std::wstring> rootsToScan;
    bool rereadTags = false;
    std::vector<std::wstring> changed;
    std::chrono::steady_clock::time_point lastChange;

    std::unique_ptr<FolderWatch> watch;  // the indexer's
    std::atomic<bool> busy{false};
    std::atomic<int> done{0}, found{0};
    std::atomic<bool> stopping{false};

    std::chrono::steady_clock::time_point lastNotice;
    std::function<void()> changedHandler;  // the UI thread's
};

Library g;

// The folders in the settings, each once, and none inside another (its files
// belong to the outer one)
std::vector<LibraryFolder> EffectiveFolders() {
    std::vector<LibraryFolder> folders;
    for (LibraryFolder folder : g_libraryFolders) {
        folder.path = Normalized(folder.path);
        if (folder.path.empty()) continue;
        folders.push_back(folder);
    }
    std::vector<LibraryFolder> result;
    for (size_t i = 0; i < folders.size(); i++) {
        bool inside = false;
        for (size_t j = 0; j < folders.size() && !inside; j++) {
            if (i == j) continue;
            if (IsUnder(folders[i].path, folders[j].path)) {
                // The same folder twice: keep the first
                inside = folders[i].path.size() != folders[j].path.size() || j < i;
            }
        }
        if (!inside) result.push_back(folders[i]);
    }
    return result;
}

// Tells the UI the index changed: now, or once a second at most while indexing
void Notice(bool now) {
    auto t = std::chrono::steady_clock::now();
    if (!now && t - g.lastNotice < std::chrono::seconds(1)) return;
    g.lastNotice = t;
    RunOnUiThread([]() {
        if (g.changedHandler) g.changedHandler();
    });
}

struct Row {
    int64_t size, modified;
    bool tagged;
};

// Reads the tags of `files` (unless the folder is untagged), several at once, and
// writes them to the index.
void Index(const std::vector<Found>& files, const std::wstring& root, bool tagged) {
    static const char* upsertSql =
        "INSERT INTO tracks(path,root,size,modified,tagged,title,artist,album,album_artist,genre,year,track,disc,"
        "duration,name,added) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
        "ON CONFLICT(path) DO UPDATE SET root=excluded.root,size=excluded.size,modified=excluded.modified,"
        "tagged=excluded.tagged,title=excluded.title,artist=excluded.artist,album=excluded.album,"
        "album_artist=excluded.album_artist,genre=excluded.genre,year=excluded.year,track=excluded.track,"
        "disc=excluded.disc,duration=excluded.duration,name=excluded.name";
    sqlite3_stmt* upsert = nullptr;
    if (sqlite3_prepare_v2(g.writer, upsertSql, -1, &upsert, nullptr) != SQLITE_OK) return;
    const unsigned threads = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    const int64_t now = static_cast<int64_t>(time(nullptr));

    for (size_t start = 0; start < files.size() && !g.stopping; start += kBatch) {
        const size_t count = std::min(kBatch, files.size() - start);
        std::vector<audio::FileTags> tags(count);
        if (tagged) {
            std::atomic<size_t> next{0};
            auto work = [&]() {
                for (size_t i; (i = next++) < count && !g.stopping;) {
                    const Found& file = files[start + i];
                    if (!file.offline) audio::ReadFileTags(file.path, tags[i]);
                }
            };
            std::vector<std::thread> pool;
            for (unsigned t = 1; t < threads && t < count; t++) pool.emplace_back(work);
            work();
            for (auto& t : pool) t.join();
        }
        Exec(g.writer, "BEGIN");
        for (size_t i = 0; i < count; i++) {
            const Found& file = files[start + i];
            const audio::FileTags& t = tags[i];
            const std::wstring name = FileNameOf(file.path);
            std::wstring title = Utf8ToWide(t.title);
            if (title.empty()) title = WithoutExtension(name);
            const std::wstring artist = Utf8ToWide(t.artist);
            std::wstring albumArtist = Utf8ToWide(t.albumArtist);
            if (albumArtist.empty()) albumArtist = artist;
            BindText(upsert, 1, file.path);
            BindText(upsert, 2, root);
            sqlite3_bind_int64(upsert, 3, file.size);
            sqlite3_bind_int64(upsert, 4, file.modified);
            sqlite3_bind_int(upsert, 5, tagged ? 1 : 0);
            BindText(upsert, 6, title);
            BindText(upsert, 7, artist);
            BindText(upsert, 8, Utf8ToWide(t.album));
            BindText(upsert, 9, albumArtist);
            BindText(upsert, 10, Utf8ToWide(t.genre));
            sqlite3_bind_int(upsert, 11, t.year);
            sqlite3_bind_int(upsert, 12, t.track);
            sqlite3_bind_int(upsert, 13, t.disc);
            sqlite3_bind_double(upsert, 14, t.duration);
            BindText(upsert, 15, name);
            sqlite3_bind_int64(upsert, 16, now);
            sqlite3_step(upsert);
            sqlite3_reset(upsert);
        }
        Exec(g.writer, "COMMIT");
        g.done += static_cast<int>(count);
        Notice(false);
    }
    sqlite3_finalize(upsert);
}

// Drops `path` from the index, with everything under it
void Drop(const std::wstring& path) {
    std::string sql = std::string("DELETE FROM tracks WHERE path = ? OR substr(path, 1, ?) = ?") + kPathCollate;
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(g.writer, sql.c_str(), -1, &s, nullptr) != SQLITE_OK) return;
    std::wstring prefix = WithSeparator(path);
    BindText(s, 1, path);
    sqlite3_bind_int(s, 2, static_cast<int>(prefix.size()));
    BindText(s, 3, prefix);
    sqlite3_step(s);
    sqlite3_finalize(s);
    if (sqlite3_changes(g.writer) > 0) Notice(false);
}

// Brings the index up to date for `tree` (the root, or a folder in it): lists it,
// and indexes what is new or changed, and drops what has gone.
void ScanTree(const LibraryFolder& root, const std::wstring& tree, bool reread) {
    // What the index has there now
    std::unordered_map<std::wstring, Row> rows;
    {
        const bool whole = tree.size() == root.path.size();
        std::string sql = "SELECT path, size, modified, tagged FROM tracks WHERE root = ?";
        if (!whole) sql += std::string(" AND substr(path, 1, ?) = ?") + kPathCollate;
        sqlite3_stmt* s = nullptr;
        if (sqlite3_prepare_v2(g.writer, sql.c_str(), -1, &s, nullptr) == SQLITE_OK) {
            BindText(s, 1, root.path);
            if (!whole) {
                std::wstring prefix = WithSeparator(tree);
                sqlite3_bind_int(s, 2, static_cast<int>(prefix.size()));
                BindText(s, 3, prefix);
            }
            while (sqlite3_step(s) == SQLITE_ROW) {
                rows[ColumnText(s, 0)] = {sqlite3_column_int64(s, 1), sqlite3_column_int64(s, 2),
                                         sqlite3_column_int(s, 3) != 0};
            }
            sqlite3_finalize(s);
        }
    }

    // What is on disk, and which of it the index lacks or has out of date
    std::vector<Found> todo;
    std::vector<std::wstring> folders{tree};
    std::vector<Listed> entries;
    while (!folders.empty() && !g.stopping) {
        std::wstring dir = folders.back();
        folders.pop_back();
        if (!ListDirectory(dir, entries)) continue;
        const std::wstring base = WithSeparator(dir);
        for (Listed& entry : entries) {
            if (entry.isFolder) {
                folders.push_back(base + entry.name);
                continue;
            }
            if (!IsAudioName(entry.name)) continue;
            g.found++;
            std::wstring path = base + entry.name;
            auto it = rows.find(path);
            if (it != rows.end()) {
                const Row row = it->second;
                rows.erase(it);
                if (!reread && row.size == entry.size && row.modified == entry.modified && row.tagged == root.tagged) {
                    g.done++;
                    continue;
                }
            }
            todo.push_back({std::move(path), entry.size, entry.modified, entry.offline});
        }
    }
    if (g.stopping) return;

    // Gone from disk (a folder that could not be read is left as it was)
    if (!rows.empty()) {
        sqlite3_stmt* s = nullptr;
        if (sqlite3_prepare_v2(g.writer, "DELETE FROM tracks WHERE path = ?", -1, &s, nullptr) == SQLITE_OK) {
            Exec(g.writer, "BEGIN");
            for (const auto& row : rows) {
                BindText(s, 1, row.first);
                sqlite3_step(s);
                sqlite3_reset(s);
            }
            Exec(g.writer, "COMMIT");
            sqlite3_finalize(s);
        }
        Notice(false);
    }
    Index(todo, root.path, root.tagged);
}

// A path the watch reported under `root`: a folder is indexed again, a file looked
// at again, and one that has gone dropped.
void Changed(const LibraryFolder& root, const std::wstring& path) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        Drop(path);
        return;
    }
    const bool isFolder = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat st;
    if (stat(WideToUtf8(path).c_str(), &st) != 0) {
        Drop(path);
        return;
    }
    const bool isFolder = S_ISDIR(st.st_mode);
#endif
    if (isFolder) {
        ScanTree(root, path, false);
        return;
    }
    // A file: its folder's listing has what is needed (size, time, whether offline)
    if (!IsAudioName(FileNameOf(path))) return;
    size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return;
    std::vector<Listed> entries;
    if (!ListDirectory(path.substr(0, slash), entries)) return;
    const std::wstring name = FileNameOf(path);
    for (const Listed& entry : entries) {
        if (entry.isFolder || entry.name != name) continue;
        sqlite3_stmt* s = nullptr;
        bool same = false;
        if (sqlite3_prepare_v2(g.writer, "SELECT size, modified, tagged FROM tracks WHERE path = ?", -1, &s,
                               nullptr) == SQLITE_OK) {
            BindText(s, 1, path);
            if (sqlite3_step(s) == SQLITE_ROW) {
                same = sqlite3_column_int64(s, 0) == entry.size && sqlite3_column_int64(s, 1) == entry.modified &&
                       (sqlite3_column_int(s, 2) != 0) == root.tagged;
            }
            sqlite3_finalize(s);
        }
        if (!same) Index({{path, entry.size, entry.modified, entry.offline}}, root.path, root.tagged);
        return;
    }
}

// Drops the folders no longer in the library
void DropOldFolders(const std::vector<LibraryFolder>& folders) {
    std::vector<std::wstring> roots;
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(g.writer, "SELECT DISTINCT root FROM tracks", -1, &s, nullptr) != SQLITE_OK) return;
    while (sqlite3_step(s) == SQLITE_ROW) roots.push_back(ColumnText(s, 0));
    sqlite3_finalize(s);
    for (const std::wstring& root : roots) {
        bool kept = std::any_of(folders.begin(), folders.end(),
                                [&](const LibraryFolder& f) { return f.path == root; });
        if (kept) continue;
        if (sqlite3_prepare_v2(g.writer, "DELETE FROM tracks WHERE root = ?", -1, &s, nullptr) != SQLITE_OK) continue;
        BindText(s, 1, root);
        sqlite3_step(s);
        sqlite3_finalize(s);
        Notice(false);
    }
}

void Watch(const std::vector<LibraryFolder>& folders) {
    g.watch.reset();
    std::vector<std::wstring> roots;
    for (const auto& folder : folders) roots.push_back(folder.path);
    g.watch = FolderWatch::Start(roots, [](const std::wstring& root, const std::vector<std::wstring>& paths,
                                           bool everything) {
        std::lock_guard<std::mutex> lock(g.mutex);
        if (everything) {
            g.rootsToScan.insert(Normalized(root));
        } else {
            g.changed.insert(g.changed.end(), paths.begin(), paths.end());
        }
        g.lastChange = std::chrono::steady_clock::now();
        g.wake.notify_one();
    });
}

void Run() {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    std::vector<LibraryFolder> folders;
    for (;;) {
        bool foldersChanged, reread;
        std::set<std::wstring> roots;
        std::vector<std::wstring> changed;
        {
            std::unique_lock<std::mutex> lock(g.mutex);
            for (;;) {
                if (g.stop) return;
                const bool settled =
                    !g.changed.empty() && std::chrono::steady_clock::now() - g.lastChange >= kSettle;
                if (g.foldersChanged || !g.rootsToScan.empty() || settled) break;
                if (g.changed.empty()) {
                    g.wake.wait(lock);
                } else {
                    g.wake.wait_for(lock, kSettle);
                }
            }
            foldersChanged = g.foldersChanged;
            g.foldersChanged = false;
            if (foldersChanged) folders = g.folders;
            roots.swap(g.rootsToScan);
            reread = g.rereadTags;
            g.rereadTags = false;
            if (std::chrono::steady_clock::now() - g.lastChange >= kSettle) changed.swap(g.changed);
        }

        g.busy = true;
        g.done = 0;
        g.found = 0;
        if (foldersChanged) {
            DropOldFolders(folders);
            Watch(folders);
        }
        for (const LibraryFolder& folder : folders) {
            if (g.stopping) break;
            if (roots.count(folder.path)) ScanTree(folder, folder.path, reread);
        }
        std::sort(changed.begin(), changed.end());
        changed.erase(std::unique(changed.begin(), changed.end()), changed.end());
        for (const std::wstring& path : changed) {
            if (g.stopping) break;
            for (const LibraryFolder& folder : folders) {
                if (IsUnder(path, folder.path)) {
                    Changed(folder, path);
                    break;
                }
            }
        }
        g.busy = false;
        Notice(true);
    }
}

// Search words as LIKE patterns ("%word%"), with LIKE's own characters escaped
std::vector<std::wstring> SearchPatterns(const std::wstring& search) {
    std::vector<std::wstring> words;
    std::wstring word;
    for (wchar_t c : search + L" ") {
        if (iswspace(c)) {
            if (!word.empty()) words.push_back(L"%" + word + L"%");
            word.clear();
        } else {
            if (c == L'%' || c == L'_' || c == L'\\') word += L'\\';
            word += c;
        }
    }
    return words;
}

// A query being put together: its SQL and the text for its ?s
struct Query {
    std::string sql;
    std::vector<std::wstring> args;
    bool where = false;

    void And(const std::string& condition) {
        sql += where ? " AND " : " WHERE ";
        sql += condition;
        where = true;
    }
    // Every word of the search in one of `columns`
    void Search(const std::wstring& search, const std::vector<const char*>& columns) {
        for (const std::wstring& pattern : SearchPatterns(search)) {
            std::string any = "(";
            for (size_t i = 0; i < columns.size(); i++) {
                if (i) any += " OR ";
                any += std::string(columns[i]) + " LIKE ? ESCAPE '\\'";
                args.push_back(pattern);
            }
            And(any + ")");
        }
    }
    void Filter(const LibraryFilter& filter) {
        if (filter.byArtist) {
            And("artist = ? COLLATE NOCASE");
            args.push_back(filter.artist);
        }
        if (filter.byAlbum) {
            And("album = ? COLLATE NOCASE AND album_artist = ? COLLATE NOCASE");
            args.push_back(filter.album);
            args.push_back(filter.albumArtist);
        }
        if (filter.byGenre) {
            And("genre = ? COLLATE NOCASE");
            args.push_back(filter.genre);
        }
    }
    sqlite3_stmt* Prepare() const {
        if (!g.reader) return nullptr;
        sqlite3_stmt* s = nullptr;
        if (sqlite3_prepare_v2(g.reader, sql.c_str(), -1, &s, nullptr) != SQLITE_OK) return nullptr;
        for (size_t i = 0; i < args.size(); i++) BindText(s, static_cast<int>(i) + 1, args[i]);
        return s;
    }
};

const char* const kSongColumns =
    "SELECT id, path, title, artist, album, album_artist, genre, year, track, disc, duration, added FROM tracks";

const char* SongOrderSql(SongOrder order) {
    switch (order) {
        case SongOrder::Artist:
            return " ORDER BY artist COLLATE NOCASE, album COLLATE NOCASE, disc, track, title COLLATE NOCASE";
        case SongOrder::Album:
            return " ORDER BY album COLLATE NOCASE, album_artist COLLATE NOCASE, disc, track, title COLLATE NOCASE";
        case SongOrder::TrackNumber:
            return " ORDER BY disc, track, title COLLATE NOCASE";
        case SongOrder::Duration:
            return " ORDER BY duration, title COLLATE NOCASE";
        case SongOrder::Year:
            return " ORDER BY year, album COLLATE NOCASE, disc, track";
        case SongOrder::DateAdded:
            return " ORDER BY added DESC, path";
        case SongOrder::FileName:
            return " ORDER BY name COLLATE NOCASE, path";
        case SongOrder::Title:
        default:
            return " ORDER BY title COLLATE NOCASE, artist COLLATE NOCASE";
    }
}

std::vector<LibrarySong> RunSongs(const Query& query) {
    std::vector<LibrarySong> songs;
    sqlite3_stmt* s = query.Prepare();
    if (!s) return songs;
    while (sqlite3_step(s) == SQLITE_ROW) {
        LibrarySong song;
        song.id = sqlite3_column_int64(s, 0);
        song.path = ColumnText(s, 1);
        song.title = ColumnText(s, 2);
        song.artist = ColumnText(s, 3);
        song.album = ColumnText(s, 4);
        song.albumArtist = ColumnText(s, 5);
        song.genre = ColumnText(s, 6);
        song.year = sqlite3_column_int(s, 7);
        song.track = sqlite3_column_int(s, 8);
        song.disc = sqlite3_column_int(s, 9);
        song.duration = sqlite3_column_double(s, 10);
        song.added = sqlite3_column_int64(s, 11);
        songs.push_back(std::move(song));
    }
    sqlite3_finalize(s);
    return songs;
}

}  // namespace

void StartLibrary() {
    if (g.thread.joinable()) return;
    g.reader = OpenIndex();
    g.writer = OpenIndex();
    if (!g.reader || !g.writer) return;
    g.stop = false;
    g.stopping = false;
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        g.folders = EffectiveFolders();
        g.foldersChanged = true;
        for (const auto& folder : g.folders) g.rootsToScan.insert(folder.path);
    }
    g.thread = std::thread(Run);
}

void StopLibrary() {
    {
        std::lock_guard<std::mutex> lock(g.mutex);
        g.stop = true;
    }
    g.stopping = true;
    g.wake.notify_all();
    if (g.thread.joinable()) g.thread.join();
    g.watch.reset();
    if (g.reader) sqlite3_close(g.reader);
    if (g.writer) sqlite3_close(g.writer);
    g.reader = g.writer = nullptr;
}

void LibraryFoldersChanged() {
    std::lock_guard<std::mutex> lock(g.mutex);
    g.folders = EffectiveFolders();
    g.foldersChanged = true;
    for (const auto& folder : g.folders) g.rootsToScan.insert(folder.path);
    g.wake.notify_one();
}

void RescanLibrary() {
    std::lock_guard<std::mutex> lock(g.mutex);
    for (const auto& folder : g.folders) g.rootsToScan.insert(folder.path);
    g.rereadTags = true;
    g.wake.notify_one();
}

bool LibraryProgress(int& done, int& found) {
    done = g.done;
    found = g.found;
    return g.busy;
}

void SetLibraryChangedHandler(std::function<void()> handler) { g.changedHandler = std::move(handler); }

std::vector<LibrarySong> LibrarySongs(const LibraryFilter& filter, SongOrder order) {
    Query q;
    q.sql = kSongColumns;
    q.And("tagged = 1");
    q.Filter(filter);
    q.Search(filter.search, {"title", "artist", "album", "name"});
    q.sql += SongOrderSql(order);
    return RunSongs(q);
}

std::vector<LibrarySong> LibrarySearchFiles(const std::wstring& search, SongOrder order) {
    Query q;
    q.sql = kSongColumns;
    q.Search(search, {"title", "artist", "album", "name"});
    q.sql += SongOrderSql(order);
    return RunSongs(q);
}

std::vector<LibraryGroup> LibraryArtists(const LibraryFilter& filter, GroupOrder order) {
    Query q;
    q.sql = "SELECT artist, COUNT(*) FROM tracks";
    q.And("tagged = 1");
    q.Filter(filter);
    q.Search(filter.search, {"artist"});
    q.sql += " GROUP BY artist COLLATE NOCASE";
    q.sql += order == GroupOrder::Songs ? " ORDER BY COUNT(*) DESC, artist COLLATE NOCASE"
                                        : " ORDER BY artist COLLATE NOCASE";
    std::vector<LibraryGroup> groups;
    if (sqlite3_stmt* s = q.Prepare()) {
        while (sqlite3_step(s) == SQLITE_ROW) {
            LibraryGroup group;
            group.name = ColumnText(s, 0);
            group.songs = sqlite3_column_int(s, 1);
            groups.push_back(std::move(group));
        }
        sqlite3_finalize(s);
    }
    return groups;
}

std::vector<LibraryGroup> LibraryAlbums(const LibraryFilter& filter, GroupOrder order) {
    Query q;
    q.sql = "SELECT album, album_artist, COUNT(*), MAX(year) FROM tracks";
    q.And("tagged = 1");
    q.Filter(filter);
    q.Search(filter.search, {"album", "album_artist"});
    q.sql += " GROUP BY album COLLATE NOCASE, album_artist COLLATE NOCASE";
    switch (order) {
        case GroupOrder::Artist:
            q.sql += " ORDER BY album_artist COLLATE NOCASE, MAX(year), album COLLATE NOCASE";
            break;
        case GroupOrder::Year:
            q.sql += " ORDER BY MAX(year), album COLLATE NOCASE";
            break;
        case GroupOrder::Songs:
            q.sql += " ORDER BY COUNT(*) DESC, album COLLATE NOCASE";
            break;
        default:
            q.sql += " ORDER BY album COLLATE NOCASE, album_artist COLLATE NOCASE";
            break;
    }
    std::vector<LibraryGroup> groups;
    if (sqlite3_stmt* s = q.Prepare()) {
        while (sqlite3_step(s) == SQLITE_ROW) {
            LibraryGroup group;
            group.name = ColumnText(s, 0);
            group.artist = ColumnText(s, 1);
            group.songs = sqlite3_column_int(s, 2);
            group.year = sqlite3_column_int(s, 3);
            groups.push_back(std::move(group));
        }
        sqlite3_finalize(s);
    }
    return groups;
}

std::vector<LibraryGroup> LibraryGenres(const LibraryFilter& filter, GroupOrder order) {
    Query q;
    q.sql = "SELECT genre, COUNT(*) FROM tracks";
    q.And("tagged = 1");
    q.Filter(filter);
    q.Search(filter.search, {"genre"});
    q.sql += " GROUP BY genre COLLATE NOCASE";
    q.sql += order == GroupOrder::Songs ? " ORDER BY COUNT(*) DESC, genre COLLATE NOCASE"
                                        : " ORDER BY genre COLLATE NOCASE";
    std::vector<LibraryGroup> groups;
    if (sqlite3_stmt* s = q.Prepare()) {
        while (sqlite3_step(s) == SQLITE_ROW) {
            LibraryGroup group;
            group.name = ColumnText(s, 0);
            group.songs = sqlite3_column_int(s, 1);
            groups.push_back(std::move(group));
        }
        sqlite3_finalize(s);
    }
    return groups;
}

std::vector<LibraryEntry> LibraryListFolder(const std::wstring& folder, FolderOrder order) {
    std::vector<LibraryEntry> result;
    std::vector<Listed> entries;
    if (!ListDirectory(folder, entries)) return result;
    const std::wstring base = WithSeparator(folder);
    for (const Listed& entry : entries) {
        if (!entry.isFolder && !IsAudioName(entry.name)) continue;
        result.push_back({entry.name, base + entry.name, entry.isFolder, ToSeconds(entry.modified)});
    }
    // Folders first, then files, each by name or newest first
    std::sort(result.begin(), result.end(), [order](const LibraryEntry& a, const LibraryEntry& b) {
        if (a.isFolder != b.isFolder) return a.isFolder;
        if (order == FolderOrder::Modified && a.modified != b.modified) return a.modified > b.modified;
        return WStrICmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return result;
}
