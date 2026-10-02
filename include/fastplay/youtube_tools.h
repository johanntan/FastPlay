#pragma once

#include <string>
#include <vector>

enum class YouTubeToolSource { Managed = 0, Installed = 1 };
enum class YouTubeTool { Ytdlp, Deno, Ffmpeg, Ffprobe };

struct YouTubeToolSettings {
	YouTubeToolSource source = YouTubeToolSource::Managed;
	std::wstring ytdlpPath;
	std::wstring denoPath;
	std::wstring ffmpegFolder;
};

// Thread-safe snapshots: workers never read mutable settings owned by the UI.
YouTubeToolSettings GetYouTubeToolSettings();
void SetYouTubeToolSettings(const YouTubeToolSettings& settings);
YouTubeToolSettings ReadYouTubeToolSettings(const std::wstring& configPath);
void WriteYouTubeToolSettings(const std::wstring& configPath, const YouTubeToolSettings& settings);

// Resolve installed tools without running, installing, or updating anything.
bool ResolveYouTubeTool(const YouTubeToolSettings& settings, YouTubeTool tool,
	std::wstring& path, std::wstring& error);
std::vector<std::wstring> YouTubeToolSearchDirectories(bool includeHomebrewFallbacks = true);
std::vector<std::wstring> YouTubeToolChildDirectories(const YouTubeToolSettings& settings);

// Blocking offline checks, for a worker thread. Missing managed tools are reported,
// never acquired. The returned text is suitable for an accessible results control.
std::wstring TestYouTubeTools(const YouTubeToolSettings& settings);
