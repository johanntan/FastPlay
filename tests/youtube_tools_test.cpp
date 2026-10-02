// Offline regression test using copies of this executable as fake tools.
// macOS:
// c++ -std=c++17 -O1 -ffunction-sections -fdata-sections -Iinclude -Iinclude/fastplay \
//   tests/youtube_tools_test.cpp src/youtube_tools.cpp src/platform/subprocess_posix.cpp \
//   src/utils.cpp src/platform/ini_portable.cpp -Wl,-dead_strip -o /tmp/youtube_tools_test
// /tmp/youtube_tools_test
// Linux: replace -Wl,-dead_strip with -Wl,--gc-sections.
// Windows: use subprocess_windows.cpp and ini_windows.cpp, -DNOMINMAX,
// and -Wl,--gc-sections with MinGW.
#include "../src/youtube.cpp"
#include "ini.h"

#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {

fs::path testRoot;
int httpRequests = 0;

void Check(bool ok, const char* message) {
	if (!ok) throw std::runtime_error(message);
}

void SetPath(const std::wstring& value) {
#ifdef _WIN32
	_wputenv_s(L"PATH", value.c_str());
#else
	setenv("PATH", WideToUtf8(value).c_str(), 1);
#endif
}

std::wstring File(const wchar_t* name) {
#ifdef _WIN32
	return std::wstring(name) + L".exe";
#else
	return name;
#endif
}

void CopyTool(const fs::path& self, const fs::path& dir, const wchar_t* name) {
	fs::create_directories(dir);
	fs::path tool = dir / File(name);
	fs::copy_file(self, tool, fs::copy_options::overwrite_existing);
	fs::permissions(tool, fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write);
}

void TestResolution(const fs::path& self) {
	fs::path first = testRoot / "first tools";
	fs::path second = testRoot / "second tools";
	for (const wchar_t* name : {L"yt-dlp", L"deno", L"ffmpeg"}) CopyTool(self, first, name);
	for (const wchar_t* name : {L"yt-dlp", L"ffmpeg", L"ffprobe"}) CopyTool(self, second, name);
#ifdef _WIN32
	const wchar_t separator = L';';
#else
	const wchar_t separator = L':';
#endif
	SetPath(first.wstring() + separator + second.wstring());
	YouTubeToolSettings tools;
	tools.source = YouTubeToolSource::Installed;
	std::wstring path, error;
	Check(ResolveYouTubeTool(tools, YouTubeTool::Ytdlp, path, error), "PATH tool not found");
	Check(fs::path(path) == first / File(L"yt-dlp"), "PATH precedence changed");
	Check(ResolveYouTubeTool(tools, YouTubeTool::Ffmpeg, path, error), "FFmpeg pair not found");
	Check(fs::path(path) == second / File(L"ffmpeg"), "Selected an incomplete FFmpeg pair");
	Check(ResolveYouTubeTool(tools, YouTubeTool::Ffprobe, path, error), "ffprobe not found");
	Check(fs::path(path) == second / File(L"ffprobe"), "ffprobe came from a different folder");
	tools.ytdlpPath = (second / File(L"yt-dlp")).wstring();
	Check(ResolveYouTubeTool(tools, YouTubeTool::Ytdlp, path, error), "Override not found");
	Check(path == tools.ytdlpPath, "Override did not win over PATH");
	tools.ytdlpPath = (testRoot / File(L"missing")).wstring();
	Check(!ResolveYouTubeTool(tools, YouTubeTool::Ytdlp, path, error) && path.empty(), "Invalid override silently fell back");
	tools.ffmpegFolder = first.wstring();
	Check(!ResolveYouTubeTool(tools, YouTubeTool::Ffmpeg, path, error), "Invalid FFmpeg override fell back");
#ifndef _WIN32
	tools.ytdlpPath = (first / File(L"yt-dlp")).wstring();
	fs::permissions(fs::path(tools.ytdlpPath), fs::perms::owner_read);
	Check(!ResolveYouTubeTool(tools, YouTubeTool::Ytdlp, path, error), "Non-executable file accepted");
	fs::permissions(fs::path(tools.ytdlpPath), fs::perms::owner_all);
#endif
#ifdef __APPLE__
	SetPath(L"/a/path/that/does/not/exist");
	auto dirs = YouTubeToolSearchDirectories();
	Check(dirs.size() == 3 && dirs[1] == L"/opt/homebrew/bin" && dirs[2] == L"/usr/local/bin", "Homebrew fallback locations missing");
#endif
	SetPath(first.wstring() + separator + second.wstring());
	tools = {};
	tools.source = YouTubeToolSource::Installed;
	SetYouTubeToolSettings(tools);
	auto snapshot = GetYouTubeToolSettings();
	tools.denoPath = L"changed";
	SetYouTubeToolSettings(tools);
	Check(snapshot.denoPath.empty(), "Settings snapshot mutated");
	tools.source = static_cast<YouTubeToolSource>(99);
	SetYouTubeToolSettings(tools);
	Check(GetYouTubeToolSettings().source == YouTubeToolSource::Managed, "Invalid source was not normalized");
}

