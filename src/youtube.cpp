// YouTube: search, listings, and getting videos ready to play.
//
// yt-dlp does the talking to YouTube. It needs a JavaScript runtime (deno) to answer
// the challenges YouTube sets, and it needs to be recent, since YouTube keeps
// changing. So FastPlay keeps its own copies of both in its data folder: downloaded
// the first time they are needed, and yt-dlp updated at most once a day. A yt-dlp
// chosen in Options is used instead when that file exists.
//
// Videos are played from a downloaded file, not streamed: YouTube only serves audio
// in fragmented MP4, over ranged requests, which BASS cannot stream. yt-dlp
// downloads it and DefragmentMp4 turns it into an ordinary M4A.

#include "youtube.h"
#include "globals.h"
#include "http.h"
#include "mp4_remux.h"
#include "paths.h"
#include "subprocess.h"
#include "utils.h"

#include <chrono>
#include <filesystem>
#include <mutex>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------
// The tools: yt-dlp and deno
// ---------------------------------------------------------------------------

#if defined(_WIN32)
const wchar_t* const kYtdlpFile = L"yt-dlp.exe";
const wchar_t* const kYtdlpUrl = L"https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe";
const wchar_t* const kDenoFile = L"deno.exe";
const wchar_t* const kDenoUrl =
    L"https://github.com/denoland/deno/releases/latest/download/deno-x86_64-pc-windows-msvc.zip";
#elif defined(__APPLE__)
const wchar_t* const kYtdlpFile = L"yt-dlp";
const wchar_t* const kYtdlpUrl = L"https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp_macos";
const wchar_t* const kDenoFile = L"deno";
#if defined(__arm64__) || defined(__aarch64__)
const wchar_t* const kDenoUrl =
    L"https://github.com/denoland/deno/releases/latest/download/deno-aarch64-apple-darwin.zip";
#else
const wchar_t* const kDenoUrl =
    L"https://github.com/denoland/deno/releases/latest/download/deno-x86_64-apple-darwin.zip";
#endif
#else
const wchar_t* const kYtdlpFile = L"yt-dlp";
const wchar_t* const kYtdlpUrl = L"https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp_linux";
const wchar_t* const kDenoFile = L"deno";
const wchar_t* const kDenoUrl =
    L"https://github.com/denoland/deno/releases/latest/download/deno-x86_64-unknown-linux-gnu.zip";
#endif

// One thread at a time downloads or updates the tools.
std::mutex g_toolsMutex;

void Say(const YouTubeStatus& status, const std::wstring& message) {
    if (status) status(message);
}

bool FileExists(const std::wstring& path) {
    std::error_code ec;
    return !path.empty() && fs::is_regular_file(fs::path(path), ec);
}

std::wstring ToolsDir() {
    std::wstring dir = GetDataDirectory() + L"tools" + kPathSeparator;
    std::error_code ec;
    fs::create_directories(fs::path(dir), ec);
    return dir;
}

void MakeExecutable(const std::wstring& path) {
#ifndef _WIN32
    chmod(WideToUtf8(path).c_str(), 0755);
#else
    (void)path;
#endif
}

// Download a file, replacing `path` only once it has arrived whole.
bool DownloadFile(const std::wstring& url, const std::wstring& path, std::wstring& error) {
    std::wstring partial = path + L".part";
    HttpOptions options;
    options.saveTo = partial;
    options.timeoutMs = 60000;
    HttpResult result = HttpGet(url, options);
    std::error_code ec;
    if (!result.completed || result.status != 200) {
        fs::remove(fs::path(partial), ec);
        error = result.completed ? L"The download failed (HTTP " + std::to_wstring(result.status) + L")."
                                 : L"The download failed: " + result.errorText;
        return false;
    }
    fs::remove(fs::path(path), ec);
    fs::rename(fs::path(partial), fs::path(path), ec);
    if (ec) {
        error = L"Could not save " + path;
        return false;
    }
    MakeExecutable(path);
    return true;
}

