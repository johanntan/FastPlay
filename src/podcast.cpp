// Podcasts: HTTP fetching (with redirects and credentials), RSS and OPML parsing,
// the iTunes directory search, and preparing episode downloads.

#include "podcast.h"
#include "database.h"
#include "globals.h"
#include "utils.h"
#include "download_manager.h"

#include <windows.h>
#include <wininet.h>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <iomanip>
#include <set>
#include <sstream>

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

// Fetch an HTTP(S) URL, following redirects manually. WinInet's automatic
// redirection refuses to follow an HTTPS->HTTP "downgrade" redirect
// (HttpSendRequestW fails outright), which breaks feeds like PowerPress that
// 301 an https:// canonical feed URL to an http:// host, and media enclosures
// that redirect an https .mp3 to a delivery-script URL. Doing it ourselves also
// lets us cross schemes and hosts freely. Credentials, if supplied, are sent
// preemptively on every hop. When bodyWanted is true the final response body is
// returned; when false the request stops after the headers (used to resolve an
// episode URL to its final target). outFinalUrl, if non-null, receives the last
// URL actually requested.
static std::wstring PodcastHttpFetch(const std::wstring& startUrl,
                                     const std::wstring& username,
                                     const std::wstring& password,
                                     PodcastFetchDiag* diag,
                                     bool bodyWanted,
                                     std::wstring* outFinalUrl) {
    std::wstring result;
    std::wstring authHeader = BuildBasicAuthHeader(username, password);

    HINTERNET hInternet = InternetOpenW(L"FastPlay/1.0", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hInternet) {
        if (diag) {
            diag->lastError = GetLastError();
            diag->errorText = FormatWinInetError(diag->lastError);
        }
        return result;
    }

    std::wstring url = startUrl;
    const int MAX_HOPS = 6;
    for (int hop = 0; hop < MAX_HOPS; hop++) {
        // Parse the URL into scheme/host/path.
        bool secure = false;
        size_t hostStart;
        if (url.compare(0, 8, L"https://") == 0)      { secure = true;  hostStart = 8; }
        else if (url.compare(0, 7, L"http://") == 0)  { secure = false; hostStart = 7; }
        else {
            if (diag) diag->errorText = L"Unsupported URL scheme (expected http:// or https://)";
            break;
        }
        std::wstring host, path;
        size_t pathStart = url.find(L'/', hostStart);
        if (pathStart == std::wstring::npos) { host = url.substr(hostStart); path = L"/"; }
        else { host = url.substr(hostStart, pathStart - hostStart); path = url.substr(pathStart); }

        DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE |
                      INTERNET_FLAG_NO_AUTO_REDIRECT | INTERNET_FLAG_KEEP_CONNECTION;
        if (secure) flags |= INTERNET_FLAG_SECURE;

        HINTERNET hConnect = InternetConnectW(hInternet, host.c_str(),
                                              secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT,
                                              username.empty() ? nullptr : username.c_str(),
                                              password.empty() ? nullptr : password.c_str(),
                                              INTERNET_SERVICE_HTTP, 0, 0);
        if (!hConnect) {
            if (diag) { diag->lastError = GetLastError(); diag->errorText = FormatWinInetError(diag->lastError); }
            break;
        }

        HINTERNET hRequest = HttpOpenRequestW(hConnect, L"GET", path.c_str(), nullptr, nullptr, nullptr, flags, 0);
        if (!hRequest) {
            if (diag) { diag->lastError = GetLastError(); diag->errorText = FormatWinInetError(diag->lastError); }
            InternetCloseHandle(hConnect);
            break;
        }

        // Send Basic credentials preemptively (WinInet only attaches the
        // InternetConnect credentials after a 401, and only on a resend).
        if (!authHeader.empty()) {
            HttpAddRequestHeadersW(hRequest, authHeader.c_str(), static_cast<DWORD>(authHeader.size()),
                                   HTTP_ADDREQ_FLAG_ADD | HTTP_ADDREQ_FLAG_REPLACE);
        }

        BOOL sent = HttpSendRequestW(hRequest, nullptr, 0, nullptr, 0);
        // If the server still challenges (Digest/NTLM realm), resend once so
        // WinInet can answer with the InternetConnect credentials.
        if (sent && (!username.empty() || !password.empty())) {
            DWORD status = 0, sz = sizeof(status);
            HttpQueryInfoW(hRequest, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &sz, nullptr);
            if (status == HTTP_STATUS_DENIED) sent = HttpSendRequestW(hRequest, nullptr, 0, nullptr, 0);
        }
        if (!sent) {
            if (diag) { diag->lastError = GetLastError(); diag->errorText = FormatWinInetError(diag->lastError); }
            InternetCloseHandle(hRequest);
            InternetCloseHandle(hConnect);
            break;
        }

        DWORD status = 0, sz = sizeof(status);
        HttpQueryInfoW(hRequest, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &sz, nullptr);

        // Follow 301/302/303/307/308 to the Location target ourselves.
        bool isRedirect = (status == 301 || status == 302 || status == 303 ||
                           status == 307 || status == 308);
        if (isRedirect && hop + 1 < MAX_HOPS) {
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
        if (diag) diag->statusCode = status;
        if (outFinalUrl) *outFinalUrl = url;
        if (bodyWanted) {
            char buffer[4096];
            DWORD bytesRead;
            std::string response;
            while (InternetReadFile(hRequest, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0) {
                response.append(buffer, bytesRead);
            }
            if (diag) {
                diag->bytesReceived = response.size();
                diag->bodyPreview = Utf8ToWide(response.substr(0, 400));
            }
            result = Utf8ToWide(response);
        }
        InternetCloseHandle(hRequest);
        InternetCloseHandle(hConnect);
        InternetCloseHandle(hInternet);
        return result;
    }

    InternetCloseHandle(hInternet);
    return result;
}

// HTTP GET for podcast operations
static std::wstring PodcastHttpGet(const std::wstring& url, PodcastFetchDiag* diag = nullptr) {
    return PodcastHttpFetch(url, L"", L"", diag, true, nullptr);
}

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

// HTTP GET with Basic Authentication support
static std::wstring PodcastHttpGetAuth(const std::wstring& url, const std::wstring& username, const std::wstring& password, PodcastFetchDiag* diag = nullptr) {
    return PodcastHttpFetch(url, username, password, diag, true, nullptr);
}

// URL encode for podcast searches
static std::wstring PodcastUrlEncode(const std::wstring& str) {
    std::string utf8 = WideToUtf8(str);
    std::wostringstream encoded;
    for (unsigned char c : utf8) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded << static_cast<wchar_t>(c);
        } else if (c == ' ') {
            encoded << L'+';
        } else {
            encoded << L'%' << std::hex << std::uppercase << std::setw(2) << std::setfill(L'0') << static_cast<int>(c);
        }
    }
    return encoded.str();
}

