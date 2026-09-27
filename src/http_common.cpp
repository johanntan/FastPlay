// HTTP helpers shared by every platform.

#include "http.h"
#include "utils.h"

// Build an "Authorization: Basic <base64(user:pass)>" header from credentials (UTF-8 encoded).
// Returns an empty string when no credentials are given.
std::wstring BuildBasicAuthHeader(const std::wstring& username, const std::wstring& password) {
    if (username.empty() && password.empty()) return L"";

    std::string creds = WideToUtf8(username) + ":" + WideToUtf8(password);

    static const wchar_t b64[] = L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::wstring encoded;
    size_t i = 0;
    while (i + 2 < creds.size()) {
        unsigned int n = (static_cast<unsigned char>(creds[i]) << 16) |
                         (static_cast<unsigned char>(creds[i + 1]) << 8) |
                         static_cast<unsigned char>(creds[i + 2]);
        encoded += b64[(n >> 18) & 63];
        encoded += b64[(n >> 12) & 63];
        encoded += b64[(n >> 6) & 63];
        encoded += b64[n & 63];
        i += 3;
    }
    if (i + 1 == creds.size()) {
        unsigned int n = static_cast<unsigned char>(creds[i]) << 16;
        encoded += b64[(n >> 18) & 63];
        encoded += b64[(n >> 12) & 63];
        encoded += L"==";
    } else if (i + 2 == creds.size()) {
        unsigned int n = (static_cast<unsigned char>(creds[i]) << 16) |
                         (static_cast<unsigned char>(creds[i + 1]) << 8);
        encoded += b64[(n >> 18) & 63];
        encoded += b64[(n >> 12) & 63];
        encoded += b64[(n >> 6) & 63];
        encoded += L"=";
    }

    return L"Authorization: Basic " + encoded;
}
