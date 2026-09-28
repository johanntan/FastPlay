#pragma once
#ifndef FASTPLAY_XHEAAC_H
#define FASTPLAY_XHEAAC_H

// xHE-AAC (MPEG-D USAC) in MP4 files, which no BASS decoder handles. The file is
// decoded with Fraunhofer's FDK AAC and served to BASS as a plain PCM WAV file, so
// it plays, seeks and reports its length like any other file.

#include "bass.h"

#include <string>

// Whether the file is an MP4 whose audio is xHE-AAC.
bool IsXheAacFile(const std::wstring& path);

// A BASS stream of the file, `flags` as for BASS_StreamCreateFile (BASS_STREAM_DECODE,
// BASS_SAMPLE_FLOAT...). 0 if the file could not be opened or decoded.
HSTREAM CreateXheAacStream(const std::wstring& path, DWORD flags);

// The file's tags, as BASS_TAG_MP4 would give them ("TITLE=...", "ARTIST=..."
// and so on, each null-terminated, the list ended by another null), or null if
// the stream is not one of these or has no tags.
const char* XheAacTags(HSTREAM stream);

// The stream's average bitrate in kbps, or 0 if it is not one of these.
int XheAacBitrate(HSTREAM stream);

#endif // FASTPLAY_XHEAAC_H
