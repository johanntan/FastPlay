// The YouTube window: search YouTube (or paste a video, playlist or channel URL) and
// play a result. It is modeless, so the main window stays usable while it is open.
// Searching and loading take seconds, so they run on worker threads; what they find
// is used only if nothing newer was asked for meanwhile.

#include "ui/dialogs.h"
#include "ui/ui_common.h"

#include "youtube.h"
#include "player.h"
#include "globals.h"
#include "accessibility.h"
#include "app_ui.h"

#include <atomic>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

class YouTubeDialog;

// The open YouTube window, if any.
YouTubeDialog* g_youTubeDialog = nullptr;

// The newest search and the newest video to play; older ones are dropped when done.
std::atomic<int> g_searchRequest{0};
std::atomic<int> g_playRequest{0};

// Progress from a worker thread, said on the UI thread.
void SpeakStatus(const std::wstring& message) {
    RunOnUiThread([message]() { SpeakW(message); });
}

// Get a video ready on a worker thread, then play it (whether or not the window is
// still open).
void StartPlaying(const std::wstring& videoId) {
    int request = ++g_playRequest;
    Speak("Loading");
    std::thread([videoId, request]() {
        YouTubeMedia media;
        std::wstring error;
        bool ok = YouTubePrepare(videoId, media, error, SpeakStatus);
        RunOnUiThread([ok, media, error, request]() {
            if (request != g_playRequest) return;  // another video was chosen meanwhile
            if (!ok) {
                Speak("Could not play the video");
                ShowMessage(L"Could not play the video.\n\n" + error, L"YouTube", MessageIcon::Error);
                return;
            }
            if (!media.file.empty()) {
                // Played like an opened file: it becomes the playlist.
                g_playlist.clear();
                g_playlist.push_back(media.file);
                PlayTrack(0);
            } else if (LoadURL(media.url.c_str())) {
                Speak("Playing");
            }
        });
    }).detach();
}

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
        m_loadMore->Enable(false);
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

    // A search, listing or "load more" finished (UI thread).
    void SearchDone(int request, bool ok, std::vector<YouTubeResult> results, const std::wstring& nextPageToken,
                    const std::wstring& error, bool append, const char* doneMessage) {
        if (request != g_searchRequest) return;
        size_t found = results.size();
        if (append) {
            m_results.insert(m_results.end(), results.begin(), results.end());
        } else {
            m_results = std::move(results);
        }
        m_nextPageToken = nextPageToken;
        UpdateResultsList();

        if (!ok && found == 0) {
            Speak("Search failed");
            if (!error.empty()) ShowMessage(L"The search failed.\n\n" + error, L"YouTube", MessageIcon::Error);
        } else if (found == 0) {
            Speak("No results");
        } else {
            Speak(std::to_string(found) + doneMessage);
        }
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

    // Run a search or listing on a worker thread; SearchDone gets the results.
    template <typename Work>
    void StartSearch(Work work, bool append, const char* doneMessage) {
        int request = ++g_searchRequest;
        std::thread([work, request, append, doneMessage]() {
            std::vector<YouTubeResult> results;
            std::wstring nextPageToken, error;
            bool ok = work(results, nextPageToken, error);
            RunOnUiThread([=]() {
                if (g_youTubeDialog) {
                    g_youTubeDialog->SearchDone(request, ok, results, nextPageToken, error, append, doneMessage);
                }
            });
        }).detach();
    }

    // Perform search
    void DoSearch() {
        std::wstring query = WS(m_search->GetValue());

        if (query.empty()) return;

        m_currentQuery = query;

        // A YouTube URL: list a playlist or channel, or play a video
        if (IsYouTubeURL(query)) {
            std::wstring id;
            bool isPlaylist, isChannel;
            if (ParseYouTubeURL(query, id, isPlaylist, isChannel)) {
                if (isPlaylist || isChannel) {
                    std::wstring listUrl = isPlaylist ? YouTubePlaylistUrl(id) : YouTubeChannelUrl(id);
                    Speak(isPlaylist ? "Loading playlist" : "Loading channel");
                    StartSearch(
                        [listUrl](std::vector<YouTubeResult>& results, std::wstring&, std::wstring& error) {
                            return YouTubeGetListContents(listUrl, results, error, SpeakStatus);
                        },
                        false, " videos");
                } else {
                    StartPlaying(id);
                }
                return;
            }
        }

        // Regular search
        Speak("Searching");
        StartSearch(
            [query](std::vector<YouTubeResult>& results, std::wstring& nextPageToken, std::wstring& error) {
                return YouTubeSearch(query, results, nextPageToken, L"", error, SpeakStatus);
            },
            false, " results");
    }

    // Load more results
    void DoLoadMore() {
        if (m_nextPageToken.empty()) return;

        Speak("Loading more");
        std::wstring query = m_currentQuery, pageToken = m_nextPageToken;
        StartSearch(
            [query, pageToken](std::vector<YouTubeResult>& results, std::wstring& nextPageToken, std::wstring& error) {
                return YouTubeSearch(query, results, nextPageToken, pageToken, error, SpeakStatus);
            },
            true, " more loaded");
    }

    // Play selected result
    void PlaySelected() {
        int sel = m_list->GetSelection();
        if (sel < 0 || sel >= static_cast<int>(m_results.size())) return;
        StartPlaying(m_results[sel].videoId);
    }

    wxTextCtrl* m_search;
    wxListBox* m_list;
    wxButton* m_loadMore;

    std::vector<YouTubeResult> m_results;
    std::wstring m_nextPageToken;
    std::wstring m_currentQuery;
};

}  // namespace

void ShowYouTubeDialog() {
    if (g_youTubeDialog && !g_youTubeDialog->IsBeingDeleted()) {
        // Already open, bring to front
        g_youTubeDialog->Raise();
        return;
    }

    // Owned by the main window, so it is destroyed with it. yt-dlp is fetched on
    // first use if there is none.
    auto* dialog = new YouTubeDialog(GetMainWindow());
    dialog->Show();
}
