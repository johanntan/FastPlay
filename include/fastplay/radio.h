#pragma once
#ifndef FASTPLAY_RADIO_H
#define FASTPLAY_RADIO_H

// Internet radio: resolving stream URLs, and searching the station directories
// (RadioBrowser, TuneIn, iHeartRadio). The Radio window is src/ui/radio_dialog.cpp.

#include <string>
#include <vector>

struct RadioStation;

// Station directories, in the order the Radio window's Source list shows them
enum RadioSearchSource {
    RADIO_SOURCE_RADIOBROWSER = 0,
    RADIO_SOURCE_TUNEIN = 1,
    RADIO_SOURCE_IHEARTRADIO = 2
};

// Radio search results
struct RadioSearchResult {
    std::wstring name;
    std::wstring url;        // Direct URL for RadioBrowser, playlist URL for TuneIn, empty for iHeart
    std::wstring stationId;  // Station ID for iHeartRadio
    std::wstring country;
    std::wstring codec;
    int bitrate = 0;
    int source = 0;          // 0=RadioBrowser, 1=TuneIn, 2=iHeartRadio
};

// Stream option for multi-URL selection
struct StreamOption {
    std::wstring url;
    std::wstring label;
};

// Search the directories. Each blocks while it downloads, clears `results`, and
// returns true if anything was found. `country` (RadioBrowser only) may be empty.
bool SearchRadioBrowser(const std::wstring& query, const std::wstring& country, std::vector<RadioSearchResult>& results);
bool SearchTuneIn(const std::wstring& query, std::vector<RadioSearchResult>& results);
bool SearchIHeartRadio(const std::wstring& query, std::vector<RadioSearchResult>& results);

// RadioBrowser's country names, most stations first. Blocks while it downloads;
// empty on failure.
std::vector<std::wstring> FetchRadioCountries();

// Resolve the stream URL for a search result (called when playing/adding).
// Blocks; empty if it could not be found.
std::wstring ResolveRadioStreamUrl(const RadioSearchResult& result);

// Resolve ALL stream URLs for a search result (for multi-URL selection). Blocks.
std::vector<StreamOption> ResolveRadioStreamUrls(const RadioSearchResult& result);

// Check if URL looks like a playlist file (.m3u, .pls, .m3u8)
bool IsPlaylistUrl(const std::wstring& url);

// Resolve a remote playlist URL (.m3u/.pls/.m3u8) to a direct stream URL.
// Returns the url unchanged if it's not a playlist URL or resolution fails.
std::wstring ResolvePlaylistUrl(const std::wstring& url);

// Resolve an HTTP(S) URL's redirects to its final target (headers only, no body).
// Returns the url unchanged if it isn't http(s) or resolution fails.
std::wstring ResolveHttpRedirects(const std::wstring& url);

// Import the stream URLs of an M3U/M3U8/PLS playlist file into the radio
// favorites, skipping URLs already saved (or repeated within the file) and
// keeping the file's order.
struct RadioImportResult {
    int imported = 0;
    int skipped = 0;
};
RadioImportResult ImportRadioFavorites(const std::wstring& playlistPath);

// Write the stations to an M3U file (UTF-8 with BOM). False if the file could
// not be opened for writing.
bool ExportRadioFavorites(const std::wstring& path, const std::vector<RadioStation>& stations);

#endif // FASTPLAY_RADIO_H
