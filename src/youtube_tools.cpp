#include "youtube_tools.h"
#include "ini.h"
#include "paths.h"
#include "subprocess.h"
#include "utils.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

std::mutex settingsMutex;
YouTubeToolSettings toolSettings;

std::wstring ToolName(YouTubeTool tool) {
	switch (tool) {
		case YouTubeTool::Ytdlp: return L"yt-dlp";
		case YouTubeTool::Deno: return L"deno";
		case YouTubeTool::Ffmpeg: return L"ffmpeg";
		case YouTubeTool::Ffprobe: return L"ffprobe";
	}
	return L"";
}

std::wstring ToolFile(YouTubeTool tool) {
	std::wstring name = ToolName(tool);
#ifdef _WIN32
	name += L".exe";
#endif
	return name;
}

bool Executable(const fs::path& path) {
	std::error_code ec;
	if (!fs::is_regular_file(path, ec)) return false;
#ifndef _WIN32
	return access(path.c_str(), X_OK) == 0;
#else
	return WStrICmp(path.extension().wstring().c_str(), L".exe") == 0;
#endif
}

bool RegularFile(const fs::path& path) {
	std::error_code ec;
	return fs::is_regular_file(path, ec);
}

void AddDirectory(std::vector<std::wstring>& dirs, std::wstring dir) {
	if (dir.size() >= 2 && dir.front() == L'"' && dir.back() == L'"') dir = dir.substr(1, dir.size() - 2);
	// Do not implicitly search the current working directory for executable tools.
	if (dir.empty()) return;
	std::error_code ec;
	fs::path path = fs::absolute(fs::path(dir), ec).lexically_normal();
	if (ec) return;
	dir = path.wstring();
	if (std::find(dirs.begin(), dirs.end(), dir) == dirs.end()) dirs.push_back(dir);
}

bool FfmpegPair(const fs::path& folder) {
	return Executable(folder / ToolFile(YouTubeTool::Ffmpeg)) &&
		Executable(folder / ToolFile(YouTubeTool::Ffprobe));
}

std::wstring ManagedPath(const YouTubeToolSettings& settings, YouTubeTool tool) {
	fs::path dir = fs::path(GetDataDirectory()) / L"tools";
	if (tool == YouTubeTool::Ytdlp) {
		if (!settings.ytdlpPath.empty() && RegularFile(fs::path(settings.ytdlpPath))) return settings.ytdlpPath;
#if defined(_WIN32)
		fs::path folderBuild = dir / L"yt-dlp_win" / L"yt-dlp.exe";
#elif defined(__APPLE__)
		fs::path folderBuild = dir / L"yt-dlp_macos" / L"yt-dlp_macos";
#endif
#if defined(_WIN32) || defined(__APPLE__)
		if (RegularFile(folderBuild)) return folderBuild.wstring();
#endif
	}
	if (tool == YouTubeTool::Ffmpeg || tool == YouTubeTool::Ffprobe) {
		fs::path managed = dir / L"ffmpeg" / ToolFile(tool);
		if (RegularFile(dir / L"ffmpeg" / ToolFile(YouTubeTool::Ffmpeg))) {
			return RegularFile(managed) ? managed.wstring() : L"";
		}
		// Managed downloads only use the inherited PATH. Homebrew fallbacks and
		// an augmented child PATH belong to installed mode.
		for (const auto& folder : YouTubeToolSearchDirectories(false)) {
			fs::path candidate = fs::path(folder) / ToolFile(tool);
			if (Executable(candidate)) return candidate.wstring();
		}
		return L"";
	}
	fs::path path = dir / ToolFile(tool);
	return RegularFile(path) ? path.wstring() : L"";
}

std::wstring FirstLine(const std::string& text) {
	std::istringstream lines(text);
	std::string line;
	while (std::getline(lines, line)) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (!line.empty()) return Utf8ToWide(line.substr(0, 1000));
	}
	return L"";
}

} // namespace

YouTubeToolSettings GetYouTubeToolSettings() {
	std::lock_guard<std::mutex> lock(settingsMutex);
	return toolSettings;
}

