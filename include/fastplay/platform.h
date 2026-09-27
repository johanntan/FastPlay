#pragma once
#ifndef FASTPLAY_PLATFORM_H
#define FASTPLAY_PLATFORM_H

// Operating system specific startup, implemented per platform in src/platform.

// Runs first thing at startup, before any audio library is loaded.
// On Windows: load DLLs from the lib folder beside FastPlay.exe.
void PlatformStartup();

#endif // FASTPLAY_PLATFORM_H
