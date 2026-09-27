#pragma once
#ifndef FASTPLAY_PLAYLIST_IO_H
#define FASTPLAY_PLAYLIST_IO_H

// Local files and playlist files: which extensions FastPlay plays, expanding a file
// or folder into a list of them, and reading .m3u / .m3u8 / .pls playlists.

#include <string>
#include <vector>

bool IsSupportedAudioExt(const std::wstring& ext);

// Replace outFiles with every supported file in filePath's folder, and return the
// index of filePath among them.
int ExpandFileToFolder(const std::wstring& filePath, std::vector<std::wstring>& outFiles);

// Append every supported file under folder (recursively) to files.
void AddFilesFromFolder(const std::wstring& folder, std::vector<std::wstring>& files);

bool IsPlaylistFile(const std::wstring& path);
std::vector<std::wstring> ParsePlaylist(const std::wstring& playlistPath);

// A line of a playlist file: UTF-8 if it is valid UTF-8, otherwise the system's
// legacy code page (Latin-1 outside Windows).
std::wstring PlaylistLineToWide(const char* line);

#endif // FASTPLAY_PLAYLIST_IO_H
