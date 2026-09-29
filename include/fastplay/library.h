#pragma once
#ifndef FASTPLAY_LIBRARY_H
#define FASTPLAY_LIBRARY_H

// The library (Ctrl+L): the audio files in the folders chosen in Options > Library
// (g_libraryFolders), indexed in library.db beside the settings. It is brought up
// to date in the background when FastPlay starts and whenever the folders are
// changed, and kept so while it runs by watching the folders (folder_watch.h).
//
// A folder can be left out of the tagged views (songs, artists, albums, genres):
// its files are then only listed by name, in the folders view and its search.
// Queries are made from the UI thread; the indexing has a thread of its own.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct LibrarySong {
    int64_t id = 0;
    std::wstring path;
    std::wstring title;        // the tag, or the file name without its extension
    std::wstring artist, album, albumArtist, genre;  // albumArtist: the tag, or the artist
    int year = 0, track = 0, disc = 0;
    double duration = 0;
    int64_t added = 0;         // when it came into the library (Unix time)
};

// An artist, album or genre, and how many songs it has
struct LibraryGroup {
    std::wstring name;         // empty: unknown
    std::wstring artist;       // an album's artist
    int songs = 0;
    int year = 0;              // an album's
};

enum class SongOrder { Title, Artist, Album, TrackNumber, Duration, Year, DateAdded, FileName };
enum class GroupOrder { Name, Songs, Artist, Year };

// What a list holds: all of it, or what is in an artist, album or genre, and what
// matches a search (every word, in the title, artist, album or file name).
struct LibraryFilter {
    std::wstring search;
    bool byArtist = false, byAlbum = false, byGenre = false;
    std::wstring artist;
    std::wstring album, albumArtist;
    std::wstring genre;
};

// Starts the library: opens the index, brings it up to date in the background and
// watches the folders. At startup, after the settings are loaded.
void StartLibrary();
void StopLibrary();
// g_libraryFolders changed (Options): folders gone are dropped, new ones indexed.
void LibraryFoldersChanged();
// Look at every file again, tags and all.
void RescanLibrary();
// While indexing: how many files have been looked at of how many found so far.
// False when the index is up to date.
bool LibraryProgress(int& done, int& found);
// Called on the UI thread when the index has changed (at most about once a second).
void SetLibraryChangedHandler(std::function<void()> handler);

std::vector<LibrarySong> LibrarySongs(const LibraryFilter& filter, SongOrder order);
std::vector<LibraryGroup> LibraryArtists(const LibraryFilter& filter, GroupOrder order);
std::vector<LibraryGroup> LibraryAlbums(const LibraryFilter& filter, GroupOrder order);
std::vector<LibraryGroup> LibraryGenres(const LibraryFilter& filter, GroupOrder order);
// Every file in the library's folders, tagged or not, matching `search`
std::vector<LibrarySong> LibrarySearchFiles(const std::wstring& search, SongOrder order);

// A folder's subfolders and audio files, as they are on disk now, for the folders view
struct LibraryEntry {
    std::wstring name, path;
    bool isFolder = false;
    int64_t modified = 0;      // seconds, for sorting
};
enum class FolderOrder { Name, Modified };
std::vector<LibraryEntry> LibraryListFolder(const std::wstring& folder, FolderOrder order);

#endif  // FASTPLAY_LIBRARY_H
