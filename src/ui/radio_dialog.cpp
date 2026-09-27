// Internet Radio: the favorites / search window, the add and edit station
// dialogs, and adding the playing stream to favorites.

#include "ui/dialogs.h"
#include "ui/ui_common.h"

#include "globals.h"
#include "player.h"
#include "database.h"
#include "radio.h"
#include "accessibility.h"
#include "app_ui.h"

#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/notebook.h>
#include <wx/weakref.h>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace {

class RadioDialog;

// Cached list of RadioBrowser country names (lazily fetched once per session).
// Only touched on the UI thread.
std::vector<std::wstring> g_radioCountries;
bool g_radioCountriesFetched = false;
bool g_radioCountriesFetching = false;

// State shared between the Radio window and the country fetch thread. `dialog`
// is only touched on the UI thread.
struct CountryFetchState {
    wxWeakRef<RadioDialog> dialog;
};

// The Add Radio Station dialog, also used (retitled) to edit a station.
class StationDialog : public wxDialog {
public:
    // Add mode: OK validates and adds the station to the database.
    // Edit mode: OK just closes; the caller reads GetStationName() / GetStationUrl().
    StationDialog(wxWindow* parent, bool editMode, const std::wstring& name, const std::wstring& url)
        : wxDialog(parent, wxID_ANY, editMode ? "Edit Station" : "Add Radio Station"), m_editMode(editMode) {
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        auto* grid = new wxFlexGridSizer(2, 5, 6);
        grid->AddGrowableCol(1);

        grid->Add(new wxStaticText(this, wxID_ANY, "Station &name:"), 0, wxALIGN_CENTER_VERTICAL);
        m_name = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(320, -1));
        grid->Add(m_name, 1, wxEXPAND);
        grid->Add(new wxStaticText(this, wxID_ANY, "Stream &URL:"), 0, wxALIGN_CENTER_VERTICAL);
        m_url = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(320, -1));
        grid->Add(m_url, 1, wxEXPAND);
        grid->AddSpacer(0);
        grid->Add(new wxStaticText(this, wxID_ANY, "(e.g., http://stream.example.com:8000/radio.mp3)"));
        sizer->Add(grid, 0, wxEXPAND | wxALL, 10);

        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        auto* ok = new wxButton(this, wxID_OK, "OK");
        ok->SetDefault();
        buttons->Add(ok, 0, wxRIGHT, 6);
        buttons->Add(new wxButton(this, wxID_CANCEL, "Cancel"));
        sizer->Add(buttons, 0, wxALIGN_RIGHT | wxLEFT | wxRIGHT | wxBOTTOM, 10);

        SetSizerAndFit(sizer);
        CentreOnParent();

        if (m_editMode) {
            m_name->SetValue(WX(name));
            m_url->SetValue(WX(url));
            // Select all text in name field
            m_name->SelectAll();
        } else if (!url.empty()) {
            // If a URL was passed, pre-fill the URL field
            m_url->SetValue(WX(url));
        }

        if (!m_editMode) {
            ok->Bind(wxEVT_BUTTON, &StationDialog::OnAddOK, this);
        }
        m_name->SetFocus();
    }

    std::wstring GetStationName() const { return WS(m_name->GetValue()); }
    std::wstring GetStationUrl() const { return WS(m_url->GetValue()); }

private:
    void OnAddOK(wxCommandEvent&) {
        std::wstring name = GetStationName();
        std::wstring url = GetStationUrl();

        // Validate
        if (name.empty()) {
            wxMessageBox("Please enter a station name.", "Add Station", wxOK | wxICON_WARNING, this);
            m_name->SetFocus();
            return;
        }
        if (url.empty()) {
            wxMessageBox("Please enter a stream URL.", "Add Station", wxOK | wxICON_WARNING, this);
            m_url->SetFocus();
            return;
        }

        // Add to database
        int id = AddRadioStation(name, url);
        if (id >= 0) {
            EndModal(wxID_OK);
        } else {
            wxMessageBox("Failed to add station.", "Add Station", wxOK | wxICON_ERROR, this);
        }
    }

    bool m_editMode;
    wxTextCtrl* m_name;
    wxTextCtrl* m_url;
};

// Trim spaces and tabs from both ends
void TrimSpacesAndTabs(std::wstring& s) {
    while (!s.empty() && (s.front() == L' ' || s.front() == L'\t')) s.erase(0, 1);
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\t')) s.pop_back();
}

