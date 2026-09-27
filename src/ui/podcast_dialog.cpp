// The Podcasts window: subscriptions and their episodes (with download), the
// iTunes search, OPML import and export, and the Add Podcast dialog.

#include "ui/dialogs.h"
#include "ui/ui_common.h"

#include "podcast.h"
#include "database.h"
#include "globals.h"
#include "player.h"
#include "accessibility.h"

#include <wx/filedlg.h>
#include <wx/notebook.h>
#ifdef __WXMSW__
#include <wx/msw/wrapwin.h>
#endif
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

class PodcastDialog;

// State shared between the Podcasts window and its background fetches. `dialog`
// is only touched on the UI thread.
// The outcome of fetching a feed.
struct FeedResult {
    bool ok = false;
    std::wstring title;
    std::vector<PodcastEpisode> episodes;
    PodcastFetchDiag diag;
};

// The episode list is a native extended-selection list box. wxListBox has no
// notion of its focused item (the caret) apart from the selection, so these reach
// the native control for it.

// The item with the focus rectangle (LB_GETCARETINDEX)
int GetListCaret(wxListBox* list) {
#ifdef __WXMSW__
    return static_cast<int>(::SendMessageW(static_cast<HWND>(list->GetHWND()), LB_GETCARETINDEX, 0, 0));
#else
    wxArrayInt selections;
    list->GetSelections(selections);
    return selections.empty() ? 0 : selections[0];
#endif
}

// Move just the focus rectangle (LB_SETCARETINDEX), leaving the selection alone.
void SetListCaret(wxListBox* list, int index) {
#ifdef __WXMSW__
    ::SendMessageW(static_cast<HWND>(list->GetHWND()), LB_SETCARETINDEX, static_cast<WPARAM>(index), FALSE);
#else
    (void)list;
    (void)index;
#endif
}

// Fire MSAA focus event so screen readers announce the item.
void NotifyItemFocused(wxListBox* list, int index) {
#ifdef __WXMSW__
    ::NotifyWinEvent(EVENT_OBJECT_FOCUS, static_cast<HWND>(list->GetHWND()), OBJID_CLIENT, index + 1);
#else
    (void)list;
    (void)index;
#endif
}

// Fire MSAA selection state change so the screen reader announces it.
void NotifyItemSelectionChanged(wxListBox* list, int index) {
#ifdef __WXMSW__
    HWND hwnd = static_cast<HWND>(list->GetHWND());
    ::NotifyWinEvent(EVENT_OBJECT_SELECTIONADD, hwnd, OBJID_CLIENT, index + 1);
    ::NotifyWinEvent(EVENT_OBJECT_STATECHANGE, hwnd, OBJID_CLIENT, index + 1);
#else
    (void)list;
    (void)index;
#endif
}

bool IsUpKey(int key) { return key == WXK_UP || key == WXK_NUMPAD_UP; }
bool IsDownKey(int key) { return key == WXK_DOWN || key == WXK_NUMPAD_DOWN; }

