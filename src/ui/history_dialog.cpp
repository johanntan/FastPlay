// The Song History window: song titles captured from stream metadata, newest
// first, with copy to clipboard and clear.

#include "ui/dialogs.h"
#include "ui/ui_common.h"

#include "database.h"
#include "accessibility.h"

#include <ctime>
#include <cwchar>
#include <string>
#include <vector>

namespace {

std::wstring FormatHistoryTimestamp(int64_t ts) {
    time_t t = static_cast<time_t>(ts);
    struct tm local;
    localtime_s(&local, &t);
    wchar_t buf[64];
    wcsftime(buf, 64, L"%Y-%m-%d %H:%M:%S", &local);
    return buf;
}

void CopyHistoryEntryToClipboard(const std::wstring& title) {
    if (title.empty()) return;
    if (SetClipboardText(title)) {
        Speak("Song copied");
    }
}

class SongHistoryDialog : public wxDialog {
public:
    explicit SongHistoryDialog(wxWindow* parent)
        : wxDialog(parent, wxID_ANY, "Song History", wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER) {
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        sizer->Add(new wxStaticText(this, wxID_ANY, "Recent songs captured from stream metadata (Ctrl+C to copy):"),
                   0, wxLEFT | wxRIGHT | wxTOP, 10);
        m_list = new wxListBox(this, wxID_ANY, wxDefaultPosition, ConvertDialogToPixels(wxSize(386, 210)),
                               0, nullptr, wxLB_SINGLE | wxLB_HSCROLL);
        sizer->Add(m_list, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        auto* copy = new wxButton(this, wxID_ANY, "&Copy");
        buttons->Add(copy, 0, wxRIGHT, 6);
        auto* clear = new wxButton(this, wxID_ANY, "C&lear");
        buttons->Add(clear);
        buttons->AddStretchSpacer();
        auto* close = new wxButton(this, wxID_CANCEL, "Close");
        close->SetDefault();
        buttons->Add(close);
        sizer->Add(buttons, 0, wxEXPAND | wxALL, 10);

        SetSizerAndFit(sizer);
        CentreOnParent();

#ifdef __WXMSW__
        // Set tab stops on the listbox so title and timestamp line up
        wxVector<int> tabStops;
        tabStops.push_back(260);
        m_list->MSWSetTabStops(tabStops);
#endif

        copy->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            int sel = m_list->GetSelection();
            if (sel >= 0 && sel < static_cast<int>(m_dialogSongHistory.size())) {
                CopyHistoryEntryToClipboard(m_dialogSongHistory[sel].title);
            }
        });

        clear->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            if (wxMessageBox("Clear all song history?", "Song History",
                             wxYES_NO | wxICON_QUESTION, this) == wxYES) {
                ClearSongHistory();
                RefreshHistoryList();
                m_list->SetFocus();
            }
        });

        // Escape and Ctrl+C in the list. Handling Ctrl+C here swallows the key
        // before the list sees it.
        Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& event) {
            if (FindFocus() == m_list) {
                if (event.GetKeyCode() == WXK_ESCAPE) {
                    EndModal(wxID_CANCEL);
                    return;
                }
                // Ctrl+C to copy selected entry's title
                if (event.GetKeyCode() == 'C' && event.ControlDown()) {
                    int sel = m_list->GetSelection();
                    if (sel >= 0 && sel < static_cast<int>(m_dialogSongHistory.size())) {
                        CopyHistoryEntryToClipboard(m_dialogSongHistory[sel].title);
                    }
                    return;
                }
            }
            event.Skip();
        });

        // Ctrl+C also makes the control character 3 (ETX), which the listbox would
        // otherwise feed into its prefix-search and move the selection. Swallow it
        // so focus stays on the song the user just copied.
        m_list->Bind(wxEVT_CHAR, [](wxKeyEvent& event) {
            if (event.GetKeyCode() == 3) return;
            event.Skip();
        });

        RefreshHistoryList();
        m_list->SetFocus();
    }

private:
    void RefreshHistoryList() {
        m_dialogSongHistory = GetSongHistory();

        wxArrayString items;
        for (const auto& entry : m_dialogSongHistory) {
            std::wstring line = entry.title + L"\t" + FormatHistoryTimestamp(entry.timestamp);
            items.Add(WX(line));
        }
        m_list->Set(items);

        if (!m_dialogSongHistory.empty()) {
            m_list->SetSelection(0);
        }
    }

    wxListBox* m_list;
    std::vector<SongHistoryEntry> m_dialogSongHistory;
};

}  // namespace

void ShowSongHistoryDialog() {
    SongHistoryDialog dlg(GetMainWindow());
    dlg.ShowModal();
}