// The yt-dlp to run.
bool EnsureYtdlp(std::wstring& ytdlp, std::wstring& error, const YouTubeStatus& status) {
    if (FileExists(g_ytdlpPath)) {  // chosen in Options; its owner keeps it up to date
        ytdlp = g_ytdlpPath;
        return true;
    }

    std::lock_guard<std::mutex> lock(g_toolsMutex);
    ytdlp = ToolsDir() + kYtdlpFile;
    std::wstring stamp = ToolsDir() + L"yt-dlp-checked";
    std::error_code ec;
    if (!FileExists(ytdlp)) {
        Say(status, L"Downloading yt-dlp for YouTube. This happens once.");
        if (!DownloadFile(kYtdlpUrl, ytdlp, error)) {
            error = L"Could not download yt-dlp. " + error;
            return false;
        }
    } else {
        // An old yt-dlp soon stops working with YouTube: update it once a day.
        auto checked = fs::last_write_time(fs::path(stamp), ec);
        if (ec || fs::file_time_type::clock::now() - checked > std::chrono::hours(24)) {
            std::string output;
            RunProcessCapture(ytdlp, {L"-U"}, output);
        } else {
            return true;
        }
    }
    if (FILE* f = FileOpen(stamp, "w")) fclose(f);
    fs::last_write_time(fs::path(stamp), fs::file_time_type::clock::now(), ec);
    return true;
}

// FastPlay's deno, or empty if it could not be had (yt-dlp then tries without).
std::wstring EnsureDeno(const YouTubeStatus& status) {
    std::lock_guard<std::mutex> lock(g_toolsMutex);
    std::wstring deno = ToolsDir() + kDenoFile;
    if (FileExists(deno)) return deno;

    Say(status, L"Downloading deno, which yt-dlp needs for YouTube. This happens once and takes a minute.");
    std::wstring zip = ToolsDir() + L"deno.zip";
    std::wstring error;
    if (!DownloadFile(kDenoUrl, zip, error)) return L"";

    // tar reads zip files, and comes with Windows 10 and macOS.
#ifdef _WIN32
    wchar_t system[kMaxPathChars] = {};
    GetSystemDirectoryW(system, kMaxPathChars);
    std::wstring tar = std::wstring(system) + L"\\tar.exe";
#else
    std::wstring tar = L"/usr/bin/tar";
#endif
    std::wstring dir = ToolsDir();
    if (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
    std::string output;
    RunProcessCapture(tar, {L"-xf", zip, L"-C", dir}, output);
    std::error_code ec;
    fs::remove(fs::path(zip), ec);
    if (!FileExists(deno)) return L"";
    MakeExecutable(deno);
    return deno;
}

// ---------------------------------------------------------------------------
// Running yt-dlp
// ---------------------------------------------------------------------------

struct YtdlpRun {
    std::string output;  // stdout, UTF-8
    std::string errors;  // stderr
    int exitCode = -1;
};

// The last "ERROR:" line yt-dlp wrote, for telling the user what went wrong.
std::wstring YtdlpError(const YtdlpRun& run) {
    std::istringstream lines(run.errors);
    std::string line, last;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("ERROR:", 0) == 0) last = line;
    }
    if (last.empty()) return L"yt-dlp failed (exit code " + std::to_wstring(run.exitCode) + L").";
    return Utf8ToWide(last);
}

bool RunYtdlp(const std::vector<std::wstring>& args, bool needsDeno, YtdlpRun& run, std::wstring& error,
              const YouTubeStatus& status) {
    std::wstring ytdlp;
    if (!EnsureYtdlp(ytdlp, error, status)) return false;
    std::vector<std::wstring> all = {L"--ignore-config", L"--no-update", L"--encoding", L"utf-8"};
    if (needsDeno) {
        std::wstring deno = EnsureDeno(status);
        if (!deno.empty()) {
            all.push_back(L"--js-runtimes");
            all.push_back(L"deno:" + deno);
        }
    }
    all.insert(all.end(), args.begin(), args.end());
    if (!RunProcessCapture(ytdlp, all, run.output, &run.errors, &run.exitCode)) {
        error = L"Could not run yt-dlp (" + ytdlp + L").";
        return false;
    }
    return true;
}

// The lines yt-dlp printed with --print, split at tabs.
std::vector<std::vector<std::wstring>> PrintedRows(const std::string& output) {
    std::vector<std::vector<std::wstring>> rows;
    std::istringstream lines(output);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.find('\t') == std::string::npos) continue;
        std::vector<std::wstring> fields;
        size_t start = 0;
        for (;;) {
            size_t tab = line.find('\t', start);
            fields.push_back(Utf8ToWide(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start)));
            if (tab == std::string::npos) break;
            start = tab + 1;
        }
        rows.push_back(std::move(fields));
    }
    return rows;
}