// Add podcast dialog: feed URL plus optional username/password for protected feeds
class PodcastAddDialog : public wxDialog {
public:
    explicit PodcastAddDialog(wxWindow* parent)
        : wxDialog(parent, wxID_ANY, "Add Podcast") {
        auto* urlLabel = new wxStaticText(this, wxID_ANY, "Feed &URL:");
        m_url = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, ConvertDialogToPixels(wxSize(228, -1)));
        auto* urlHint = new wxStaticText(this, wxID_ANY, "(RSS feed URL, e.g., https://example.com/feed.xml)");
        auto* userLabel = new wxStaticText(this, wxID_ANY, "&Username:");
        m_username = new wxTextCtrl(this, wxID_ANY);
        auto* passLabel = new wxStaticText(this, wxID_ANY, "&Password:");
        m_password = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_PASSWORD);
        auto* authHint = new wxStaticText(this, wxID_ANY, "(Leave blank if feed doesn't require authentication)");
        auto* ok = new wxButton(this, wxID_OK, "OK");
        auto* cancel = new wxButton(this, wxID_CANCEL, "Cancel");
        ok->SetDefault();

        auto* grid = new wxFlexGridSizer(2, 5, 6);
        grid->AddGrowableCol(1);
        grid->Add(urlLabel, 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(m_url, 1, wxEXPAND);
        grid->AddSpacer(0);
        grid->Add(urlHint);
        grid->Add(userLabel, 0, wxALIGN_CENTER_VERTICAL);
        auto* authRow = new wxBoxSizer(wxHORIZONTAL);
        authRow->Add(m_username, 1, wxEXPAND | wxRIGHT, 6);
        authRow->Add(passLabel, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
        authRow->Add(m_password, 1, wxEXPAND);
        grid->Add(authRow, 1, wxEXPAND);
        grid->AddSpacer(0);
        grid->Add(authHint);

        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        buttons->Add(ok, 0, wxRIGHT, 6);
        buttons->Add(cancel);

        auto* sizer = new wxBoxSizer(wxVERTICAL);
        sizer->Add(grid, 1, wxEXPAND | wxALL, 10);
        sizer->Add(buttons, 0, wxALIGN_RIGHT | wxLEFT | wxRIGHT | wxBOTTOM, 10);
        SetSizerAndFit(sizer);
        CentreOnParent();

        Bind(wxEVT_BUTTON, &PodcastAddDialog::OnOK, this, wxID_OK);
        m_url->SetFocus();
    }

    std::wstring url;
    std::wstring username;
    std::wstring password;

private:
    void OnOK(wxCommandEvent&) {
        // Trim whitespace from url
        std::wstring u = WS(m_url->GetValue());
        while (!u.empty() && (u.front() == L' ' || u.front() == L'\t')) u.erase(0, 1);
        while (!u.empty() && (u.back() == L' ' || u.back() == L'\t')) u.pop_back();

        if (u.empty()) {
            wxMessageBox("Please enter a feed URL.", "Add Podcast", wxOK | wxICON_WARNING, this);
            return;
        }

        url = u;
        username = WS(m_username->GetValue());
        password = WS(m_password->GetValue());
        EndModal(wxID_OK);
    }

    wxTextCtrl* m_url;
    wxTextCtrl* m_username;
    wxTextCtrl* m_password;
};