// Sanitize filename for saving
static std::wstring SanitizeFilename(const std::wstring& name) {
    std::wstring result;
    for (wchar_t c : name) {
        if (c == L'/' || c == L'\\' || c == L':' || c == L'*' || c == L'?' ||
            c == L'"' || c == L'<' || c == L'>' || c == L'|') {
            result += L'_';
        } else {
            result += c;
        }
    }
    // Trim trailing spaces and dots
    while (!result.empty() && (result.back() == L' ' || result.back() == L'.')) {
        result.pop_back();
    }
    return result;
}

// Get file extension from URL
static std::wstring GetUrlExtension(const std::wstring& url) {
    size_t queryPos = url.find(L'?');
    std::wstring path = (queryPos != std::wstring::npos) ? url.substr(0, queryPos) : url;
    size_t dotPos = path.rfind(L'.');
    if (dotPos != std::wstring::npos && dotPos > path.rfind(L'/')) {
        return path.substr(dotPos);
    }
    return L".mp3";  // Default to mp3
}

// Extract text content from an XML element
static std::wstring ExtractXmlContent(const std::wstring& xml, const std::wstring& tagName) {
    std::wstring startTag = L"<" + tagName;
    std::wstring endTag = L"</" + tagName + L">";

    size_t start = xml.find(startTag);
    if (start == std::wstring::npos) return L"";

    // Find the end of the opening tag
    size_t tagEnd = xml.find(L'>', start);
    if (tagEnd == std::wstring::npos) return L"";

    // Check for CDATA
    size_t contentStart = tagEnd + 1;
    size_t end = xml.find(endTag, contentStart);
    if (end == std::wstring::npos) return L"";

    std::wstring content = xml.substr(contentStart, end - contentStart);

    // Strip CDATA wrapper if present (may have leading whitespace)
    size_t cdataStart = content.find(L"<![CDATA[");
    if (cdataStart != std::wstring::npos) {
        size_t cdataEnd = content.rfind(L"]]>");
        if (cdataEnd != std::wstring::npos && cdataEnd > cdataStart) {
            content = content.substr(cdataStart + 9, cdataEnd - cdataStart - 9);
        }
    }

    // Basic HTML entity decode - named entities
    size_t pos = 0;
    while ((pos = content.find(L"&amp;", pos)) != std::wstring::npos) {
        content.replace(pos, 5, L"&");
    }
    pos = 0;
    while ((pos = content.find(L"&lt;", pos)) != std::wstring::npos) {
        content.replace(pos, 4, L"<");
    }
    pos = 0;
    while ((pos = content.find(L"&gt;", pos)) != std::wstring::npos) {
        content.replace(pos, 4, L">");
    }
    pos = 0;
    while ((pos = content.find(L"&quot;", pos)) != std::wstring::npos) {
        content.replace(pos, 6, L"\"");
    }
    pos = 0;
    while ((pos = content.find(L"&apos;", pos)) != std::wstring::npos) {
        content.replace(pos, 6, L"'");
    }
    pos = 0;
    while ((pos = content.find(L"&nbsp;", pos)) != std::wstring::npos) {
        content.replace(pos, 6, L" ");
    }

    // Numeric HTML entities (&#039; &#39; &#34; etc.)
    pos = 0;
    while ((pos = content.find(L"&#", pos)) != std::wstring::npos) {
        size_t semicolon = content.find(L';', pos);
        if (semicolon != std::wstring::npos && semicolon - pos < 8) {
            std::wstring numStr = content.substr(pos + 2, semicolon - pos - 2);
            int codePoint = 0;
            if (!numStr.empty() && (numStr[0] == L'x' || numStr[0] == L'X')) {
                // Hex: &#x27;
                codePoint = wcstol(numStr.c_str() + 1, nullptr, 16);
            } else {
                // Decimal: &#39;
                codePoint = wcstol(numStr.c_str(), nullptr, 10);
            }
            if (codePoint > 0 && codePoint < 0x10000) {
                wchar_t ch = static_cast<wchar_t>(codePoint);
                content.replace(pos, semicolon - pos + 1, 1, ch);
            } else {
                pos++;  // Skip invalid entity
            }
        } else {
            pos++;  // Skip malformed entity
        }
    }

    return content;
}

