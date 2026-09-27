#include "ui/ui_common.h"
#include "ui/main_frame.h"

#include <wx/clipbrd.h>
#include <wx/filedlg.h>
#include <wx/filename.h>

wxWindow* GetMainWindow() {
    return GetMainFrame();
}

wxWindow* GetActiveOwner() {
    wxWindow* active = wxGetActiveWindow();
    if (active) {
        wxWindow* top = wxGetTopLevelParent(active);
        if (top && top->IsShown()) return top;
    }
    return GetMainWindow();
}

wxIcon GetAppIcon() {
#ifdef __WXMSW__
    // The stock Windows application icon, as the main window and tray icon always used.
    wxIcon icon;
    icon.CreateFromHICON(static_cast<WXHICON>(::LoadIcon(nullptr, IDI_APPLICATION)));
    return icon;
#else
    return wxArtProvider::GetIcon(wxART_EXECUTABLE_FILE, wxART_OTHER, wxSize(16, 16));
#endif
}

void SetDigitsOnly(wxTextCtrl* edit) {
#ifdef __WXMSW__
    HWND hwnd = static_cast<HWND>(edit->GetHWND());
    ::SetWindowLongPtrW(hwnd, GWL_STYLE, ::GetWindowLongPtrW(hwnd, GWL_STYLE) | ES_NUMBER);
#else
    edit->SetValidator(wxTextValidator(wxFILTER_DIGITS));
#endif
}

wxString AskSavePath(wxWindow* parent, const wxString& defaultName, const wxString& filter,
                     const wxString& defaultExt) {
    wxString name = defaultName;
    for (;;) {
        wxFileDialog dlg(parent, "Save As", "", name, filter, wxFD_SAVE);
        if (dlg.ShowModal() != wxID_OK) return wxString();

        wxFileName file(dlg.GetPath());
        if (!file.HasExt()) file.SetExt(defaultExt);
        if (!file.FileExists()) return file.GetFullPath();

        wxString question = file.GetFullName() + " already exists.\nDo you want to replace it?";
        if (wxMessageBox(question, "Confirm Save As", wxYES_NO | wxICON_WARNING, parent) == wxYES) {
            return file.GetFullPath();
        }
        name = file.GetFullName();
    }
}

bool SetClipboardText(const std::wstring& text) {
    wxClipboardLocker locker;
    if (!locker) return false;
    return wxTheClipboard->SetData(new wxTextDataObject(WX(text)));
}
