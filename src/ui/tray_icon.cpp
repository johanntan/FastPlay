#include "ui/tray_icon.h"
#include "ui/main_frame.h"
#include "ui/ui_common.h"

#include "commands.h"
#include "globals.h"

TrayIcon::TrayIcon(MainFrame* frame) : m_frame(frame) {
    SetIcon(GetAppIcon(), APP_NAME);

    Bind(wxEVT_TASKBAR_LEFT_DCLICK, [this](wxTaskBarIconEvent&) {
        m_frame->RestoreFromTray();
    });
    // Commands from the popup menu arrive here; run them as the main window would.
    Bind(wxEVT_MENU, [this](wxCommandEvent& event) {
        m_frame->RunCommand(event.GetId());
    });
}

wxMenu* TrayIcon::CreatePopupMenu() {
    auto* menu = new wxMenu();
    menu->Append(IDM_TRAY_RESTORE, "&Restore");
    menu->AppendSeparator();
    menu->Append(IDM_PLAY_PLAYPAUSE, "&Play/Pause");
    menu->Append(IDM_PLAY_STOP, "&Stop");
    menu->Append(IDM_PLAY_PREV, "P&revious");
    menu->Append(IDM_PLAY_NEXT, "&Next");
    menu->AppendSeparator();
    menu->Append(IDM_TRAY_EXIT, "E&xit");
    return menu;
}