// Extract enclosure URL from an item
static std::wstring ExtractEnclosureUrl(const std::wstring& item) {
    size_t encPos = item.find(L"<enclosure");
    if (encPos == std::wstring::npos) return L"";

    size_t urlStart = item.find(L"url=\"", encPos);
    if (urlStart == std::wstring::npos) {
        urlStart = item.find(L"url='", encPos);
        if (urlStart == std::wstring::npos) return L"";
        urlStart += 5;
        size_t urlEnd = item.find(L"'", urlStart);
        if (urlEnd == std::wstring::npos) return L"";
        return item.substr(urlStart, urlEnd - urlStart);
    }
    urlStart += 5;
    size_t urlEnd = item.find(L"\"", urlStart);
    if (urlEnd == std::wstring::npos) return L"";
    return item.substr(urlStart, urlEnd - urlStart);
}

// Parse iTunes duration (HH:MM:SS or MM:SS or seconds)
static int ParseDuration(const std::wstring& duration) {
    if (duration.empty()) return 0;

    // Check if it's just seconds
    bool hasColon = duration.find(L':') != std::wstring::npos;
    if (!hasColon) {
        return _wtoi(duration.c_str());
    }

    // Parse HH:MM:SS or MM:SS
    int h = 0, m = 0, s = 0;
    if (swscanf(duration.c_str(), L"%d:%d:%d", &h, &m, &s) == 3) {
        return h * 3600 + m * 60 + s;
    } else if (swscanf(duration.c_str(), L"%d:%d", &m, &s) == 2) {
        return m * 60 + s;
    }
    return 0;
}

// Extract attribute value from an XML element string
static std::wstring ExtractXmlAttribute(const std::wstring& element, const std::wstring& attrName) {
    std::wstring search = attrName + L"=\"";
    size_t start = element.find(search);
    if (start == std::wstring::npos) {
        // Try single quotes
        search = attrName + L"='";
        start = element.find(search);
        if (start == std::wstring::npos) return L"";
    }
    start += search.length();
    wchar_t quoteChar = (search.back() == L'"') ? L'"' : L'\'';
    size_t end = element.find(quoteChar, start);
    if (end == std::wstring::npos) return L"";
    return element.substr(start, end - start);
}

