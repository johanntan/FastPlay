// Windows file associations: FastPlay registers itself per user (HKCU\Software\Classes)
// as the handler for the extensions in g_fileAssocs.

#include "file_assoc.h"
#include "globals.h"
#include <windows.h>
#include <shlobj.h>
#include <cstdio>

// Get executable path
static std::wstring GetExePath() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return path;
}

// Check if file extension is associated with FastPlay
static bool IsExtensionAssociated(const wchar_t* ext) {
    wchar_t keyPath[256];
    swprintf(keyPath, 256, L"Software\\Classes\\%s", ext);

    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, keyPath, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return false;
    }

    wchar_t value[256] = {0};
    DWORD size = sizeof(value);
    DWORD type;
    bool associated = false;

    if (RegQueryValueExW(hKey, nullptr, nullptr, &type, reinterpret_cast<LPBYTE>(value), &size) == ERROR_SUCCESS) {
        associated = (wcscmp(value, L"FastPlay.AudioFile") == 0);
    }
    RegCloseKey(hKey);
    return associated;
}

// Set or remove file association
static void SetFileAssociation(const wchar_t* ext, bool associate) {
    wchar_t extKeyPath[256];
    swprintf(extKeyPath, 256, L"Software\\Classes\\%s", ext);

    if (associate) {
        // Create extension key pointing to our ProgId
        HKEY hKey;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, extKeyPath, 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
            const wchar_t* progId = L"FastPlay.AudioFile";
            RegSetValueExW(hKey, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(progId), static_cast<DWORD>((wcslen(progId) + 1) * sizeof(wchar_t)));
            RegCloseKey(hKey);
        }

        // Create ProgId with shell\open\command
        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\FastPlay.AudioFile", 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
            const wchar_t* desc = L"FastPlay Audio File";
            RegSetValueExW(hKey, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(desc), static_cast<DWORD>((wcslen(desc) + 1) * sizeof(wchar_t)));
            RegCloseKey(hKey);
        }

        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\FastPlay.AudioFile\\shell\\open\\command", 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
            std::wstring cmd = L"\"" + GetExePath() + L"\" \"%1\"";
            RegSetValueExW(hKey, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()), static_cast<DWORD>((cmd.length() + 1) * sizeof(wchar_t)));
            RegCloseKey(hKey);
        }
    } else {
        // Remove association by deleting the extension key
        RegDeleteKeyW(HKEY_CURRENT_USER, extKeyPath);
    }

    // Notify shell of change
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

// Register all supported file types
void RegisterAllFileTypes() {
    for (int i = 0; i < g_fileAssocCount; i++) {
        if (!IsExtensionAssociated(g_fileAssocs[i].ext)) {
            SetFileAssociation(g_fileAssocs[i].ext, true);
        }
    }
}

// Unregister all supported file types
void UnregisterAllFileTypes() {
    for (int i = 0; i < g_fileAssocCount; i++) {
        if (IsExtensionAssociated(g_fileAssocs[i].ext)) {
            SetFileAssociation(g_fileAssocs[i].ext, false);
        }
    }
}