void TestExecution(const fs::path& self) {
	YouTubeToolSettings tools;
	tools.source = YouTubeToolSource::Installed;
	std::wstring error, path;
	YtdlpRun run;
	Check(RunYtdlp({L"--echo", L"argument with spaces", L"quote\"and\\slash"}, true, run, error, nullptr, tools), "Installed yt-dlp did not run");
	Check(run.exitCode == 0 && run.output.find("argument with spaces\n") != std::string::npos, "Argument boundaries changed");
	Check(run.output.find("quote\"and\\slash\n") != std::string::npos, "Quoted argument changed");
	Check(run.output.find("--no-update\n") != std::string::npos, "Installed yt-dlp may update itself");
	Check(run.output.find("deno:" + WideToUtf8((testRoot / "first tools" / File(L"deno")).wstring())) != std::string::npos,
		"Deno path was not passed explicitly");
	Check(EnsureFfmpeg(path, error, nullptr, tools), "Installed FFmpeg did not resolve");
	Check(fs::path(path) == testRoot / "second tools", "FFmpeg location is wrong");

	// Seed managed copies: installed mode must still reject invalid explicit paths.
	fs::path managed = testRoot / "data" / "tools";
	CopyTool(self, managed, L"yt-dlp");
	CopyTool(self, managed, L"deno");
	CopyTool(self, managed / "ffmpeg", L"ffmpeg");
	CopyTool(self, managed / "ffmpeg", L"ffprobe");
	tools.ytdlpPath = (testRoot / File(L"missing")).wstring();
	Check(!RunYtdlp({}, true, run, error, nullptr, tools), "Missing installed yt-dlp used managed copy");
	tools.ytdlpPath.clear();
	tools.denoPath = (testRoot / File(L"missing")).wstring();
	Check(!RunYtdlp({}, true, run, error, nullptr, tools), "Missing installed Deno used managed copy");
	Check(RunYtdlp({L"--echo"}, false, run, error, nullptr, tools), "Flat search unnecessarily required Deno");
	tools.ffmpegFolder = (testRoot / "missing").wstring();
	Check(!EnsureFfmpeg(path, error, nullptr, tools), "Missing installed FFmpeg used managed copy");
	Check(httpRequests == 0, "Installed mode made tool download or update requests");
	Check(!fs::exists(managed / "yt-dlp-checked"), "Installed mode updated a managed tool");
}

void TestPersistence() {
	std::wstring config = (testRoot / "settings.ini").wstring();
	auto tools = ReadYouTubeToolSettings(config);
	Check(tools.source == YouTubeToolSource::Managed && tools.ytdlpPath.empty(), "Missing settings did not default to managed");
	IniWriteString(L"YouTube", L"YtdlpPath", L"/legacy yt-dlp", config.c_str());
	tools = ReadYouTubeToolSettings(config);
	Check(tools.source == YouTubeToolSource::Managed && tools.ytdlpPath == L"/legacy yt-dlp", "Legacy settings did not migrate");
	tools.source = YouTubeToolSource::Installed;
	tools.denoPath = L"/custom Deno";
	tools.ffmpegFolder = L"/custom FFmpeg";
	WriteYouTubeToolSettings(config, tools);
	auto loaded = ReadYouTubeToolSettings(config);
	Check(loaded.source == tools.source && loaded.ytdlpPath == tools.ytdlpPath &&
		loaded.denoPath == tools.denoPath && loaded.ffmpegFolder == tools.ffmpegFolder, "Settings did not round trip");
	IniWriteString(L"YouTube", L"ToolSource", L"99", config.c_str());
	Check(ReadYouTubeToolSettings(config).source == YouTubeToolSource::Managed, "Invalid persisted source not defaulted");
}

