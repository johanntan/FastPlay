#pragma once
#ifndef FASTPLAY_AUDIO_INTERNAL_H
#define FASTPLAY_AUDIO_INTERNAL_H

// Shared between the audio engine's own files (src/audio).

#include "audio.h"

#include <memory>
#include <string>

namespace audio {

// http:// or https://
bool IsNetworkPath(const std::wstring& path);

// The decoders behind OpenDecoder()
std::unique_ptr<Decoder> OpenFfmpegDecoder(const std::wstring& pathOrUrl, std::wstring& error);
std::unique_ptr<Decoder> OpenXheAacDecoder(const std::wstring& path, std::wstring& error);
std::unique_ptr<Decoder> OpenMidiDecoder(const std::wstring& path, std::wstring& error);
std::unique_ptr<Decoder> OpenTrackerDecoder(const std::wstring& path, std::wstring& error);
// A tracker module, by its extension (as libopenmpt knows them)
bool IsTrackerPath(const std::wstring& path);

// Whether a stream's title changed since the last call (FFmpeg streams only).
bool TakeStreamTitleChange(Decoder* decoder);

// For tests: what the device callback hands the device, block by block. With the
// environment variable FASTPLAY_NULL_AUDIO set, Init() opens miniaudio's null
// device, which takes audio in real time and plays none.
void SetOutputMonitor(TapProc proc, void* user);

}  // namespace audio

#endif  // FASTPLAY_AUDIO_INTERNAL_H
