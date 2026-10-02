// Standalone persistence regression test. Uses a temporary INI, never user settings.
// macOS:
// c++ -std=c++17 -O1 -ffunction-sections -fdata-sections -Iinclude/fastplay \
//   tests/hotkeys_persistence_test.cpp src/hotkeys.cpp src/globals.cpp \
//   src/platform/ini_portable.cpp src/utils.cpp -Wl,-dead_strip -o /tmp/hotkeys_persistence_test
// /tmp/hotkeys_persistence_test
// Linux: replace -Wl,-dead_strip with -Wl,--gc-sections.
// Windows: use ini_windows.cpp, -DNOMINMAX and -Wl,--gc-sections with MinGW.
#include "globals.h"
#include "hotkeys.h"
#include "ini.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace {

void Check(bool ok, const char* message) {
	if (!ok) throw std::runtime_error(message);
}

void Write(const wchar_t* key, const wchar_t* value) {
	Check(IniWriteString(L"Hotkeys", key, value, g_configPath.c_str()), "Could not write test settings");
}

void CheckAction(size_t row, const wchar_t* name, bool global) {
	Check(row < g_hotkeys.size(), "Named hotkey was dropped while loading");
	const auto& key = g_hotkeys[row];
	Check(std::wstring(g_hotkeyActions[key.actionIdx].key) == name, "Hotkey action changed during loading");
	Check(key.global == global, "Global/local flag changed during loading");
}

void TestNamedActions() {
	Write(L"Count", L"5");
	Write(L"Enabled", L"1");
	Write(L"Hotkey0", L"10,80,PlayPause,1");
	Write(L"Hotkey1", L"11,37,SeekBackward,1");
	Write(L"Hotkey2", L"11,39,SeekForward,1");
	Write(L"Hotkey3", L"11,190,NextSeekUnit,1");
	Write(L"Hotkey4", L"11,188,PreviousSeekUnit,1");
	LoadHotkeys();
	Check(g_hotkeys.size() == 5, "Named hotkeys were dropped while loading");
	Check(g_hotkeysEnabled, "Enabled state was not loaded");
	const wchar_t* actions[] = {L"PlayPause", L"SeekBackward", L"SeekForward", L"NextSeekUnit", L"PreviousSeekUnit"};
	const unsigned keys[] = {80, 37, 39, 190, 188};
	for (size_t i = 0; i < 5; ++i) {
		CheckAction(i, actions[i], true);
		Check(g_hotkeys[i].vk == keys[i] && g_hotkeys[i].modifiers == (i == 0 ? 10u : 11u), "Key or modifiers changed");
	}
	auto saved = g_hotkeys;
	SaveHotkeys();
	Check(IniGetInt(L"Hotkeys", L"Count", -1, g_configPath.c_str()) == 5, "Saving replaced the count with zero");
	g_hotkeys.clear();
	LoadHotkeys();
	Check(g_hotkeys.size() == saved.size(), "Hotkeys did not survive save and reload");
	for (size_t i = 0; i < saved.size(); ++i) {
		Check(g_hotkeys[i].vk == saved[i].vk && g_hotkeys[i].modifiers == saved[i].modifiers &&
			g_hotkeys[i].actionIdx == saved[i].actionIdx && g_hotkeys[i].global == saved[i].global,
			"Hotkey changed across save and reload");
	}
}

void TestLegacyAndLocalActions() {
	Write(L"Count", L"4");
	Write(L"Enabled", L"0");
	Write(L"Hotkey0", L"2,37,6"); // Legacy SeekBackward, missing global means true.
	Write(L"Hotkey1", L"2,39,7,0"); // Legacy SeekForward, explicitly local.
	Write(L"Hotkey2", L"2,76,Library,0");
	Write(L"Hotkey3", L"2,65,UnknownAction,1");
	LoadHotkeys();
	Check(!g_hotkeysEnabled && g_hotkeys.size() == 3, "Legacy/local actions or disabled state did not load");
	CheckAction(0, L"SeekBackward", true);
	CheckAction(1, L"SeekForward", false);
	CheckAction(2, L"Library", false);
	SaveHotkeys();
	LoadHotkeys();
	Check(g_hotkeys.size() == 3, "Migrated legacy/local hotkeys did not reload");
	CheckAction(0, L"SeekBackward", true);
	CheckAction(1, L"SeekForward", false);
	CheckAction(2, L"Library", false);
	// Removing all hotkeys intentionally must not resurrect stale INI entries.
	g_hotkeys.clear();
	SaveHotkeys();
	LoadHotkeys();
	Check(g_hotkeys.empty(), "An intentionally empty list resurrected old entries");
}

} // namespace

int main() {
	fs::path temp = fs::temp_directory_path() / ("FastPlay hotkeys regression " +
		std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	fs::create_directories(temp);
	g_configPath = (temp / "settings.ini").wstring();
	int result = 0;
	try {
		TestNamedActions();
		TestLegacyAndLocalActions();
		std::cout << "Hotkey persistence regression checks passed\n";
	} catch (const std::exception& error) {
		std::cerr << error.what() << '\n';
		result = 1;
	}
	fs::remove_all(temp);
	return result;
}