bool IsEnterKey(int key) { return key == WXK_RETURN || key == WXK_NUMPAD_ENTER; }
bool IsDeleteKey(int key) { return key == WXK_DELETE || key == WXK_NUMPAD_DELETE; }
bool IsUpKey(int key) { return key == WXK_UP || key == WXK_NUMPAD_UP; }
bool IsDownKey(int key) { return key == WXK_DOWN || key == WXK_NUMPAD_DOWN; }

// The Internet Radio window: a Favorites tab and a Search tab.
class RadioDialog : public wxDialog {
public:
    explicit RadioDialog(wxWindow* parent)
        : wxDialog(parent, wxID_ANY, "Internet Radio", wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER) {
        auto* sizer = new wxBoxSizer(wxVERTICAL);

        m_notebook = new wxNotebook(this, wxID_ANY);
        m_notebook->AddPage(CreateFavoritesPage(), "Favorites");
        m_notebook->AddPage(CreateSearchPage(), "Search");
        sizer->Add(m_notebook, 1, wxEXPAND | wxALL, 10);

        // Common
        sizer->Add(new wxButton(this, wxID_CANCEL, "Close"), 0, wxALIGN_RIGHT | wxLEFT | wxRIGHT | wxBOTTOM, 10);

        SetSizer(sizer);
        SetClientSize(ConvertDialogToPixels(wxSize(350, 280)));
        SetMinSize(FromDIP(wxSize(300, 200)));
        CentreOnParent();

        // Initialize search source combo
        m_source->Append("RadioBrowser");
        m_source->Append("TuneIn");
        m_source->Append("iHeartRadio");
        m_source->SetSelection(0);

        // Initialize country combo with (Any); populate from cache or start async fetch
        m_country->Append("(Any)");
        m_country->SetSelection(0);
        if (g_radioCountriesFetched) {
            PopulateCountryCombo();
        } else {
            EnsureRadioCountriesFetched();
        }

        // Load favorites
        RefreshRadioList();

        // Country controls match the selected source
        UpdateCountryVisibility();

        Bind(wxEVT_CHAR_HOOK, &RadioDialog::OnCharHook, this);
        m_list->Bind(wxEVT_LISTBOX_DCLICK, [this](wxCommandEvent&) { PlaySelectedFavorite(); });
        m_searchList->Bind(wxEVT_LISTBOX_DCLICK, [this](wxCommandEvent&) { PlaySelectedSearchResult(); });
        m_source->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { UpdateCountryVisibility(); });
        m_addButton->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnAdd(); });
        m_importButton->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnImport(); });
        m_exportButton->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnExport(); });
        m_searchButton->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnSearch(); });
        m_searchAddButton->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { OnSearchAdd(); });

        // Focus on list
        m_list->SetFocus();
        if (m_list->GetCount() > 0) {
            m_list->SetSelection(0);
        }
    }

    // Fill the country combo from g_radioCountries, preserving the user's typed text
    void PopulateCountryCombo() {
        wxString current = m_country->GetValue();

        m_country->Clear();
        m_country->Append("(Any)");
        for (const auto& c : g_radioCountries) {
            m_country->Append(WX(c));
        }

        if (!current.empty()) {
            m_country->SetValue(current);
        } else {
            m_country->SetSelection(0);
        }
    }

    ~RadioDialog() override {
        // A country fetch still running finds the window gone. Cleared here, on the UI
        // thread, so the weak reference is never released by the worker thread.
        if (m_countryState) m_countryState->dialog = nullptr;
    }

