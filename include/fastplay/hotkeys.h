#pragma once
#ifndef FASTPLAY_HOTKEYS_H
#define FASTPLAY_HOTKEYS_H

#include <string>

// Registering the hotkeys with the system is the main window's job
// (MainFrame::RegisterGlobalHotkeys).

// Hotkey persistence
void LoadHotkeys();
void SaveHotkeys();

// Hotkey formatting
std::wstring FormatHotkey(unsigned modifiers, unsigned vk);

#endif // FASTPLAY_HOTKEYS_H