// A flat listing (search results, playlist, channel): one line per video.
const wchar_t* const kListFormat = L"%(id)s\t%(title)s\t%(channel,uploader|)s\t%(duration_string|)s\t%(live_status|)s";

bool ListWithYtdlp(const std::wstring& target, std::vector<YouTubeResult>& results, std::wstring& error,
                   const YouTubeStatus& status) {
    YtdlpRun run;
    if (!RunYtdlp({L"--flat-playlist", L"--print", kListFormat, target}, false, run, error, status)) return false;
    for (const auto& row : PrintedRows(run.output)) {
        if (row.size() < 5 || row[0].empty() || row[1].empty()) continue;
        YouTubeResult result;
        result.videoId = row[0];
        result.title = row[1];
        result.channel = row[2];
        result.duration = row[4] == L"is_live" ? L"live" : row[3];
        results.push_back(result);
    }
    if (results.empty() && run.exitCode != 0) {
        error = YtdlpError(run);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// The YouTube Data API (search with a key)
// ---------------------------------------------------------------------------

// URL encode a string
std::wstring UrlEncode(const std::wstring& str) {
    std::string utf8 = WideToUtf8(str);
    std::wostringstream encoded;
    for (unsigned char c : utf8) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded << static_cast<wchar_t>(c);
        } else {
            encoded << L'%' << std::hex << std::uppercase << ((c >> 4) & 0xF) << (c & 0xF);
        }
    }
    return encoded.str();
}

// The string value of "key" in a JSON text (the first one), with its escapes decoded.
std::wstring ParseJsonString(const std::wstring& json, const std::wstring& key) {
    std::wstring searchKey = L"\"" + key + L"\"";
    size_t keyPos = json.find(searchKey);
    if (keyPos == std::wstring::npos) return L"";
    size_t colon = json.find(L':', keyPos + searchKey.length());
    if (colon == std::wstring::npos) return L"";
    size_t pos = json.find_first_not_of(L" \t\r\n", colon + 1);
    if (pos == std::wstring::npos || json[pos] != L'"') return L"";

    std::u16string units;  // \u escapes are UTF-16 code units (pairs for emoji)
    std::wstring value;
    auto flushUnits = [&]() {
        if (units.empty()) return;
        value += Utf8ToWide(Utf16ToUtf8(units));
        units.clear();
    };
    for (pos++; pos < json.size() && json[pos] != L'"'; pos++) {
        wchar_t c = json[pos];
        if (c != L'\\' || pos + 1 >= json.size()) {
            flushUnits();
            value += c;
            continue;
        }
        wchar_t e = json[++pos];
        if (e == L'u' && pos + 4 < json.size()) {
            units += static_cast<char16_t>(std::wcstol(json.substr(pos + 1, 4).c_str(), nullptr, 16));
            pos += 4;
            continue;
        }
        flushUnits();
        switch (e) {
            case L'n': value += L' '; break;
            case L't': value += L'\t'; break;
            case L'r': break;
            case L'b': case L'f': break;
            default: value += e; break;  // \" \\ \/
        }
    }
    flushUnits();
    return value;
}

bool SearchWithAPI(const std::wstring& query, std::vector<YouTubeResult>& results, std::wstring& nextPageToken,
                   const std::wstring& pageToken) {
    std::wstring url = L"https://www.googleapis.com/youtube/v3/search?part=snippet&type=video&maxResults=25&q=";
    url += UrlEncode(query);
    url += L"&key=" + g_ytApiKey;
    if (!pageToken.empty()) {
        url += L"&pageToken=" + pageToken;
    }

    std::wstring response = Utf8ToWide(HttpGet(url).body);
    if (response.empty()) return false;

    // Parse results (simple parsing, not full JSON)
    nextPageToken = ParseJsonString(response, L"nextPageToken");

    // Find items array and parse each item
    size_t itemsPos = response.find(L"\"items\"");
    if (itemsPos == std::wstring::npos) return false;

    size_t searchStart = itemsPos;
    while ((searchStart = response.find(L"\"videoId\"", searchStart)) != std::wstring::npos) {
        YouTubeResult result;
        result.videoId = ParseJsonString(response.substr(searchStart, 500), L"videoId");

        // Find the snippet for this item
        size_t snippetPos = response.rfind(L"\"snippet\"", searchStart);
        if (snippetPos != std::wstring::npos && snippetPos > itemsPos) {
            std::wstring snippet = response.substr(snippetPos, searchStart - snippetPos + 1000);
            result.title = ParseJsonString(snippet, L"title");
            result.channel = ParseJsonString(snippet, L"channelTitle");
        }

        if (!result.videoId.empty() && !result.title.empty()) {
            results.push_back(result);
        }
        searchStart += 10;
    }

    return !results.empty();
}

