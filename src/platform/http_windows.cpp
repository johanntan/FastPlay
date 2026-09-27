// HTTP through WinInet.

#include "http.h"
#include "utils.h"

#include <windows.h>
#include <wininet.h>

#include <cstdio>

#pragma comment(lib, "wininet.lib")

// Format a WinInet / Win32 error code into a human-readable message
static std::wstring FormatWinInetError(DWORD code) {
    if (code == 0) return L"";
    wchar_t* buf = nullptr;
    HMODULE hWinInet = GetModuleHandleW(L"wininet.dll");
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_IGNORE_INSERTS |
                  FORMAT_MESSAGE_FROM_SYSTEM;
    if (hWinInet) flags |= FORMAT_MESSAGE_FROM_HMODULE;
    FormatMessageW(flags, hWinInet, code, 0, reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    std::wstring result = buf ? buf : L"";
    if (buf) LocalFree(buf);
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n' ||
                               result.back() == L' ' || result.back() == L'.')) {
        result.pop_back();
    }
    return result;
}

static void Fail(HttpResult& result) {
    result.systemError = GetLastError();
    result.errorText = FormatWinInetError(result.systemError);
}

// Redirects are followed by hand. WinInet's automatic redirection refuses an
// HTTPS -> HTTP "downgrade" (HttpSendRequestW fails outright), which breaks feeds
// that 301 an https:// canonical URL to an http:// host and media that redirects to
// a delivery-script URL; doing it here also crosses schemes and hosts freely.
// Credentials and extra headers are sent on every hop.
HttpResult HttpGet(const std::wstring& startUrl, const HttpOptions& options) {
    HttpResult result;
    std::wstring authHeader = BuildBasicAuthHeader(options.username, options.password);
    std::wstring extraHeaders;
    for (const auto& header : options.headers) {
        extraHeaders += header + L"\r\n";
    }
    if (!authHeader.empty()) extraHeaders += authHeader + L"\r\n";

    HINTERNET hInternet = InternetOpenW(Utf8ToWide(UserAgent()).c_str(), INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hInternet) {
        Fail(result);
        return result;
    }
    if (options.timeoutMs) {
        DWORD timeout = options.timeoutMs;
        InternetSetOptionW(hInternet, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
        InternetSetOptionW(hInternet, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));
        InternetSetOptionW(hInternet, INTERNET_OPTION_SEND_TIMEOUT, &timeout, sizeof(timeout));
    }

    std::wstring url = startUrl;
    const int MAX_HOPS = 6;
    for (int hop = 0; hop < MAX_HOPS; hop++) {
        // Split the URL (host, port, path and query).
        wchar_t host[256] = {0}, path[4096] = {0}, extra[4096] = {0};
        URL_COMPONENTSW parts = {};
        parts.dwStructSize = sizeof(parts);
        parts.lpszHostName = host;
        parts.dwHostNameLength = 256;
        parts.lpszUrlPath = path;
        parts.dwUrlPathLength = 4096;
        parts.lpszExtraInfo = extra;
        parts.dwExtraInfoLength = 4096;
        if (!InternetCrackUrlW(url.c_str(), 0, 0, &parts) ||
            (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS)) {
            result.errorText = L"Unsupported URL scheme (expected http:// or https://)";
            break;
        }
        bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
        std::wstring object = std::wstring(path[0] ? path : L"/") + extra;

        DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE |
                      INTERNET_FLAG_NO_AUTO_REDIRECT | INTERNET_FLAG_KEEP_CONNECTION;
        if (secure) flags |= INTERNET_FLAG_SECURE;

        HINTERNET hConnect = InternetConnectW(hInternet, host, parts.nPort,
                                              options.username.empty() ? nullptr : options.username.c_str(),
                                              options.password.empty() ? nullptr : options.password.c_str(),
                                              INTERNET_SERVICE_HTTP, 0, 0);
        if (!hConnect) {
            Fail(result);
            break;
        }

        HINTERNET hRequest = HttpOpenRequestW(hConnect, L"GET", object.c_str(), nullptr, nullptr, nullptr, flags, 0);
        if (!hRequest) {
            Fail(result);
            InternetCloseHandle(hConnect);
            break;
        }

        // Send the headers (and Basic credentials, preemptively: WinInet only attaches
        // the InternetConnect credentials after a 401, and only on a resend).
        if (!extraHeaders.empty()) {
            HttpAddRequestHeadersW(hRequest, extraHeaders.c_str(), static_cast<DWORD>(extraHeaders.size()),
                                   HTTP_ADDREQ_FLAG_ADD | HTTP_ADDREQ_FLAG_REPLACE);
        }

        BOOL sent = HttpSendRequestW(hRequest, nullptr, 0, nullptr, 0);
        // If the server still challenges (Digest/NTLM realm), resend once so
        // WinInet can answer with the InternetConnect credentials.
        if (sent && (!options.username.empty() || !options.password.empty())) {
            DWORD status = 0, sz = sizeof(status);
            HttpQueryInfoW(hRequest, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &sz, nullptr);
            if (status == HTTP_STATUS_DENIED) sent = HttpSendRequestW(hRequest, nullptr, 0, nullptr, 0);
        }
        if (!sent) {
            Fail(result);
            InternetCloseHandle(hRequest);
            InternetCloseHandle(hConnect);
            break;
        }

        DWORD status = 0, sz = sizeof(status);
        HttpQueryInfoW(hRequest, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &sz, nullptr);

        // Follow 301/302/303/307/308 to the Location target ourselves.
        bool isRedirect = (status == 301 || status == 302 || status == 303 ||
                           status == 307 || status == 308);
        if (options.followRedirects && isRedirect && hop + 1 < MAX_HOPS) {
            wchar_t loc[2048];
            DWORD locBytes = sizeof(loc);
            if (HttpQueryInfoW(hRequest, HTTP_QUERY_LOCATION, loc, &locBytes, nullptr)) {
                // Resolve a possibly-relative Location against the current URL.
                wchar_t combined[2048];
                DWORD combChars = 2048;
                if (InternetCombineUrlW(url.c_str(), loc, combined, &combChars, ICU_NO_ENCODE)) {
                    url = combined;
                } else {
                    url = loc;
                }
                InternetCloseHandle(hRequest);
                InternetCloseHandle(hConnect);
                continue;  // Next hop.
            }
        }

        // Terminal (non-redirect) response.
        result.completed = true;
        result.status = status;
        result.finalUrl = url;
        if (options.readBody) {
            DWORD contentLength = 0, clSize = sizeof(contentLength);
            HttpQueryInfoW(hRequest, HTTP_QUERY_CONTENT_LENGTH | HTTP_QUERY_FLAG_NUMBER, &contentLength, &clSize, nullptr);

            FILE* file = nullptr;
            if (!options.saveTo.empty()) {
                file = FileOpen(options.saveTo, "wb");
                if (!file) {
                    result.completed = false;
                    result.errorText = L"Could not create the file";
                }
            }
            if (options.saveTo.empty() || file) {
                char buffer[8192];
                DWORD bytesRead;
                while (InternetReadFile(hRequest, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0) {
                    if (file) fwrite(buffer, 1, bytesRead, file);
                    else result.body.append(buffer, bytesRead);
                    result.bytesReceived += bytesRead;
                    if (options.progress && !options.progress(result.bytesReceived, contentLength)) {
                        result.cancelled = true;
                        break;
                    }
                }
                if (file) fclose(file);
            }
        }
        InternetCloseHandle(hRequest);
        InternetCloseHandle(hConnect);
        break;
    }

    InternetCloseHandle(hInternet);
    return result;
}