class PodcastDialog : public wxDialog {
public:
    explicit PodcastDialog(wxWindow* parent)
        : wxDialog(parent, wxID_ANY, "Podcasts", wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER) {

        m_tabs = new wxNotebook(this, wxID_ANY);

        // Subscriptions tab
        auto* subsPage = new wxPanel(m_tabs);
        auto* subsLabel = new wxStaticText(subsPage, wxID_ANY, "&Subscriptions:");
        m_subsList = new wxListBox(subsPage, wxID_ANY, wxDefaultPosition, wxDefaultSize, 0, nullptr, wxLB_SINGLE);
        auto* epLabel = new wxStaticText(subsPage, wxID_ANY, "&Episodes:");
        m_episodesList = new wxListBox(subsPage, wxID_ANY, wxDefaultPosition, wxDefaultSize, 0, nullptr, wxLB_EXTENDED);
        m_desc = new wxTextCtrl(subsPage, wxID_ANY, "", wxDefaultPosition, wxDefaultSize,
                                wxTE_MULTILINE | wxTE_READONLY);
        auto* subsHelp = new wxStaticText(subsPage, wxID_ANY,
                                          "Enter = load/play, Delete = unsubscribe, F5 = refresh, Escape = close");
        auto* download = new wxButton(subsPage, wxID_ANY, "&Download");
        auto* downloadAll = new wxButton(subsPage, wxID_ANY, "Download &All");
        auto* exportOpml = new wxButton(subsPage, wxID_ANY, "&Export OPML...");
        auto* refresh = new wxButton(subsPage, wxID_ANY, "&Refresh");

        // Subscriptions on the left third; episodes above their description on the right
        auto* subsColumn = new wxBoxSizer(wxVERTICAL);
        subsColumn->Add(subsLabel, 0, wxBOTTOM, 2);
        subsColumn->Add(m_subsList, 1, wxEXPAND);
        auto* epColumn = new wxBoxSizer(wxVERTICAL);
        epColumn->Add(epLabel, 0, wxBOTTOM, 2);
        epColumn->Add(m_episodesList, 55, wxEXPAND | wxBOTTOM, 4);
        epColumn->Add(m_desc, 45, wxEXPAND);
        auto* lists = new wxBoxSizer(wxHORIZONTAL);
        lists->Add(subsColumn, 1, wxEXPAND | wxRIGHT, 8);
        lists->Add(epColumn, 2, wxEXPAND);
        auto* subsBottom = new wxBoxSizer(wxHORIZONTAL);
        subsBottom->Add(subsHelp, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        subsBottom->Add(download, 0, wxRIGHT, 4);
        subsBottom->Add(downloadAll, 0, wxRIGHT, 4);
        subsBottom->Add(exportOpml, 0, wxRIGHT, 4);
        subsBottom->Add(refresh);
        auto* subsSizer = new wxBoxSizer(wxVERTICAL);
        subsSizer->Add(lists, 1, wxEXPAND | wxALL, 7);
        subsSizer->Add(subsBottom, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 7);
        subsPage->SetSizer(subsSizer);
        m_tabs->AddPage(subsPage, "Subscriptions");

        // Search tab
        auto* searchPage = new wxPanel(m_tabs);
        auto* searchLabel = new wxStaticText(searchPage, wxID_ANY, "&Search iTunes:");
        m_searchEdit = new wxTextCtrl(searchPage, wxID_ANY);
        auto* searchBtn = new wxButton(searchPage, wxID_ANY, "&Search");
        m_searchList = new wxListBox(searchPage, wxID_ANY, wxDefaultPosition, wxDefaultSize, 0, nullptr, wxLB_SINGLE);
        auto* searchHelp = new wxStaticText(searchPage, wxID_ANY, "Enter = preview, Escape = close");
        auto* subscribe = new wxButton(searchPage, wxID_ANY, "S&ubscribe");
        auto* importOpml = new wxButton(searchPage, wxID_ANY, "&Import OPML...");
        auto* addUrl = new wxButton(searchPage, wxID_ANY, "&Add URL...");

        auto* searchRow = new wxBoxSizer(wxHORIZONTAL);
        searchRow->Add(searchLabel, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
        searchRow->Add(m_searchEdit, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        searchRow->Add(searchBtn, 0, wxALIGN_CENTER_VERTICAL);
        auto* searchBottom = new wxBoxSizer(wxHORIZONTAL);
        searchBottom->Add(searchHelp, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        searchBottom->Add(subscribe, 0, wxRIGHT, 4);
        searchBottom->Add(importOpml, 0, wxRIGHT, 4);
        searchBottom->Add(addUrl);
        auto* searchSizer = new wxBoxSizer(wxVERTICAL);
        searchSizer->Add(searchRow, 0, wxEXPAND | wxALL, 7);
        searchSizer->Add(m_searchList, 1, wxEXPAND | wxLEFT | wxRIGHT, 7);
        searchSizer->Add(searchBottom, 0, wxEXPAND | wxALL, 7);
        searchPage->SetSizer(searchSizer);
        m_tabs->AddPage(searchPage, "Search");

        auto* close = new wxButton(this, wxID_CANCEL, "Close");

        auto* sizer = new wxBoxSizer(wxVERTICAL);
        sizer->Add(m_tabs, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 7);
        sizer->Add(close, 0, wxALIGN_RIGHT | wxALL, 7);
        SetSizer(sizer);
        SetMinSize(wxSize(400, 250));
        SetClientSize(ConvertDialogToPixels(wxSize(450, 300)));
        CentreOnParent();

        Bind(wxEVT_CHAR_HOOK, &PodcastDialog::OnCharHook, this);

        download->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { DownloadSelected(); });
        downloadAll->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { DownloadAll(); });
        exportOpml->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ExportOpml(); });
        refresh->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { RefreshEpisodes(); });
        searchBtn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Search(); });
        subscribe->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Subscribe(); });
        importOpml->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ImportOpml(); });
        addUrl->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { AddUrl(); });

        // Subscriptions list: Delete unsubscribes, Ctrl+Up/Down reorders, double-click loads
        m_subsList->Bind(wxEVT_KEY_DOWN, &PodcastDialog::OnSubsKeyDown, this);
        m_subsList->Bind(wxEVT_LISTBOX_DCLICK, [this](wxCommandEvent&) { LoadSelectedSubscription(); });

        // Episodes list: Ctrl+Up/Down and Ctrl+Space, plus deferred description
        // updates whenever a key or click moves the caret.
        m_episodesList->Bind(wxEVT_KEY_DOWN, &PodcastDialog::OnEpisodesKeyDown, this);
        m_episodesList->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& event) {
            event.Skip();
            CallAfter(&PodcastDialog::CheckEpisodeCaret);
        });
        m_episodesList->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& event) {
            event.Skip();
            CallAfter(&PodcastDialog::CheckEpisodeCaret);
        });
        m_episodesList->Bind(wxEVT_LISTBOX_DCLICK, [this](wxCommandEvent&) { PlayFocusedEpisode(); });

        // Search results: double-click subscribes
        m_searchList->Bind(wxEVT_LISTBOX_DCLICK, [this](wxCommandEvent&) { Subscribe(); });

        // Description box: no text selected when it gets the focus
        m_desc->Bind(wxEVT_SET_FOCUS, [this](wxFocusEvent& event) {
            event.Skip();
            m_desc->SetSelection(0, 0);
        });

        // Load subscriptions
        RefreshSubsList();
        m_subsList->SetFocus();
    }

    // A feed fetched for the episode list has arrived.
    void OnEpisodesLoaded(const std::wstring& feedUrl, const FeedResult& result, bool focusEpisodes) {
        m_episodes = result.episodes;
        if (result.ok) {
            wxArrayString items;
            for (const auto& ep : m_episodes) {
                items.Add(WX(FormatPodcastEpisodeDisplay(ep)));
            }
            m_episodesList->Append(items);

            // Select first episode (multi-select listbox needs the selection and the caret set)
            if (!m_episodes.empty()) {
                m_episodesList->SetSelection(wxNOT_FOUND);
                m_episodesList->SetSelection(0, true);
                SetListCaret(m_episodesList, 0);
                // Force a description update for the newly selected first episode.
                m_lastDescCaret = -1;
                CallAfter([this]() { UpdateDescription(0); });
            }

            char buf[64];
            snprintf(buf, sizeof(buf), "%d episodes", static_cast<int>(m_episodes.size()));
            Speak(buf);
        } else {
            Speak("Failed to load episodes");
            ShowTagDialog(L"Podcast Load Failed", BuildPodcastDiagMessage(feedUrl, result.diag));
        }

        if (focusEpisodes) m_episodesList->SetFocus();
    }

    void OnSearchDone(const std::vector<PodcastSearchResult>& results, bool ok) {
        m_searchResults = results;
        if (ok) {
            wxArrayString items;
            for (const auto& r : m_searchResults) {
                std::wstring display = r.name;
                if (!r.artistName.empty()) {
                    display += L" - " + r.artistName;
                }
                items.Add(WX(display));
            }
            m_searchList->Append(items);
            m_searchList->SetSelection(0);
            m_searchList->SetFocus();
            char buf[64];
            snprintf(buf, sizeof(buf), "%d results", static_cast<int>(m_searchResults.size()));
            Speak(buf);
        } else {
            Speak("No results");
        }
    }

    void OnFeedAdded(const std::wstring& url, const std::wstring& username, const std::wstring& password,
                     const FeedResult& result) {
        if (result.ok) {
            std::wstring title = result.title;
            if (title.empty()) title = L"Unknown Podcast";
            if (AddPodcastSubscription(title, url, L"", username, password) > 0) {
                RefreshSubsList();
                Speak("Podcast added");
            } else if (UpdatePodcastAuth(url, username, password)) {
                // Already subscribed - treat re-adding as updating the stored credentials
                RefreshSubsList();
                Speak("Already subscribed, credentials updated");
            } else {
                Speak("Already subscribed or failed");
            }
        } else {
            Speak("Failed to fetch feed");
            ShowTagDialog(L"Podcast Fetch Failed", BuildPodcastDiagMessage(url, result.diag));
        }
    }

    void OnFeedsImported(const std::vector<OpmlFeed>& feeds) {
        int added = 0;
        int skipped = 0;
        for (const auto& feed : feeds) {
            if (AddPodcastSubscription(feed.title, feed.feedUrl) > 0) {
                added++;
            } else {
                skipped++;
            }
        }

        RefreshSubsList();

        // Report results
        char msg[128];
        if (skipped > 0) {
            snprintf(msg, sizeof(msg), "Imported %d feeds, %d skipped", added, skipped);
        } else {
            snprintf(msg, sizeof(msg), "Imported %d feeds", added);
        }
        Speak(msg);
    }