// ---------------------------------------------------------------------------
// Downloaded videos
// ---------------------------------------------------------------------------

std::wstring CacheDir() {
    std::wstring dir = GetTempDir() + L"FastPlay YouTube" + kPathSeparator;
    std::error_code ec;
    fs::create_directories(fs::path(dir), ec);
    return dir;
}

// A title made safe as a file name.
std::wstring FileNameFrom(const std::wstring& title) {
    std::wstring name;
    for (wchar_t c : title) {
        if (c < 32 || std::wstring(L"\\/:*?\"<>|").find(c) != std::wstring::npos) {
            name += L'_';
        } else {
            name += c;
        }
    }
    if (name.size() > 120) name.resize(120);
    while (!name.empty() && (name.back() == L' ' || name.back() == L'.')) name.pop_back();
    return name.empty() ? L"YouTube" : name;
}

// A video downloaded before, found by the " [id].m4a" its file name ends with.
std::wstring FindDownloaded(const std::wstring& videoId) {
    std::wstring suffix = L" [" + videoId + L"].m4a";
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(fs::path(CacheDir()), ec)) {
        std::wstring name = entry.path().filename().wstring();
        if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            return entry.path().wstring();
        }
    }
    return L"";
}

}  // namespace

// ---------------------------------------------------------------------------
// Public functions
// ---------------------------------------------------------------------------

bool YouTubeSearch(const std::wstring& query, std::vector<YouTubeResult>& results, std::wstring& nextPageToken,
                   const std::wstring& pageToken, std::wstring& error, const YouTubeStatus& status) {
    results.clear();
    nextPageToken.clear();
    error.clear();

    // Try API first if available
    if (!g_ytApiKey.empty() && SearchWithAPI(query, results, nextPageToken, pageToken)) {
        return true;
    }

    // Fall back to yt-dlp (only for first page, no pagination support)
    if (!pageToken.empty()) return false;
    return ListWithYtdlp(L"ytsearch25:" + query, results, error, status);
}

bool YouTubeGetListContents(const std::wstring& listUrl, std::vector<YouTubeResult>& results, std::wstring& error,
                            const YouTubeStatus& status) {
    results.clear();
    error.clear();
    return ListWithYtdlp(listUrl, results, error, status);
}

std::wstring YouTubePlaylistUrl(const std::wstring& playlistId) {
    return L"https://www.youtube.com/playlist?list=" + playlistId;
}

std::wstring YouTubeChannelUrl(const std::wstring& channelId) {
    // Channel IDs are "UC" and 22 more characters; anything else is a @handle.
    if (channelId.size() == 24 && channelId.compare(0, 2, L"UC") == 0) {
        return L"https://www.youtube.com/channel/" + channelId + L"/videos";
    }
    return L"https://www.youtube.com/@" + channelId + L"/videos";
}

