#pragma once
#ifndef FASTPLAY_RECORDER_H
#define FASTPLAY_RECORDER_H

// Recording what plays to a file: WAV (16-bit), MP3 (LAME), OGG Vorbis (libvorbis)
// or FLAC (FFmpeg). The audio engine's tap hands over each block of stereo float;
// it is queued, and encoded and written on a thread of the recorder's own, so a
// slow disk never holds up playback.

#include <memory>
#include <string>

namespace audio {

enum class RecordFormat { Wav = 0, Mp3 = 1, Ogg = 2, Flac = 3 };

class Recorder {
public:
    // A recording of stereo audio at `sampleRate` into `path`; `bitrateKbps` for
    // MP3 (constant) and OGG (nominal). Null with `error` set if it cannot start.
    static std::unique_ptr<Recorder> Start(const std::wstring& path, RecordFormat format, int bitrateKbps,
                                           int sampleRate, std::wstring& error);
    // Encodes what is still queued and finishes the file.
    virtual ~Recorder() = default;

    // Queues a block (from the mix thread, through the engine's tap).
    virtual void Write(const float* samples, int frames) = 0;
};

}  // namespace audio

#endif  // FASTPLAY_RECORDER_H
