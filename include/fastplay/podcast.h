#pragma once
#ifndef FASTPLAY_PODCAST_H
#define FASTPLAY_PODCAST_H

// Podcasts: fetching and parsing RSS feeds (including password-protected ones),
// searching the iTunes directory, OPML import and export, and preparing episode
// downloads for the download manager. The Podcasts window is src/ui/podcast_dialog.cpp.
//
// The feed and search functions block on the network and are safe to call from a
// background thread. The download functions touch the file system and the download
// queue and belong on the UI thread.

#include "database.h"

#include <string>
#include "http.h"  // BuildBasicAuthHeader
#include <tuple>
#include <vector>

// Podcast search result (from iTunes API)
struct PodcastSearchResult {
    std::wstring name;
    std::wstring feedUrl;
    std::wstring imageUrl;
    std::wstring artistName;
};

// Diagnostic info captured during feed fetch/parse, shown to the user on failure
struct PodcastFetchDiag {
    unsigned long statusCode = 0;  // HTTP status (0 if request never completed)
    unsigned long lastError = 0;   // Win32 error code if the request failed
    std::wstring errorText;        // Human-readable network error
    size_t bytesReceived = 0;
    int itemTagsFound = 0;         // Raw <item tags in the XML
    int episodesExtracted = 0;     // Items with both audioUrl and title
    std::wstring bodyPreview;      // First ~400 chars of the response
};

// Structure for OPML feed entry
struct OpmlFeed {
    std::wstring title;
    std::wstring feedUrl;
};

// Parse RSS feed and extract episodes (with optional authentication). Returns
// false when the feed could not be fetched or had no playable episodes.
bool ParsePodcastFeed(const std::wstring& feedUrl, std::wstring& outTitle,
                      std::vector<PodcastEpisode>& episodes,
                      const std::wstring& username = L"", const std::wstring& password = L"",
                      PodcastFetchDiag* diag = nullptr);

// Search iTunes podcast directory
bool SearchItunesPodcasts(const std::wstring& query, std::vector<PodcastSearchResult>& results);

// Build a human-readable diagnostic report from a failed feed fetch/parse
std::wstring BuildPodcastDiagMessage(const std::wstring& feedUrl, const PodcastFetchDiag& diag);

// The line shown for an episode in the episode list: title, date and the start
// of the description.
std::wstring FormatPodcastEpisodeDisplay(const PodcastEpisode& ep);

// Compute the cleaned plain-text description for an episode, ready for the
// description box.
std::wstring CleanPodcastDescription(const std::wstring& raw);

// Parse OPML file and extract feed URLs
std::vector<OpmlFeed> ParseOpmlFile(const std::wstring& filePath);

// Write subscriptions to an OPML file. False if the file could not be created.
bool ExportOpmlFile(const std::wstring& filePath, const std::vector<PodcastSubscription>& subs);

// Build an "Authorization: Basic <base64(user:pass)>" header from credentials (UTF-8 encoded).

// Episodes ready to download: (url, destination path, title), plus how many were
// skipped because they have no audio URL or are already on disk.
struct PodcastDownloadBatch {
    std::vector<std::tuple<std::wstring, std::wstring, std::wstring>> items;
    int skipped = 0;
};

// Work out where each episode goes in the downloads folder (g_downloadPath, which
// must be set), in a subfolder named after `feed` when downloads are organized by
// feed. `feed` is null when the episodes don't belong to a known subscription.
PodcastDownloadBatch PreparePodcastDownloads(const std::vector<const PodcastEpisode*>& episodes,
                                             const PodcastSubscription* feed);

// Hand a batch to the download manager: a single item is queued on its own, more
// as one batch. `asBatch` queues even a single item as a batch.
void QueuePodcastDownloads(const PodcastDownloadBatch& batch, const std::wstring& authHeader, bool asBatch);

#endif // FASTPLAY_PODCAST_H
