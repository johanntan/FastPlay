// Installing an update on Windows: run the downloaded installer, or unpack the
// portable zip over FastPlay's folder with a batch script once FastPlay has quit.

#include "updater.h"
#include "updater_internal.h"
#include "app_ui.h"

#include <windows.h>
#include <shellapi.h>

#include <fstream>
#include <string>

// Get path to downloaded update zip
std::wstring UpdateZipPath() {
    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    return std::wstring(tempPath) + L"FastPlay-update.zip";
}

// Get path to app directory
static std::wstring GetAppDirectory() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    std::wstring appPath(path);
    size_t lastSlash = appPath.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) {
        return appPath.substr(0, lastSlash);
    }
    return L".";
}

// Get path to downloaded installer
std::wstring UpdateInstallerPath() {
    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    return std::wstring(tempPath) + L"FastPlay-Setup.exe";
}

void ApplyUpdate() {
    if (UpdateWithInstaller()) {
        std::wstring installerPath = UpdateInstallerPath();

        if (GetFileAttributesW(installerPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            ShowMessage(L"Update file not found. The download may have failed.", L"Update Error", MessageIcon::Error);
            return;
        }

        HINSTANCE result = ShellExecuteW(NULL, L"open", installerPath.c_str(), L"/SILENT", NULL, SW_SHOWNORMAL);
        if (reinterpret_cast<intptr_t>(result) <= 32) {
            ShowMessage(L"Failed to launch installer.", L"Update Error", MessageIcon::Error);
            return;
        }
        CloseMainWindow();
    } else {
        std::wstring appDir = GetAppDirectory();
        std::wstring zipPath = UpdateZipPath();
        std::wstring batchPath = appDir + L"\\update.bat";
        std::wstring extractDir = appDir + L"\\update_temp";

        if (GetFileAttributesW(zipPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            ShowMessage(L"Update file not found. The download may have failed.", L"Update Error", MessageIcon::Error);
            return;
        }

        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(NULL, exePath, MAX_PATH);
        std::wstring exeName(exePath);
        size_t lastSlash = exeName.find_last_of(L"\\/");
        if (lastSlash != std::wstring::npos) {
            exeName = exeName.substr(lastSlash + 1);
        }

        std::ofstream batch(batchPath);
        batch << "@echo off\r\n";
        batch << "echo Updating FastPlay...\r\n";
        batch << "timeout /t 2 /nobreak > nul\r\n";
        batch << "powershell -Command \"Expand-Archive -Path '" << std::string(zipPath.begin(), zipPath.end()) << "' -DestinationPath '" << std::string(extractDir.begin(), extractDir.end()) << "' -Force\"\r\n";
        batch << "xcopy /s /y /q \"" << std::string(extractDir.begin(), extractDir.end()) << "\\*\" \"" << std::string(appDir.begin(), appDir.end()) << "\\\"\r\n";
        batch << "rmdir /s /q \"" << std::string(extractDir.begin(), extractDir.end()) << "\"\r\n";
        batch << "del \"" << std::string(zipPath.begin(), zipPath.end()) << "\"\r\n";
        batch << "start \"\" \"" << std::string(appDir.begin(), appDir.end()) << "\\" << std::string(exeName.begin(), exeName.end()) << "\"\r\n";
        batch << "del \"%~f0\"\r\n";
        batch.close();

        HINSTANCE result = ShellExecuteW(NULL, L"open", batchPath.c_str(), NULL, appDir.c_str(), SW_HIDE);
        if (reinterpret_cast<intptr_t>(result) <= 32) {
            ShowMessage(L"Failed to launch update script.", L"Update Error", MessageIcon::Error);
            return;
        }
        CloseMainWindow();
    }
}
