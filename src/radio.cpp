// Internet radio: the station directory searches (RadioBrowser, TuneIn,
// iHeartRadio), resolving playlist and redirecting stream URLs, and importing /
// exporting radio favorites. The Radio window is src/ui/radio_dialog.cpp.

#include "radio.h"
#include "http.h"
#include "playlist_io.h"
#include "ini.h"
#include "database.h"
#include "utils.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// HTTP GET request; extraHeaders are "Name: value\r\n" lines sent after "Accept: */*".
static std::wstring RadioHttpGet(const std::wstring& url, const wchar_t* extraHeaders = nullptr) {
    HttpOptions options;
    options.headers.push_back(L"Accept: */*");
    if (extraHeaders) {
        std::wstring extra = extraHeaders;
        size_t start = 0;
        while (start < extra.size()) {
            size_t end = extra.find(L"\r\n", start);
            if (end == std::wstring::npos) end = extra.size();
            if (end > start) options.headers.push_back(extra.substr(start, end - start));
            start = end + 2;
        }
    }
    HttpResult response = HttpGet(url, options);
    return Utf8ToWide(response.body);
}

// URL encode a string
static std::wstring RadioUrlEncode(const std::wstring& str) {
    std::wstring result;
    for (wchar_t c : str) {
        if ((c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') ||
            (c >= L'0' && c <= L'9') || c == L'-' || c == L'_' || c == L'.' || c == L'~') {
            result += c;
        } else if (c == L' ') {
            result += L'+';
        } else {
            // Convert to UTF-8 and percent-encode
            for (unsigned char byte : WideToUtf8(std::wstring(1, c))) {
                wchar_t hex[4];
                swprintf(hex, 4, L"%%%02X", byte);
                result += hex;
            }
        }
    }
    return result;
}

// Helper to extract JSON string value
static std::wstring ExtractJsonString(const std::wstring& obj, const std::wstring& key) {
    std::wstring search = L"\"" + key + L"\":\"";
    size_t start = obj.find(search);
    if (start == std::wstring::npos) return L"";
    start += search.length();
    size_t end = start;
    while (end < obj.length()) {
        if (obj[end] == L'"' && (end == start || obj[end-1] != L'\\')) break;
        end++;
    }
    return obj.substr(start, end - start);
}

// Helper to extract JSON int value
static int ExtractJsonInt(const std::wstring& obj, const std::wstring& key) {
    std::wstring search = L"\"" + key + L"\":";
    size_t start = obj.find(search);
    if (start == std::wstring::npos) return 0;
    start += search.length();
    while (start < obj.length() && (obj[start] == L' ' || obj[start] == L'\t')) start++;
    return static_cast<int>(std::wcstol(obj.c_str() + start, nullptr, 10));
}

// Helper to extract JSON value as string (handles both "key":"value" and "key":123)
static std::wstring ExtractJsonValue(const std::wstring& obj, const std::wstring& key) {
    std::wstring search = L"\"" + key + L"\":";
    size_t start = obj.find(search);
    if (start == std::wstring::npos) return L"";
    start += search.length();
    while (start < obj.length() && (obj[start] == L' ' || obj[start] == L'\t')) start++;
    if (start >= obj.length()) return L"";

    // Check if it's a quoted string
    if (obj[start] == L'"') {
        start++;
        size_t end = start;
        while (end < obj.length()) {
            if (obj[end] == L'"' && (end == start || obj[end-1] != L'\\')) break;
            end++;
        }
        return obj.substr(start, end - start);
    }

    // It's a number or other unquoted value
    size_t end = start;
    while (end < obj.length() && obj[end] != L',' && obj[end] != L'}' && obj[end] != L']') {
        end++;
    }
    return obj.substr(start, end - start);
}
// Fetch RadioBrowser's country names (most stations first)
std::vector<std::wstring> FetchRadioCountries() {
    std::vector<std::wstring> countries;
    std::wstring url = L"https://de1.api.radio-browser.info/json/countries?hidebroken=true&order=stationcount&reverse=true";
    std::wstring json = RadioHttpGet(url);

    if (!json.empty()) {
        size_t pos = 0;
        while ((pos = json.find(L'{', pos)) != std::wstring::npos) {
            int depth = 1;
            size_t endPos = pos + 1;
            bool inString = false;
            while (endPos < json.length() && depth > 0) {
                wchar_t c = json[endPos];
                if (c == L'"' && (endPos == 0 || json[endPos-1] != L'\\')) {
                    inString = !inString;
                } else if (!inString) {
                    if (c == L'{') depth++;
                    else if (c == L'}') depth--;
                }
                endPos++;
            }
            if (depth != 0) break;
            std::wstring obj = json.substr(pos, endPos - pos);
            std::wstring name = ExtractJsonString(obj, L"name");
            if (!name.empty()) {
                countries.push_back(name);
            }
            pos = endPos;
        }
    }
    return countries;
}
// Search RadioBrowser API
bool SearchRadioBrowser(const std::wstring& query, const std::wstring& country, std::vector<RadioSearchResult>& results) {
    results.clear();

    // Use search endpoint; name and country are both optional filters
    std::wstring url = L"https://de1.api.radio-browser.info/json/stations/search?limit=100&hidebroken=true";
    if (!query.empty()) {
        url += L"&name=" + RadioUrlEncode(query);
    }
    if (!country.empty()) {
        url += L"&country=" + RadioUrlEncode(country);
        // When browsing a whole country, sort by popularity so the good stations surface first
        if (query.empty()) {
            url += L"&order=clickcount&reverse=true";
        }
    }
    std::wstring json = RadioHttpGet(url);
    if (json.empty()) return false;

    // Simple JSON array parser - find each object by tracking brace depth
    size_t pos = 0;
    while ((pos = json.find(L'{', pos)) != std::wstring::npos) {
        // Find matching closing brace by tracking depth
        int depth = 1;
        size_t endPos = pos + 1;
        bool inString = false;
        while (endPos < json.length() && depth > 0) {
            wchar_t c = json[endPos];
            if (c == L'"' && (endPos == 0 || json[endPos-1] != L'\\')) {
                inString = !inString;
            } else if (!inString) {
                if (c == L'{') depth++;
                else if (c == L'}') depth--;
            }
            endPos++;
        }
        if (depth != 0) break;

        std::wstring obj = json.substr(pos, endPos - pos);
        RadioSearchResult result;
        result.source = 0;  // RadioBrowser

        result.name = ExtractJsonString(obj, L"name");
        result.url = ExtractJsonString(obj, L"url_resolved");
        if (result.url.empty()) result.url = ExtractJsonString(obj, L"url");
        result.country = ExtractJsonString(obj, L"country");
        result.codec = ExtractJsonString(obj, L"codec");
        result.bitrate = ExtractJsonInt(obj, L"bitrate");

        // Only add if we have a name and URL
        if (!result.name.empty() && !result.url.empty()) {
            results.push_back(result);
        }

        pos = endPos;
    }

    return !results.empty();
}

// Parse playlist content (M3U or PLS) to extract stream URL
static std::wstring ParsePlaylistContent(const std::string& content) {
    // Make lowercase copy for case-insensitive searching
    std::string lower = content;
    for (auto& c : lower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));

    // Check if it's a PLS file (case-insensitive)
    if (lower.find("[playlist]") != std::string::npos) {
        // Look for File1= (case-insensitive search, then extract from original)
        size_t filePos = lower.find("file1=");
        if (filePos != std::string::npos) {
            filePos += 6;
            size_t endPos = content.find_first_of("\r\n", filePos);
            if (endPos == std::string::npos) endPos = content.length();
            std::string url = content.substr(filePos, endPos - filePos);
            // Trim whitespace
            while (!url.empty() && (url.back() == ' ' || url.back() == '\t')) url.pop_back();
            while (!url.empty() && (url.front() == ' ' || url.front() == '\t')) url.erase(0, 1);
            if (!url.empty()) return Utf8ToWide(url);
        }
    }

    // Check if it's an M3U file
    // Look for first non-comment, non-empty line that starts with http
    std::istringstream iss(content);
    std::string line;
    while (std::getline(iss, line)) {
        // Trim
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.erase(0, 1);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) line.pop_back();

        if (line.empty() || line[0] == '#') continue;
        if (line.find("http") == 0) {
            return Utf8ToWide(line);
        }
    }

    // Maybe the content itself is a redirect URL
    if (content.find("http") == 0) {
        size_t endPos = content.find_first_of("\r\n \t");
        if (endPos == std::string::npos) endPos = content.length();
        return Utf8ToWide(content.substr(0, endPos));
    }

    return L"";
}

