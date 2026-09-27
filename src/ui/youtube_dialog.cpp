// The YouTube window: search YouTube (or paste a video or playlist URL) and play
// a result. It is modeless, so the main window stays usable while it is open.

#include "ui/dialogs.h"
#include "ui/ui_common.h"

#include "youtube.h"
#include "player.h"
#include "accessibility.h"

#include <string>
#include <vector>

namespace {

class YouTubeDialog;

// The open YouTube window, if any.
YouTubeDialog* g_youTubeDialog = nullptr;

class YouTubeDialog : public wxDialog {
public:
    explicit YouTubeDialog(wxWindow* parent)
        : wxDialog(parent, wxID_ANY, "YouTube", wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER) {
        g_youTubeDialog = this;

        auto* sizer = new wxBoxSizer(wxVERTICAL);
        sizer->Add(new wxStaticText(this, wxID_ANY, "&Search or paste URL:"), 0, wxLEFT | wxRIGHT | wxTOP, 10);
        m_search = new wxTextCtrl(this, wxID_ANY, "");
        sizer->Add(m_search, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

        sizer->Add(new wxStaticText(this, wxID_ANY, "&Results:"), 0, wxLEFT | wxRIGHT | wxTOP, 10);
        m_list = new wxListBox(this, wxID_ANY, wxDefaultPosition, ConvertDialogToPixels(wxSize(336, 200)),
                               0, nullptr, wxLB_SINGLE);
        sizer->Add(m_list, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        m_loadMore = new wxButton(this, wxID_ANY, "&Load More");
        buttons->Add(m_loadMore);
        buttons->AddStretchSpacer();
        buttons->Add(new wxButton(this, wxID_CANCEL, "Close"));
        sizer->Add(buttons, 0, wxEXPAND | wxALL, 10);

        SetSizerAndFit(sizer);
        CentreOnParent();

        m_list->Bind(wxEVT_LISTBOX_DCLICK, [this](wxCommandEvent&) { PlaySelected(); });
        m_loadMore->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { DoLoadMore(); });

        // Close and Escape destroy the window rather than hiding it
        Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Destroy(); }, wxID_CANCEL);
        Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) { Destroy(); });

        // Enter searches from the search box and plays from the results list.
        // Anywhere else it does nothing (a focused button still presses itself).
        Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& event) {
            int key = event.GetKeyCode();
            if (key == WXK_RETURN || key == WXK_NUMPAD_ENTER) {
                wxWindow* focus = FindFocus();
                if (focus == m_search) {
                    DoSearch();
                    return;
                }
                if (focus == m_list) {
                    PlaySelected();
                    return;
                }
            }
            event.Skip();
        });

        m_search->SetFocus();
    }

    ~YouTubeDialog() override {
        if (g_youTubeDialog == this) g_youTubeDialog = nullptr;
    }

private:
    // Update results list in dialog
    void UpdateResultsList() {
        wxArrayString items;
        for (const auto& result : m_results) {
            std::wstring display = result.title;
            if (!result.channel.empty()) {
                display += L" - " + result.channel;
            }
            if (!result.duration.empty()) {
                display += L" [" + result.duration + L"]";
            }
            items.Add(WX(display));
        }
        m_list->Set(items);

        // Update load more button
        m_loadMore->Enable(!m_nextPageToken.empty());
    }

    // Perform search
    void DoSearch() {
        std::wstring query = WS(m_search->GetValue());

        if (query.empty()) return;

        m_currentQuery = query;
        m_results.clear();
        m_nextPageToken.clear();
        m_isPlaylistView = false;

        // Check if it's a YouTube URL
        if (IsYouTubeURL(query)) {
            std::wstring id;
            bool isPlaylist, isChannel;
            if (ParseYouTubeURL(query, id, isPlaylist, isChannel)) {
                if (isPlaylist) {
                    m_isPlaylistView = true;
                    m_currentPlaylistId = id;
                    YouTubeGetPlaylistContents(id, m_results, m_nextPageToken, L"");
                    UpdateResultsList();
                    Speak("Playlist loaded");
                    return;
                } else if (!isPlaylist && !isChannel) {
                    // Single video - try to play it directly
                    std::wstring streamUrl;
                    Speak("Loading video");
                    if (YouTubeGetStreamURL(id, streamUrl)) {
                        LoadURL(streamUrl.c_str());
                        Speak("Playing");
                    } else {
                        Speak("Failed to get stream URL");
                    }
                    return;
                }
            }
        }

        // Regular search
        Speak("Searching");
        if (YouTubeSearch(query, m_results, m_nextPageToken, L"")) {
            UpdateResultsList();
            Speak(std::to_string(m_results.size()) + " results");
        } else {
            Speak("No results or search failed");
        }
    }

    // Load more results
    void DoLoadMore() {
        if (m_nextPageToken.empty()) return;

        std::vector<YouTubeResult> moreResults;
        std::wstring newToken;

        Speak("Loading more");
        if (m_isPlaylistView) {
            YouTubeGetPlaylistContents(m_currentPlaylistId, moreResults, newToken, m_nextPageToken);
        } else {
            YouTubeSearch(m_currentQuery, moreResults, newToken, m_nextPageToken);
        }

        m_nextPageToken = newToken;
        for (const auto& r : moreResults) {
            m_results.push_back(r);
        }
        UpdateResultsList();

        Speak(std::to_string(moreResults.size()) + " more loaded");
    }

    // Play selected result
    void PlaySelected() {
        int sel = m_list->GetSelection();
        if (sel < 0 || sel >= static_cast<int>(m_results.size())) return;

        const YouTubeResult& result = m_results[sel];
        std::wstring streamUrl;

        Speak("Loading");
        if (YouTubeGetStreamURL(result.videoId, streamUrl)) {
            LoadURL(streamUrl.c_str());
            Speak("Playing");
        } else {
            Speak("Failed to get stream URL");
        }
    }

    wxTextCtrl* m_search;
    wxListBox* m_list;
    wxButton* m_loadMore;

    std::vector<YouTubeResult> m_results;
    std::wstring m_nextPageToken;
    std::wstring m_currentQuery;
    bool m_isPlaylistView = false;
    std::wstring m_currentPlaylistId;
};

}  // namespace

void ShowYouTubeDialog() {
    if (g_youTubeDialog && !g_youTubeDialog->IsBeingDeleted()) {
        // Already open, bring to front
        g_youTubeDialog->Raise();
        return;
    }

    // Check if yt-dlp is available
    if (!IsYtdlpAvailable()) {
        wxMessageBox("yt-dlp is not configured. Please set the yt-dlp path in Options > YouTube tab.",
                     "YouTube", wxOK | wxICON_WARNING, GetMainWindow());
        return;
    }

    // Owned by the main window, so it is destroyed with it
    auto* dialog = new YouTubeDialog(GetMainWindow());
    dialog->Show();
}
