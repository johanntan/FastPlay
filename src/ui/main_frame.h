#pragma once
#ifndef FASTPLAY_MAIN_FRAME_H
#define FASTPLAY_MAIN_FRAME_H

#include <wx/wx.h>
#include <memory>
#include <string>

class TrayIcon;

// The main window: menu bar, keyboard shortcuts, status bar, tray icon and global
// hotkeys. Every command, from whichever of those it comes, runs through RunCommand.
class MainFrame : public wxFrame {
public:
    MainFrame();
    ~MainFrame() override;

    // Start audio, the database, effects, speech and hotkeys, and play any files
    // FastPlay was started with. False if audio could not start (already reported).
    bool Initialize();

    // Run an IDM_* command. `param` as in PostCommand().
    void RunCommand(int id, int param = 0);

    void UpdateTitle();
    void UpdateStatus();

    // Tray
    void HideToTray();
    void RestoreFromTray();
    void ToggleWindow();

    // Global hotkeys (media keys and the user's own).
    void RegisterGlobalHotkeys();
    void UnregisterGlobalHotkeys();

    // A file handed over by another FastPlay started with it (e.g. from Explorer).
    void ReceiveFile(const std::wstring& path);

    void StartScheduleDurationTimer(int ms);
    void StopScheduleDurationTimer();

    // Popup menus opened from the keyboard.
    void ShowEffectPresetsMenu();
    void ShowAudioDeviceMenu();

private:
    void BuildMenuBar();
    void BuildAccelerators();
    void RebuildRecentFilesMenu();
    void OnMenu(wxCommandEvent& event);
    void OnMenuOpen(wxMenuEvent& event);
    void OnIconize(wxIconizeEvent& event);
    void OnClose(wxCloseEvent& event);
    void OnHotkey(wxKeyEvent& event);
    void RunHotkey(int id);
    void OnBatchTimer(wxTimerEvent& event);
    void SeekBackOrForward(int direction);
#ifdef __WXMSW__
    // The keyboard shortcuts as a native accelerator table, so punctuation keys
    // are matched by their key position (VK_OEM_*) on every keyboard layout.
    bool MSWTranslateMessage(WXMSG* msg) override;
    void* m_nativeAccel = nullptr;
#endif

    wxMenu* m_recentMenu = nullptr;
    wxMenuItem* m_shuffleItem = nullptr;
    wxTimer m_titleTimer;
    wxTimer m_schedulerTimer;
    wxTimer m_batchTimer;
    wxTimer m_durationTimer;
    std::unique_ptr<TrayIcon> m_tray;
    bool m_hotkeysRegistered = false;
#ifdef __WXOSX__
    std::wstring m_nowPlayingTitle;  // for Control Center's Now Playing
#endif
};

#endif // FASTPLAY_MAIN_FRAME_H