// Check if URL looks like a playlist file
bool IsPlaylistUrl(const std::wstring& url) {
    std::wstring lower = url;
    for (auto& c : lower) c = towlower(c);

    // Check extension (before any query string)
    size_t queryPos = lower.find(L'?');
    std::wstring path = (queryPos != std::wstring::npos) ? lower.substr(0, queryPos) : lower;

    return path.length() > 4 && (
        path.substr(path.length() - 4) == L".m3u" ||
        path.substr(path.length() - 4) == L".pls" ||
        (path.length() > 5 && path.substr(path.length() - 5) == L".m3u8")
    );
}

// Parse playlist content (M3U or PLS) to extract ALL stream URLs
static std::vector<StreamOption> ParsePlaylistContentMultiple(const std::string& content) {
    std::vector<StreamOption> urls;

    // Make lowercase copy for case-insensitive searching
    std::string lower = content;
    for (auto& c : lower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));

    // Check if it's a PLS file (case-insensitive)
    if (lower.find("[playlist]") != std::string::npos) {
        // Look for File1=, File2=, etc.
        for (int i = 1; i <= 20; i++) {
            std::string key = "file" + std::to_string(i) + "=";
            size_t filePos = lower.find(key);
            if (filePos == std::string::npos) continue;

            filePos += key.length();
            size_t endPos = content.find_first_of("\r\n", filePos);
            if (endPos == std::string::npos) endPos = content.length();
            std::string url = content.substr(filePos, endPos - filePos);
            // Trim whitespace
            while (!url.empty() && (url.back() == ' ' || url.back() == '\t')) url.pop_back();
            while (!url.empty() && (url.front() == ' ' || url.front() == '\t')) url.erase(0, 1);

            if (!url.empty()) {
                // Look for corresponding Title
                std::string titleKey = "title" + std::to_string(i) + "=";
                size_t titlePos = lower.find(titleKey);
                std::wstring label = L"Stream " + std::to_wstring(i);
                if (titlePos != std::string::npos) {
                    titlePos += titleKey.length();
                    size_t titleEnd = content.find_first_of("\r\n", titlePos);
                    if (titleEnd == std::string::npos) titleEnd = content.length();
                    std::string title = content.substr(titlePos, titleEnd - titlePos);
                    while (!title.empty() && (title.back() == ' ' || title.back() == '\t')) title.pop_back();
                    while (!title.empty() && (title.front() == ' ' || title.front() == '\t')) title.erase(0, 1);
                    if (!title.empty()) label = Utf8ToWide(title);
                }
                urls.push_back({Utf8ToWide(url), label});
            }
        }
        if (!urls.empty()) return urls;
    }

    // Check if it's an M3U file - get all http URLs
    std::istringstream iss(content);
    std::string line;
    std::string pendingTitle;
    int streamNum = 1;
    while (std::getline(iss, line)) {
        // Trim
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.erase(0, 1);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) line.pop_back();

        if (line.empty()) continue;

        // Check for #EXTINF line with title
        if (line.find("#EXTINF:") == 0) {
            size_t commaPos = line.find(',');
            if (commaPos != std::string::npos && commaPos + 1 < line.length()) {
                pendingTitle = line.substr(commaPos + 1);
            }
            continue;
        }

        if (line[0] == '#') continue;

        if (line.find("http") == 0) {
            std::wstring label = pendingTitle.empty() ?
                L"Stream " + std::to_wstring(streamNum) : Utf8ToWide(pendingTitle);
            urls.push_back({Utf8ToWide(line), label});
            pendingTitle.clear();
            streamNum++;
        }
    }

    return urls;
}

