#include "updater.h"
#include "version.h"
#include "globals.h"
#include "accessibility.h"
#include "app_ui.h"
#include <winhttp.h>
#include <shlobj.h>
#include <fstream>
#include <sstream>
#include <thread>
#include <regex>

#pragma comment(lib, "winhttp.lib")

// Simple JSON value extraction (no external library needed)
static std::string ExtractJsonString(const std::string& json, const std::string& key) {
    std::string searchKey = "\"" + key + "\"";
    size_t keyPos = json.find(searchKey);
    if (keyPos == std::string::npos) return "";

    size_t colonPos = json.find(':', keyPos + searchKey.length());
    if (colonPos == std::string::npos) return "";

    size_t startQuote = json.find('"', colonPos + 1);
    if (startQuote == std::string::npos) return "";

    size_t endQuote = startQuote + 1;
    while (endQuote < json.length()) {
        if (json[endQuote] == '"' && json[endQuote - 1] != '\\') break;
        endQuote++;
    }

    return json.substr(startQuote + 1, endQuote - startQuote - 1);
}

// Extract first object from JSON array
static std::string ExtractFirstArrayObject(const std::string& json) {
    size_t start = json.find('[');
    if (start == std::string::npos) return "";

    size_t objStart = json.find('{', start);
    if (objStart == std::string::npos) return "";

    int depth = 1;
    size_t objEnd = objStart + 1;
    while (objEnd < json.length() && depth > 0) {
        if (json[objEnd] == '{') depth++;
        else if (json[objEnd] == '}') depth--;
        objEnd++;
    }

    return json.substr(objStart, objEnd - objStart);
}

// Convert string to lowercase
static std::string ToLower(const std::string& str) {
    std::string result = str;
    for (char& c : result) {
        c = (char)tolower((unsigned char)c);
    }
    return result;
}

// Asset URLs for Windows
struct WindowsAssets {
    std::string zipUrl;
    std::string installerUrl;
};

// Find Windows assets in release (both zip and installer)
static WindowsAssets FindWindowsAssets(const std::string& releaseJson) {
    WindowsAssets assets;

    size_t assetsPos = releaseJson.find("\"assets\"");
    if (assetsPos == std::string::npos) return assets;

    size_t arrayStart = releaseJson.find('[', assetsPos);
    if (arrayStart == std::string::npos) return assets;

    int depth = 1;
    size_t arrayEnd = arrayStart + 1;
    while (arrayEnd < releaseJson.length() && depth > 0) {
        if (releaseJson[arrayEnd] == '[') depth++;
        else if (releaseJson[arrayEnd] == ']') depth--;
        arrayEnd++;
    }

    std::string assetsArray = releaseJson.substr(arrayStart, arrayEnd - arrayStart);

    std::string fallbackZipUrl;

    size_t pos = 0;
    while (pos < assetsArray.length()) {
        size_t objStart = assetsArray.find('{', pos);
        if (objStart == std::string::npos) break;

        int objDepth = 1;
        size_t objEnd = objStart + 1;
        while (objEnd < assetsArray.length() && objDepth > 0) {
            if (assetsArray[objEnd] == '{') objDepth++;
            else if (assetsArray[objEnd] == '}') objDepth--;
            objEnd++;
        }

        std::string asset = assetsArray.substr(objStart, objEnd - objStart);
        std::string name = ExtractJsonString(asset, "name");
        std::string nameLower = ToLower(name);
        std::string url = ExtractJsonString(asset, "browser_download_url");

        // Skip non-Windows platforms
        if (nameLower.find("linux") != std::string::npos ||
            nameLower.find("macos") != std::string::npos ||
            nameLower.find("darwin") != std::string::npos ||
            nameLower.find("mac-") != std::string::npos ||
            nameLower.find("-mac") != std::string::npos) {
            pos = objEnd;
            continue;
        }

        // Installer exe (Setup.exe, Installer.exe, etc.)
        if ((nameLower.find("setup") != std::string::npos ||
             nameLower.find("installer") != std::string::npos) &&
            nameLower.find(".exe") != std::string::npos) {
            assets.installerUrl = url;
        }
        // Zip file
        else if (nameLower.find(".zip") != std::string::npos) {
            if (nameLower.find("windows") != std::string::npos ||
                nameLower.find("win64") != std::string::npos ||
                nameLower.find("win32") != std::string::npos ||
                nameLower.find("win-") != std::string::npos ||
                nameLower.find("-win") != std::string::npos) {
                assets.zipUrl = url;
            } else if (fallbackZipUrl.empty()) {
                fallbackZipUrl = url;
            }
        }

        pos = objEnd;
    }

    if (assets.zipUrl.empty() && !fallbackZipUrl.empty()) {
        assets.zipUrl = fallbackZipUrl;
    }

    return assets;
}

