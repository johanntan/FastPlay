// The Library window (Ctrl+L): the library (library.h) by songs, artists, albums,
// genres and folders, one tab each, with a search above them all and a sort for
// each list.
//
// A list goes down a level at a time: an artist opens to its albums, with "All
// songs" first; an album, genre or folder opens to what is in it. Enter opens or
// plays, Backspace goes back up. Playing a song plays the list it is in, from it.

#include "ui/dialogs.h"
#include "ui/ui_common.h"

#include "accessibility.h"
#include "globals.h"
#include "library.h"
#include "player.h"
#include "playlist_io.h"
#include "utils.h"

#include <wx/listctrl.h>
#include <wx/notebook.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>

namespace {

// A list of lines that stays quick with tens of thousands of them. On Windows a
// list box takes seconds to fill with that many (each line is its own message
// and allocation), so it is a virtual list view there instead, which asks only
// for the lines it shows; screen readers read it as they do any list. On macOS
// the list box is a table view that already works that way.
#ifdef __WXMSW__
class ItemList : public wxListCtrl {
public:
    ItemList(wxWindow* parent, const wxSize& size)
        : wxListCtrl(parent, wxID_ANY, wxDefaultPosition, size,
                     wxLC_REPORT | wxLC_VIRTUAL | wxLC_SINGLE_SEL | wxLC_NO_HEADER) {
        AppendColumn("");
        Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
            SetColumnWidth(0, GetClientSize().x);
            event.Skip();
        });
        Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent&) {
            if (m_activate) m_activate();
        });
    }

    void Set(wxArrayString texts) {
        m_texts = std::move(texts);
        SetItemCount(static_cast<long>(m_texts.size()));
        Refresh();
    }
    int GetSelection() const { return static_cast<int>(GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED)); }
    void SetSelection(int index) {
        const long state = wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED;
        SetItemState(index, state, state);
        EnsureVisible(index);
    }
    void OnActivate(std::function<void()> activate) { m_activate = std::move(activate); }

private:
    wxString OnGetItemText(long item, long) const override {
        return item >= 0 && item < static_cast<long>(m_texts.size()) ? m_texts[item] : wxString();
    }

    wxArrayString m_texts;
    std::function<void()> m_activate;
};
#else
class ItemList : public wxListBox {
public:
    ItemList(wxWindow* parent, const wxSize& size)
        : wxListBox(parent, wxID_ANY, wxDefaultPosition, size, 0, nullptr, wxLB_SINGLE) {
        Bind(wxEVT_LISTBOX_DCLICK, [this](wxCommandEvent&) {
            if (m_activate) m_activate();
        });
    }
    void Set(const wxArrayString& texts) {
        Freeze();
        wxListBox::Set(texts);
        Thaw();
    }
    void OnActivate(std::function<void()> activate) { m_activate = std::move(activate); }

private:
    std::function<void()> m_activate;
};
#endif

class LibraryDialog;
LibraryDialog* g_libraryDialog = nullptr;

// What a list shows
enum class Kind { Songs, Artists, Albums, Genres, Folder };

// The ways each kind of list sorts, as the sort box names them
struct SortChoice {
    const char* label;
    int order;  // a SongOrder, GroupOrder or FolderOrder
};
const std::vector<SortChoice>& SortChoices(Kind kind) {
    static const std::vector<SortChoice> songs = {
        {"Title", (int)SongOrder::Title},          {"Artist", (int)SongOrder::Artist},
        {"Album", (int)SongOrder::Album},          {"Track number", (int)SongOrder::TrackNumber},
        {"Duration", (int)SongOrder::Duration},    {"Year", (int)SongOrder::Year},
        {"Date added", (int)SongOrder::DateAdded}, {"File name", (int)SongOrder::FileName},
    };
    static const std::vector<SortChoice> artists = {{"Name", (int)GroupOrder::Name},
                                                    {"Number of songs", (int)GroupOrder::Songs}};
    static const std::vector<SortChoice> albums = {{"Name", (int)GroupOrder::Name},
                                                   {"Artist", (int)GroupOrder::Artist},
                                                   {"Year", (int)GroupOrder::Year},
                                                   {"Number of songs", (int)GroupOrder::Songs}};
    static const std::vector<SortChoice> folder = {{"Name", (int)FolderOrder::Name},
                                                   {"Date modified", (int)FolderOrder::Modified}};
    switch (kind) {
        case Kind::Songs: return songs;
        case Kind::Albums: return albums;
        case Kind::Folder: return folder;
        default: return artists;  // artists and genres
    }
}

