#pragma once
#ifndef FASTPLAY_HTTP_H
#define FASTPLAY_HTTP_H

// HTTP(S) GET requests: radio and podcast lookups, feed fetches, redirect resolution
// and downloads. WinInet on Windows (src/platform/http_windows.cpp), libcurl
// elsewhere (src/platform/http_curl.cpp). Blocking; call from a worker thread for
// anything long.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct HttpOptions {
    // Extra request headers, one "Name: value" per entry.
    std::vector<std::wstring> headers;
    // Credentials for protected feeds: sent up front as Basic authentication, and
    // used once more if the server still asks.
    std::wstring username;
    std::wstring password;
    // Follow redirects (up to 6), across schemes and hosts.
    bool followRedirects = true;
    // False: stop once the final response's headers arrive (to learn where a URL
    // leads without downloading it).
    bool readBody = true;
    // Stream the body into this file instead of memory.
    std::wstring saveTo;
    // Called as data arrives with bytes received and the total (0 if unknown).
    // Returning false cancels the request.
    std::function<bool(uint64_t received, uint64_t total)> progress;
    // Connect / send / receive timeout; 0 keeps the system default.
    unsigned timeoutMs = 0;
};

struct HttpResult {
    bool completed = false;          // a final response arrived (any status)
    bool cancelled = false;          // the progress callback stopped it
    unsigned long status = 0;        // HTTP status of the final response
    std::string body;                // the body, unless readBody was false or saveTo was set
    uint64_t bytesReceived = 0;
    std::wstring finalUrl;           // the URL after redirects
    unsigned long systemError = 0;   // the system's error code when the request failed
    std::wstring errorText;          // and a readable description of it
};

HttpResult HttpGet(const std::wstring& url, const HttpOptions& options = HttpOptions());

// "Authorization: Basic <base64(user:pass)>" (UTF-8), or empty if both are empty.
std::wstring BuildBasicAuthHeader(const std::wstring& username, const std::wstring& password);

#endif // FASTPLAY_HTTP_H
