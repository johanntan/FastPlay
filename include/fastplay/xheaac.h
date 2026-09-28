#pragma once
#ifndef FASTPLAY_XHEAAC_H
#define FASTPLAY_XHEAAC_H

// xHE-AAC (MPEG-D USAC) in MP4 files, decoded with Fraunhofer's FDK AAC. The audio
// engine opens one through audio::OpenXheAacDecoder (src/audio/audio_internal.h).

#include <string>

// Whether the file is an MP4 whose audio is xHE-AAC.
bool IsXheAacFile(const std::wstring& path);

#endif // FASTPLAY_XHEAAC_H
