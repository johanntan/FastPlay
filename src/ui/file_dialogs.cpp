// File > Open and File > Add Folder.

#include "ui/dialogs.h"
#include "ui/ui_common.h"

#include "globals.h"
#include "player.h"
#include "playlist_io.h"
#include "accessibility.h"

#include <wx/dirdlg.h>
#include <wx/filedlg.h>
#include "utils.h"
#include <algorithm>
#include <string>

void ShowOpenDialog() {
    wxFileDialog dlg(GetMainWindow(), "Open", "", "",
        "All Supported|*.mp3;*.mp2;*.mp1;*.wav;*.ogg;*.oga;*.flac;*.m4a;*.m4b;*.m4r;*.mp4;*.wma;*.wmv;*.aac;*.opus;*.aiff;*.aif;*.ape;*.wv;*.alac;*.mid;*.midi;*.rmi;*.kar;*.dff;*.dsf;*.cda;*.mod;*.s3m;*.xm;*.it;*.mtm;*.umx;*.m3u;*.m3u8;*.pls"
        "|Audio Files|*.mp3;*.mp2;*.mp1;*.wav;*.ogg;*.oga;*.flac;*.m4a;*.m4b;*.m4r;*.mp4;*.wma;*.aac;*.opus;*.aiff;*.aif;*.ape;*.wv;*.alac;*.mid;*.midi;*.rmi;*.kar;*.dff;*.dsf;*.cda;*.mod;*.s3m;*.xm;*.it;*.mtm;*.umx"
        "|Video Files|*.wmv;*.mp4"
        "|Playlists|*.m3u;*.m3u8;*.pls"
        "|All Files (*.*)|*.*",
        wxFD_OPEN | wxFD_FILE_MUST_EXIST | wxFD_MULTIPLE);
    if (dlg.ShowModal() != wxID_OK) return;

    wxArrayString paths;
    dlg.GetPaths(paths);
    if (paths.empty()) return;

    g_playlist.clear();
    g_currentTrack = -1;

    int startIndex = 0;
    if (paths.size() == 1) {
        std::wstring path = WS(paths[0]);
        if (IsPlaylistFile(path)) {
            g_playlist = ParsePlaylist(path);
        } else if (g_loadFolder) {
            // Expand to folder if option enabled
            startIndex = ExpandFileToFolder(path, g_playlist);
        } else {
            g_playlist.push_back(path);
        }
    } else {
        for (const auto& p : paths) {
            std::wstring path = WS(p);
            if (IsPlaylistFile(path)) {
                auto entries = ParsePlaylist(path);
                g_playlist.insert(g_playlist.end(), entries.begin(), entries.end());
            } else {
                g_playlist.push_back(path);
            }
        }
    }

    // Play selected file
    if (!g_playlist.empty()) {
        PlayTrack(startIndex);
    }
}

void ShowAddFolderDialog() {
    wxDirDialog dlg(GetMainWindow(), "Select folder to add to playlist", "",
                    wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK) return;

    // Collect all audio files recursively, in natural order ("2" before "10")
    std::vector<std::wstring> newFiles;
    AddFilesFromFolder(WS(dlg.GetPath()), newFiles);
    std::sort(newFiles.begin(), newFiles.end(), [](const std::wstring& a, const std::wstring& b) {
        return WStrNaturalCmp(a.c_str(), b.c_str()) < 0;
    });

    if (!newFiles.empty()) {
        // Replace playlist with new files and start from the beginning
        g_playlist = newFiles;
        g_currentTrack = -1;
        PlayTrack(0);
        Speak(std::to_string(newFiles.size()) + " files loaded");
    } else {
        Speak("No audio files found");
    }
}