// Parse OPML file and extract feed URLs
std::vector<OpmlFeed> ParseOpmlFile(const std::wstring& filePath) {
    std::vector<OpmlFeed> feeds;

    // Read file content
    FILE* f = _wfopen(filePath.c_str(), L"rb");
    if (!f) return feeds;

    fseek(f, 0, SEEK_END);
    long fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fileSize <= 0 || fileSize > 10 * 1024 * 1024) {  // Max 10MB
        fclose(f);
        return feeds;
    }

    std::vector<char> buffer(fileSize + 1);
    size_t bytesRead = fread(buffer.data(), 1, fileSize, f);
    fclose(f);
    buffer[bytesRead] = '\0';

    // Convert to wide string (assuming UTF-8)
    std::wstring xml = Utf8ToWide(std::string(buffer.data(), bytesRead));

    // Find all <outline elements with xmlUrl attribute
    size_t pos = 0;
    while ((pos = xml.find(L"<outline", pos)) != std::wstring::npos) {
        size_t elementEnd = xml.find(L'>', pos);
        if (elementEnd == std::wstring::npos) break;

        std::wstring element = xml.substr(pos, elementEnd - pos + 1);

        // Extract xmlUrl attribute (this is the feed URL)
        std::wstring feedUrl = ExtractXmlAttribute(element, L"xmlUrl");
        if (feedUrl.empty()) {
            // Try alternate attribute names
            feedUrl = ExtractXmlAttribute(element, L"xmlurl");
        }

        if (!feedUrl.empty()) {
            OpmlFeed feed;
            feed.feedUrl = feedUrl;
            // Try to get title from text attribute
            feed.title = ExtractXmlAttribute(element, L"text");
            if (feed.title.empty()) {
                feed.title = ExtractXmlAttribute(element, L"title");
            }
            feeds.push_back(feed);
        }

        pos = elementEnd;
    }

    return feeds;
}

// Export subscriptions to OPML file
bool ExportOpmlFile(const std::wstring& filePath, const std::vector<PodcastSubscription>& subs) {
    FILE* f = _wfopen(filePath.c_str(), L"wb");
    if (!f) return false;

    // Write UTF-8 BOM
    fwrite("\xEF\xBB\xBF", 1, 3, f);

    // Write OPML header
    fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    fprintf(f, "<opml version=\"2.0\">\n");
    fprintf(f, "  <head>\n");
    fprintf(f, "    <title>FastPlay Podcast Subscriptions</title>\n");
    fprintf(f, "  </head>\n");
    fprintf(f, "  <body>\n");

    // Write each subscription
    for (const auto& sub : subs) {
        std::string title = WideToUtf8(sub.name);
        std::string feedUrl = WideToUtf8(sub.feedUrl);

        // Escape XML special characters
        std::string escapedTitle, escapedUrl;
        for (char c : title) {
            if (c == '&') escapedTitle += "&amp;";
            else if (c == '<') escapedTitle += "&lt;";
            else if (c == '>') escapedTitle += "&gt;";
            else if (c == '"') escapedTitle += "&quot;";
            else if (c == '\'') escapedTitle += "&apos;";
            else escapedTitle += c;
        }
        for (char c : feedUrl) {
            if (c == '&') escapedUrl += "&amp;";
            else if (c == '<') escapedUrl += "&lt;";
            else if (c == '>') escapedUrl += "&gt;";
            else if (c == '"') escapedUrl += "&quot;";
            else escapedUrl += c;
        }

        fprintf(f, "    <outline type=\"rss\" text=\"%s\" xmlUrl=\"%s\"/>\n",
                escapedTitle.c_str(), escapedUrl.c_str());
    }

    fprintf(f, "  </body>\n");
    fprintf(f, "</opml>\n");
    fclose(f);
    return true;
}