void SetYouTubeToolSettings(const YouTubeToolSettings& settings) {
	std::lock_guard<std::mutex> lock(settingsMutex);
	toolSettings = settings;
	if (toolSettings.source != YouTubeToolSource::Installed) toolSettings.source = YouTubeToolSource::Managed;
}

YouTubeToolSettings ReadYouTubeToolSettings(const std::wstring& configPath) {
	YouTubeToolSettings settings;
	settings.source = IniGetInt(L"YouTube", L"ToolSource", 0, configPath.c_str()) == 1 ?
		YouTubeToolSource::Installed : YouTubeToolSource::Managed;
	auto read = [&](const wchar_t* key) {
		wchar_t value[4096] = {};
		IniGetString(L"YouTube", key, L"", value, 4096, configPath.c_str());
		return std::wstring(value);
	};
	settings.ytdlpPath = read(L"YtdlpPath");
	settings.denoPath = read(L"DenoPath");
	settings.ffmpegFolder = read(L"FfmpegFolder");
	return settings;
}

void WriteYouTubeToolSettings(const std::wstring& configPath, const YouTubeToolSettings& settings) {
	IniWriteString(L"YouTube", L"ToolSource", settings.source == YouTubeToolSource::Installed ? L"1" : L"0", configPath.c_str());
	IniWriteString(L"YouTube", L"YtdlpPath", settings.ytdlpPath.c_str(), configPath.c_str());
	IniWriteString(L"YouTube", L"DenoPath", settings.denoPath.c_str(), configPath.c_str());
	IniWriteString(L"YouTube", L"FfmpegFolder", settings.ffmpegFolder.c_str(), configPath.c_str());
}

std::vector<std::wstring> YouTubeToolSearchDirectories(bool includeHomebrewFallbacks) {
	std::wstring path;
#ifdef _WIN32
	DWORD size = GetEnvironmentVariableW(L"PATH", nullptr, 0);
	if (size) {
		std::vector<wchar_t> buffer(size);
		DWORD read = GetEnvironmentVariableW(L"PATH", buffer.data(), size);
		if (read && read < size) path.assign(buffer.data(), read);
	}
	const wchar_t separator = L';';
#else
	if (const char* value = std::getenv("PATH")) path = Utf8ToWide(value);
	const wchar_t separator = L':';
#endif
	std::vector<std::wstring> dirs;
	std::wistringstream parts(path);
	std::wstring dir;
	while (std::getline(parts, dir, separator)) AddDirectory(dirs, dir);
#ifdef __APPLE__
	if (includeHomebrewFallbacks) {
		AddDirectory(dirs, L"/opt/homebrew/bin");
		AddDirectory(dirs, L"/usr/local/bin");
	}
#else
	(void)includeHomebrewFallbacks;
#endif
	return dirs;
}

bool ResolveYouTubeTool(const YouTubeToolSettings& settings, YouTubeTool tool,
	std::wstring& path, std::wstring& error) {
	path.clear();
	error.clear();
	std::wstring overridePath = tool == YouTubeTool::Ytdlp ? settings.ytdlpPath :
		tool == YouTubeTool::Deno ? settings.denoPath : settings.ffmpegFolder;
	bool ffmpeg = tool == YouTubeTool::Ffmpeg || tool == YouTubeTool::Ffprobe;
	auto resolve = [&](const fs::path& candidate) {
		if (!Executable(candidate) || (ffmpeg && !FfmpegPair(candidate.parent_path()))) return false;
		std::error_code ec;
		fs::path absolute = fs::absolute(candidate, ec).lexically_normal();
		if (ec) return false;
		path = absolute.wstring(); // Keep symlink paths so Homebrew's sibling helpers remain discoverable.
		return true;
	};
	if (!overridePath.empty()) {
		fs::path candidate(overridePath);
		if (ffmpeg) candidate /= ToolFile(tool);
		if (resolve(candidate)) return true;
		error = ffmpeg ? L"The FFmpeg folder must contain executable ffmpeg and ffprobe: " + overridePath :
			L"The selected " + ToolName(tool) + L" is missing or not executable: " + overridePath;
		return false;
	}
	for (const auto& dir : YouTubeToolSearchDirectories()) {
		if (resolve(fs::path(dir) / ToolFile(tool))) return true;
	}
	error = L"Could not find " + ToolName(tool) + L" on PATH";
#ifdef __APPLE__
	error += L" or in the standard Homebrew locations";
#endif
	error += ffmpeg ? L". Install FFmpeg and ffprobe in the same folder, or choose their folder in Settings > YouTube." :
		L". Install it or choose its path in Settings > YouTube.";
	return false;
}

