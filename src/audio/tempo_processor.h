#pragma once
#ifndef FASTPLAY_TEMPO_PROCESSOR_H
#define FASTPLAY_TEMPO_PROCESSOR_H

// The tempo stage of the audio engine: changes tempo and pitch (Speedy or
// Signalsmith Stretch), then rate and sample rate (a resampler, taking the audio
// to the output device's rate). With tempo and pitch both at 0 the audio skips
// the stretcher and is only resampled.
//
// It also keeps the map from the output it produced back to source time, so the
// position reported is what is being heard, not how far the decoder has read.

#include "types.h"

#include <cstdint>
#include <memory>

namespace audio {

// The processor's input: decoded stereo float at the source's rate, buffered by
// the decode thread and read without waiting.
class PcmSource {
public:
    virtual ~PcmSource() = default;
    // Frames buffered now
    virtual int Available() = 0;
    // Up to `frames` of what is buffered
    virtual int Read(float* out, int frames) = 0;
    // The decoder is done and everything buffered has been read
    virtual bool Ended() = 0;
};

class TempoProcessor {
public:
    static std::unique_ptr<TempoProcessor> Create(TempoAlgorithm algorithm, int sourceRate, int outputRate);
    virtual ~TempoProcessor() = default;

    // Starts again at source time `seconds` (after a seek), dropping held audio.
    virtual void Restart(double seconds) = 0;
    // Up to `frames` stereo frames at the output rate into `out`: fewer if the
    // source is behind. `ended` is set once everything has come out.
    virtual int Fill(PcmSource& source, float* out, int frames, bool& ended) = 0;
    // Source seconds at output frame `played`, counted from the last Restart().
    virtual double PositionAt(uint64_t played) = 0;

    // Tempo in percent, pitch in semitones, rate as a multiplier (speed and pitch).
    virtual void SetTempo(float percent) = 0;
    virtual void SetPitch(float semitones) = 0;
    virtual void SetRate(float rate) = 0;
};

}  // namespace audio

#endif  // FASTPLAY_TEMPO_PROCESSOR_H
