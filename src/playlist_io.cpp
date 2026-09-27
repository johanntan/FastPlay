#include "playlist_io.h"
#include "utils.h"
#include <windows.h>
#include <cstdio>
#include <cstring>

// Check if a file extension is a supported audio format
bool IsSupportedAudioExt(const std::wstring& ext) {
    static const wchar_t* exts[] = {
        L".mp3", L".wav", L".ogg", L".oga", L".flac", L".m4a", L".m4b", L".wma", L".aac",
        L".opus", L".aiff", L".ape", L".wv", L".mid", L".midi", L".dff", L".dsf"
    };
    std::wstring lowerExt = ext;
    for (auto& c : lowerExt) c = towlower(c);
    for (const auto& e : exts) {
        if (lowerExt == e) return true;
    }
    return false;
}

// Expand a single file to all audio files in its folder
// Returns the index of the original file in the expanded list
int ExpandFileToFolder(const std::wstring& filePath, std::vector<std::wstring>& outFiles) {
    outFiles.clear();

    // Get directory and filename
    size_t lastSlash = filePath.find_last_of(L"\\/");
    if (lastSlash == std::wstring::npos) {
        outFiles.push_back(filePath);
        return 0;
    }

    std::wstring dir = filePath.substr(0, lastSlash + 1);
    std::wstring targetFile = filePath.substr(lastSlash + 1);

    // Find all audio files in the directory
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW((dir + L"*").c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        outFiles.push_back(filePath);
        return 0;
    }

    std::vector<std::wstring> files;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            std::wstring name = fd.cFileName;
            size_t dotPos = name.find_last_of(L'.');
            if (dotPos != std::wstring::npos) {
                std::wstring ext = name.substr(dotPos);
                if (IsSupportedAudioExt(ext)) {
                    files.push_back(dir + name);
                }
            }
        }
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);

    // Find the index of the original file
    int targetIndex = 0;
    for (size_t i = 0; i < files.size(); i++) {
        if (_wcsicmp(GetFileName(files[i]).c_str(), targetFile.c_str()) == 0) {
            targetIndex = static_cast<int>(i);
            break;
        }
    }

    outFiles = std::move(files);
    return targetIndex;
}

// Recursively add audio files from a folder
static void AddFilesFromFolderRecursive(const std::wstring& folder, std::vector<std::wstring>& files, int depth) {
    // Limit recursion depth to prevent stack overflow
    if (depth > 32) return;

    WIN32_FIND_DATAW fd;
    std::wstring searchPath = folder + L"\\*";
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &fd);

    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;

        // Skip reparse points (junctions, symlinks) to avoid infinite loops
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;

        std::wstring fullPath = folder + L"\\" + fd.cFileName;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            // Recurse into subdirectory
            AddFilesFromFolderRecursive(fullPath, files, depth + 1);
        } else {
            // Check if it's a supported audio file
            size_t dotPos = fullPath.rfind(L'.');
            if (dotPos != std::wstring::npos) {
                std::wstring ext = fullPath.substr(dotPos);
                if (IsSupportedAudioExt(ext)) {
                    files.push_back(fullPath);
                }
            }
        }
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
}

void AddFilesFromFolder(const std::wstring& folder, std::vector<std::wstring>& files) {
    AddFilesFromFolderRecursive(folder, files, 0);
}

// Check if file is a playlist
bool IsPlaylistFile(const std::wstring& path) {
    size_t dotPos = path.find_last_of(L'.');
    if (dotPos == std::wstring::npos) return false;
    std::wstring ext = path.substr(dotPos);
    for (auto& c : ext) c = towlower(c);
    return (ext == L".m3u" || ext == L".m3u8" || ext == L".pls");
}

