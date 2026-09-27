#pragma once
#ifndef FASTPLAY_HOTKEYS_H
#define FASTPLAY_HOTKEYS_H

#include <windows.h>
#include <string>

// Registering the hotkeys with the system is the main window's job
// (MainFrame::RegisterGlobalHotkeys).

// Hotkey persistence
void LoadHotkeys();
void SaveHotkeys();

// Hotkey formatting
std::wstring FormatHotkey(UINT modifiers, UINT vk);

#endif // FASTPLAY_HOTKEYS_H