bool YouTubePrepare(const std::wstring& videoId, YouTubeMedia& media, std::wstring& error,
                    const YouTubeStatus& status) {
    media = YouTubeMedia();
    error.clear();

    std::wstring cached = FindDownloaded(videoId);
    if (!cached.empty()) {
        std::error_code ec;
        fs::last_write_time(fs::path(cached), fs::file_time_type::clock::now(), ec);  // keep it another week
        media.file = cached;
        return true;
    }

    std::wstring url = L"https://www.youtube.com/watch?v=" + videoId;
    std::wstring download = CacheDir() + videoId + L".download.m4a";
    std::error_code ec;
    fs::remove(fs::path(download), ec);

    // The AAC audio (all BASS and macOS can play of what YouTube offers). A live
    // stream does not pass the filter, and is handled below.
    YtdlpRun run;
    if (!RunYtdlp({L"--no-playlist", L"--match-filter", L"!is_live", L"-f", L"140/bestaudio[ext=m4a]",
                   L"--fixup", L"never", L"--no-part", L"--no-mtime", L"-o", download, L"--no-simulate",
                   L"--print", L"%(title)s\t%(channel,uploader|)s", url},
                  true, run, error, status)) {
        return false;
    }
    auto rows = PrintedRows(run.output);
    if (run.exitCode == 0 && FileExists(download) && !rows.empty()) {
        media.title = rows[0][0];
        std::wstring channel = rows[0].size() > 1 ? rows[0][1] : L"";
        std::wstring file = CacheDir() + FileNameFrom(media.title) + L" [" + videoId + L"].m4a";
        if (!DefragmentMp4(download, file, media.title, channel)) {
            // Not fragmented after all: it plays as it is.
            fs::remove(fs::path(file), ec);
            fs::rename(fs::path(download), fs::path(file), ec);
            if (ec) {
                error = L"Could not save the downloaded audio.";
                return false;
            }
        }
        fs::remove(fs::path(download), ec);
        media.file = file;
        return true;
    }
    fs::remove(fs::path(download), ec);
    if (run.exitCode != 0) {
        error = YtdlpError(run);
        return false;
    }

    // A live stream: play it from its HLS address.
    run = YtdlpRun();
    if (!RunYtdlp({L"--no-playlist", L"-f", L"bestaudio/93/94/92/91/best", L"--print", L"%(title)s\t%(url)s", url},
                  true, run, error, status)) {
        return false;
    }
    rows = PrintedRows(run.output);
    if (rows.empty() || rows[0].size() < 2 || rows[0][1].empty()) {
        error = run.exitCode != 0 ? YtdlpError(run) : L"YouTube gave no audio for this video.";
        return false;
    }
    media.title = rows[0][0];
    media.url = rows[0][1];
    return true;
}

void YouTubeCleanup() {
    std::error_code ec;
    auto now = fs::file_time_type::clock::now();
    for (const auto& entry : fs::directory_iterator(fs::path(CacheDir()), ec)) {
        std::error_code entryError;
        auto modified = fs::last_write_time(entry.path(), entryError);
        std::wstring name = entry.path().filename().wstring();
        bool partial = name.find(L".download.") != std::wstring::npos;
        if (!entryError && (partial || now - modified > std::chrono::hours(24 * 7))) {
            fs::remove(entry.path(), entryError);
        }
    }
}

// Check if input is a YouTube URL
bool IsYouTubeURL(const std::wstring& input) {
    return input.find(L"youtube.com") != std::wstring::npos ||
           input.find(L"youtu.be") != std::wstring::npos;
}

// Parse YouTube URL
bool ParseYouTubeURL(const std::wstring& url, std::wstring& id, bool& isPlaylist, bool& isChannel) {
    isPlaylist = false;
    isChannel = false;
    id.clear();

    // Check for playlist
    size_t listPos = url.find(L"list=");
    if (listPos != std::wstring::npos) {
        size_t start = listPos + 5;
        size_t end = url.find_first_of(L"&# ", start);
        id = url.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        isPlaylist = true;
        return !id.empty();
    }

    // Check for channel
    if (url.find(L"/channel/") != std::wstring::npos || url.find(L"/@") != std::wstring::npos) {
        isChannel = true;
        // Extract channel ID or handle
        size_t pos = url.find(L"/channel/");
        if (pos != std::wstring::npos) {
            pos += 9;
        } else {
            pos = url.find(L"/@");
            if (pos != std::wstring::npos) pos += 2;
        }
        if (pos != std::wstring::npos) {
            size_t end = url.find_first_of(L"/?# ", pos);
            id = url.substr(pos, end == std::wstring::npos ? std::wstring::npos : end - pos);
            return !id.empty();
        }
    }

    // Check for video ID
    size_t vPos = url.find(L"v=");
    if (vPos != std::wstring::npos) {
        size_t start = vPos + 2;
        size_t end = url.find_first_of(L"&# ", start);
        id = url.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        return !id.empty();
    }

    // youtu.be format
    size_t bePos = url.find(L"youtu.be/");
    if (bePos != std::wstring::npos) {
        size_t start = bePos + 9;
        size_t end = url.find_first_of(L"?# ", start);
        id = url.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        return !id.empty();
    }

    return false;
}
