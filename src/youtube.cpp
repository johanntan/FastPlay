#include "youtube.h"
#include "globals.h"
#include "utils.h"
#include <windows.h>
#include <wininet.h>
#include <shlwapi.h>
#include <regex>
#include <sstream>

#pragma comment(lib, "wininet.lib")

// Forward declarations
static bool SearchWithAPI(const std::wstring& query, std::vector<YouTubeResult>& results,
                          std::wstring& nextPageToken, const std::wstring& pageToken);
static bool SearchWithYtdlp(const std::wstring& query, std::vector<YouTubeResult>& results);
static std::wstring RunYtdlp(const std::wstring& args);
static std::wstring UrlEncode(const std::wstring& str);
static std::wstring HttpGet(const std::wstring& url);
static std::wstring ParseJsonString(const std::wstring& json, const std::wstring& key);
static std::vector<std::wstring> ParseJsonArray(const std::wstring& json, const std::wstring& arrayKey);

// Check if yt-dlp is available
bool IsYtdlpAvailable() {
    if (g_ytdlpPath.empty()) return false;
    return PathFileExistsW(g_ytdlpPath.c_str()) != FALSE;
}

// Check if API key is available
static bool HasApiKey() {
    return !g_ytApiKey.empty();
}

// URL encode a string
static std::wstring UrlEncode(const std::wstring& str) {
    std::string utf8 = WideToUtf8(str);
    std::wostringstream encoded;
    for (unsigned char c : utf8) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded << static_cast<wchar_t>(c);
        } else {
            encoded << L'%' << std::hex << std::uppercase
                    << ((c >> 4) & 0xF) << (c & 0xF);
        }
    }
    return encoded.str();
}

// Simple HTTP GET request
static std::wstring HttpGet(const std::wstring& url) {
    std::wstring result;
    HINTERNET hInternet = InternetOpenW(L"FastPlay/1.0", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hInternet) return result;

    HINTERNET hConnect = InternetOpenUrlW(hInternet, url.c_str(), nullptr, 0,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_SECURE, 0);
    if (hConnect) {
        char buffer[4096];
        DWORD bytesRead;
        std::string response;
        while (InternetReadFile(hConnect, buffer, sizeof(buffer) - 1, &bytesRead) && bytesRead > 0) {
            buffer[bytesRead] = '\0';
            response += buffer;
        }
        // Convert UTF-8 to wide string
        int len = MultiByteToWideChar(CP_UTF8, 0, response.c_str(), -1, nullptr, 0);
        if (len > 0) {
            result.resize(len - 1);
            MultiByteToWideChar(CP_UTF8, 0, response.c_str(), -1, &result[0], len);
        }
        InternetCloseHandle(hConnect);
    }
    InternetCloseHandle(hInternet);
    return result;
}

// Simple JSON string value parser (not a full JSON parser)
static std::wstring ParseJsonString(const std::wstring& json, const std::wstring& key) {
    std::wstring searchKey = L"\"" + key + L"\"";
    size_t keyPos = json.find(searchKey);
    if (keyPos == std::wstring::npos) return L"";

    size_t colonPos = json.find(L':', keyPos + searchKey.length());
    if (colonPos == std::wstring::npos) return L"";

    size_t startQuote = json.find(L'"', colonPos + 1);
    if (startQuote == std::wstring::npos) return L"";

    size_t endQuote = startQuote + 1;
    while (endQuote < json.length()) {
        if (json[endQuote] == L'"' && json[endQuote - 1] != L'\\') break;
        endQuote++;
    }

    std::wstring value = json.substr(startQuote + 1, endQuote - startQuote - 1);
    // Unescape basic sequences
    size_t pos = 0;
    while ((pos = value.find(L"\\n", pos)) != std::wstring::npos) {
        value.replace(pos, 2, L" ");
    }
    pos = 0;
    while ((pos = value.find(L"\\\"", pos)) != std::wstring::npos) {
        value.replace(pos, 2, L"\"");
    }
    return value;
}

// Run yt-dlp and capture output
static std::wstring RunYtdlp(const std::wstring& args) {
    if (!IsYtdlpAvailable()) return L"";

    std::wstring cmdLine = L"\"" + g_ytdlpPath + L"\" " + args;

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE hReadPipe, hWritePipe;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) return L"";

    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi;
    if (!CreateProcessW(nullptr, &cmdLine[0], nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(hReadPipe);
        CloseHandle(hWritePipe);
        return L"";
    }

    CloseHandle(hWritePipe);

    std::string output;
    char buffer[4096];
    DWORD bytesRead;
    while (ReadFile(hReadPipe, buffer, sizeof(buffer) - 1, &bytesRead, nullptr) && bytesRead > 0) {
        buffer[bytesRead] = '\0';
        output += buffer;
    }

    CloseHandle(hReadPipe);
    WaitForSingleObject(pi.hProcess, 30000);  // 30 second timeout
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    // Convert UTF-8 to wide string
    int len = MultiByteToWideChar(CP_UTF8, 0, output.c_str(), -1, nullptr, 0);
    if (len <= 0) return L"";
    std::wstring result(len - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, output.c_str(), -1, &result[0], len);
    return result;
}

