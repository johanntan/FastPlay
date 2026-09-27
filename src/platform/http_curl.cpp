// HTTP through libcurl (macOS ships it with the system).

#include "http.h"
#include "utils.h"

#include <curl/curl.h>

#include <cstdio>
#include <mutex>

namespace {

struct Transfer {
    const HttpOptions* options;
    HttpResult* result;
    FILE* file = nullptr;
    bool stopAfterHeaders = false;
};

size_t OnData(char* data, size_t size, size_t count, void* user) {
    Transfer* t = static_cast<Transfer*>(user);
    size_t bytes = size * count;
    // Only the headers were wanted: stop at the first byte of the body.
    if (t->stopAfterHeaders) return 0;
    if (t->file) {
        if (fwrite(data, 1, bytes, t->file) != bytes) return 0;
    } else {
        t->result->body.append(data, bytes);
    }
    t->result->bytesReceived += bytes;
    return bytes;
}

int OnProgress(void* user, curl_off_t total, curl_off_t received, curl_off_t, curl_off_t) {
    Transfer* t = static_cast<Transfer*>(user);
    if (t->options->progress && received > 0 &&
        !t->options->progress(static_cast<uint64_t>(received), static_cast<uint64_t>(total))) {
        t->result->cancelled = true;
        return 1;  // abort
    }
    return 0;
}

std::once_flag g_curlInit;

}  // namespace

HttpResult HttpGet(const std::wstring& url, const HttpOptions& options) {
    std::call_once(g_curlInit, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });

    HttpResult result;
    CURL* curl = curl_easy_init();
    if (!curl) {
        result.errorText = L"Could not start a network request";
        return result;
    }

    Transfer transfer;
    transfer.options = &options;
    transfer.result = &result;
    transfer.stopAfterHeaders = !options.readBody;

    if (!options.saveTo.empty() && options.readBody) {
        transfer.file = FileOpen(options.saveTo, "wb");
        if (!transfer.file) {
            result.errorText = L"Could not create the file";
            curl_easy_cleanup(curl);
            return result;
        }
    }

    std::string urlUtf8 = WideToUtf8(url);
    curl_easy_setopt(curl, CURLOPT_URL, urlUtf8.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, UserAgent().c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, options.followRedirects ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);  // six requests in all, as on Windows
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, OnData);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &transfer);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, OnProgress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &transfer);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, options.progress ? 0L : 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if (options.timeoutMs) {
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(options.timeoutMs));
        // A stalled transfer (under 1 byte/s for the timeout) is abandoned.
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, static_cast<long>((options.timeoutMs + 999) / 1000));
    }

    std::string username = WideToUtf8(options.username), password = WideToUtf8(options.password);
    if (!options.username.empty() || !options.password.empty()) {
        // Basic up front; other schemes if the server asks. Kept across redirects, as on Windows.
        curl_easy_setopt(curl, CURLOPT_USERNAME, username.c_str());
        curl_easy_setopt(curl, CURLOPT_PASSWORD, password.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, static_cast<long>(CURLAUTH_ANY));
        curl_easy_setopt(curl, CURLOPT_UNRESTRICTED_AUTH, 1L);
    }

    struct curl_slist* headers = nullptr;
    for (const auto& header : options.headers) {
        headers = curl_slist_append(headers, WideToUtf8(header).c_str());
    }
    if (!options.username.empty() || !options.password.empty()) {
        headers = curl_slist_append(headers, WideToUtf8(BuildBasicAuthHeader(options.username, options.password)).c_str());
    }
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    CURLcode code = curl_easy_perform(curl);

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    char* effective = nullptr;
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective);

    // Stopping on purpose after the headers is still a complete answer.
    bool stoppedAtBody = transfer.stopAfterHeaders && code == CURLE_WRITE_ERROR;
    if ((code == CURLE_OK || stoppedAtBody) && status > 0) {
        result.completed = true;
        result.status = static_cast<unsigned long>(status);
        result.finalUrl = effective ? Utf8ToWide(effective) : url;
    } else if (!result.cancelled) {
        result.systemError = static_cast<unsigned long>(code);
        result.errorText = Utf8ToWide(curl_easy_strerror(code));
    }

    if (transfer.file) fclose(transfer.file);
    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return result;
}
