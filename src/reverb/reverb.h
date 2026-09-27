// The simple reverb: a 16 line feedback delay network.
//
// The input is smeared by four short allpass diffusers, then fed into sixteen delay lines of prime lengths
// spread from 30 to 82 ms, mixed back into each other every pass through a 16 x 16 Hadamard matrix. Every
// line is slowly modulated at its own rate, a few samples either way, and loses energy and treble in
// proportion to its own length. Together that keeps the tail dense and even: no one short delay rings out
// (the "tube" sound), no mode stands still long enough to sound metallic, and every part of the tail dies
// away at the same rate.
//
// room_size sets the decay time (about 0.6 s at
// 0, 1.3 s at 0.5, 11 s at 1), damping how much faster the treble dies, and wet is scaled so 1/3 is unity
// and the same settings come out at the same level as before. Extras, off by default: a pre-delay and a low
// cut / high cut on the wet signal.
#pragma once
#include "dsp.h"
#include <vector>
#include <cstdint>

namespace fastplay::audio {

struct ReverbParams {
    float room_size = 0.5f;          // 0 .. 1: the decay time, from about 0.6 s to 11 s
    float damping = 0.25f;           // 0 .. 1: how much faster high frequencies die away
    float wet = 1.0f / 3.0f;         // wet gain (scaled by 3, so 1/3 is unity)
    float dry = 0.0f;                // dry pass-through (scaled by 2; 0 when used as a send effect)
    float width = 1.0f;              // 0 .. 1: stereo width of the tail
    float input_width = 0.0f;        // 0 sums the input to mono; above 0 keeps (and widens) its stereo image
    bool freeze = false;             // infinite hold: no decay, no damping, no new input
    float pre_delay_ms = 0.0f;       // 0 .. 250, extra
    float low_cut_hz = 0.0f;         // high pass on the wet signal, 0 = off, extra
    float high_cut_hz = 0.0f;        // low pass on the wet signal, 0 = off, extra
};

class Reverb {
public:
    void init(int sample_rate);
    void set_params(const ReverbParams& p);
    const ReverbParams& params() const { return p_; }
    void reset();
    // out_l/out_r += dry * in + wet. Buffers must not alias.
    void process(const float* in_l, const float* in_r, float* out_l, float* out_r, int frames);
    // True when the tail has decayed to silence and there was no recent input;
    // callers may skip process() until new input arrives.
    bool idle() const { return idle_; }

    static constexpr int kLines = 16;
    static constexpr int kDiffusers = 4;

private:
    // A delay line read slightly behind its nominal length, the read point swaying slowly.
    struct Line {
        std::vector<float> buf;
        int mask = 0, write = 0;
        float length = 0.0f;         // nominal delay in samples
        float gain = 0.0f;           // per pass decay for the current decay time
        float damp = 0.0f;           // one pole lowpass coefficient for the current damping
        float lp = 0.0f;             // lowpass state
        float mod_sin = 0.0f, mod_cos = 1.0f, mod_step_sin = 0.0f, mod_step_cos = 1.0f;
    };
    struct Diffuser {
        std::vector<float> buf;
        int idx = 0;
        float gain = 0.7f;
        float process(float in) {
            const float d = dsp::undenormal(buf[idx]);
            const float v = in + d * gain;
            buf[idx] = v;
            if (++idx >= static_cast<int>(buf.size())) idx = 0;
            return d - v * gain;
        }
    };

    void update_derived();

    int rate_ = 48000;
    ReverbParams p_;
    bool idle_ = true;
    Line lines_[kLines];
    Diffuser diff_l_[kDiffusers], diff_r_[kDiffusers];
    float mod_depth_ = 6.0f;         // samples either side
    float input_gain_ = 0.0f;
    std::vector<float> pre_l_, pre_r_;
    int pre_mask_ = 0, pre_write_ = 0, pre_len_ = 0;
    int silent_frames_ = 0;          // input frames since the last non-silent one
    int quiet_frames_ = 0;           // output frames since the tail was last above the idle threshold
    int settle_frames_ = 0;          // longest path from input to output (diffusers + longest line), in frames
    dsp::Biquad low_cut_[2], high_cut_[2];
    dsp::Smoother wet1_, wet2_, dry_;
};

} // namespace fastplay::audio
