#pragma once
#ifndef FASTPLAY_MP4_REMUX_H
#define FASTPLAY_MP4_REMUX_H

#include <string>

// YouTube serves audio as fragmented MP4 (the adaptive streaming layout), which not
// every decoder reads: BASS_AAC does not, and on Windows only Media Foundation does.
// This rewrites one as an ordinary MP4 with the same audio, tagged with the given
// title and artist. Handles one audio track. False if the input is not a fragmented
// MP4 FastPlay can rewrite.
bool DefragmentMp4(const std::wstring& inPath, const std::wstring& outPath,
                   const std::wstring& title, const std::wstring& artist);

#endif // FASTPLAY_MP4_REMUX_H