// Resolve a TuneIn playlist URL to get the actual stream URL
static std::wstring ResolveTuneInUrl(const std::wstring& playlistUrl) {
    std::wstring currentUrl = playlistUrl;

    // Follow up to 3 levels of playlist redirects
    for (int i = 0; i < 3; i++) {
        std::wstring content = RadioHttpGet(currentUrl);
        if (content.empty()) return L"";

        // Convert to narrow string for parsing
        std::string narrow = WideToUtf8(content);

        // Parse the playlist content
        std::wstring streamUrl = ParsePlaylistContent(narrow);
        if (streamUrl.empty()) return L"";

        // If the result is another playlist, fetch and parse it
        if (IsPlaylistUrl(streamUrl)) {
            currentUrl = streamUrl;
            continue;
        }

        // Found a direct stream URL
        return streamUrl;
    }

    // If we've followed too many redirects, return the last URL we got
    return currentUrl;
}

// Resolve a TuneIn playlist URL to get ALL stream URLs
static std::vector<StreamOption> ResolveTuneInUrls(const std::wstring& playlistUrl) {
    std::vector<StreamOption> result;
    std::wstring currentUrl = playlistUrl;

    // First fetch the playlist
    std::wstring content = RadioHttpGet(currentUrl);
    if (content.empty()) return result;

    std::string narrow = WideToUtf8(content);

    // Get all URLs from the playlist
    result = ParsePlaylistContentMultiple(narrow);

    // If we got multiple results, check if any are playlists themselves and resolve them
    if (result.size() == 1 && IsPlaylistUrl(result[0].url)) {
        // Single result is another playlist, follow it
        std::wstring nested = RadioHttpGet(result[0].url);
        if (!nested.empty()) {
            std::string nestedNarrow = WideToUtf8(nested);
            auto nestedUrls = ParsePlaylistContentMultiple(nestedNarrow);
            if (!nestedUrls.empty()) {
                result = nestedUrls;
            }
        }
    }

    // Filter out any remaining playlist URLs and deduplicate
    std::vector<StreamOption> filtered;
    std::set<std::wstring> seen;
    for (const auto& opt : result) {
        if (!IsPlaylistUrl(opt.url) && seen.find(opt.url) == seen.end()) {
            filtered.push_back(opt);
            seen.insert(opt.url);
        }
    }

    return filtered;
}