// The index of `order` in the kind's sort choices
int SortIndex(Kind kind, int order) {
    const auto& choices = SortChoices(kind);
    for (size_t i = 0; i < choices.size(); i++) {
        if (choices[i].order == order) return static_cast<int>(i);
    }
    return 0;
}

// A level of a list: what it shows, and where it was
struct Level {
    Kind kind;
    LibraryFilter filter;
    bool allSongs = false;   // "All songs" first (an artist's albums, a genre's artists)
    std::wstring folder;     // Folder: empty for the library's folders themselves
    int sort = 0;            // index into SortChoices(kind)
    wxString selected;       // the key of the item selected, to come back to
};

// An item in a list
struct Item {
    enum Type { Song, Group, AllSongs, Folder, File } type;
    LibrarySong song;
    LibraryGroup group;
    std::wstring path;       // a folder's or file's
    wxString key;            // to find it again after a refresh
};

wxString Unknown(const std::wstring& name, const char* what) {
    return name.empty() ? wxString(what) : WX(name);
}

wxString SongsText(int count) { return wxString::Format(count == 1 ? "%d song" : "%d songs", count); }

// The tab each list is on
const char* const kTabNames[] = {"Songs", "Artists", "Albums", "Genres", "Folders"};
const Kind kTabKinds[] = {Kind::Songs, Kind::Artists, Kind::Albums, Kind::Genres, Kind::Folder};
const int kTabs = 5;

// The top-level sorts chosen, kept while FastPlay runs
int g_topSort[kTabs] = {0, 0, 0, 0, 0};
int g_lastTab = 0;

class LibraryDialog : public wxDialog {
public:
    explicit LibraryDialog(wxWindow* parent)
        : wxDialog(parent, wxID_ANY, "Library", wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER) {
        g_libraryDialog = this;
        auto* sizer = new wxBoxSizer(wxVERTICAL);

        auto* searchRow = new wxBoxSizer(wxHORIZONTAL);
        searchRow->Add(new wxStaticText(this, wxID_ANY, "&Search:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        m_search = new wxTextCtrl(this, wxID_ANY, "");
        searchRow->Add(m_search, 1);
        sizer->Add(searchRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

        m_book = new wxNotebook(this, wxID_ANY);
        for (int i = 0; i < kTabs; i++) BuildPage(i);
        sizer->Add(m_book, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

        m_status = new wxStaticText(this, wxID_ANY, "");
        sizer->Add(m_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        auto* play = new wxButton(this, wxID_ANY, "&Play");
        buttons->Add(play, 0, wxRIGHT, 6);
        auto* add = new wxButton(this, wxID_ANY, "Add to Play&list");
        buttons->Add(add, 0, wxRIGHT, 6);
        auto* back = new wxButton(this, wxID_ANY, "&Back");
        buttons->Add(back, 0, wxRIGHT, 6);
        buttons->AddStretchSpacer();
        buttons->Add(new wxButton(this, wxID_CANCEL, "Close"));
        sizer->Add(buttons, 0, wxEXPAND | wxALL, 10);

        SetSizerAndFit(sizer);
        CentreOnParent();

        Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Destroy(); }, wxID_CANCEL);
        Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) { Destroy(); });
        Bind(wxEVT_CHAR_HOOK, &LibraryDialog::OnCharHook, this);
        play->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { PlaySelected(false); });
        add->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { PlaySelected(true); });
        back->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { GoBack(); });
        m_book->Bind(wxEVT_NOTEBOOK_PAGE_CHANGED, [this](wxBookCtrlEvent&) {
            g_lastTab = m_book->GetSelection();
            Fill(CurrentTab());
        });
        // The search is made a moment after typing stops
        m_searchTimer.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { Fill(CurrentTab()); });
        m_search->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { m_searchTimer.StartOnce(300); });
        m_statusTimer.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { ShowStatus(); });
        m_statusTimer.Start(1000);

        SetLibraryChangedHandler([]() {
            if (g_libraryDialog) g_libraryDialog->LibraryChanged();
        });

        m_book->SetSelection(g_lastTab);
        Fill(CurrentTab());
        m_pages[CurrentTab()].list->SetFocus();
    }

    ~LibraryDialog() override {
        if (g_libraryDialog == this) g_libraryDialog = nullptr;
        SetLibraryChangedHandler(nullptr);
    }