// Parse RSS feed and extract episodes (with optional authentication)
bool ParsePodcastFeed(const std::wstring& feedUrl, std::wstring& outTitle,
                      std::vector<PodcastEpisode>& episodes,
                      const std::wstring& username, const std::wstring& password,
                      PodcastFetchDiag* diag) {
    episodes.clear();

    std::wstring xml = PodcastHttpGetAuth(feedUrl, username, password, diag);
    if (xml.empty()) return false;

    // Extract channel title
    size_t channelStart = xml.find(L"<channel");
    if (channelStart != std::wstring::npos) {
        size_t channelEnd = xml.find(L"</channel>", channelStart);
        if (channelEnd != std::wstring::npos) {
            std::wstring channel = xml.substr(channelStart, channelEnd - channelStart);
            // Get title before first <item>
            size_t firstItem = channel.find(L"<item");
            if (firstItem != std::wstring::npos) {
                std::wstring header = channel.substr(0, firstItem);
                outTitle = ExtractXmlContent(header, L"title");
            }
        }
    }

    // Find all <item> elements
    int itemCount = 0;
    size_t pos = 0;
    while ((pos = xml.find(L"<item", pos)) != std::wstring::npos) {
        itemCount++;
        size_t itemEnd = xml.find(L"</item>", pos);
        if (itemEnd == std::wstring::npos) break;

        std::wstring item = xml.substr(pos, itemEnd - pos + 7);
        PodcastEpisode ep;

        ep.title = ExtractXmlContent(item, L"title");
        ep.description = ExtractXmlContent(item, L"description");
        ep.pubDate = ExtractXmlContent(item, L"pubDate");
        ep.guid = ExtractXmlContent(item, L"guid");
        ep.audioUrl = ExtractEnclosureUrl(item);

        // Try to get duration from itunes:duration
        std::wstring durationStr = ExtractXmlContent(item, L"itunes:duration");
        ep.durationSeconds = ParseDuration(durationStr);

        if (!ep.audioUrl.empty() && !ep.title.empty()) {
            episodes.push_back(ep);
        }

        pos = itemEnd;
    }

    if (diag) {
        diag->itemTagsFound = itemCount;
        diag->episodesExtracted = static_cast<int>(episodes.size());
    }

    return !episodes.empty();
}

// Search iTunes podcast directory
bool SearchItunesPodcasts(const std::wstring& query, std::vector<PodcastSearchResult>& results) {
    results.clear();

    std::wstring url = L"https://itunes.apple.com/search?term=" +
                       PodcastUrlEncode(query) + L"&media=podcast&limit=25";

    std::wstring json = PodcastHttpGet(url);
    if (json.empty()) return false;

    // Parse JSON results - look for each result object
    size_t pos = 0;
    while ((pos = json.find(L"\"collectionName\"", pos)) != std::wstring::npos) {
        PodcastSearchResult r;

        // Extract collectionName
        size_t valueStart = json.find(L":", pos);
        if (valueStart != std::wstring::npos) {
            size_t strStart = json.find(L"\"", valueStart + 1);
            if (strStart != std::wstring::npos) {
                strStart++;
                size_t strEnd = json.find(L"\"", strStart);
                if (strEnd != std::wstring::npos) {
                    r.name = json.substr(strStart, strEnd - strStart);
                }
            }
        }

        // Find feedUrl in the same object (search backwards and forwards)
        size_t searchStart = (pos > 500) ? pos - 500 : 0;
        size_t searchEnd = pos + 1000;
        if (searchEnd > json.length()) searchEnd = json.length();
        std::wstring context = json.substr(searchStart, searchEnd - searchStart);

        size_t feedPos = context.find(L"\"feedUrl\"");
        if (feedPos != std::wstring::npos) {
            size_t fvalueStart = context.find(L":", feedPos);
            if (fvalueStart != std::wstring::npos) {
                size_t fstrStart = context.find(L"\"", fvalueStart + 1);
                if (fstrStart != std::wstring::npos) {
                    fstrStart++;
                    size_t fstrEnd = context.find(L"\"", fstrStart);
                    if (fstrEnd != std::wstring::npos) {
                        r.feedUrl = context.substr(fstrStart, fstrEnd - fstrStart);
                    }
                }
            }
        }

        // Extract artistName
        size_t artistPos = context.find(L"\"artistName\"");
        if (artistPos != std::wstring::npos) {
            size_t avalueStart = context.find(L":", artistPos);
            if (avalueStart != std::wstring::npos) {
                size_t astrStart = context.find(L"\"", avalueStart + 1);
                if (astrStart != std::wstring::npos) {
                    astrStart++;
                    size_t astrEnd = context.find(L"\"", astrStart);
                    if (astrEnd != std::wstring::npos) {
                        r.artistName = context.substr(astrStart, astrEnd - astrStart);
                    }
                }
            }
        }

        if (!r.name.empty() && !r.feedUrl.empty()) {
            results.push_back(r);
        }

        pos++;
    }

    return !results.empty();
}

