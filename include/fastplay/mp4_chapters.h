#pragma once
#ifndef FASTPLAY_MP4_CHAPTERS_H
#define FASTPLAY_MP4_CHAPTERS_H

#include "globals.h"

#include <string>
#include <vector>

// The chapters of an MP4 file (M4B audiobooks, M4A, MP4), which BASS does not read.
// MP4 keeps them in one of two places, and this reads both: the QuickTime chapter
// track, a text track the audio track points at, whose samples are the titles (Apple
// Books, Audible conversions, m4b-tool, ffmpeg), and Nero's chpl box, a plain list
// of times and titles (ffmpeg, older tools). False if the file is not an MP4 or has
// no chapters. Chapters come back in order of position.
bool ReadMp4Chapters(const std::wstring& path, std::vector<Chapter>& chapters);

#endif // FASTPLAY_MP4_CHAPTERS_H
