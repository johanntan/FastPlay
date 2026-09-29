#pragma once
#ifndef FASTPLAY_SCRUBBER_H
#define FASTPLAY_SCRUBBER_H

// Scrubbing: playing through the audio at speed while a seek key is held (see
// audio::StartScrub). While it lasts, the scrubber is what fills the output in
// place of the tempo processor.
//
// Its input is the decoded audio in the order it is to be heard: forward, or for
// scrubbing backward, the source reversed (the decode thread reads it backward).
// Tape: resampled, so pitch rises with speed, spinning up over a moment. Spring:
// time-stretched (Signalsmith Stretch), so pitch stays, and it winds up the longer
// it is held, doubling in speed every half second up to the top speed.

#include "audio.h"
#include "tempo_processor.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

namespace audio {

class Scrubber {
public:
    // `speed`: tape's speed, or spring's top speed (times normal).
    Scrubber(ScrubStyle style, int direction, float speed, int sourceRate, int outputRate, double start);
    ~Scrubber();

    // Fills `frames` of stereo output at the output rate: fewer if the source is
    // behind. Past the start or end of the source it is silence, staying there.
    int Fill(PcmSource& source, float* out, int frames);
    // Source seconds at output frame `played` (counted from the start of scrubbing)
    double PositionAt(uint64_t played);
    void SetSpeed(float speed) { m_topSpeed = speed; }

private:
    struct Stretch;

    ScrubStyle m_style;
    const int m_direction;
    std::atomic<float> m_topSpeed;
    const double m_sourceRate, m_outputRate;
    const double m_start;
    std::unique_ptr<Stretch> m_stretch;

    // What the resampler reads: source frames (tape) or stretched ones (spring),
    // interleaved, with a fractional read position into it
    std::vector<float> m_fifo;
    double m_pos = 0.0;
    uint64_t m_consumed = 0;  // source frames taken since the start (spring: after its pre-roll)
    bool m_sourceEnded = false;
    uint64_t m_produced = 0;  // output frames
    float m_last[2] = {};     // the last output frame, to fade from at the start or end

    struct Point {
        uint64_t out;
        double seconds;
    };
    std::deque<Point> m_points;

    double Speed() const;
    size_t FifoFrames() const { return m_fifo.size() / 2; }
    bool Supply(PcmSource& source, size_t need, double speed);
    void Resample(float* out, int frames, double step);
    double SourcePosition(double speed) const;
};

}  // namespace audio

#endif  // FASTPLAY_SCRUBBER_H
