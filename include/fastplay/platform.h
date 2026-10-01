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

#ifdef __APPLE__
class wxFrame;
// Catch the keys typed into a window before macOS dispatches them, so a shortcut
// that is also a menu item's key equivalent does not perform the menu item (which
// VoiceOver would announce). The handler gets the key as MOD_* flags (MOD_CONTROL
// is Command, MOD_ALT is Option, MOD_WIN is Control) and a Windows virtual key
// code, and returns true to swallow it. `holdHandler` sees keys going down (and
// repeating) and coming up, first, for keys that act while held. Menu key
// equivalents are suspended while another window is focused, so dialogs keep
// their typing and editing keys.
void StartShortcutMonitor(wxFrame* frame, bool (*handler)(unsigned modifiers, unsigned vk),
                          bool (*holdHandler)(unsigned modifiers, unsigned vk, bool down, bool repeat));
void StopShortcutMonitor();
#endif

#endif // FASTPLAY_PLATFORM_H
