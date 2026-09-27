#include "paths.h"

#include <windows.h>
#include <shlobj.h>

const wchar_t kPathSeparator = L'\\';

std::wstring GetExecutableDir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir(path);
    size_t pos = dir.find_last_of(L"\\/");
    return pos != std::wstring::npos ? dir.substr(0, pos + 1) : std::wstring(L".\\");
}

// Installed by the installer, which leaves an installed.txt marker beside the executable.
bool IsInstalledMode() {
    DWORD attrs = GetFileAttributesW((GetExecutableDir() + L"installed.txt").c_str());
    return attrs != INVALID_FILE_ATTRIBUTES;
}

std::wstring GetDataDirectory() {
    if (IsInstalledMode()) {
        // Installed mode: use AppData\Roaming\FastPlay
        wchar_t appDataPath[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appDataPath))) {
            std::wstring dir = std::wstring(appDataPath) + L"\\FastPlay";
            CreateDirectoryW(dir.c_str(), nullptr);
            return dir + L"\\";
        }
    }
    // Portable mode (or AppData unavailable): beside the executable
    return GetExecutableDir();
}

std::wstring GetUserMusicDir() {
    wchar_t musicPath[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_MYMUSIC, nullptr, 0, musicPath))) {
        return musicPath;
    }
    return std::wstring();
}

std::wstring GetTempDir() {
    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    return tempPath;
}

std::wstring GetLibraryDir() {
    return GetExecutableDir() + L"lib\\";
}
