// The Playlist Manager: the current playlist, with keys to reorder, remove, paste
// and play entries, and a button to save it as an M3U playlist.

#include "ui/dialogs.h"
#include "ui/ui_common.h"
#include "utils.h"

#include "globals.h"
#include "player.h"
#include "playlist_io.h"
#include "accessibility.h"
#include "app_ui.h"

#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/filedlg.h>
#include <wx/filefn.h>
#ifdef __WXMSW__
#include <wx/msw/wrapwin.h>
#endif
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace {

class PlaylistDialog;

// The open playlist window, for NotifyPlaylistTrackChanged.
PlaylistDialog* g_playlistDlg = nullptr;

// Add a clipboard path: a folder's audio files recursively, or the file itself if
// it is a supported audio file.
void AddPathFromClipboard(const std::wstring& path, std::vector<std::wstring>& files) {
    if (wxDirExists(WX(path))) {
        // Recursively add folder contents
        AddFilesFromFolder(path, files);
    } else if (wxFileExists(WX(path))) {
        // Check if it's a supported audio file
        size_t dotPos = path.rfind(L'.');
        if (dotPos != std::wstring::npos) {
            std::wstring ext = path.substr(dotPos);
            if (IsSupportedAudioExt(ext)) {
                files.push_back(path);
            }
        }
    }
}

// Get files from clipboard (supports files, folders, and text URLs/paths)
std::vector<std::wstring> GetFilesFromClipboard() {
    std::vector<std::wstring> files;

    if (!wxTheClipboard->Open()) return files;

    // First try file drop format
    if (wxTheClipboard->IsSupported(wxDF_FILENAME)) {
        wxFileDataObject data;
        if (wxTheClipboard->GetData(data)) {
            for (const wxString& name : data.GetFilenames()) {
                AddPathFromClipboard(WS(name), files);
            }
        }
    }

    // If no files from drop, try text format (URLs or file paths)
    if (files.empty() && wxTheClipboard->IsSupported(wxDF_UNICODETEXT)) {
        wxTextDataObject data;
        if (wxTheClipboard->GetData(data)) {
            std::wstring clipText = WS(data.GetText());

            // Split by newlines and process each line
            size_t start = 0;
            while (start < clipText.length()) {
                size_t end = clipText.find_first_of(L"\r\n", start);
                if (end == std::wstring::npos) end = clipText.length();

                std::wstring line = clipText.substr(start, end - start);
                // Trim whitespace
                while (!line.empty() && (line[0] == L' ' || line[0] == L'\t')) line.erase(0, 1);
                while (!line.empty() && (line.back() == L' ' || line.back() == L'\t')) line.pop_back();

                if (!line.empty()) {
                    // Check if it's a URL
                    if (line.find(L"http://") == 0 || line.find(L"https://") == 0 ||
                        line.find(L"mms://") == 0 || line.find(L"rtsp://") == 0) {
                        files.push_back(line);
                    } else {
                        // Check if it's a valid file/folder path
                        AddPathFromClipboard(line, files);
                    }
                }

                start = end + 1;
                // Skip consecutive newlines
                while (start < clipText.length() && (clipText[start] == L'\r' || clipText[start] == L'\n')) start++;
            }
        }
    }

    wxTheClipboard->Close();
    return files;
}

// The item with the focus rectangle, which is what the list's "current" item
// means for a multiple-selection list box (LB_GETCURSEL returns it).
int GetCurrentItem(wxListBox* list) {
#ifdef __WXMSW__
    return static_cast<int>(::SendMessageW(static_cast<HWND>(list->GetHWND()), LB_GETCURSEL, 0, 0));
#else
    wxArrayInt selections;
    list->GetSelections(selections);
    return selections.empty() ? -1 : selections[0];
#endif
}

// Make an item the current one: the only one selected, with the focus rectangle.
// (LB_SETCURSEL does nothing in a multiple-selection list box.)
void SetCurrentItem(wxListBox* list, int index) {
#ifdef __WXMSW__
    if (index < 0 || index >= static_cast<int>(list->GetCount())) return;
    HWND hwnd = static_cast<HWND>(list->GetHWND());
    ::SendMessageW(hwnd, LB_SETSEL, FALSE, -1);
    ::SendMessageW(hwnd, LB_SETSEL, TRUE, static_cast<LPARAM>(index));  // scrolls it into view
    ::SendMessageW(hwnd, LB_SETCARETINDEX, static_cast<WPARAM>(index), FALSE);
#else
    if (index >= 0 && index < static_cast<int>(list->GetCount())) {
        list->DeselectAll();
        list->SetSelection(index);
        list->EnsureVisible(index);
    }
#endif
}

// Select every item (LB_SETSEL with -1)
void SelectAllItems(wxListBox* list) {
#ifdef __WXMSW__
    ::SendMessageW(static_cast<HWND>(list->GetHWND()), LB_SETSEL, TRUE, -1);
#else
    for (unsigned int i = 0; i < list->GetCount(); i++) {
        list->SetSelection(static_cast<int>(i));
    }
#endif
}

// Selected indices, in ascending order
std::vector<int> GetSelectedIndices(wxListBox* list) {
    wxArrayInt selections;
    list->GetSelections(selections);
    return std::vector<int>(selections.begin(), selections.end());
}

class PlaylistDialog : public wxDialog {
public:
    explicit PlaylistDialog(wxWindow* parent)
        : wxDialog(parent, wxID_ANY, "Playlist Manager") {
        auto* sizer = new wxBoxSizer(wxVERTICAL);

        m_list = new wxListBox(this, wxID_ANY, wxDefaultPosition, ConvertDialogToPixels(wxSize(336, 205)),
                               0, nullptr, wxLB_EXTENDED);
        sizer->Add(m_list, 1, wxEXPAND | wxALL, 10);

        auto* row = new wxBoxSizer(wxHORIZONTAL);
        auto* save = new wxButton(this, wxID_ANY, "&Save...");
        row->Add(save, 0, wxRIGHT, 10);
        row->Add(new wxStaticText(this, wxID_ANY,
                     "Alt+Up/Down: Move  |  Delete: Remove  |  Enter: Play  |  Ctrl+V: Paste  |  Esc: Close"),
                 0, wxALIGN_CENTER_VERTICAL);
        sizer->Add(row, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);

        SetSizerAndFit(sizer);
        CentreOnParent();

        save->Bind(wxEVT_BUTTON, &PlaylistDialog::OnSave, this);
        m_list->Bind(wxEVT_LISTBOX_DCLICK, &PlaylistDialog::OnDoubleClick, this);
        Bind(wxEVT_CHAR_HOOK, &PlaylistDialog::OnCharHook, this);

        g_playlistDlg = this;

        RebuildPlaylistList(g_currentTrack);
        m_list->SetFocus();
    }

    ~PlaylistDialog() override {
        g_playlistDlg = nullptr;
    }

    // Update selection to follow current track
    void TrackChanged() {
        if (g_playlistFollowPlayback && g_currentTrack >= 0) {
            SetCurrentItem(m_list, g_currentTrack);
        }
    }

private:
    // Helper to rebuild playlist listbox
    void RebuildPlaylistList(int selectIndex = -1) {
        wxArrayString items;
        items.reserve(g_playlist.size());
        for (size_t i = 0; i < g_playlist.size(); i++) {
            std::wstring filename = GetTrackName(g_playlist[i]);
            items.push_back(wxString::Format("%d. %s", static_cast<int>(i + 1), WX(filename)));
        }
        m_list->Set(items);
        if (selectIndex >= 0 && selectIndex < static_cast<int>(g_playlist.size())) {
            SetCurrentItem(m_list, selectIndex);
        }
    }

    void OnCharHook(wxKeyEvent& event) {
        int key = event.GetKeyCode();

        // Escape closes the window from any control
        if (key == WXK_ESCAPE) {
            EndModal(wxID_CANCEL);
            return;
        }

        if (FindFocus() != m_list) {
            event.Skip();
            return;
        }

        // Alt without Ctrl makes these system keys (Alt+Up / Alt+Down); everything
        // else is an ordinary key press.
        bool sysKey = event.AltDown() && !event.ControlDown();
        bool handled = sysKey ? OnListSysKey(key) : OnListKey(key, event.ControlDown());
        if (!handled) {
            event.Skip();
        }
    }

    bool OnListKey(int key, bool ctrl) {
        int sel = GetCurrentItem(m_list);

        if (key == WXK_RETURN || key == WXK_NUMPAD_ENTER) {
            if (sel >= 0 && sel < static_cast<int>(g_playlist.size())) {
                PlayTrack(sel);
                EndModal(wxID_OK);
            }
            // The list takes Enter even when there is nothing to play
            return true;
        }

        if (key == WXK_DELETE || key == WXK_NUMPAD_DELETE) {
            std::vector<int> selected = GetSelectedIndices(m_list);
            if (!selected.empty()) {
                // Remove from end to preserve indices
                for (int i = static_cast<int>(selected.size()) - 1; i >= 0; i--) {
                    int idx = selected[i];
                    if (idx >= 0 && idx < static_cast<int>(g_playlist.size())) {
                        g_playlist.erase(g_playlist.begin() + idx);
                        if (g_currentTrack > idx) {
                            g_currentTrack--;
                        } else if (g_currentTrack == idx) {
                            g_currentTrack = -1;
                        }
                    }
                }
                int newSel = selected[0];
                if (newSel >= static_cast<int>(g_playlist.size())) newSel = static_cast<int>(g_playlist.size()) - 1;
                RebuildPlaylistList(newSel);
                Speak(std::to_string(selected.size()) + " removed");
                return true;
            }
            return false;
        }

        // Ctrl+A: Select all
        if (ctrl && key == 'A') {
            SelectAllItems(m_list);
            return true;
        }

        // Ctrl+V: Paste
        if (ctrl && key == 'V') {
            try {
                std::vector<std::wstring> newFiles = GetFilesFromClipboard();
                if (!newFiles.empty()) {
                    int insertPos = (sel >= 0 && sel < static_cast<int>(g_playlist.size())) ? sel + 1 : static_cast<int>(g_playlist.size());
                    for (size_t i = 0; i < newFiles.size(); i++) {
                        g_playlist.insert(g_playlist.begin() + insertPos + i, newFiles[i]);
                    }
                    if (g_currentTrack >= insertPos) {
                        g_currentTrack += static_cast<int>(newFiles.size());
                    }
                    RebuildPlaylistList(insertPos);
                    Speak(std::to_string(newFiles.size()) + " files pasted");
                }
            } catch (...) {
                // Silently ignore clipboard errors
            }
            return true;
        }

        return false;
    }

    bool OnListSysKey(int key) {
        std::vector<int> selected = GetSelectedIndices(m_list);
        if (selected.empty()) return false;
        // The playlist can change under an open window (a scheduled event, say).
        if (selected.back() >= static_cast<int>(g_playlist.size())) return false;

        // Alt+Up: Move selected items up
        if ((key == WXK_UP || key == WXK_NUMPAD_UP) && selected[0] > 0) {
            // Move items up one by one from the top
            for (int idx : selected) {
                std::swap(g_playlist[idx], g_playlist[idx - 1]);
                if (g_currentTrack == idx) g_currentTrack--;
                else if (g_currentTrack == idx - 1) g_currentTrack++;
            }
            // Rebuild and reselect
            RebuildPlaylistList(selected[0] - 1);
            // Reselect all moved items
            for (int idx : selected) {
                m_list->SetSelection(idx - 1);
            }
            return true;
        }

        // Alt+Down: Move selected items down
        int lastIdx = selected[selected.size() - 1];
        if ((key == WXK_DOWN || key == WXK_NUMPAD_DOWN) && lastIdx < static_cast<int>(g_playlist.size()) - 1) {
            // Move items down one by one from the bottom
            for (int i = static_cast<int>(selected.size()) - 1; i >= 0; i--) {
                int idx = selected[i];
                std::swap(g_playlist[idx], g_playlist[idx + 1]);
                if (g_currentTrack == idx) g_currentTrack++;
                else if (g_currentTrack == idx + 1) g_currentTrack--;
            }
            // Rebuild and reselect
            RebuildPlaylistList(selected[0] + 1);
            // Reselect all moved items
            for (int idx : selected) {
                m_list->SetSelection(idx + 1);
            }
            return true;
        }

        return false;
    }

    // Double-click plays the item
    void OnDoubleClick(wxCommandEvent&) {
        int sel = GetCurrentItem(m_list);
        if (sel >= 0 && sel < static_cast<int>(g_playlist.size())) {
            PlayTrack(sel);
            EndModal(wxID_OK);
        }
    }

    void OnSave(wxCommandEvent&) {
        if (g_playlist.empty()) {
            wxMessageBox("Playlist is empty.", "Save Playlist", wxOK | wxICON_INFORMATION, this);
            return;
        }

        wxString path = AskSavePath(this, "playlist.m3u",
                                    "M3U Playlist (*.m3u)|*.m3u|M3U8 Playlist (*.m3u8)|*.m3u8|All Files (*.*)|*.*",
                                    "m3u");
        if (path.empty()) return;

        std::wstring filePath = WS(path);
        // UTF-8 with a byte order mark, one entry per line
        FILE* f = FileOpen(filePath, "w");
        if (f) {
            fputs("\xEF\xBB\xBF#EXTM3U\n", f);
            for (const auto& path : g_playlist) {
                fputs((WideToUtf8(path) + "\n").c_str(), f);
            }
            fclose(f);
            Speak("Playlist saved");
        } else {
            wxMessageBox("Failed to save playlist.", "Error", wxOK | wxICON_ERROR, this);
        }
    }

    wxListBox* m_list;
};

}  // namespace

// Show playlist manager dialog
void ShowPlaylistDialog() {
    PlaylistDialog dlg(GetMainWindow());
    dlg.ShowModal();
}

// Notify playlist dialog about track change
void NotifyPlaylistTrackChanged() {
    if (g_playlistDlg) {
        g_playlistDlg->TrackChanged();
    }
}