std::vector<std::wstring> YouTubeToolChildDirectories(const YouTubeToolSettings& settings) {
	if (settings.source != YouTubeToolSource::Installed) return {};
	auto dirs = YouTubeToolSearchDirectories();
	for (const auto& path : {settings.ytdlpPath, settings.denoPath}) {
		if (!path.empty()) AddDirectory(dirs, fs::path(path).parent_path().wstring());
	}
	AddDirectory(dirs, settings.ffmpegFolder);
	return dirs;
}

std::wstring TestYouTubeTools(const YouTubeToolSettings& settings) {
	std::wstring report = settings.source == YouTubeToolSource::Installed ? L"Installed tools — local validation\n\n" :
		L"FastPlay managed — local validation\n\n";
	ProcessOptions options;
	options.timeoutMs = 10000;
	options.pathDirectories = YouTubeToolChildDirectories(settings);
	for (YouTubeTool tool : {YouTubeTool::Ytdlp, YouTubeTool::Deno, YouTubeTool::Ffmpeg, YouTubeTool::Ffprobe}) {
		std::wstring path, error;
		if (settings.source == YouTubeToolSource::Installed) {
			ResolveYouTubeTool(settings, tool, path, error);
		} else {
			path = ManagedPath(settings, tool);
			if (path.empty()) error = L"Not installed yet. FastPlay installs managed tools when needed.";
		}
		report += ToolName(tool) + L": ";
		if (path.empty()) {
			report += error;
		} else {
			std::error_code ec;
			path = fs::absolute(fs::path(path), ec).wstring();
			report += path + L"\n";
			auto check = [&](const std::vector<std::wstring>& args, std::string& output) {
				std::string errors;
				int code = -1;
				if (!RunProcessCapture(path, args, output, &errors, &code, options)) {
					error = L"Could not start the executable.";
				} else if (code == -2) {
					error = L"Timed out after 10 seconds.";
				} else if (code != 0) {
					error = L"Failed (exit code " + std::to_wstring(code) + L"). " + FirstLine(errors);
				}
				return error.empty();
			};
			std::string output;
			std::vector<std::wstring> versionArgs = tool == YouTubeTool::Ytdlp ?
				std::vector<std::wstring>{L"--ignore-config", L"--no-update", L"--version"} :
				std::vector<std::wstring>{tool == YouTubeTool::Deno ? L"--version" : L"-version"};
			bool ok = check(versionArgs, output);
			std::wstring version = FirstLine(output);
			if (ok && version.empty()) {
				error = L"The executable returned no version information.";
				ok = false;
			}
			if (ok && tool == YouTubeTool::Ytdlp) {
				ok = check({L"--ignore-config", L"--no-update", L"--help"}, output);
				if (ok && output.find("--js-runtimes") == std::string::npos) {
					error = L"This yt-dlp does not support --js-runtimes. Update your installation.";
					ok = false;
				}
			}
			if (ok && tool == YouTubeTool::Deno) {
				ok = check({L"eval", L"--no-config", L"--no-lock", L"console.log('FastPlay:' + (6 * 7))"}, output);
				if (ok && FirstLine(output) != L"FastPlay:42") {
					error = L"JavaScript check returned an unexpected result.";
					ok = false;
				}
			}
			report += ok ? L"OK — " + version : error;
		}
		if (tool == YouTubeTool::Ffmpeg || tool == YouTubeTool::Ffprobe) report += L" (needed for processed downloads)";
		report += L"\n\n";
	}
	report += L"These offline checks confirm local tools run. YouTube access and extraction were not tested.";
	return report;
}
