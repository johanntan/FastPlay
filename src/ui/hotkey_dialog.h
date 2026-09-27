#pragma once
#ifndef FASTPLAY_HOTKEY_DIALOG_H
#define FASTPLAY_HOTKEY_DIALOG_H

#include <wx/wx.h>
#include "types.h"

// The Add / Edit Global Hotkey dialog. `data` holds the starting action and key
// (data.isEdit picks the title) and receives the user's choice. Returns true if
// the user pressed OK with a valid hotkey.
bool ShowHotkeyDialog(wxWindow* parent, HotkeyDlgData& data);

#endif // FASTPLAY_HOTKEY_DIALOG_H