// Resolve a remote playlist URL (.m3u/.pls/.m3u8) to a direct stream URL.
// Used by the playback path so saved favorites or directly-opened URLs that
// point at a playlist file get resolved to a real stream before reaching BASS
// (which can't parse a playlist). Returns the url unchanged when it isn't a
// playlist URL, or when the fetch/parse fails (so the caller can still try it).
std::wstring ResolvePlaylistUrl(const std::wstring& url) {
    if (!IsPlaylistUrl(url)) return url;

    std::wstring currentUrl = url;
    // Follow up to 3 nested playlist levels (a playlist can point at another).
    for (int i = 0; i < 3; i++) {
        std::wstring content = RadioHttpGet(currentUrl);
        if (content.empty()) return url;  // Fetch failed - fall back to original.

        // An HLS playlist is the stream itself (BASSHLS plays it); its entries are
        // variants or few-second segments, not alternative stream addresses.
        if (content.find(L"#EXT-X-") != std::wstring::npos) return currentUrl;

        std::string narrow = WideToUtf8(content);
        std::vector<StreamOption> options = ParsePlaylistContentMultiple(narrow);

        // Pick the first non-empty entry (playlists often list failover mirrors).
        std::wstring next;
        for (const auto& opt : options) {
            if (!opt.url.empty()) { next = opt.url; break; }
        }
        if (next.empty()) return url;  // Couldn't parse - fall back to original.

        if (IsPlaylistUrl(next)) { currentUrl = next; continue; }  // Nested playlist.
        return next;  // Resolved to a direct stream.
    }
    return currentUrl;
}

