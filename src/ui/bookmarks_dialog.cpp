// The Bookmarks window: the saved bookmarks for the current file or all files.
// Enter jumps to one, Delete removes it.

#include "ui/dialogs.h"
#include "ui/ui_common.h"

#include "globals.h"
#include "player.h"
#include "database.h"
#include "accessibility.h"

#include <cwchar>
#include <string>
#include <vector>

namespace {

// Jump to a bookmark (load file if needed and seek to position)
void JumpToBookmark(const Bookmark& bm) {
    // Check if the file is in the current playlist
    int trackIndex = -1;
    for (size_t i = 0; i < g_playlist.size(); i++) {
        if (_wcsicmp(g_playlist[i].c_str(), bm.filePath.c_str()) == 0) {
            trackIndex = static_cast<int>(i);
            break;
        }
    }

    if (trackIndex >= 0) {
        // File is in playlist, switch to it if not current
        if (trackIndex != g_currentTrack) {
            PlayTrack(trackIndex);
        }
    } else {
        // Load the file
        g_playlist.clear();
        g_playlist.push_back(bm.filePath);
        g_currentTrack = -1;
        PlayTrack(0);
    }

    // Seek to position (give a moment for file to load if needed)
    SeekToPosition(bm.position);
}

class BookmarksDialog : public wxDialog {
public:
    explicit BookmarksDialog(wxWindow* parent)
        : wxDialog(parent, wxID_ANY, "Bookmarks", wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER) {
        // Get current file path
        if (g_currentTrack >= 0 && g_currentTrack < static_cast<int>(g_playlist.size())) {
            m_currentFilePath = g_playlist[g_currentTrack];
        }

        // Load all bookmarks
        m_allBookmarks = GetAllBookmarks();

        auto* sizer = new wxBoxSizer(wxVERTICAL);

        auto* top = new wxBoxSizer(wxHORIZONTAL);
        top->Add(new wxStaticText(this, wxID_ANY, "&Show:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        m_filter = new wxChoice(this, wxID_ANY, wxDefaultPosition, ConvertDialogToPixels(wxSize(120, -1)));
        m_filter->Append("Current file");
        m_filter->Append("All bookmarks");
        top->Add(m_filter, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 15);
        top->Add(new wxStaticText(this, wxID_ANY, "Enter = jump, Delete = remove, Escape = close"),
                 0, wxALIGN_CENTER_VERTICAL);
        sizer->Add(top, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

        m_list = new wxListBox(this, wxID_ANY, wxDefaultPosition, ConvertDialogToPixels(wxSize(336, 198)),
                               0, nullptr, wxLB_SINGLE);
        sizer->Add(m_list, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

        sizer->Add(new wxButton(this, wxID_CANCEL, "Close"), 0, wxALIGN_RIGHT | wxALL, 10);

        SetSizerAndFit(sizer);
        CentreOnParent();

        // Default to current file if playing, otherwise all bookmarks
        m_filter->SetSelection(m_currentFilePath.empty() ? 1 : 0);

        m_filter->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
            RefreshBookmarkList();
            m_list->SetFocus();
        });

        // Double-click to jump to bookmark
        m_list->Bind(wxEVT_LISTBOX_DCLICK, [this](wxCommandEvent&) {
            int sel = m_list->GetSelection();
            if (sel >= 0 && sel < static_cast<int>(m_dialogBookmarks.size())) {
                JumpToBookmark(m_dialogBookmarks[sel]);
                EndModal(wxID_OK);
            }
        });

        // Enter, Delete and Escape in the bookmark list
        Bind(wxEVT_CHAR_HOOK, &BookmarksDialog::OnCharHook, this);

        // Populate listbox
        RefreshBookmarkList();

        m_list->SetFocus();
    }

private:
    // Refresh the bookmark listbox based on current filter
    void RefreshBookmarkList() {
        // Get filter selection (0 = current file, 1 = all)
        bool showAll = (m_filter->GetSelection() == 1);

        // Clear and repopulate
        m_dialogBookmarks.clear();
        wxArrayString items;
        for (const auto& bm : m_allBookmarks) {
            if (showAll || _wcsicmp(bm.filePath.c_str(), m_currentFilePath.c_str()) == 0) {
                m_dialogBookmarks.push_back(bm);
                items.Add(WX(bm.displayName));
            }
        }
        m_list->Set(items);

        // Select first item if any
        if (!m_dialogBookmarks.empty()) {
            m_list->SetSelection(0);
        }
    }

    void OnCharHook(wxKeyEvent& event) {
        if (FindFocus() != m_list) {
            event.Skip();
            return;
        }

        int key = event.GetKeyCode();
        if (key == WXK_RETURN || key == WXK_NUMPAD_ENTER) {
            // Enter - jump to bookmark
            int sel = m_list->GetSelection();
            if (sel >= 0 && sel < static_cast<int>(m_dialogBookmarks.size())) {
                JumpToBookmark(m_dialogBookmarks[sel]);
                EndModal(wxID_OK);
            }
            return;
        } else if (key == WXK_DELETE || key == WXK_NUMPAD_DELETE) {
            // Delete - remove bookmark
            int sel = m_list->GetSelection();
            if (sel >= 0 && sel < static_cast<int>(m_dialogBookmarks.size())) {
                int bookmarkId = m_dialogBookmarks[sel].id;
                RemoveBookmark(bookmarkId);

                // Remove from both lists
                m_dialogBookmarks.erase(m_dialogBookmarks.begin() + sel);
                for (auto it = m_allBookmarks.begin(); it != m_allBookmarks.end(); ++it) {
                    if (it->id == bookmarkId) {
                        m_allBookmarks.erase(it);
                        break;
                    }
                }

                m_list->Delete(sel);

                // Select next item
                int count = static_cast<int>(m_list->GetCount());
                if (count > 0) {
                    if (sel >= count) sel = count - 1;
                    m_list->SetSelection(sel);
                }

                Speak("Bookmark removed");
            }
            return;
        } else if (key == WXK_ESCAPE) {
            // Escape - close dialog
            EndModal(wxID_CANCEL);
            return;
        }
        event.Skip();
    }

    wxChoice* m_filter;
    wxListBox* m_list;

    std::vector<Bookmark> m_allBookmarks;       // All bookmarks from database
    std::vector<Bookmark> m_dialogBookmarks;    // Filtered bookmarks shown in list
    std::wstring m_currentFilePath;             // Current file path for filtering
};

}  // namespace

// Show bookmarks dialog
void ShowBookmarksDialog() {
    BookmarksDialog dlg(GetMainWindow());
    dlg.ShowModal();
}