// Build a human-readable diagnostic report from a failed feed fetch/parse
std::wstring BuildPodcastDiagMessage(const std::wstring& feedUrl, const PodcastFetchDiag& diag) {
    std::wstring msg;
    msg += L"Feed URL:\r\n";
    msg += feedUrl;
    msg += L"\r\n\r\n";

    wchar_t num[32];
    msg += L"HTTP status: ";
    if (diag.statusCode > 0) {
        swprintf(num, 32, L"%lu", diag.statusCode);
        msg += num;
    } else {
        msg += L"(not reached)";
    }
    msg += L"\r\n";

    if (diag.lastError != 0) {
        swprintf(num, 32, L"%lu", diag.lastError);
        msg += L"Network error: ";
        msg += num;
        if (!diag.errorText.empty()) {
            msg += L" - " + diag.errorText;
        }
        msg += L"\r\n";
    } else if (!diag.errorText.empty()) {
        msg += L"Error: " + diag.errorText + L"\r\n";
    }

    swprintf(num, 32, L"%zu", diag.bytesReceived);
    msg += L"Bytes received: ";
    msg += num;
    msg += L"\r\n";

    swprintf(num, 32, L"%d", diag.itemTagsFound);
    msg += L"<item> tags in XML: ";
    msg += num;
    msg += L"\r\n";

    swprintf(num, 32, L"%d", diag.episodesExtracted);
    msg += L"Episodes extracted (had audio URL + title): ";
    msg += num;
    msg += L"\r\n";

    if (!diag.bodyPreview.empty()) {
        msg += L"\r\nResponse preview (first 400 chars):\r\n";
        msg += diag.bodyPreview;
    }

    return msg;
}

// The episode list line: title, then the date part of the pub date, then the
// start of the description with HTML stripped.
std::wstring FormatPodcastEpisodeDisplay(const PodcastEpisode& ep) {
    std::wstring display = ep.title;
    if (!ep.pubDate.empty()) {
        // Truncate pub date to just the date part
        size_t commaPos = ep.pubDate.find(L',');
        if (commaPos != std::wstring::npos && commaPos + 12 < ep.pubDate.length()) {
            display += L" (" + ep.pubDate.substr(commaPos + 2, 11) + L")";
        }
    }
    if (!ep.description.empty()) {
        // Clean up description - remove HTML tags and limit length
        std::wstring desc = ep.description;
        // Remove HTML tags
        size_t pos;
        while ((pos = desc.find(L'<')) != std::wstring::npos) {
            size_t endPos = desc.find(L'>', pos);
            if (endPos != std::wstring::npos) {
                desc.erase(pos, endPos - pos + 1);
            } else {
                break;
            }
        }
        // Replace &nbsp; and other entities
        while ((pos = desc.find(L"&nbsp;")) != std::wstring::npos) {
            desc.replace(pos, 6, L" ");
        }
        while ((pos = desc.find(L"&amp;")) != std::wstring::npos) {
            desc.replace(pos, 5, L"&");
        }
        while ((pos = desc.find(L"&quot;")) != std::wstring::npos) {
            desc.replace(pos, 6, L"\"");
        }
        while ((pos = desc.find(L"&apos;")) != std::wstring::npos) {
            desc.replace(pos, 6, L"'");
        }
        while ((pos = desc.find(L"&lt;")) != std::wstring::npos) {
            desc.replace(pos, 4, L"<");
        }
        while ((pos = desc.find(L"&gt;")) != std::wstring::npos) {
            desc.replace(pos, 4, L">");
        }
        // Trim whitespace and collapse multiple spaces
        while (!desc.empty() && (desc[0] == L' ' || desc[0] == L'\n' || desc[0] == L'\r' || desc[0] == L'\t')) {
            desc.erase(0, 1);
        }
        // Truncate to reasonable length
        if (desc.length() > 150) {
            desc = desc.substr(0, 147) + L"...";
        }
        if (!desc.empty()) {
            display += L" - " + desc;
        }
    }
    return display;
}