// Search TuneIn API (returns OPML/XML)
bool SearchTuneIn(const std::wstring& query, std::vector<RadioSearchResult>& results) {
    results.clear();

    // TuneIn search endpoint
    std::wstring url = L"http://opml.radiotime.com/Search.ashx?query=" + RadioUrlEncode(query);
    std::wstring xml = RadioHttpGet(url);
    if (xml.empty()) return false;

    // Parse OPML - look for <outline> elements with type="audio"
    size_t pos = 0;
    while ((pos = xml.find(L"<outline", pos)) != std::wstring::npos) {
        size_t endPos = xml.find(L"/>", pos);
        if (endPos == std::wstring::npos) {
            endPos = xml.find(L"</outline>", pos);
            if (endPos == std::wstring::npos) break;
        }

        std::wstring elem = xml.substr(pos, endPos - pos + 2);

        // Check if it's an audio type (station)
        if (elem.find(L"type=\"audio\"") != std::wstring::npos) {
            RadioSearchResult result;
            result.source = 1;  // TuneIn
            result.bitrate = 0;

            // Extract attributes
            auto extractAttr = [&elem](const std::wstring& attr) -> std::wstring {
                std::wstring search = attr + L"=\"";
                size_t start = elem.find(search);
                if (start == std::wstring::npos) return L"";
                start += search.length();
                size_t end = elem.find(L'"', start);
                if (end == std::wstring::npos) return L"";
                return elem.substr(start, end - start);
            };

            result.name = extractAttr(L"text");
            result.url = extractAttr(L"URL");  // Store TuneIn URL, resolve later
            std::wstring subtext = extractAttr(L"subtext");

            // subtext often contains location info
            if (!subtext.empty()) {
                result.country = subtext;
            }

            // Bitrate might be in bitrate attribute
            std::wstring bitrateStr = extractAttr(L"bitrate");
            if (!bitrateStr.empty()) {
                result.bitrate = static_cast<int>(std::wcstol(bitrateStr.c_str(), nullptr, 10));
            }

            // Decode HTML entities in name
            size_t ampPos;
            while ((ampPos = result.name.find(L"&amp;")) != std::wstring::npos) {
                result.name.replace(ampPos, 5, L"&");
            }
            while ((ampPos = result.name.find(L"&apos;")) != std::wstring::npos) {
                result.name.replace(ampPos, 6, L"'");
            }
            while ((ampPos = result.name.find(L"&quot;")) != std::wstring::npos) {
                result.name.replace(ampPos, 6, L"\"");
            }

            // Only add if we have a name and URL
            if (!result.name.empty() && !result.url.empty()) {
                results.push_back(result);
            }
        }

        pos = endPos + 1;
    }

    return !results.empty();
}

// Get stream URL for an iHeartRadio station by ID
static std::wstring GetIHeartStreamUrl(const std::wstring& stationId) {
    // Use the live station API to get stream URLs
    std::wstring url = L"https://api.iheart.com/api/v2/content/liveStations/" + stationId;
    std::wstring json = RadioHttpGet(url, L"Accept: application/json\r\n");
    if (json.empty()) return L"";

    // Look for streams section
    size_t streamsPos = json.find(L"\"streams\"");
    if (streamsPos == std::wstring::npos) return L"";

    std::wstring streamsSection = json.substr(streamsPos);

    // Try different stream types in order of preference
    std::wstring streamUrl = ExtractJsonString(streamsSection, L"shoutcast_stream");
    if (streamUrl.empty()) {
        streamUrl = ExtractJsonString(streamsSection, L"secure_shoutcast_stream");
    }
    if (streamUrl.empty()) {
        streamUrl = ExtractJsonString(streamsSection, L"pls_stream");
    }
    if (streamUrl.empty()) {
        streamUrl = ExtractJsonString(streamsSection, L"hls_stream");
    }

    return streamUrl;
}

// Get ALL stream URLs for an iHeartRadio station by ID
static std::vector<StreamOption> GetIHeartStreamUrls(const std::wstring& stationId) {
    std::vector<StreamOption> result;

    // Use the live station API to get stream URLs
    std::wstring url = L"https://api.iheart.com/api/v2/content/liveStations/" + stationId;
    std::wstring json = RadioHttpGet(url, L"Accept: application/json\r\n");
    if (json.empty()) return result;

    // Look for streams section
    size_t streamsPos = json.find(L"\"streams\"");
    if (streamsPos == std::wstring::npos) return result;

    std::wstring streamsSection = json.substr(streamsPos);

    // Get all available stream types
    struct { const wchar_t* key; const wchar_t* label; } streamTypes[] = {
        {L"shoutcast_stream", L"Shoutcast"},
        {L"secure_shoutcast_stream", L"Shoutcast (Secure)"},
        {L"pls_stream", L"PLS"},
        {L"hls_stream", L"HLS"},
        {L"stw_stream", L"STW"},
        {L"flv_stream", L"FLV"},
        {L"secure_pls_stream", L"PLS (Secure)"},
        {L"secure_hls_stream", L"HLS (Secure)"},
    };

    std::set<std::wstring> seenUrls;
    for (const auto& type : streamTypes) {
        std::wstring streamUrl = ExtractJsonString(streamsSection, type.key);
        if (!streamUrl.empty() && seenUrls.find(streamUrl) == seenUrls.end()) {
            result.push_back({streamUrl, type.label});
            seenUrls.insert(streamUrl);
        }
    }

    return result;
}

