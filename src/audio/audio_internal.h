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

// Whether a stream's title changed since the last call (FFmpeg streams only).
bool TakeStreamTitleChange(Decoder* decoder);

}  // namespace audio

#endif  // FASTPLAY_AUDIO_INTERNAL_H
