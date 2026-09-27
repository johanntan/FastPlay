#pragma once
#ifndef FASTPLAY_UI_COMMON_H
#define FASTPLAY_UI_COMMON_H

// Helpers shared by FastPlay's windows and dialogs.

#include <wx/wx.h>
#include <string>

class MainFrame;

// The main window. Null before it is created and after it is destroyed.
MainFrame* GetMainFrame();
wxWindow* GetMainWindow();

// The FastPlay window that should own a message box or dialog: the active one,
// falling back to the main window.
wxWindow* GetActiveOwner();

// std::wstring <-> wxString without surprises.
inline wxString WX(const std::wstring& s) { return wxString(s); }
inline std::wstring WS(const wxString& s) { return s.ToStdWstring(); }

// The application icon (the same stock icon the window and tray always used).
wxIcon GetAppIcon();

// Put text on the clipboard. Returns false if the clipboard could not be opened.
bool SetClipboardText(const std::wstring& text);

// Let an edit box accept only digits. On Windows this is the native number style,
// which explains a rejected character in a tooltip that screen readers read.
void SetDigitsOnly(wxTextCtrl* edit);

// A Save As dialog. A name typed without an extension gets `defaultExt` (without the
// dot), and only then is an existing file checked for, with the choice to replace it
// or pick another name. Returns the path, or an empty string if cancelled.
wxString AskSavePath(wxWindow* parent, const wxString& defaultName, const wxString& filter,
                     const wxString& defaultExt);

#endif // FASTPLAY_UI_COMMON_H