// Search iHeartRadio API
bool SearchIHeartRadio(const std::wstring& query, std::vector<RadioSearchResult>& results) {
    results.clear();

    // Try the v2 live stations search endpoint
    std::wstring url = L"https://api.iheart.com/api/v2/content/liveStations?countryCode=US&limit=20&q=" + RadioUrlEncode(query);
    std::wstring json = RadioHttpGet(url, L"Accept: application/json\r\n");

    // If v2 fails, try v3
    if (json.empty() || json.find(L"\"hits\"") == std::wstring::npos) {
        url = L"https://api.iheart.com/api/v3/search/all?keywords=" + RadioUrlEncode(query) +
              L"&startIndex=0&maxRows=20";
        json = RadioHttpGet(url, L"Accept: application/json\r\n");
    }

    if (json.empty()) return false;

    // Find the array containing stations - try multiple possible locations
    size_t arrayStart = std::wstring::npos;

    // Try "hits" array first (v2 response)
    size_t hitsPos = json.find(L"\"hits\"");
    if (hitsPos != std::wstring::npos) {
        arrayStart = json.find(L'[', hitsPos);
    }

    // Try "stations" -> "results" (v3 response)
    if (arrayStart == std::wstring::npos) {
        size_t stationsPos = json.find(L"\"stations\"");
        if (stationsPos != std::wstring::npos) {
            size_t resultsPos = json.find(L"\"results\"", stationsPos);
            if (resultsPos != std::wstring::npos) {
                arrayStart = json.find(L'[', resultsPos);
            }
        }
    }

    // Try just finding first array after "stations"
    if (arrayStart == std::wstring::npos) {
        size_t stationsPos = json.find(L"\"stations\"");
        if (stationsPos != std::wstring::npos) {
            arrayStart = json.find(L'[', stationsPos);
        }
    }

    if (arrayStart == std::wstring::npos) return false;

    // Find array end
    size_t arrayEnd = arrayStart + 1;
    int depth = 1;
    bool inString = false;
    while (arrayEnd < json.length() && depth > 0) {
        wchar_t c = json[arrayEnd];
        if (c == L'"' && (arrayEnd == 0 || json[arrayEnd-1] != L'\\')) {
            inString = !inString;
        } else if (!inString) {
            if (c == L'[') depth++;
            else if (c == L']') depth--;
        }
        arrayEnd++;
    }

    std::wstring stationsArray = json.substr(arrayStart, arrayEnd - arrayStart);

    // Parse each station object
    size_t pos = 0;
    while ((pos = stationsArray.find(L'{', pos)) != std::wstring::npos) {
        // Find matching closing brace
        int objDepth = 1;
        size_t endPos = pos + 1;
        bool objInString = false;
        while (endPos < stationsArray.length() && objDepth > 0) {
            wchar_t c = stationsArray[endPos];
            if (c == L'"' && (endPos == 0 || stationsArray[endPos-1] != L'\\')) {
                objInString = !objInString;
            } else if (!objInString) {
                if (c == L'{') objDepth++;
                else if (c == L'}') objDepth--;
            }
            endPos++;
        }
        if (objDepth != 0) break;

        std::wstring obj = stationsArray.substr(pos, endPos - pos);
        RadioSearchResult result;
        result.source = 2;  // iHeartRadio
        result.bitrate = 0;

        result.name = ExtractJsonString(obj, L"name");
        if (result.name.empty()) {
            result.name = ExtractJsonString(obj, L"description");
        }

        result.stationId = ExtractJsonValue(obj, L"id");
        result.country = ExtractJsonString(obj, L"city");
        std::wstring state = ExtractJsonString(obj, L"state");
        if (!state.empty()) {
            if (!result.country.empty()) result.country += L", ";
            result.country += state;
        }

        // Get call letters for display
        std::wstring callLetters = ExtractJsonString(obj, L"callLetters");
        if (!callLetters.empty() && result.name.find(callLetters) == std::wstring::npos) {
            result.name = callLetters + L" - " + result.name;
        }

        // Only add if we have a name and station ID (URL resolved later)
        if (!result.name.empty() && !result.stationId.empty()) {
            results.push_back(result);
        }

        pos = endPos;
    }

    return !results.empty();
}

