#pragma once
#ifndef FASTPLAY_APP_UI_H
#define FASTPLAY_APP_UI_H

// How code outside the UI reaches it: the player, updater, download manager and
// background threads. Nothing here names a window system type, so none of that
// code needs to know which toolkit draws the windows.
//
// Every function is safe to call from any thread unless it says otherwise; calls
// from another thread are queued to the UI thread.

#include <functional>
#include <string>

// Run a main window command (an IDM_* id), as if chosen from the menu or a hotkey.
// `param` is passed to commands that take one (IDM_PLAY_NEXT: 1 = load without playing).
void PostCommand(int commandId, int param = 0);

// Run `fn` on the UI thread, after the current event has been handled.
void RunOnUiThread(std::function<void()> fn);

enum class MessageIcon { Info, Warning, Error, Question };

// A message box owned by whichever FastPlay window is active. From another thread
// it is shown once the UI thread gets to it, and the call does not wait.
void ShowMessage(const std::wstring& text, const std::wstring& title, MessageIcon icon = MessageIcon::Info);

// A Yes/No question owned by the active FastPlay window. UI thread only.
bool AskYesNo(const std::wstring& text, const std::wstring& title);

// The main window's native handle (an HWND on Windows), for libraries that want
// one. Null before the main window exists.
void* GetMainWindowHandle();

// Close the main window, which ends the program (saving state as usual).
void CloseMainWindow();

// Refresh the main window's title and status bar.
void UpdateWindowTitle();
void UpdateStatusBar();

// The one-shot timer that ends a scheduled event's duration: it calls
// HandleScheduledDurationEnd() after `ms` milliseconds. Starting it again restarts it.
void StartScheduleDurationTimer(int ms);
void StopScheduleDurationTimer();

// The playing track changed: an open playlist window moves its selection to it
// (when "follow playback" is on). UI thread only.
void NotifyPlaylistTrackChanged();

#endif // FASTPLAY_APP_UI_H