// Parse M3U playlist file
static std::vector<std::wstring> ParseM3U(const std::wstring& playlistPath) {
    std::vector<std::wstring> entries;

    // Get directory of playlist for relative paths
    std::wstring baseDir;
    size_t lastSlash = playlistPath.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) {
        baseDir = playlistPath.substr(0, lastSlash + 1);
    }

    // Read file as binary first to detect BOM
    FILE* f = _wfopen(playlistPath.c_str(), L"rb");
    if (!f) return entries;

    // Check for UTF-8 BOM
    unsigned char bom[3] = {0};
    fread(bom, 1, 3, f);
    bool isUtf8 = (bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF);
    if (!isUtf8) {
        fseek(f, 0, SEEK_SET);  // No BOM, rewind
    }

    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        // Trim whitespace
        char* start = line;
        while (*start && (*start == ' ' || *start == '\t')) start++;
        size_t slen = strlen(start);
        if (slen == 0) continue;
        char* end = start + slen - 1;
        while (end >= start && (*end == '\r' || *end == '\n' || *end == ' ' || *end == '\t')) {
            *end-- = '\0';
        }

        // Skip empty lines and comments
        if (*start == '\0' || *start == '#') continue;

        // Convert to wide string (try UTF-8 first, then ACP)
        std::wstring entry;
        int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, start, -1, nullptr, 0);
        if (len > 0) {
            entry.resize(len);
            MultiByteToWideChar(CP_UTF8, 0, start, -1, &entry[0], len);
        } else {
            len = MultiByteToWideChar(CP_ACP, 0, start, -1, nullptr, 0);
            if (len <= 0) continue;
            entry.resize(len);
            MultiByteToWideChar(CP_ACP, 0, start, -1, &entry[0], len);
        }
        // Remove null terminator from string
        if (!entry.empty() && entry.back() == L'\0') {
            entry.pop_back();
        }

        if (entry.empty()) continue;

        // Build full path
        std::wstring fullPath;
        if (_wcsnicmp(entry.c_str(), L"http://", 7) == 0 ||
            _wcsnicmp(entry.c_str(), L"https://", 8) == 0 ||
            _wcsnicmp(entry.c_str(), L"ftp://", 6) == 0 ||
            (entry.length() > 2 && entry[1] == L':')) {
            fullPath = entry;
        } else {
            // Relative path - prepend base directory
            fullPath = baseDir + entry;
        }

        // Check if it's a folder and expand it
        DWORD attrs = GetFileAttributesW(fullPath.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            // Recursively add folder contents
            AddFilesFromFolder(fullPath, entries);
        } else {
            entries.push_back(fullPath);
        }
    }
    fclose(f);
    return entries;
}

// Parse PLS playlist file
static std::vector<std::wstring> ParsePLS(const std::wstring& playlistPath) {
    std::vector<std::wstring> entries;

    // Get directory of playlist for relative paths
    std::wstring baseDir;
    size_t lastSlash = playlistPath.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) {
        baseDir = playlistPath.substr(0, lastSlash + 1);
    }

    // Read entries using GetPrivateProfileString
    for (int i = 1; i <= 1000; i++) {  // Reasonable max
        wchar_t key[32];
        swprintf(key, 32, L"File%d", i);
        wchar_t value[4096] = {0};
        GetPrivateProfileStringW(L"playlist", key, L"", value, 4096, playlistPath.c_str());
        if (value[0] == L'\0') break;

        std::wstring entry = value;
        std::wstring fullPath;
        // Check if it's a URL or absolute path
        if (_wcsnicmp(entry.c_str(), L"http://", 7) == 0 ||
            _wcsnicmp(entry.c_str(), L"https://", 8) == 0 ||
            _wcsnicmp(entry.c_str(), L"ftp://", 6) == 0 ||
            (entry.length() > 2 && entry[1] == L':')) {
            fullPath = entry;
        } else {
            // Relative path - prepend base directory
            fullPath = baseDir + entry;
        }

        // Check if it's a folder and expand it
        DWORD attrs = GetFileAttributesW(fullPath.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            // Recursively add folder contents
            AddFilesFromFolder(fullPath, entries);
        } else {
            entries.push_back(fullPath);
        }
    }
    return entries;
}

// Parse playlist file (M3U or PLS)
std::vector<std::wstring> ParsePlaylist(const std::wstring& playlistPath) {
    size_t dotPos = playlistPath.find_last_of(L'.');
    if (dotPos == std::wstring::npos) return {};

    std::wstring ext = playlistPath.substr(dotPos);
    for (auto& c : ext) c = towlower(c);

    if (ext == L".pls") {
        return ParsePLS(playlistPath);
    } else {
        return ParseM3U(playlistPath);
    }
}