// HTTP GET request using WinHTTP
static std::string HttpGet(const std::wstring& host, const std::wstring& path, bool https = true) {
    std::string result;

    HINTERNET hSession = WinHttpOpen(L"FastPlay/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);

    if (!hSession) return "";

    // Enable TLS 1.2 (required for GitHub API)
    DWORD secureProtocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    WinHttpSetOption(hSession, WINHTTP_OPTION_SECURE_PROTOCOLS, &secureProtocols, sizeof(secureProtocols));

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(),
        https ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT, 0);

    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return "";
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(),
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        https ? WINHTTP_FLAG_SECURE : 0);

    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    // GitHub API requires a User-Agent header
    WinHttpAddRequestHeaders(hRequest,
        L"Accept: application/vnd.github.v3+json\r\nUser-Agent: FastPlay/1.0",
        -1, WINHTTP_ADDREQ_FLAG_ADD);

    if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hRequest, NULL)) {

        DWORD bytesAvailable;
        while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable) && bytesAvailable > 0) {
            std::vector<char> buffer(bytesAvailable + 1);
            DWORD bytesRead;
            if (WinHttpReadData(hRequest, buffer.data(), bytesAvailable, &bytesRead)) {
                buffer[bytesRead] = 0;
                result += buffer.data();
            }
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return result;
}

// Download file with progress callback
static bool HttpDownload(const std::string& url, const std::wstring& destPath,
                         DownloadProgressCallback progressCallback) {
    std::wstring wurl(url.begin(), url.end());
    URL_COMPONENTS urlComp = {0};
    urlComp.dwStructSize = sizeof(urlComp);

    wchar_t hostName[256] = {0};
    wchar_t urlPath[2048] = {0};
    urlComp.lpszHostName = hostName;
    urlComp.dwHostNameLength = 256;
    urlComp.lpszUrlPath = urlPath;
    urlComp.dwUrlPathLength = 2048;

    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &urlComp)) {
        return false;
    }

    bool https = (urlComp.nScheme == INTERNET_SCHEME_HTTPS);

    HINTERNET hSession = WinHttpOpen(L"FastPlay/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);

    if (!hSession) return false;

    HINTERNET hConnect = WinHttpConnect(hSession, hostName, urlComp.nPort, 0);

    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return false;
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", urlPath,
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        https ? WINHTTP_FLAG_SECURE : 0);

    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    bool success = false;

    if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hRequest, NULL)) {

        // Check for redirect
        DWORD statusCode = 0;
        DWORD statusCodeSize = sizeof(statusCode);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeSize, WINHTTP_NO_HEADER_INDEX);

        if (statusCode >= 300 && statusCode < 400) {
            wchar_t redirectUrl[2048] = {0};
            DWORD redirectUrlSize = sizeof(redirectUrl);
            if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_LOCATION,
                    WINHTTP_HEADER_NAME_BY_INDEX, redirectUrl, &redirectUrlSize, WINHTTP_NO_HEADER_INDEX)) {
                WinHttpCloseHandle(hRequest);
                WinHttpCloseHandle(hConnect);
                WinHttpCloseHandle(hSession);

                std::wstring wRedirect(redirectUrl);
                std::string redirect(wRedirect.begin(), wRedirect.end());
                return HttpDownload(redirect, destPath, progressCallback);
            }
        }

        DWORD contentLength = 0;
        DWORD contentLengthSize = sizeof(contentLength);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &contentLength, &contentLengthSize, WINHTTP_NO_HEADER_INDEX);

        std::ofstream outFile(destPath, std::ios::binary);
        if (!outFile) {
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return false;
        }

        size_t totalDownloaded = 0;
        DWORD bytesAvailable;
        std::vector<char> buffer(65536);

        while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable) && bytesAvailable > 0) {
            DWORD toRead = (bytesAvailable < (DWORD)buffer.size()) ? bytesAvailable : (DWORD)buffer.size();
            DWORD bytesRead;
            if (WinHttpReadData(hRequest, buffer.data(), toRead, &bytesRead)) {
                outFile.write(buffer.data(), bytesRead);
                totalDownloaded += bytesRead;

                if (progressCallback) {
                    if (!progressCallback(totalDownloaded, contentLength)) {
                        outFile.close();
                        DeleteFileW(destPath.c_str());
                        WinHttpCloseHandle(hRequest);
                        WinHttpCloseHandle(hConnect);
                        WinHttpCloseHandle(hSession);
                        return false;
                    }
                }
            }
        }

        outFile.close();
        success = (totalDownloaded > 0);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return success;
}

