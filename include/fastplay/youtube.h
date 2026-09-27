#pragma once
#ifndef FASTPLAY_YOUTUBE_H
#define FASTPLAY_YOUTUBE_H

// YouTube: search (the YouTube Data API when there is a key, yt-dlp otherwise),
// playlist and channel listings, and getting a video ready to play. Everything here
// blocks, often for seconds: call it from a worker thread.

#include <functional>
#include <string>
#include <vector>

// YouTube search result
struct YouTubeResult {
    std::wstring videoId;
    std::wstring title;
    std::wstring channel;
    std::wstring duration;    // Human-readable duration ("live" for a live stream)
    std::wstring uploadDate;  // Human-readable upload date
    bool isPlaylist = false;
    bool isChannel = false;
};

// A video ready to play: a local audio file, or for a live stream its URL.
struct YouTubeMedia {
    std::wstring file;
    std::wstring url;
    std::wstring title;
};

// Short progress messages worth saying while something slow happens (a first-time
// download of the YouTube tools, say). Called on the worker thread.
using YouTubeStatus = std::function<void(const std::wstring& message)>;

// Search YouTube. With an API key, nextPageToken is set if more results are
// available; yt-dlp search returns one page. `error` explains a failure.
bool YouTubeSearch(const std::wstring& query, std::vector<YouTubeResult>& results,
                   std::wstring& nextPageToken, const std::wstring& pageToken,
                   std::wstring& error, const YouTubeStatus& status = nullptr);

// The videos of a playlist or channel page (a YouTube URL).
bool YouTubeGetListContents(const std::wstring& listUrl, std::vector<YouTubeResult>& results,
                            std::wstring& error, const YouTubeStatus& status = nullptr);

// Get a video ready to play. The audio is downloaded (with yt-dlp) and kept for a
// week, so playing it again is immediate. On first use this also downloads yt-dlp
// and deno, the JavaScript runtime yt-dlp needs for YouTube.
bool YouTubePrepare(const std::wstring& videoId, YouTubeMedia& media, std::wstring& error,
                    const YouTubeStatus& status = nullptr);

// Remove downloaded videos not played for a week (call on startup and exit)
void YouTubeCleanup();

// Check if input looks like a YouTube URL
bool IsYouTubeURL(const std::wstring& input);

// Parse YouTube URL to extract video/playlist/channel ID
bool ParseYouTubeURL(const std::wstring& url, std::wstring& id, bool& isPlaylist, bool& isChannel);

// The URL listing a playlist's videos, and a channel's (from ParseYouTubeURL's ID)
std::wstring YouTubePlaylistUrl(const std::wstring& playlistId);
std::wstring YouTubeChannelUrl(const std::wstring& channelId);

#endif // FASTPLAY_YOUTUBE_H
