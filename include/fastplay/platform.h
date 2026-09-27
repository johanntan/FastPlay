#pragma once
#ifndef FASTPLAY_PLATFORM_H
#define FASTPLAY_PLATFORM_H

// Operating system specific startup and facts, implemented per platform in src/platform.

#include <string>

// Runs first thing at startup, before any audio library is loaded.
// On Windows: load DLLs from the lib folder beside FastPlay.exe.
void PlatformStartup();

// The system and processor, for the user agent: "Windows 11 10.0.26200; x64",
// "macOS 14.5; arm64".
std::string GetSystemDescription();

#endif // FASTPLAY_PLATFORM_H