private:
    wxPanel* CreateFavoritesPage() {
        auto* page = new wxPanel(m_notebook);
        auto* sizer = new wxBoxSizer(wxVERTICAL);

        m_list = new wxListBox(page, wxID_ANY, wxDefaultPosition, wxSize(-1, 60), 0, nullptr, wxLB_SINGLE);
        sizer->Add(m_list, 1, wxEXPAND | wxALL, 6);

        auto* row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(new wxStaticText(page, wxID_ANY, "Enter = play, Delete = remove, Escape = close"),
                 0, wxALIGN_CENTER_VERTICAL);
        row->AddStretchSpacer();
        m_addButton = new wxButton(page, wxID_ANY, "&Add...");
        row->Add(m_addButton, 0, wxLEFT, 6);
        m_importButton = new wxButton(page, wxID_ANY, "&Import...");
        row->Add(m_importButton, 0, wxLEFT, 6);
        m_exportButton = new wxButton(page, wxID_ANY, "&Export...");
        row->Add(m_exportButton, 0, wxLEFT, 6);
        sizer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);

        page->SetSizer(sizer);
        return page;
    }

    wxPanel* CreateSearchPage() {
        auto* page = new wxPanel(m_notebook);
        m_searchPage = page;
        auto* sizer = new wxBoxSizer(wxVERTICAL);

        auto* top = new wxBoxSizer(wxHORIZONTAL);
        top->Add(new wxStaticText(page, wxID_ANY, "&Source:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
        m_source = new wxChoice(page, wxID_ANY);
        top->Add(m_source, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        m_searchEdit = new wxTextCtrl(page, wxID_ANY);
        top->Add(m_searchEdit, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        m_searchButton = new wxButton(page, wxID_ANY, "&Search");
        top->Add(m_searchButton, 0, wxALIGN_CENTER_VERTICAL);
        sizer->Add(top, 0, wxEXPAND | wxALL, 6);

        auto* countryRow = new wxBoxSizer(wxHORIZONTAL);
        m_countryLabel = new wxStaticText(page, wxID_ANY, "Co&untry:");
        countryRow->Add(m_countryLabel, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
        m_country = new wxComboBox(page, wxID_ANY, "", wxDefaultPosition, wxSize(230, -1), 0, nullptr, wxCB_DROPDOWN);
        countryRow->Add(m_country, 0, wxALIGN_CENTER_VERTICAL);
        sizer->Add(countryRow, 0, wxLEFT | wxRIGHT | wxBOTTOM, 6);

        sizer->Add(new wxStaticText(page, wxID_ANY, "S&tations:"), 0, wxLEFT | wxRIGHT, 6);
        m_searchList = new wxListBox(page, wxID_ANY, wxDefaultPosition, wxSize(-1, 60), 0, nullptr, wxLB_SINGLE);
        sizer->Add(m_searchList, 1, wxEXPAND | wxALL, 6);

        auto* row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(new wxStaticText(page, wxID_ANY, "Enter = play, Escape = close"), 0, wxALIGN_CENTER_VERTICAL);
        row->AddStretchSpacer();
        m_searchAddButton = new wxButton(page, wxID_ANY, "Add to &Favorites");
        row->Add(m_searchAddButton, 0, wxLEFT, 6);
        sizer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);

        page->SetSizer(sizer);
        return page;
    }

    // Country controls: only visible with RadioBrowser selected
    void UpdateCountryVisibility() {
        bool showCountry = (m_source->GetSelection() == RADIO_SOURCE_RADIOBROWSER);
        m_country->Show(showCountry);
        m_countryLabel->Show(showCountry);
        m_searchPage->Layout();
    }

    void EnsureRadioCountriesFetched() {
        if (g_radioCountriesFetched || g_radioCountriesFetching) return;
        g_radioCountriesFetching = true;

        // The window keeps the state too, so it can clear the weak reference itself.
        m_countryState = std::make_shared<CountryFetchState>();
        m_countryState->dialog = this;
        auto state = m_countryState;
        try {
            // Background thread: fetch RadioBrowser countries and notify the window
            std::thread([state]() mutable {
                std::vector<std::wstring> countries = FetchRadioCountries();
                RunOnUiThread([state = std::move(state), countries = std::move(countries)]() {
                    g_radioCountries = countries;
                    g_radioCountriesFetched = true;
                    g_radioCountriesFetching = false;
                    if (state->dialog) state->dialog->PopulateCountryCombo();
                });
            }).detach();
        } catch (const std::system_error&) {
            g_radioCountriesFetching = false;
        }
    }

    // Refresh radio list from database
    void RefreshRadioList() {
        m_list->Clear();

        m_stations = GetRadioFavorites();

        for (const auto& station : m_stations) {
            m_list->Append(WX(station.name));
        }
    }

    void PlayUrl(const std::wstring& url) {
        g_playlist.clear();
        g_playlist.push_back(url);
        PlayTrack(0);
    }

    void PlaySelectedFavorite() {
        int sel = m_list->GetSelection();
        if (sel >= 0 && sel < static_cast<int>(m_stations.size())) {
            PlayUrl(m_stations[sel].url);
        }
    }

    // Show context menu for stream URL selection
    // Returns the selected URL, or empty string if cancelled
    std::wstring ShowStreamSelectionMenu(const std::vector<StreamOption>& options) {
        if (options.empty()) return L"";
        if (options.size() == 1) return options[0].url;  // Only one option, no menu needed

        wxMenu menu;
        for (size_t i = 0; i < options.size(); i++) {
            menu.Append(static_cast<int>(i + 1), WX(options[i].label));
        }

        // Show the menu at the mouse pointer and wait for selection
        int cmd = GetPopupMenuSelectionFromUser(menu, ScreenToClient(wxGetMousePosition()));

        if (cmd > 0 && cmd <= static_cast<int>(options.size())) {
            return options[cmd - 1].url;
        }

        return L"";  // Cancelled
    }

    // Get the stream URL of a search result, asking which stream when there are
    // several. hadOptions is false when no stream could be found at all.
    std::wstring ResolveSearchResult(const RadioSearchResult& r, bool& hadOptions) {
        std::wstring streamUrl;
        hadOptions = false;

        // For TuneIn and iHeartRadio, check for multiple streams
        if (r.source == RADIO_SOURCE_TUNEIN || r.source == RADIO_SOURCE_IHEARTRADIO) {
            std::vector<StreamOption> urls;
            {
                wxBusyCursor wait;
                urls = ResolveRadioStreamUrls(r);
            }
            hadOptions = !urls.empty();
            if (urls.size() > 1) {
                streamUrl = ShowStreamSelectionMenu(urls);
            } else if (!urls.empty()) {
                streamUrl = urls[0].url;
            }
        } else {
            // RadioBrowser - single URL
            {
                wxBusyCursor wait;
                streamUrl = ResolveRadioStreamUrl(r);
            }
            hadOptions = !streamUrl.empty();
        }
        return streamUrl;
    }

    void PlaySelectedSearchResult() {
        int sel = m_searchList->GetSelection();
        if (sel >= 0 && sel < static_cast<int>(m_searchResults.size())) {
            bool hadOptions = false;
            std::wstring streamUrl = ResolveSearchResult(m_searchResults[sel], hadOptions);

            if (!streamUrl.empty()) {
                PlayUrl(streamUrl);
            } else if (!hadOptions) {
                Speak("Could not get stream URL");
            }
            // If hadOptions but streamUrl is empty, user cancelled - do nothing
        }
    }

    void OnCharHook(wxKeyEvent& event) {
        // Alt combinations are menu / mnemonic keys, never the lists' own.
        if (event.AltDown()) {
            event.Skip();
            return;
        }
        wxWindow* focus = FindFocus();
        int key = event.GetKeyCode();
        bool ctrl = event.ControlDown();

        if (focus == m_list) {
            if (IsEnterKey(key)) {
                // Play selected station
                PlaySelectedFavorite();
                return;
            } else if (key == WXK_F2) {
                EditSelectedFavorite();
                return;
            } else if (IsDeleteKey(key)) {
                RemoveSelectedFavorite();
                return;
            } else if ((IsUpKey(key) || IsDownKey(key)) && ctrl) {
                MoveSelectedFavorite(IsUpKey(key) ? -1 : 1);
                return;
            } else if (key == 'C' && ctrl) {
                // Copy stream URL to clipboard
                int sel = m_list->GetSelection();
                if (sel >= 0 && sel < static_cast<int>(m_stations.size())) {
                    if (SetClipboardText(m_stations[sel].url)) {
                        Speak("URL copied");
                    }
                }
                return;
            }
        } else if (focus == m_searchList) {
            if (IsEnterKey(key)) {
                // Play selected station
                PlaySelectedSearchResult();
                return;
            } else if (key == 'C' && ctrl) {
                CopySelectedSearchResultUrl();
                return;
            }
        } else if (focus == m_searchEdit) {
            if (IsEnterKey(key)) {
                // Enter in the search box searches
                OnSearch();
                return;
            }
        }
        // Escape falls through to the dialog, which closes it.
        event.Skip();
    }

    // Edit selected station
    void EditSelectedFavorite() {
        int sel = m_list->GetSelection();
        if (sel < 0 || sel >= static_cast<int>(m_stations.size())) return;

        StationDialog dlg(this, true, m_stations[sel].name, m_stations[sel].url);
        if (dlg.ShowModal() != wxID_OK) return;

        std::wstring name = dlg.GetStationName();
        std::wstring url = dlg.GetStationUrl();
        // Trim whitespace from name and URL
        TrimSpacesAndTabs(name);
        TrimSpacesAndTabs(url);
        if (!name.empty() && !url.empty()) {
            if (UpdateRadioStation(m_stations[sel].id, name, url)) {
                Speak("Station updated");
                RefreshRadioList();
                m_list->SetSelection(sel);
            }
        }
    }

    // Remove selected station
    void RemoveSelectedFavorite() {
        int sel = m_list->GetSelection();
        if (sel >= 0 && sel < static_cast<int>(m_stations.size())) {
            if (RemoveRadioStation(m_stations[sel].id)) {
                Speak("Station removed");
                RefreshRadioList();
                // Select next item or previous if at end
                int count = static_cast<int>(m_list->GetCount());
                if (count > 0) {
                    if (sel >= count) sel = count - 1;
                    m_list->SetSelection(sel);
                }
            }
        }
    }

    // Reorder station with Ctrl+Up/Down
    void MoveSelectedFavorite(int direction) {
        int sel = m_list->GetSelection();
        int count = static_cast<int>(m_stations.size());
        int newSel = sel + direction;
        if (sel >= 0 && sel < count && newSel >= 0 && newSel < count) {
            std::swap(m_stations[sel], m_stations[newSel]);
            UpdateRadioSortOrders(m_stations);
            RefreshRadioList();
            m_list->SetSelection(newSel);
            // Speak position feedback
            const std::wstring& name = m_stations[newSel].name;
            if (newSel == 0)
                SpeakW(name + L" moved to top");
            else
                SpeakW(name + L" moved below " + m_stations[newSel - 1].name);
        }
    }

    void CopySelectedSearchResultUrl() {
        int sel = m_searchList->GetSelection();
        if (sel < 0 || sel >= static_cast<int>(m_searchResults.size())) return;

        // Resolve the stream URL
        bool hadOptions = false;
        std::wstring streamUrl = ResolveSearchResult(m_searchResults[sel], hadOptions);

        if (!streamUrl.empty()) {
            if (SetClipboardText(streamUrl)) {
                Speak("URL copied");
            }
        } else {
            Speak("Could not get stream URL");
        }
    }

    void OnAdd() {
        // Check if currently playing a URL stream
        std::wstring currentUrl;
        if (g_currentTrack >= 0 && g_currentTrack < static_cast<int>(g_playlist.size())) {
            const std::wstring& path = g_playlist[g_currentTrack];
            if (IsURL(path.c_str())) {
                currentUrl = path;
            }
        }
        StationDialog dlg(this, false, L"", currentUrl);
        if (dlg.ShowModal() == wxID_OK) {
            RefreshRadioList();
            Speak("Station added");
        }
    }

    void OnImport() {
        // Open file dialog for playlist files
        wxFileDialog dlg(this, "Open", "", "",
                         "Playlist Files|*.m3u;*.m3u8;*.pls"
                         "|M3U Playlists|*.m3u;*.m3u8"
                         "|PLS Playlists|*.pls"
                         "|All Files (*.*)|*.*",
                         wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dlg.ShowModal() != wxID_OK) return;

        RadioImportResult result = ImportRadioFavorites(WS(dlg.GetPath()));

        if (result.imported > 0) {
            RefreshRadioList();
            if (result.skipped > 0) {
                Speak(wxString::Format("Imported %d stations, %d already present",
                                       result.imported, result.skipped).ToStdString());
            } else {
                Speak(wxString::Format("Imported %d stations", result.imported).ToStdString());
            }
        } else if (result.skipped > 0) {
            Speak("All stations already present");
        } else {
            Speak("No stations found to import");
        }
    }

    void OnExport() {
        // Export favorites to M3U file
        if (m_stations.empty()) {
            Speak("No favorites to export");
            return;
        }

        wxString path = AskSavePath(this, "radio_favorites.m3u", "M3U Playlist|*.m3u|All Files (*.*)|*.*", "m3u");
        if (path.empty()) return;

        if (ExportRadioFavorites(WS(path), m_stations)) {
            Speak(wxString::Format("Exported %d stations", static_cast<int>(m_stations.size())).ToStdString());
        } else {
            Speak("Failed to write file");
        }
    }

    void OnSearch() {
        // Ensure country controls match the current source before we proceed
        UpdateCountryVisibility();

        // Get search query
        std::wstring query = WS(m_searchEdit->GetValue());

        // Read country filter (only applies to RadioBrowser)
        int source = m_source->GetSelection();
        std::wstring countryFilter;
        if (source == RADIO_SOURCE_RADIOBROWSER) {
            countryFilter = WS(m_country->GetValue());
            if (countryFilter == L"(Any)") countryFilter.clear();
        }

        // Allow empty query only for RadioBrowser if a country is selected
        if (query.empty() && countryFilter.empty()) {
            Speak("Enter a search term");
            return;
        }

        // Clear results
        m_searchList->Clear();
        m_searchResults.clear();

        // Show searching message
        Speak("Searching");

        bool found = false;
        {
            wxBusyCursor wait;
            if (source == RADIO_SOURCE_RADIOBROWSER) {
                found = SearchRadioBrowser(query, countryFilter, m_searchResults);
            } else if (source == RADIO_SOURCE_TUNEIN) {
                found = SearchTuneIn(query, m_searchResults);
            } else if (source == RADIO_SOURCE_IHEARTRADIO) {
                found = SearchIHeartRadio(query, m_searchResults);
            }
        }

        if (found) {
            // Populate list
            for (const auto& r : m_searchResults) {
                std::wstring display = r.name;
                if (!r.country.empty()) {
                    display += L" (" + r.country + L")";
                }
                if (r.bitrate > 0) {
                    display += L" [" + std::to_wstring(r.bitrate) + L"k]";
                }
                m_searchList->Append(WX(display));
            }

            Speak(wxString::Format("Found %d stations", static_cast<int>(m_searchResults.size())).ToStdString());

            // Select first item
            if (m_searchList->GetCount() > 0) {
                m_searchList->SetSelection(0);
                m_searchList->SetFocus();
            }
        } else {
            Speak("No stations found");
        }
    }

    // Add selected search result to favorites
    void OnSearchAdd() {
        int sel = m_searchList->GetSelection();
        if (sel >= 0 && sel < static_cast<int>(m_searchResults.size())) {
            const auto& r = m_searchResults[sel];
            bool hadOptions = false;
            std::wstring streamUrl = ResolveSearchResult(r, hadOptions);

            if (!streamUrl.empty()) {
                if (AddRadioStation(r.name, streamUrl) >= 0) {
                    RefreshRadioList();
                    Speak("Added to favorites");
                } else {
                    Speak("Failed to add station");
                }
            } else if (!hadOptions) {
                Speak("Could not get stream URL");
            }
            // If hadOptions but streamUrl is empty, user cancelled - do nothing
        } else {
            Speak("Select a station first");
        }
    }

    std::vector<RadioStation> m_stations;
    std::vector<RadioSearchResult> m_searchResults;

    wxNotebook* m_notebook = nullptr;
    wxListBox* m_list = nullptr;
    wxButton* m_addButton = nullptr;
    wxButton* m_importButton = nullptr;
    wxButton* m_exportButton = nullptr;
    wxPanel* m_searchPage = nullptr;
    wxChoice* m_source = nullptr;
    wxTextCtrl* m_searchEdit = nullptr;
    wxButton* m_searchButton = nullptr;
    wxStaticText* m_countryLabel = nullptr;
    wxComboBox* m_country = nullptr;
    wxListBox* m_searchList = nullptr;
    wxButton* m_searchAddButton = nullptr;
    std::shared_ptr<CountryFetchState> m_countryState;
};

}  // namespace

// Show radio dialog
void ShowRadioDialog() {
    RadioDialog dlg(GetMainWindow());
    dlg.ShowModal();
}

// Add the currently playing stream URL to radio favorites
void AddCurrentStreamToFavorites() {
    if (g_currentTrack < 0 || g_currentTrack >= static_cast<int>(g_playlist.size())) {
        Speak("No stream playing");
        return;
    }
    const std::wstring& path = g_playlist[g_currentTrack];
    if (!IsURL(path.c_str())) {
        Speak("Not a stream");
        return;
    }

    std::vector<RadioStation> existing = GetRadioFavorites();
    for (const auto& s : existing) {
        if (WX(s.url).CmpNoCase(WX(path)) == 0) {
            Speak("Stream already in favorites");
            return;
        }
    }

    StationDialog dlg(GetMainWindow(), false, L"", path);
    if (dlg.ShowModal() == wxID_OK) {
        Speak("Station added");
    }
}