void TestDiagnostics(const fs::path& self) {
	YouTubeToolSettings tools;
	tools.source = YouTubeToolSource::Installed;
	std::wstring report = TestYouTubeTools(tools);
	size_t successes = 0;
	for (size_t pos = 0; (pos = report.find(L"OK —", pos)) != std::wstring::npos; pos += 4) ++successes;
	Check(successes == 4, "Offline diagnostics did not validate all four tools");
	Check(report.find(L"extraction were not tested") != std::wstring::npos, "Offline test overstates validation");
	Check(httpRequests == 0, "Diagnostics made network requests");
	tools.ytdlpPath = (testRoot / File(L"missing")).wstring();
	report = TestYouTubeTools(tools);
	Check(report.find(L"missing or not executable") != std::wstring::npos, "Invalid diagnostic path not reported");
	CopyTool(self, testRoot / "unsupported", L"yt-dlp");
	tools.ytdlpPath = (testRoot / "unsupported" / File(L"yt-dlp")).wstring();
	Check(TestYouTubeTools(tools).find(L"does not support --js-runtimes") != std::wstring::npos, "Old yt-dlp not diagnosed");
	CopyTool(self, testRoot / "failing", L"deno");
	tools.ytdlpPath.clear();
	tools.denoPath = (testRoot / "failing" / File(L"deno")).wstring();
	Check(TestYouTubeTools(tools).find(L"exit code 7") != std::wstring::npos, "Failed tool not diagnosed");
	CopyTool(self, testRoot / "bad-js", L"deno");
	tools.denoPath = (testRoot / "bad-js" / File(L"deno")).wstring();
	Check(TestYouTubeTools(tools).find(L"unexpected result") != std::wstring::npos, "Failed JavaScript check not diagnosed");
	CopyTool(self, testRoot / "hanging", L"deno");
	tools.denoPath = (testRoot / "hanging" / File(L"deno")).wstring();
	Check(TestYouTubeTools(tools).find(L"Timed out after 10 seconds") != std::wstring::npos, "Diagnostic timeout not reported");
	fs::remove_all(testRoot / "data" / "tools");
	tools = {};
	report = TestYouTubeTools(tools);
	Check(report.find(L"Not installed yet") != std::wstring::npos, "Missing managed tools not reported");
	Check(report.find(L"ffmpeg: " + (testRoot / "first tools" / File(L"ffmpeg")).wstring()) != std::wstring::npos &&
		report.find(L"ffprobe: " + (testRoot / "second tools" / File(L"ffprobe")).wstring()) != std::wstring::npos,
		"Managed diagnostics did not independently discover FFmpeg and ffprobe on PATH");
	Check(!fs::exists(testRoot / "data" / "tools"), "Managed diagnostics created tools directory");
	std::wstring savedPath = Utf8ToWide(std::getenv("PATH"));
	SetPath(L"/a/path/that/does/not/exist");
	report = TestYouTubeTools(tools);
	Check(report.find(L"ffmpeg: Not installed yet") != std::wstring::npos &&
		report.find(L"ffprobe: Not installed yet") != std::wstring::npos,
		"Managed diagnostics selected tools outside the inherited PATH");
	SetPath(savedPath);
}

void TestSubprocess(const fs::path& self) {
	std::string output, errors;
	int code;
	ProcessOptions options;
	options.timeoutMs = 100;
	auto start = std::chrono::steady_clock::now();
	Check(RunProcessCapture(self.wstring(), {L"--hang"}, output, &errors, &code, options), "Timed process could not start");
	Check(code == -2 && std::chrono::steady_clock::now() - start < std::chrono::seconds(2), "Timed process did not terminate promptly");
	Check(RunProcessCapture(self.wstring(), {L"--close-and-hang"}, output, &errors, &code, options) && code == -2,
		"Timeout failed when the process closed its pipes");
	Check(RunProcessCapture(self.wstring(), {L"--spawn-and-hang"}, output, &errors, &code, options) && code == -2,
		"Timeout failed for a process with helpers");
	Check(!RunProcessCapture((testRoot / File(L"missing")).wstring(), {}, output, &errors, &code, options) && code == -1,
		"Launch failure not distinguished from timeout");
	options.timeoutMs = 5000;
	Check(RunProcessCapture(self.wstring(), {L"--flood"}, output, &errors, &code, options) && code == 0,
		"Concurrent stdout and stderr caused a stall");
	Check(output.size() >= 131072 && errors.size() >= 131072, "Pipe output was truncated");
	options.pathDirectories = {(testRoot / "child helpers").wstring()};
	Check(RunProcessCapture(self.wstring(), {L"--path"}, output, &errors, &code, options), "Child PATH test failed");
	Check(output.find("child helpers") != std::string::npos, "Child PATH was not extended");
	Check(std::string(std::getenv("PATH")).find("child helpers") == std::string::npos, "Parent PATH was changed");
}

} // namespace