private:
    struct Page {
        wxChoice* sort = nullptr;
        int sortKind = -1;  // the Kind its choices are for
        ItemList* list = nullptr;
        wxArrayString shown;  // what the list says, to leave it be when that is the same
        std::vector<Level> levels;
        std::vector<Item> items;
    };

    // ------------------------------------------------------------------
    // Layout
    // ------------------------------------------------------------------

    void BuildPage(int tab) {
        auto* panel = new wxPanel(m_book);
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        auto* sortRow = new wxBoxSizer(wxHORIZONTAL);
        sortRow->Add(new wxStaticText(panel, wxID_ANY, "S&ort by:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        Page& page = m_pages[tab];
        page.sort = new wxChoice(panel, wxID_ANY);
        sortRow->Add(page.sort);
        sizer->Add(sortRow, 0, wxLEFT | wxRIGHT | wxTOP, 10);
        page.list = new ItemList(panel, ConvertDialogToPixels(wxSize(336, 200)));
        sizer->Add(page.list, 1, wxEXPAND | wxALL, 10);
        panel->SetSizer(sizer);
        m_book->AddPage(panel, kTabNames[tab]);

        Level top;
        top.kind = kTabKinds[tab];
        top.sort = g_topSort[tab];
        page.levels.push_back(top);

        page.sort->Bind(wxEVT_CHOICE, [this, tab](wxCommandEvent&) {
            Page& p = m_pages[tab];
            p.levels.back().sort = p.sort->GetSelection();
            if (p.levels.size() == 1) g_topSort[tab] = p.levels.back().sort;
            Fill(tab);
        });
        page.list->OnActivate([this]() { Activate(); });
    }

    int CurrentTab() const { return std::max(0, m_book->GetSelection()); }

    // ------------------------------------------------------------------
    // Filling a list
    // ------------------------------------------------------------------

    // Makes `tab`'s list show its current level, keeping the selection if it can
    void Fill(int tab) {
        Page& page = m_pages[tab];
        Level& level = page.levels.back();
        // The sort box for this kind of list (made again only when the kind changes,
        // so a screen reader is not told of it again for nothing)
        if (page.sortKind != static_cast<int>(level.kind)) {
            page.sort->Clear();
            for (const auto& choice : SortChoices(level.kind)) page.sort->Append(choice.label);
            page.sortKind = static_cast<int>(level.kind);
        }
        if (page.sort->GetSelection() != level.sort) page.sort->SetSelection(level.sort);

        const int order = SortChoices(level.kind)[level.sort].order;
        LibraryFilter filter = level.filter;
        filter.search = WS(m_search->GetValue());
        std::vector<Item> items;
        if (level.allSongs) items.push_back({Item::AllSongs, {}, {}, {}, "*all"});
        switch (level.kind) {
            case Kind::Songs:
                for (auto& song : LibrarySongs(filter, static_cast<SongOrder>(order))) AddSong(items, std::move(song));
                break;
            case Kind::Artists:
                for (auto& group : LibraryArtists(filter, static_cast<GroupOrder>(order))) AddGroup(items, std::move(group));
                break;
            case Kind::Albums:
                for (auto& group : LibraryAlbums(filter, static_cast<GroupOrder>(order))) AddGroup(items, std::move(group));
                break;
            case Kind::Genres:
                for (auto& group : LibraryGenres(filter, static_cast<GroupOrder>(order))) AddGroup(items, std::move(group));
                break;
            case Kind::Folder:
                FillFolder(level, filter.search, static_cast<FolderOrder>(order), items);
                break;
        }

        wxArrayString texts;
        texts.reserve(items.size());
        for (const Item& item : items) texts.push_back(Text(level, item));
        // The same as before (a refresh while indexing changed nothing here): leave it be
        const bool same = page.shown.size() == texts.size() && std::equal(texts.begin(), texts.end(), page.shown.begin());
        wxString selected = SelectedKey(page);
        if (selected.empty()) selected = level.selected;
        page.items = std::move(items);
        if (!same) {
            page.shown = texts;
            page.list->Set(std::move(texts));
        }
        int select = page.items.empty() ? -1 : 0;
        for (size_t i = 0; i < page.items.size(); i++) {
            if (page.items[i].key == selected) {
                select = static_cast<int>(i);
                break;
            }
        }
        if (select >= 0 && page.list->GetSelection() != select) page.list->SetSelection(select);
        ShowStatus();
    }

    void AddSong(std::vector<Item>& items, LibrarySong song) {
        Item item{Item::Song, {}, {}, song.path, WX(song.path)};
        item.song = std::move(song);
        items.push_back(std::move(item));
    }

    void AddGroup(std::vector<Item>& items, LibraryGroup group) {
        Item item{Item::Group, {}, {}, {}, WX(group.name + L"\x1f" + group.artist)};
        item.group = std::move(group);
        items.push_back(std::move(item));
    }

    void FillFolder(const Level& level, const std::wstring& search, FolderOrder order, std::vector<Item>& items) {
        if (!search.empty()) {
            // A search looks through every folder in the library, by name and tags
            for (auto& song : LibrarySearchFiles(search, SongOrder::FileName)) {
                Item item{Item::File, {}, {}, song.path, WX(song.path)};
                item.song = std::move(song);
                items.push_back(std::move(item));
            }
            return;
        }
        if (level.folder.empty()) {
            for (const LibraryFolder& folder : g_libraryFolders) {
                items.push_back({Item::Folder, {}, {}, folder.path, WX(folder.path)});
            }
            return;
        }
        for (const LibraryEntry& entry : LibraryListFolder(level.folder, order)) {
            items.push_back({entry.isFolder ? Item::Folder : Item::File, {}, {}, entry.path, WX(entry.path)});
        }
    }

    // What an item says in its list
    wxString Text(const Level& level, const Item& item) const {
        switch (item.type) {
            case Item::AllSongs:
                return "All songs";
            case Item::Folder: {
                if (level.folder.empty() && level.kind == Kind::Folder) return WX(item.path) + ", folder";
                size_t slash = item.path.find_last_of(L"\\/");
                return WX(slash == std::wstring::npos ? item.path : item.path.substr(slash + 1)) + ", folder";
            }
            case Item::File: {
                if (!item.song.path.empty()) return WX(item.song.path);  // a search result: where it is
                size_t slash = item.path.find_last_of(L"\\/");
                return WX(slash == std::wstring::npos ? item.path : item.path.substr(slash + 1));
            }
            case Item::Group: {
                const LibraryGroup& g = item.group;
                switch (level.kind) {
                    case Kind::Artists:
                        return Unknown(g.name, "Unknown artist") + ", " + SongsText(g.songs);
                    case Kind::Genres:
                        return Unknown(g.name, "Unknown genre") + ", " + SongsText(g.songs);
                    default: {
                        wxString text = Unknown(g.name, "Unknown album");
                        // An artist's albums need not say whose they are
                        if (!level.filter.byArtist) text += " - " + Unknown(g.artist, "Unknown artist");
                        if (g.year > 0) text += wxString::Format(" (%d)", g.year);
                        return text;
                    }
                }
            }
            case Item::Song:
            default: {
                const LibrarySong& s = item.song;
                wxString text;
                const bool inAlbum = level.filter.byAlbum;
                if (inAlbum && s.track > 0) text += wxString::Format("%d. ", s.track);
                text += WX(s.title);
                // In an album, the artist only where it is not the album's
                if (!s.artist.empty() && (!inAlbum || s.artist != s.albumArtist)) text += " - " + WX(s.artist);
                if (s.duration > 0) text += " [" + WX(FormatTime(s.duration)) + "]";
                return text;
            }
        }
    }

    wxString SelectedKey(const Page& page) const {
        int sel = page.list->GetSelection();
        if (sel < 0 || sel >= static_cast<int>(page.items.size())) return wxString();
        return page.items[sel].key;
    }

    // The index changed: the list shown now shows it too
    // While indexing, the list follows every few seconds (a big one takes a
    // moment to fetch); once indexing is done, at once
    void LibraryChanged() {
        int done, found;
        const auto now = std::chrono::steady_clock::now();
        if (LibraryProgress(done, found) && now - m_lastRefresh < std::chrono::seconds(3)) return;
        m_lastRefresh = now;
        Fill(CurrentTab());
    }

    void ShowStatus() {
        wxString text;
        int done = 0, found = 0;
        if (g_libraryFolders.empty()) {
            text = "The library has no folders yet. Add them in Options, on the Library tab.";
        } else {
            const Page& page = m_pages[CurrentTab()];
            int count = static_cast<int>(page.items.size());
            if (!page.items.empty() && page.items[0].type == Item::AllSongs) count--;
            text = wxString::Format(count == 1 ? "%d item" : "%d items", count);
            if (LibraryProgress(done, found)) text += wxString::Format(". Indexing: %d of %d files.", done, found);
        }
        if (m_status->GetLabel() != text) m_status->SetLabel(text);
    }

    // ------------------------------------------------------------------
    // Going down and back up
    // ------------------------------------------------------------------

    const Item* Selected() const {
        const Page& page = m_pages[CurrentTab()];
        int sel = page.list->GetSelection();
        if (sel < 0 || sel >= static_cast<int>(page.items.size())) return nullptr;
        return &page.items[sel];
    }

    void Open(Level next) {
        const int tab = CurrentTab();
        Page& page = m_pages[tab];
        page.levels.back().selected = SelectedKey(page);
        page.levels.push_back(std::move(next));
        // What was searched for found what is being opened; inside it, show all
        if (!m_search->IsEmpty()) {
            m_search->ChangeValue("");
            m_searchTimer.Stop();
        }
        Fill(tab);
        page.list->SetFocus();
    }

    void GoBack() {
        const int tab = CurrentTab();
        Page& page = m_pages[tab];
        if (page.levels.size() <= 1) return;
        page.levels.pop_back();
        Fill(tab);
        page.list->SetFocus();
    }

    // Enter, or a double click: into a group or folder, or play a song
    void Activate() {
        const Item* item = Selected();
        if (!item) return;
        const Level& level = m_pages[CurrentTab()].levels.back();
        switch (item->type) {
            case Item::Song:
            case Item::File:
                PlaySelected(false);
                return;
            case Item::AllSongs: {
                Level next{Kind::Songs, level.filter};
                next.sort = SortIndex(Kind::Songs, (int)(level.filter.byGenre && !level.filter.byArtist ? SongOrder::Artist
                                                                                                     : SongOrder::Album));
                Open(next);
                return;
            }
            case Item::Folder: {
                Level next{Kind::Folder};
                next.folder = item->path;
                next.sort = level.sort;
                Open(next);
                return;
            }
            case Item::Group: {
                Level next{Kind::Songs, level.filter};
                switch (level.kind) {
                    case Kind::Artists:
                        // An artist: its albums, with all its songs first
                        next.kind = Kind::Albums;
                        next.filter.byArtist = true;
                        next.filter.artist = item->group.name;
                        next.allSongs = true;
                        next.sort = SortIndex(Kind::Albums, (int)GroupOrder::Year);
                        break;
                    case Kind::Genres:
                        // A genre: its artists, with all its songs first
                        next.kind = Kind::Artists;
                        next.filter.byGenre = true;
                        next.filter.genre = item->group.name;
                        next.allSongs = true;
                        break;
                    default:
                        // An album: its songs in order
                        next.filter.byAlbum = true;
                        next.filter.album = item->group.name;
                        next.filter.albumArtist = item->group.artist;
                        next.sort = SortIndex(Kind::Songs, (int)SongOrder::TrackNumber);
                        break;
                }
                Open(next);
                return;
            }
        }
    }

    // ------------------------------------------------------------------
    // Playing
    // ------------------------------------------------------------------

    // The songs the selected item stands for, and which of them to start at
    bool SongsFor(const Item& item, std::vector<std::wstring>& paths, int& start) const {
        const Page& page = m_pages[CurrentTab()];
        const Level& level = page.levels.back();
        start = 0;
        paths.clear();
        if (item.type == Item::Song || item.type == Item::File) {
            // The list it is in, from it
            for (const Item& other : page.items) {
                if (other.type != Item::Song && other.type != Item::File) continue;
                if (&other == &item) start = static_cast<int>(paths.size());
                paths.push_back(other.path);
            }
            return true;
        }
        if (item.type == Item::Folder) {
            AddFilesFromFolder(item.path, paths);
            std::sort(paths.begin(), paths.end(), [](const std::wstring& a, const std::wstring& b) {
                return WStrNaturalCmp(a.c_str(), b.c_str()) < 0;
            });
            return !paths.empty();
        }
        LibraryFilter filter = level.filter;
        SongOrder order = SongOrder::Album;
        if (item.type == Item::Group) {
            switch (level.kind) {
                case Kind::Artists:
                    filter.byArtist = true;
                    filter.artist = item.group.name;
                    break;
                case Kind::Genres:
                    filter.byGenre = true;
                    filter.genre = item.group.name;
                    order = SongOrder::Artist;
                    break;
                default:
                    filter.byAlbum = true;
                    filter.album = item.group.name;
                    filter.albumArtist = item.group.artist;
                    order = SongOrder::TrackNumber;
                    break;
            }
        }
        for (const LibrarySong& song : LibrarySongs(filter, order)) paths.push_back(song.path);
        return !paths.empty();
    }

    // Plays the selection (a song: its list from it; anything else: all its songs),
    // or adds it to the end of the playlist
    void PlaySelected(bool add) {
        const Item* item = Selected();
        if (!item) return;
        std::vector<std::wstring> paths;
        int start = 0;
        if (!SongsFor(*item, paths, start)) {
            Speak("No songs");
            return;
        }
        if (add) {
            // A single song adds just itself
            if (item->type == Item::Song || item->type == Item::File) paths = {item->path};
            g_playlist.insert(g_playlist.end(), paths.begin(), paths.end());
            Speak(paths.size() == 1 ? std::string("Added") : std::to_string(paths.size()) + " added");
            return;
        }
        g_playlist = paths;
        g_currentTrack = -1;
        PlayTrack(start);
    }

    void OnCharHook(wxKeyEvent& event) {
        const Page& page = m_pages[CurrentTab()];
        wxWindow* focus = wxWindow::FindFocus();
        const bool inList = focus == page.list;
        switch (event.GetKeyCode()) {
            case WXK_RETURN:
            case WXK_NUMPAD_ENTER:
                if (inList) {
                    Activate();
                    return;
                }
                if (focus == m_search) {
                    // Search now, and on to the results
                    m_searchTimer.Stop();
                    Fill(CurrentTab());
                    page.list->SetFocus();
                    return;
                }
                break;
            case WXK_BACK:
                if (inList) {
                    GoBack();
                    return;
                }
                break;
            case WXK_ESCAPE:
                Destroy();
                return;
        }
        event.Skip();
    }

    wxTextCtrl* m_search = nullptr;
    wxNotebook* m_book = nullptr;
    wxStaticText* m_status = nullptr;
    Page m_pages[kTabs];
    wxTimer m_searchTimer;
    wxTimer m_statusTimer;
    std::chrono::steady_clock::time_point m_lastRefresh;
};

}  // namespace

void ShowLibraryDialog() {
    if (g_libraryDialog && !g_libraryDialog->IsBeingDeleted()) {
        g_libraryDialog->Raise();
        return;
    }
    auto* dialog = new LibraryDialog(GetMainWindow());
    dialog->Show();
}