// Search using YouTube Data API
static bool SearchWithAPI(const std::wstring& query, std::vector<YouTubeResult>& results,
                          std::wstring& nextPageToken, const std::wstring& pageToken) {
    if (!HasApiKey()) return false;

    std::wstring url = L"https://www.googleapis.com/youtube/v3/search?part=snippet&type=video&maxResults=25&q=";
    url += UrlEncode(query);
    url += L"&key=" + g_ytApiKey;
    if (!pageToken.empty()) {
        url += L"&pageToken=" + pageToken;
    }

    std::wstring response = HttpGet(url);
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

// Helper to try multiple JSON field names
static std::wstring ParseJsonStringMulti(const std::wstring& json, std::initializer_list<const wchar_t*> keys) {
    for (const wchar_t* key : keys) {
        std::wstring value = ParseJsonString(json, key);
        if (!value.empty()) return value;
    }
    return L"";
}

// Search using yt-dlp
static bool SearchWithYtdlp(const std::wstring& query, std::vector<YouTubeResult>& results) {
    // Use yt-dlp to search YouTube
    std::wstring args = L"--flat-playlist --dump-json \"ytsearch25:" + query + L"\"";
    OutputDebugStringW((L"[YT] Running: " + g_ytdlpPath + L" " + args + L"\n").c_str());
    std::wstring output = RunYtdlp(args);
    OutputDebugStringW((L"[YT] Output length: " + std::to_wstring(output.length()) + L"\n").c_str());
    if (output.length() < 500) {
        OutputDebugStringW((L"[YT] Output: " + output + L"\n").c_str());
    }
    if (output.empty()) return false;

    // Parse JSON lines (each line is a video)
    std::wistringstream iss(output);
    std::wstring line;
    while (std::getline(iss, line)) {
        // Strip trailing \r if present (Windows line endings)
        if (!line.empty() && line.back() == L'\r') {
            line.pop_back();
        }
        if (line.empty() || line[0] != L'{') continue;

        YouTubeResult result;
        // Try multiple field names for compatibility with different yt-dlp versions
        result.videoId = ParseJsonStringMulti(line, {L"id", L"video_id", L"display_id"});
        result.title = ParseJsonStringMulti(line, {L"title", L"fulltitle"});
        result.channel = ParseJsonStringMulti(line, {L"channel", L"uploader", L"uploader_id", L"channel_id"});
        result.duration = ParseJsonStringMulti(line, {L"duration_string", L"duration"});

        if (!result.videoId.empty() && !result.title.empty()) {
            results.push_back(result);
        }
    }

    return !results.empty();
}

// Public search function
bool YouTubeSearch(const std::wstring& query, std::vector<YouTubeResult>& results,
                   std::wstring& nextPageToken, const std::wstring& pageToken) {
    results.clear();
    nextPageToken.clear();

    OutputDebugStringW(L"[YT] YouTubeSearch called\n");
    OutputDebugStringW((L"[YT] HasApiKey: " + std::wstring(HasApiKey() ? L"yes" : L"no") + L"\n").c_str());
    OutputDebugStringW((L"[YT] IsYtdlpAvailable: " + std::wstring(IsYtdlpAvailable() ? L"yes" : L"no") + L"\n").c_str());

    // Try API first if available
    if (HasApiKey() && SearchWithAPI(query, results, nextPageToken, pageToken)) {
        OutputDebugStringW(L"[YT] API search succeeded\n");
        return true;
    }

    // Fall back to yt-dlp (only for first page, no pagination support)
    if (pageToken.empty() && IsYtdlpAvailable()) {
        OutputDebugStringW(L"[YT] Trying yt-dlp search\n");
        return SearchWithYtdlp(query, results);
    }

    OutputDebugStringW(L"[YT] No search method available\n");
    return false;
}

// Get playlist contents
bool YouTubeGetPlaylistContents(const std::wstring& playlistId, std::vector<YouTubeResult>& results,
                                std::wstring& nextPageToken, const std::wstring& pageToken) {
    results.clear();
    nextPageToken.clear();

    if (!IsYtdlpAvailable()) return false;

    std::wstring url = L"https://www.youtube.com/playlist?list=" + playlistId;
    std::wstring args = L"--flat-playlist --dump-json \"" + url + L"\"";
    std::wstring output = RunYtdlp(args);
    if (output.empty()) return false;

    std::wistringstream iss(output);
    std::wstring line;
    while (std::getline(iss, line)) {
        // Strip trailing \r if present (Windows line endings)
        if (!line.empty() && line.back() == L'\r') {
            line.pop_back();
        }
        if (line.empty() || line[0] != L'{') continue;

        YouTubeResult result;
        // Try multiple field names for compatibility with different yt-dlp versions
        result.videoId = ParseJsonStringMulti(line, {L"id", L"video_id", L"display_id"});
        result.title = ParseJsonStringMulti(line, {L"title", L"fulltitle"});
        result.channel = ParseJsonStringMulti(line, {L"channel", L"uploader", L"uploader_id", L"channel_id"});
        result.duration = ParseJsonStringMulti(line, {L"duration_string", L"duration"});

        if (!result.videoId.empty() && !result.title.empty()) {
            results.push_back(result);
        }
    }

    return !results.empty();
}

// Get stream URL for a video
bool YouTubeGetStreamURL(const std::wstring& videoId, std::wstring& streamUrl) {
    if (!IsYtdlpAvailable()) return false;

    // Get best audio format URL
    std::wstring url = L"https://www.youtube.com/watch?v=" + videoId;
    std::wstring args = L"-f bestaudio --get-url \"" + url + L"\"";
    std::wstring output = RunYtdlp(args);

    // Trim whitespace
    size_t start = output.find_first_not_of(L" \t\r\n");
    size_t end = output.find_last_not_of(L" \t\r\n");
    if (start != std::wstring::npos && end != std::wstring::npos) {
        streamUrl = output.substr(start, end - start + 1);
        return !streamUrl.empty() && streamUrl.find(L"http") == 0;
    }

    return false;
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

// Cleanup temporary files and resources
// (search results are kept by the YouTube window, so nothing is held here)
void YouTubeCleanup() {
}