// Get path to downloaded update zip
static std::wstring GetUpdateZipPath() {
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
static std::wstring GetUpdateInstallerPath() {
    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    return std::wstring(tempPath) + L"FastPlay-Setup.exe";
}

UpdateInfo CheckForUpdates() {
    UpdateInfo info = {false, "", "", "", "", "", ""};

    // Fetch releases from GitHub API
    std::string response = HttpGet(L"api.github.com", L"/repos/masonasons/FastPlay/releases");

    if (response.empty()) {
        info.errorMessage = "Failed to connect to GitHub. Please check your internet connection.";
        return info;
    }

    // Get first (latest) release
    std::string release = ExtractFirstArrayObject(response);
    if (release.empty()) {
        info.errorMessage = "No releases found.";
        return info;
    }

    std::string body = ExtractJsonString(release, "body");
    std::string tagName = ExtractJsonString(release, "tag_name");

    // Look for commit SHA in body: "commit XXXX"
    std::regex commitRegex("commit ([a-f0-9]+)");
    std::smatch commitMatch;
    if (std::regex_search(body, commitMatch, commitRegex)) {
        info.latestCommit = commitMatch[1].str();
    }

    // Look for version in body: "**Version:** X.Y.Z"
    std::regex versionRegex("\\*\\*Version:\\*\\* ([0-9.]+)");
    std::smatch versionMatch;
    if (std::regex_search(body, versionMatch, versionRegex)) {
        info.latestVersion = versionMatch[1].str();
    } else {
        info.latestVersion = tagName;
    }

    WindowsAssets assets = FindWindowsAssets(release);
    if (assets.zipUrl.empty() && assets.installerUrl.empty()) {
        info.errorMessage = "No Windows download available for this release.";
        return info;
    }

    info.downloadUrl = assets.zipUrl;
    info.installerUrl = assets.installerUrl;
    info.releaseNotes = body;

    // Compare commits if both are available, otherwise fall back to version strings
    std::string localCommit = BUILD_COMMIT;
    std::string localVersion = APP_VERSION;

    if (!info.latestCommit.empty() && !localCommit.empty()) {
        std::string latestShort = info.latestCommit.substr(0, 7);
        std::string localShort = localCommit.substr(0, 7);
        info.available = (latestShort != localShort);
    } else {
        info.available = (info.latestVersion != localVersion);
    }

    return info;
}

// Track whether we're updating with installer or zip
static bool g_updateWithInstaller = false;

bool DownloadUpdate(const std::string& url, DownloadProgressCallback progressCallback) {
    std::wstring destPath;
    std::string urlLower = ToLower(url);
    if ((urlLower.find("setup") != std::string::npos || urlLower.find("installer") != std::string::npos) &&
        urlLower.find(".exe") != std::string::npos) {
        destPath = GetUpdateInstallerPath();
        g_updateWithInstaller = true;
    } else {
        destPath = GetUpdateZipPath();
        g_updateWithInstaller = false;
    }
    return HttpDownload(url, destPath, progressCallback);
}

void ApplyUpdate() {
    if (g_updateWithInstaller) {
        std::wstring installerPath = GetUpdateInstallerPath();

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
        std::wstring zipPath = GetUpdateZipPath();
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

// Check for updates on startup: after a short delay, a silent check that only
// speaks up when an update is available.
void CheckForUpdatesOnStartup() {
    if (!g_checkForUpdates) return;

    std::thread([]() {
        Sleep(3000);
        RunOnUiThread([]() { ShowCheckForUpdatesDialog(true); });
    }).detach();
}
