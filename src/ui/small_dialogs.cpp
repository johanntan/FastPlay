// The small dialogs: Open URL, Jump to Time, Save Effect Preset, and the tag viewer.

#include "ui/dialogs.h"
#include "ui/ui_common.h"

#include "globals.h"
#include "player.h"
#include "effects.h"
#include "accessibility.h"

#include <wx/clipbrd.h>
#include <cstdio>

namespace {

// A prompt, a single line edit box, and OK / Cancel.
class TextPromptDialog : public wxDialog {
public:
    TextPromptDialog(wxWindow* parent, const wxString& title, const wxString& prompt, int width)
        : wxDialog(parent, wxID_ANY, title) {
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        sizer->Add(new wxStaticText(this, wxID_ANY, prompt), 0, wxLEFT | wxRIGHT | wxTOP, 10);
        m_edit = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(width, -1));
        sizer->Add(m_edit, 0, wxEXPAND | wxALL, 10);

        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        auto* ok = new wxButton(this, wxID_OK, "OK");
        ok->SetDefault();
        buttons->Add(ok, 0, wxRIGHT, 6);
        buttons->Add(new wxButton(this, wxID_CANCEL, "Cancel"));
        sizer->Add(buttons, 0, wxALIGN_RIGHT | wxLEFT | wxRIGHT | wxBOTTOM, 10);

        SetSizerAndFit(sizer);
        CentreOnParent();
        m_edit->SetFocus();
    }

    // Prefill the edit box with its text selected.
    void SetValueSelected(const wxString& value) {
        m_edit->SetValue(value);
        m_edit->SelectAll();
    }

    wxString GetValue() const { return m_edit->GetValue(); }

private:
    wxTextCtrl* m_edit;
};

double ParseTimeString(const wchar_t* str) {
    int h = 0, m = 0, s = 0;

    // Try hh:mm:ss format
    if (swscanf(str, L"%d:%d:%d", &h, &m, &s) == 3) {
        return h * 3600.0 + m * 60.0 + s;
    }
    // Try mm:ss format
    if (swscanf(str, L"%d:%d", &m, &s) == 2) {
        return m * 60.0 + s;
    }
    // Try just seconds
    double secs = 0;
    if (swscanf(str, L"%lf", &secs) == 1) {
        return secs;
    }
    return -1;
}

// Format seconds to time string (mm:ss or hh:mm:ss)
wxString FormatTimeForEdit(double seconds) {
    if (seconds < 0) seconds = 0;
    int h = static_cast<int>(seconds) / 3600;
    int m = (static_cast<int>(seconds) % 3600) / 60;
    int s = static_cast<int>(seconds) % 60;
    if (h > 0) {
        return wxString::Format("%d:%02d:%02d", h, m, s);
    }
    return wxString::Format("%d:%02d", m, s);
}

}  // namespace

void ShowOpenURLDialog() {
    TextPromptDialog dlg(GetMainWindow(), "Open URL", "Enter stream &URL (http/https):", 430);

    // Prefill with a URL from the clipboard
    if (wxTheClipboard->Open()) {
        if (wxTheClipboard->IsSupported(wxDF_UNICODETEXT)) {
            wxTextDataObject data;
            wxTheClipboard->GetData(data);
            wxString clip = data.GetText();
            if (clip.Lower().StartsWith("http://") || clip.Lower().StartsWith("https://")) {
                dlg.SetValueSelected(clip);
            }
        }
        wxTheClipboard->Close();
    }

    if (dlg.ShowModal() == wxID_OK) {
        std::wstring url = WS(dlg.GetValue());
        if (!url.empty()) {
            // Add URL to playlist and play
            g_playlist.clear();
            g_playlist.push_back(url);
            g_currentTrack = -1;
            PlayTrack(0);
        }
    }
}

void ShowJumpToTimeDialog() {
    TextPromptDialog dlg(GetMainWindow(), "Jump to Time", "Enter &time (mm:ss or hh:mm:ss):", 280);
    // Prefill with current position
    dlg.SetValueSelected(FormatTimeForEdit(GetCurrentPosition()));

    if (dlg.ShowModal() == wxID_OK) {
        double seconds = ParseTimeString(WS(dlg.GetValue()).c_str());
        if (seconds >= 0) {
            SeekToPosition(seconds);
        }
    }
}

void ShowSaveEffectPresetDialog() {
    TextPromptDialog dlg(GetMainWindow(), "Save Effect Preset", "Preset &name:", 310);
    if (dlg.ShowModal() != wxID_OK) return;

    // Trim whitespace and reject characters that would break INI section names
    std::wstring name = WS(dlg.GetValue());
    while (!name.empty() && (name.front() == L' ' || name.front() == L'\t')) name.erase(name.begin());
    while (!name.empty() && (name.back() == L' ' || name.back() == L'\t')) name.pop_back();
    for (auto& c : name) {
        if (c == L'[' || c == L']' || c == L'=' || c == L'\r' || c == L'\n') c = L'_';
    }
    if (!name.empty() && SaveEffectPreset(name)) {
        SpeakW(L"Saved preset " + name);
    }
}

void ShowTagDialog(const wchar_t* title, const std::wstring& text) {
    // Owned by the active window, so focus returns to it (it can be opened from another dialog).
    wxDialog dlg(GetActiveOwner(), wxID_ANY, title);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    auto* edit = new wxTextCtrl(&dlg, wxID_ANY, WX(text), wxDefaultPosition, wxSize(430, 60),
                                wxTE_MULTILINE | wxTE_READONLY);
    sizer->Add(edit, 1, wxEXPAND | wxALL, 10);
    dlg.SetSizerAndFit(sizer);
    dlg.CentreOnParent();

    // No buttons: Enter or Escape closes it. The text is selected for easy copying.
    dlg.Bind(wxEVT_CHAR_HOOK, [&dlg](wxKeyEvent& event) {
        int key = event.GetKeyCode();
        if (key == WXK_ESCAPE || key == WXK_RETURN || key == WXK_NUMPAD_ENTER) {
            dlg.EndModal(wxID_CANCEL);
            return;
        }
        event.Skip();
    });
    edit->SelectAll();
    edit->SetFocus();
    dlg.ShowModal();
}
