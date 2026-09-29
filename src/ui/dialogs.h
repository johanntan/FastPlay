#pragma once
#ifndef FASTPLAY_DIALOGS_H
#define FASTPLAY_DIALOGS_H

// Every window FastPlay opens, as the main window's commands call them. All of
// these run on the UI thread and own themselves: modal ones return when closed.

#include <string>

// File menu (file_dialogs.cpp)
void ShowOpenDialog();
void ShowAddFolderDialog();

// Small dialogs (small_dialogs.cpp)
void ShowOpenURLDialog();
void ShowJumpToTimeDialog();
void ShowSaveEffectPresetDialog();
void ShowTagDialog(const wchar_t* title, const std::wstring& text);

// Larger windows, one file each
void ShowPlaylistDialog();      // playlist_dialog.cpp
void ShowOptionsDialog();       // options_dialog.cpp
void ShowBookmarksDialog();     // bookmarks_dialog.cpp
void ShowSongHistoryDialog();   // history_dialog.cpp
void ShowRadioDialog();         // radio_dialog.cpp
void AddCurrentStreamToFavorites();  // radio_dialog.cpp
void ShowPodcastDialog();       // podcast_dialog.cpp
void ShowSchedulerDialog();     // scheduler_dialog.cpp
void ShowYouTubeDialog();       // youtube_dialog.cpp (modeless)
void ShowLibraryDialog();       // library_dialog.cpp (modeless)

// Refresh the YouTube favorites on the schedule set in Options > YouTube (at
// startup, then every so often), saying when there are new videos. Call when the
// main window is up (atStartup) and after the options change; stop at exit.
void StartYouTubeAutoRefresh(bool atStartup);
void StopYouTubeAutoRefresh();

// Help > Check for Updates is ShowCheckForUpdatesDialog() in updater.h (update_dialog.cpp).

#endif // FASTPLAY_DIALOGS_H