private:
    // Refresh podcast subscriptions list
    void RefreshSubsList() {
        m_subsList->Clear();
        m_subs = GetPodcastSubscriptions();
        wxArrayString items;
        for (const auto& sub : m_subs) {
            items.Add(WX(sub.name));
        }
        m_subsList->Append(items);
    }

    // The subscription whose episodes are loaded, or null
    const PodcastSubscription* FindCurrentSubscription() const {
        if (m_currentPodcastId >= 0) {
            for (const auto& sub : m_subs) {
                if (sub.id == m_currentPodcastId) return &sub;
            }
        }
        return nullptr;
    }

    // Enter goes by the focused control, as the dialog's default action did.
    // Buttons keep Enter for themselves.
    void OnCharHook(wxKeyEvent& event) {
        int key = event.GetKeyCode();
        if ((key == WXK_RETURN || key == WXK_NUMPAD_ENTER) && !event.AltDown()) {
            wxWindow* focus = FindFocus();
            if (!wxDynamicCast(focus, wxButton)) {
                if (focus == m_subsList) {
                    // Load episodes for selected subscription
                    LoadSelectedSubscription();
                } else if (focus == m_episodesList) {
                    // Play selected episode(s) - multiselect adds all to the playlist
                    PlaySelectedEpisodes();
                } else if (focus == m_searchEdit) {
                    // Trigger search
                    Search();
                } else if (focus == m_searchList) {
                    // Preview/play first episode of selected podcast
                    PreviewSelectedResult();
                }
                return;
            }
        }
        event.Skip();
    }

    void OnSubsKeyDown(wxKeyEvent& event) {
        int key = event.GetKeyCode();
        if ((key == WXK_DELETE || key == WXK_NUMPAD_DELETE) && !event.AltDown()) {
            int sel = m_subsList->GetSelection();
            if (sel >= 0 && sel < static_cast<int>(m_subs.size())) {
                // Confirm deletion
                std::wstring msg = L"Unsubscribe from \"" + m_subs[sel].name + L"\"?";
                int id = m_subs[sel].id;
                if (wxMessageBox(WX(msg), "Unsubscribe", wxYES_NO | wxICON_QUESTION, this) == wxYES) {
                    RemovePodcastSubscription(id);
                    RefreshSubsList();
                    Speak("Unsubscribed");
                }
            }
            return;
        }
        if ((IsUpKey(key) || IsDownKey(key)) && event.ControlDown()) {
            // Reorder podcast with Ctrl+Up/Down
            int sel = m_subsList->GetSelection();
            int count = static_cast<int>(m_subs.size());
            int newSel = IsUpKey(key) ? sel - 1 : sel + 1;
            if (sel >= 0 && sel < count && newSel >= 0 && newSel < count) {
                std::swap(m_subs[sel], m_subs[newSel]);
                UpdatePodcastSortOrders(m_subs);
                RefreshSubsList();
                m_subsList->SetSelection(newSel);
                // Speak position feedback
                const std::wstring& name = m_subs[newSel].name;
                if (newSel == 0)
                    SpeakW(name + L" moved to top");
                else
                    SpeakW(name + L" moved below " + m_subs[newSel - 1].name);
            }
            return;
        }
        event.Skip();
    }

    // Ctrl+Up / Ctrl+Down move the caret without changing the selection, and
    // Ctrl+Space toggles the selection of the focused item, since not all
    // environments honor the list box's built-in extended-selection keyboard
    // interface for caret-only moves. All other keys fall through unchanged so
    // plain and Shift+arrow extension keep their stock behavior.
    void OnEpisodesKeyDown(wxKeyEvent& event) {
        int key = event.GetKeyCode();
        bool ctrl = event.ControlDown();
        bool shift = event.ShiftDown();
        int count = static_cast<int>(m_episodesList->GetCount());
        if (ctrl && !shift && (IsUpKey(key) || IsDownKey(key)) && count > 0) {
            int caret = GetListCaret(m_episodesList);
            int newCaret = caret + (IsDownKey(key) ? 1 : -1);
            if (newCaret < 0) newCaret = 0;
            if (newCaret >= count) newCaret = count - 1;
            if (newCaret != caret) {
                // Move just the focus rectangle; preserve selection state of all items.
                SetListCaret(m_episodesList, newCaret);
                NotifyItemFocused(m_episodesList, newCaret);
                CheckEpisodeCaret();
            }
            return;
        }
        if (ctrl && !shift && key == WXK_SPACE && count > 0) {
            int caret = GetListCaret(m_episodesList);
            if (caret >= 0 && caret < count) {
                bool selected = m_episodesList->IsSelected(caret);
                m_episodesList->SetSelection(caret, !selected);
                NotifyItemSelectionChanged(m_episodesList, caret);
            }
            return;
        }
        // Let the list box handle the key, then see where its caret went.
        event.Skip();
        CallAfter(&PodcastDialog::CheckEpisodeCaret);
    }

    // Post a deferred description update if the caret moved. Deferring lets the
    // list box finish its own selection bookkeeping and accessibility events
    // before the description box is rewritten, and tracking the last caret keeps
    // Ctrl+Space toggles and other events that don't move it from rewriting it.
    void CheckEpisodeCaret() {
        int caret = GetListCaret(m_episodesList);
        if (caret != m_lastDescCaret) {
            m_lastDescCaret = caret;
            CallAfter([this, caret]() { UpdateDescription(caret); });
        }
    }

    void UpdateDescription(int sel) {
        if (sel >= 0 && sel < static_cast<int>(m_episodes.size())) {
            m_desc->ChangeValue(WX(CleanPodcastDescription(m_episodes[sel].description)));
        } else {
            m_desc->ChangeValue("");
        }
        // Deselect any text after a text change, and show the top
        m_desc->SetSelection(0, 0);
        m_desc->ShowPosition(0);
    }

    void LoadSelectedSubscription() {
        int sel = m_subsList->GetSelection();
        if (sel >= 0 && sel < static_cast<int>(m_subs.size())) {
            m_currentPodcastId = m_subs[sel].id;
            LoadEpisodes(m_subs[sel].feedUrl, m_subs[sel].username, m_subs[sel].password, true);
        }
    }

    void RefreshEpisodes() {
        int sel = m_subsList->GetSelection();
        if (sel >= 0 && sel < static_cast<int>(m_subs.size())) {
            LoadEpisodes(m_subs[sel].feedUrl, m_subs[sel].username, m_subs[sel].password, false);
            UpdatePodcastLastUpdated(m_subs[sel].id);
        }
    }

    // Load episodes for a subscription, using its stored credentials for
    // password-protected feeds.
    void LoadEpisodes(const std::wstring& feedUrl, const std::wstring& username, const std::wstring& password,
                      bool focusEpisodes) {
        m_episodesList->Clear();
        m_episodes.clear();
        UpdateDescription(-1);

        Speak("Loading episodes");

        FeedResult result;
        result.ok = ParsePodcastFeed(feedUrl, result.title, result.episodes, username, password, &result.diag);
        OnEpisodesLoaded(feedUrl, result, focusEpisodes);
    }

    void PlaySelectedEpisodes() {
        wxArrayInt selections;
        if (m_episodesList->GetSelections(selections) <= 0) return;

        g_playlist.clear();
        for (int idx : selections) {
            if (idx >= 0 && idx < static_cast<int>(m_episodes.size())) {
                g_playlist.push_back(m_episodes[idx].audioUrl);
            }
        }
        if (!g_playlist.empty()) {
            PlayTrack(0, true);
            if (g_playlist.size() == 1) {
                Speak("Playing");
            } else {
                char msg[64];
                snprintf(msg, sizeof(msg), "Playing %d episodes", static_cast<int>(g_playlist.size()));
                Speak(msg);
            }
        }
    }

    // Double-click plays only the item that was clicked (the focused one)
    void PlayFocusedEpisode() {
        int sel = GetListCaret(m_episodesList);
        if (sel >= 0 && sel < static_cast<int>(m_episodes.size())) {
            g_playlist.clear();
            g_playlist.push_back(m_episodes[sel].audioUrl);
            PlayTrack(0, true);
            Speak("Playing");
        }
    }

    // Download all currently selected episodes
    void DownloadSelected() {
        // Episodes from a password-protected feed need the feed's credentials
        const PodcastSubscription* feed = FindCurrentSubscription();
        std::wstring authHeader = feed ? BuildBasicAuthHeader(feed->username, feed->password) : L"";

        wxArrayInt selections;
        if (m_episodesList->GetSelections(selections) <= 0) {
            Speak("No episode selected");
            return;
        }

        if (g_downloadPath.empty()) {
            Speak("Please set a downloads folder in Options");
            return;
        }

        std::vector<const PodcastEpisode*> episodes;
        for (int idx : selections) {
            if (idx < 0 || idx >= static_cast<int>(m_episodes.size())) continue;
            episodes.push_back(&m_episodes[idx]);
        }
        PodcastDownloadBatch batch = PreparePodcastDownloads(episodes, feed);

        if (batch.items.empty()) {
            if (batch.skipped > 0) Speak("All selected episodes already downloaded");
            else Speak("No episodes to download");
            return;
        }

        if (batch.items.size() == 1) {
            QueuePodcastDownloads(batch, authHeader, false);
            Speak("Downloading");
        } else {
            QueuePodcastDownloads(batch, authHeader, true);
            char msg[128];
            if (batch.skipped > 0) {
                snprintf(msg, sizeof(msg), "Downloading %d episodes, %d skipped",
                         static_cast<int>(batch.items.size()), batch.skipped);
            } else {
                snprintf(msg, sizeof(msg), "Downloading %d episodes", static_cast<int>(batch.items.size()));
            }
            Speak(msg);
        }
    }

    // Download all episodes in the current feed using download queue
    void DownloadAll() {
        // Episodes from a password-protected feed need the feed's credentials
        const PodcastSubscription* feed = FindCurrentSubscription();
        std::wstring authHeader = feed ? BuildBasicAuthHeader(feed->username, feed->password) : L"";

        if (m_episodes.empty()) {
            Speak("No episodes loaded");
            return;
        }

        if (g_downloadPath.empty()) {
            Speak("Please set a downloads folder in Options");
            return;
        }

        std::vector<const PodcastEpisode*> episodes;
        for (const auto& ep : m_episodes) {
            episodes.push_back(&ep);
        }
        PodcastDownloadBatch batch = PreparePodcastDownloads(episodes, feed);

        // Enqueue all downloads (manager handles concurrency limit and speech)
        QueuePodcastDownloads(batch, authHeader, true);

        char msg[128];
        int downloadCount = static_cast<int>(batch.items.size());
        if (downloadCount == 0) {
            Speak("No episodes to download");
        } else if (batch.skipped > 0) {
            snprintf(msg, sizeof(msg), "Downloading %d episodes, %d skipped", downloadCount, batch.skipped);
            Speak(msg);
        } else {
            snprintf(msg, sizeof(msg), "Downloading %d episodes", downloadCount);
            Speak(msg);
        }
    }

    // Export subscriptions to OPML file
    void ExportOpml() {
        if (m_subs.empty()) {
            Speak("No subscriptions to export");
            return;
        }

        wxFileDialog dlg(this, "Export OPML", "", "podcasts.opml",
                         "OPML Files (*.opml)|*.opml|All Files (*.*)|*.*",
                         wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dlg.ShowModal() != wxID_OK) return;

        if (ExportOpmlFile(WS(dlg.GetPath()), m_subs)) {
            char msg[64];
            snprintf(msg, sizeof(msg), "Exported %d subscriptions", static_cast<int>(m_subs.size()));
            Speak(msg);
        } else {
            Speak("Failed to create file");
        }
    }

    void Search() {
        std::wstring query = WS(m_searchEdit->GetValue());
        if (query.empty()) return;

        Speak("Searching");
        m_searchList->Clear();
        m_searchResults.clear();

        std::vector<PodcastSearchResult> results;
        bool ok = SearchItunesPodcasts(query, results);
        OnSearchDone(results, ok);
    }

    void Subscribe() {
        int sel = m_searchList->GetSelection();
        if (sel >= 0 && sel < static_cast<int>(m_searchResults.size())) {
            const auto& r = m_searchResults[sel];
            if (AddPodcastSubscription(r.name, r.feedUrl, r.imageUrl) > 0) {
                RefreshSubsList();
                Speak("Subscribed");
            } else {
                Speak("Already subscribed or failed");
            }
        } else {
            Speak("Select a podcast first");
        }
    }

    // Play the first episode of the selected search result
    void PreviewSelectedResult() {
        int sel = m_searchList->GetSelection();
        if (sel < 0 || sel >= static_cast<int>(m_searchResults.size())) return;

        std::wstring feedUrl = m_searchResults[sel].feedUrl;
        Speak("Loading preview");
        std::vector<PodcastEpisode> eps;
        std::wstring title;
        if (ParsePodcastFeed(feedUrl, title, eps) && !eps.empty()) {
            g_playlist.clear();
            g_playlist.push_back(eps[0].audioUrl);
            PlayTrack(0, true);
            Speak("Playing");
        } else {
            Speak("No episodes found");
        }
    }

    // Add a feed by URL, with credentials for password-protected feeds
    void AddUrl() {
        PodcastAddDialog dlg(this);
        if (dlg.ShowModal() != wxID_OK || dlg.url.empty()) return;

        Speak("Fetching feed");
        FeedResult result;
        result.ok = ParsePodcastFeed(dlg.url, result.title, result.episodes, dlg.username, dlg.password, &result.diag);
        OnFeedAdded(dlg.url, dlg.username, dlg.password, result);
    }

    void ImportOpml() {
        wxFileDialog dlg(this, "Import OPML", "", "",
                         "OPML Files (*.opml;*.xml)|*.opml;*.xml|All Files (*.*)|*.*",
                         wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dlg.ShowModal() != wxID_OK) return;

        std::vector<OpmlFeed> feeds = ParseOpmlFile(WS(dlg.GetPath()));
        if (feeds.empty()) {
            Speak("No feeds found in file");
            return;
        }

        Speak("Importing feeds");

        for (auto& feed : feeds) {
            // If we don't have a title, try to get it from the feed
            if (feed.title.empty()) {
                std::vector<PodcastEpisode> eps;
                ParsePodcastFeed(feed.feedUrl, feed.title, eps);
            }
            if (feed.title.empty()) {
                feed.title = L"Unknown Podcast";
            }
        }
        OnFeedsImported(feeds);
    }

    wxNotebook* m_tabs;
    wxListBox* m_subsList;
    wxListBox* m_episodesList;
    wxTextCtrl* m_desc;
    wxTextCtrl* m_searchEdit;
    wxListBox* m_searchList;

    std::vector<PodcastSubscription> m_subs;
    std::vector<PodcastEpisode> m_episodes;
    std::vector<PodcastSearchResult> m_searchResults;
    int m_currentPodcastId = -1;
    // The caret the description was last shown for
    int m_lastDescCaret = -1;
};

}  // namespace

void ShowPodcastDialog() {
    PodcastDialog dlg(GetMainWindow());
    dlg.ShowModal();
}