// Resolve stream URL for a radio search result (called when playing/adding)
std::wstring ResolveRadioStreamUrl(const RadioSearchResult& result) {
    std::wstring url;

    if (result.source == 0) {
        // RadioBrowser - URL is already resolved
        url = result.url;
    } else if (result.source == 1) {
        // TuneIn - resolve playlist URL to get actual stream
        url = ResolveTuneInUrl(result.url);
    } else if (result.source == 2) {
        // iHeartRadio - get stream URL from station ID
        url = GetIHeartStreamUrl(result.stationId);
    } else {
        url = result.url;
    }

    // Safety check: if the resolved URL is still a playlist, try to resolve it
    if (!url.empty() && IsPlaylistUrl(url)) {
        std::wstring resolved = ResolveTuneInUrl(url);
        if (!resolved.empty()) {
            url = resolved;
        }
    }

    return url;
}

// Resolve ALL stream URLs for a radio search result (for multi-URL selection)
std::vector<StreamOption> ResolveRadioStreamUrls(const RadioSearchResult& result) {
    std::vector<StreamOption> urls;

    if (result.source == 0) {
        // RadioBrowser - URL is already resolved, just return it
        if (!result.url.empty()) {
            urls.push_back({result.url, L"Stream"});
        }
    } else if (result.source == 1) {
        // TuneIn - resolve playlist URL to get all streams
        urls = ResolveTuneInUrls(result.url);
    } else if (result.source == 2) {
        // iHeartRadio - get all stream URLs from station ID
        urls = GetIHeartStreamUrls(result.stationId);
    } else {
        if (!result.url.empty()) {
            urls.push_back({result.url, L"Stream"});
        }
    }

    // Safety check: resolve any remaining playlist URLs
    for (auto& opt : urls) {
        if (IsPlaylistUrl(opt.url)) {
            std::wstring resolved = ResolveTuneInUrl(opt.url);
            if (!resolved.empty()) {
                opt.url = resolved;
            }
        }
    }

    return urls;
}

// Follow an HTTP(S) URL's redirects manually and return the last URL actually
// requested, stopping after the headers. WinInet's automatic redirection refuses
// to follow an HTTPS->HTTP "downgrade" redirect (HttpSendRequestW fails
// outright), which breaks media URLs that redirect an https .mp3 to a
// delivery-script URL. Doing it ourselves also lets us cross schemes and hosts
// freely. Returns an empty string if no response was received.
static std::wstring FollowHttpRedirects(const std::wstring& startUrl) {
    HttpOptions options;
    options.readBody = false;
    HttpResult response = HttpGet(startUrl, options);
    return response.completed ? response.finalUrl : std::wstring();
}

// Resolve an HTTP(S) URL's redirects to its final target, without downloading the
// body. Handed a media URL that redirects (e.g. an episode enclosure that 302s to
// a delivery script), this returns the real URL so the player gets something it
// can open directly. Returns the original URL unchanged when it isn't http(s), or
// when resolution fails / nothing redirects.
std::wstring ResolveHttpRedirects(const std::wstring& url) {
    if (url.compare(0, 7, L"http://") != 0 && url.compare(0, 8, L"https://") != 0) {
        return url;
    }
    std::wstring finalUrl = FollowHttpRedirects(url);
    return finalUrl.empty() ? url : finalUrl;
}