const wchar_t kPathSeparator = fs::path::preferred_separator;
std::wstring GetDataDirectory() { return (testRoot / "data").wstring() + kPathSeparator; }
HttpResult HttpGet(const std::wstring&, const HttpOptions&) { ++httpRequests; return {}; }

#ifdef _WIN32
// PE linkers retain more of the included implementation than macOS dead_strip.
// Any unrelated playback/UI call is a test failure, rather than a real side effect.
std::wstring g_ytApiKey;
YouTubeDownloadSettings g_ytDownload;
std::wstring GetTempDir() { return testRoot.wstring() + kPathSeparator; }
std::wstring GetUserDownloadsDir() { return (testRoot / "downloads").wstring(); }
std::wstring GetUserMusicDir() { return (testRoot / "music").wstring(); }
void SpeakW(const std::wstring&, bool) { throw std::runtime_error("Unexpected speech"); }
void RunOnUiThread(std::function<void()>) { throw std::runtime_error("Unexpected UI dispatch"); }
void ShowMessage(const std::wstring&, const std::wstring&, MessageIcon) { throw std::runtime_error("Unexpected message"); }
bool DefragmentMp4(const std::wstring&, const std::wstring&, const std::wstring&, const std::wstring&) {
	throw std::runtime_error("Unexpected media conversion");
}
#endif

int main(int argc, char** argv) {
	// Child tool behavior, shared by copied executables on macOS and Windows.
	if (argc > 1) {
		std::string where = argv[0], command = argv[1];
		if (where.find("hanging") != std::string::npos) std::this_thread::sleep_for(std::chrono::seconds(30));
		if (command == "--ignore-config") {
			for (int i = 2; i < argc; ++i) {
				if (std::string(argv[i]) == "--version" || std::string(argv[i]) == "--help") command = argv[i];
			}
		}
		if (where.find("failing") != std::string::npos) { std::cerr << "Tool failure\n"; return 7; }
		if (command == "--version" || command == "-version") std::cout << "test 1.0\n";
		else if (command == "--help")
			std::cout << (where.find("unsupported") != std::string::npos ? "old tool\n" : "--js-runtimes\n");
		else if (command == "eval") std::cout << (where.find("bad-js") != std::string::npos ? "wrong result\n" : "FastPlay:42\n");
		else if (command == "--hang" || command == "--close-and-hang") {
			if (command == "--close-and-hang") { fclose(stdout); fclose(stderr); }
			std::this_thread::sleep_for(std::chrono::seconds(5));
		} else if (command == "--spawn-and-hang") {
			std::string output;
			RunProcessCapture(fs::absolute(argv[0]).wstring(), {L"--hang"}, output);
		} else if (command == "--flood") {
			for (int i = 0; i < 128; ++i) {
				std::cout << std::string(1024, 'o');
				std::cerr << std::string(1024, 'e');
			}
		} else if (command == "--path") std::cout << std::getenv("PATH");
		else for (int i = 1; i < argc; ++i) std::cout << argv[i] << '\n';
		return 0;
	}
	const std::string savedPath = std::getenv("PATH") ? std::getenv("PATH") : "";
	testRoot = fs::temp_directory_path() / ("FastPlay tools regression " +
		std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	fs::create_directories(testRoot);
	int result = 0;
	try {
		fs::path self = fs::absolute(argv[0]);
		TestResolution(self);
		TestExecution(self);
		TestPersistence();
		TestDiagnostics(self);
		TestSubprocess(self);
		std::cout << "YouTube tools regression checks passed\n";
	} catch (const std::exception& error) {
		std::cerr << error.what() << '\n';
		result = 1;
	}
	SetPath(Utf8ToWide(savedPath));
	fs::remove_all(testRoot);
	return result;
}
