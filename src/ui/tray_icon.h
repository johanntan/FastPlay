#pragma once
#ifndef FASTPLAY_TRAY_ICON_H
#define FASTPLAY_TRAY_ICON_H

#include <wx/taskbar.h>

class MainFrame;

// The notification area icon, shown once the window is first hidden to the tray.
// Double-click restores the window; right-click opens a small playback menu.
class TrayIcon : public wxTaskBarIcon {
public:
    explicit TrayIcon(MainFrame* frame);

protected:
    wxMenu* CreatePopupMenu() override;

private:
    MainFrame* m_frame;
};

#endif // FASTPLAY_TRAY_ICON_H