// Compute the cleaned plain-text description for an episode, ready for the
// description box.
std::wstring CleanPodcastDescription(const std::wstring& raw) {
    std::wstring desc = raw;
    size_t pos;
    while ((pos = desc.find(L"<br")) != std::wstring::npos) {
        size_t endPos = desc.find(L'>', pos);
        if (endPos != std::wstring::npos) {
            desc.replace(pos, endPos - pos + 1, L"\n");
        } else {
            break;
        }
    }
    while ((pos = desc.find(L"</p>")) != std::wstring::npos) desc.replace(pos, 4, L"\n\n");
    while ((pos = desc.find(L"</div>")) != std::wstring::npos) desc.replace(pos, 6, L"\n");
    while ((pos = desc.find(L'<')) != std::wstring::npos) {
        size_t endPos = desc.find(L'>', pos);
        if (endPos != std::wstring::npos) {
            desc.erase(pos, endPos - pos + 1);
        } else {
            break;
        }
    }
    while ((pos = desc.find(L"&nbsp;")) != std::wstring::npos) desc.replace(pos, 6, L" ");
    while ((pos = desc.find(L"&amp;")) != std::wstring::npos) desc.replace(pos, 5, L"&");
    while ((pos = desc.find(L"&quot;")) != std::wstring::npos) desc.replace(pos, 6, L"\"");
    while ((pos = desc.find(L"&apos;")) != std::wstring::npos) desc.replace(pos, 6, L"'");
    while ((pos = desc.find(L"&lt;")) != std::wstring::npos) desc.replace(pos, 4, L"<");
    while ((pos = desc.find(L"&gt;")) != std::wstring::npos) desc.replace(pos, 4, L">");
    while ((pos = desc.find(L"&#39;")) != std::wstring::npos) desc.replace(pos, 5, L"'");
    while ((pos = desc.find(L"\n\n\n")) != std::wstring::npos) desc.erase(pos, 1);
    pos = 0;
    while ((pos = desc.find(L'\n', pos)) != std::wstring::npos) {
        if (pos == 0 || desc[pos - 1] != L'\r') {
            desc.insert(pos, 1, L'\r');
            pos += 2;
        } else {
            pos++;
        }
    }
    return desc;
}

PodcastDownloadBatch PreparePodcastDownloads(const std::vector<const PodcastEpisode*>& episodes,
                                             const PodcastSubscription* feed) {
    PodcastDownloadBatch batch;

    // Build download folder path
    std::wstring downloadFolder = g_downloadPath;
    if (!downloadFolder.empty() && downloadFolder.back() != L'\\') downloadFolder += L'\\';

    // If organize by feed is enabled, create subfolder
    if (g_downloadOrganizeByFeed && feed) {
        std::wstring feedFolder = SanitizeFilename(feed->name);
        if (!feedFolder.empty()) {
            downloadFolder += feedFolder + L'\\';
            CreateDirectoryW(downloadFolder.c_str(), nullptr);
        }
    }

    std::set<std::wstring> usedFilenames;

    for (const PodcastEpisode* epPtr : episodes) {
        const auto& ep = *epPtr;
        if (ep.audioUrl.empty()) {
            batch.skipped++;
            continue;
        }

        std::wstring baseName = SanitizeFilename(ep.title);
        if (baseName.empty()) baseName = L"episode";
        std::wstring ext = GetUrlExtension(ep.audioUrl);
        std::wstring filename = baseName + ext;
        std::wstring filepath = downloadFolder + filename;

        // Skip if file already exists on disk
        if (GetFileAttributesW(filepath.c_str()) != INVALID_FILE_ATTRIBUTES) {
            batch.skipped++;
            continue;
        }

        // Handle duplicate titles within this batch by adding number suffix
        int dupCount = 1;
        while (usedFilenames.count(filename) > 0) {
            dupCount++;
            wchar_t suffix[16];
            swprintf(suffix, 16, L" (%d)", dupCount);
            filename = baseName + suffix + ext;
            filepath = downloadFolder + filename;
        }
        usedFilenames.insert(filename);

        batch.items.push_back(std::make_tuple(ep.audioUrl, filepath, ep.title));
    }

    return batch;
}

void QueuePodcastDownloads(const PodcastDownloadBatch& batch, const std::wstring& authHeader, bool asBatch) {
    if (batch.items.empty()) return;
    if (batch.items.size() == 1 && !asBatch) {
        const auto& d = batch.items[0];
        DownloadManager::Instance().Enqueue(std::get<0>(d), std::get<1>(d), std::get<2>(d), authHeader);
    } else {
        DownloadManager::Instance().EnqueueMultiple(batch.items, authHeader);
    }
}