// Import radio stations from an M3U/M3U8/PLS playlist file
RadioImportResult ImportRadioFavorites(const std::wstring& playlistPath) {
    RadioImportResult res;
    int& imported = res.imported;
    int& skipped = res.skipped;
    // Track imported station ids in file order so we can preserve that
    // order (e.g. an order previously arranged and exported to M3U).
    std::vector<int> importedIds;

    // Skip URLs that are already saved (or repeated within the file), so
    // re-importing an exported playlist doesn't create duplicates. Keyed
    // on a lowercased URL for robustness against case differences.
    std::set<std::wstring> seenUrls;
    auto urlKey = [](std::wstring u) {
        while (!u.empty() && (u.front() == L' ' || u.front() == L'\t')) u.erase(0, 1);
        while (!u.empty() && (u.back() == L' ' || u.back() == L'\t')) u.pop_back();
        for (auto& c : u) c = towlower(c);
        return u;
    };
    for (const auto& s : GetRadioFavorites()) seenUrls.insert(urlKey(s.url));

    // Determine file type and parse
    size_t dotPos = playlistPath.find_last_of(L'.');
    std::wstring ext = dotPos == std::wstring::npos ? L"" : playlistPath.substr(dotPos);
    for (auto& c : ext) c = towlower(c);

    if (ext == L".pls") {
        // Parse PLS - has Title entries
        for (int i = 1; i <= 1000; i++) {
            wchar_t fileKey[32], titleKey[32];
            swprintf(fileKey, 32, L"File%d", i);
            swprintf(titleKey, 32, L"Title%d", i);

            wchar_t url[4096] = {0}, title[512] = {0};
            IniGetString(L"playlist", fileKey, L"", url, 4096, playlistPath.c_str());
            if (url[0] == L'\0') break;

            // Only import URLs (not local files)
            if (WStrNICmp(url, L"http://", 7) != 0 && WStrNICmp(url, L"https://", 8) != 0) {
                continue;
            }

            IniGetString(L"playlist", titleKey, L"", title, 512, playlistPath.c_str());
            std::wstring name = title[0] ? title : url;

            std::wstring key = urlKey(url);
            if (!seenUrls.insert(key).second) {
                skipped++;
                continue;
            }

            int sid = AddRadioStation(name, url);
            if (sid >= 0) {
                imported++;
                importedIds.push_back(sid);
            }
        }
    } else {
        // Parse M3U/M3U8
        FILE* f = FileOpen(playlistPath, "rb");
        if (f) {
            // Check for UTF-8 BOM
            unsigned char bom[3] = {0};
            fread(bom, 1, 3, f);
            if (!(bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF)) {
                fseek(f, 0, SEEK_SET);
            }

            char line[4096];
            std::wstring pendingName;

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
                if (*start == '\0') continue;

                // Convert to wide string
                std::wstring wline;
                wline = PlaylistLineToWide(start);
                if (!wline.empty() && wline.back() == L'\0') wline.pop_back();
                if (wline.empty()) continue;

                // Check for #EXTINF line (contains station name)
                if (WStrNICmp(wline.c_str(), L"#EXTINF:", 8) == 0) {
                    // Format: #EXTINF:duration,Station Name
                    const wchar_t* comma = wcschr(wline.c_str() + 8, L',');
                    if (comma) {
                        // Skip comma and any leading whitespace
                        const wchar_t* nameStart = comma + 1;
                        while (*nameStart == L' ' || *nameStart == L'\t') nameStart++;
                        pendingName = nameStart;
                    }
                    continue;
                }

                // Skip other comments
                if (wline[0] == L'#') continue;

                // This should be a URL
                if (WStrNICmp(wline.c_str(), L"http://", 7) == 0 ||
                    WStrNICmp(wline.c_str(), L"https://", 8) == 0) {
                    std::wstring name = pendingName.empty() ? wline : pendingName;
                    std::wstring key = urlKey(wline);
                    if (!seenUrls.insert(key).second) {
                        skipped++;
                        pendingName.clear();
                        continue;
                    }
                    int sid = AddRadioStation(name, wline);
                    if (sid >= 0) {
                        imported++;
                        importedIds.push_back(sid);
                    }
                }
                pendingName.clear();
            }
            fclose(f);
        }
    }

    if (imported > 0) {
        // Preserve the playlist's file order. Without this the imported
        // stations all get sort_order 0 and fall back to alphabetical,
        // so a previously-arranged, exported order would be lost on import.
        AppendRadioSortOrder(importedIds);
    }
    return res;
}

// Export radio favorites to an M3U file
bool ExportRadioFavorites(const std::wstring& path, const std::vector<RadioStation>& stations) {
    FILE* f = FileOpen(path, "wb");
    if (!f) return false;

    // Write UTF-8 BOM
    fwrite("\xEF\xBB\xBF", 1, 3, f);
    // Write M3U header
    fprintf(f, "#EXTM3U\r\n");

    for (const auto& station : stations) {
        // Write EXTINF with station name
        std::string name = WideToUtf8(station.name);
        std::string url = WideToUtf8(station.url);
        fprintf(f, "#EXTINF:-1,%s\r\n", name.c_str());
        fprintf(f, "%s\r\n", url.c_str());
    }
    fclose(f);
    return true;
}
