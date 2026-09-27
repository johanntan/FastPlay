#pragma once
#ifndef FASTPLAY_PATHS_H
#define FASTPLAY_PATHS_H

// Where things are, per platform (src/platform/paths_*.cpp). Folder paths end with
// a separator unless noted.

#include <string>

// The path separator ("\\" on Windows, "/" elsewhere).
extern const wchar_t kPathSeparator;

// The folder FastPlay's executable is in.
std::wstring GetExecutableDir();

// Whether FastPlay was installed (Windows installer) rather than unpacked portably.
bool IsInstalledMode();

// Where FastPlay keeps its settings (FastPlay.ini) and database (FastPlay.db). Created
// if needed. Windows: %APPDATA%\FastPlay when installed, beside the executable when
// portable. macOS: ~/Library/Application Support/FastPlay.
std::wstring GetDataDirectory();

// The user's Music folder (no trailing separator), or empty if unknown.
std::wstring GetUserMusicDir();

// The folder for temporary files.
std::wstring GetTempDir();

// Where the audio libraries (BASS and its add-ons) are loaded from.
std::wstring GetLibraryDir();

#endif // FASTPLAY_PATHS_H
