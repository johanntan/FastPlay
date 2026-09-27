// The EFX model reverb: the parameter set of the OpenAL EFX / EAX reverb, rendered by its own network.
//
// Written from the published EFX model, not from any implementation of it. A reverb is two parts fed from one
// delay line after the input filters (gain, and shelves for gain_hf / gain_lf):
//
//   early reflections  a tapped delay: four taps from reflections_delay on, spread wider as density grows, each
//                      leaning to one side, then two more generations of the same taps through a 4 x 4 mix, each
//                      generation weaker by the decay time over its length. diffusion smears every tap through
//                      two short allpasses. Level reflections_gain.
//   late reverb        taken reflections_delay + late_reverb_delay after the input: an optional cyclic echo
//                      (echo_time, echo_depth), four allpass diffusers (diffusion), then eight delay lines mixed
//                      through an 8 x 8 Hadamard matrix. The lines grow with density; each loses, per pass, what
//                      makes the tail fall 60 dB in decay_time, and a low and a high shelf make the bands around
//                      lf_reference and hf_reference fall in decay_time * decay_lf_ratio and * decay_hf_ratio
//                      (the high ratio capped by the air absorption when decay_hf_limit is set). The lines sway
//                      at modulation_time by modulation_depth. Level late_reverb_gain, normalised so the level
//                      does not change with the decay time.
//
// Each part leaves through its pan vector (x is left/right), and everything through boost_db. The levels,
// the smear of the diffusers and the spread of the taps were matched by measurement against OpenAL Soft's
// output of the same settings (rendered as a black box), so a room tuned by ear there sounds alike here.
#pragma once
#include "dsp.h"
#include <cstdint>
#include <vector>

namespace fastplay::audio {

struct EfxReverbParams {
    float density = 1.0f;                  // 0 .. 1
    float diffusion = 1.0f;                // 0 .. 1
    float gain = 0.32f;                    // 0 .. 1
    float gain_hf = 0.89f;                 // 0 .. 1
    float gain_lf = 1.0f;                  // 0 .. 1
    float decay_time = 1.49f;              // 0.1 .. 20 s
    float decay_hf_ratio = 0.83f;          // 0.1 .. 2
    float decay_lf_ratio = 1.0f;           // 0.1 .. 2
    float reflections_gain = 0.05f;        // 0 .. 3.16
    float reflections_delay = 0.007f;      // 0 .. 0.3 s
    float reflections_pan[3] = { 0, 0, 0 };
    float late_reverb_gain = 1.26f;        // 0 .. 10
    float late_reverb_delay = 0.011f;      // 0 .. 0.1 s
    float late_reverb_pan[3] = { 0, 0, 0 };
    float echo_time = 0.25f;               // 0.075 .. 0.25 s
    float echo_depth = 0.0f;               // 0 .. 1
    float modulation_time = 0.25f;         // 0.04 .. 4 s
    float modulation_depth = 0.0f;         // 0 .. 1
    float air_absorption_gain_hf = 0.994f; // 0.892 .. 1
    float hf_reference = 5000.0f;          // 1000 .. 20000 Hz
    float lf_reference = 250.0f;           // 20 .. 1000 Hz
    float room_rolloff_factor = 0.0f;      // 0 .. 10 (a per source distance setting in EFX; kept, not used here)
    bool decay_hf_limit = true;
    float boost_db = 0.0f;                 // -24 .. 24 dB on the whole output (OpenAL Soft's [reverb] boost)
};

class EfxReverb {
public:
    void init(int sample_rate);
    void set_params(const EfxReverbParams& p);
    const EfxReverbParams& params() const { return p_; }
    void reset();
    // out_l/out_r += wet. The input is summed to mono. Buffers must not alias.
    void process(const float* in_l, const float* in_r, float* out_l, float* out_r, int frames);
    // True when the tail has decayed to silence and there was no recent input;
    // callers may skip process() until new input arrives.
    bool idle() const { return idle_; }

    static constexpr int kLines = 8;       // late network
    static constexpr int kTaps = 4;        // early reflections per generation
    static constexpr int kDiffusers = 4;   // late input allpasses

private:
    struct Delay {
        std::vector<float> buf;
        int mask = 0, write = 0;
        void alloc(int max_delay);
        void clear();
        inline void push(float v) { buf[static_cast<size_t>(write)] = v; write = (write + 1) & mask; }
        // The sample written `delay` pushes ago (1 = the last one).
        inline float tap(int delay) const { return buf[static_cast<size_t>((write - delay) & mask)]; }
    };
    // Schroeder allpass of a variable length within its buffer.
    struct Allpass {
        Delay d;
        int length = 1;
        float gain = 0.0f;
        inline float process(float in) {
            const float z = d.tap(length);
            const float v = dsp::undenormal(in + z * gain);
            d.push(v);
            return z - v * gain;
        }
    };
    struct Line {
        Delay d;
        int length = 1;               // samples
        float gain = 0.0f;            // mid band gain per pass
        float hf_coef = 0.0f;         // one pole lowpass coefficient when the treble dies faster
        bool hf_lift = false;         // or a first order high shelf when it dies slower
        float hf_b0 = 1.0f, hf_b1 = 0.0f, hf_a1 = 0.0f;
        float hf_state = 0.0f, hf_state2 = 0.0f;
        float lf_b0 = 1.0f, lf_b1 = 0.0f, lf_a1 = 0.0f;   // first order low shelf for decay_lf_ratio
        float lf_state = 0.0f, lf_state2 = 0.0f;
        float mod_sin = 0.0f, mod_cos = 1.0f;
    };

    void update_derived();

    int rate_ = 48000;
    EfxReverbParams p_;
    bool idle_ = true;
    int silent_frames_ = 0, quiet_frames_ = 0, settle_frames_ = 0;

    dsp::Biquad shelf_hf_, shelf_lf_;
    bool use_hf_shelf_ = false, use_lf_shelf_ = false;
    Delay input_;
    // early reflections
    int tap_delay_[kTaps] = {};
    Allpass er_diff_[kTaps][2];
    Delay er_gen_[2][kTaps];
    int er_len_[2][kTaps] = {};
    float er_gen_gain_[2][kTaps] = {};
    // late reverb
    int late_delay_ = 0;
    Delay echo_;
    int echo_len_ = 1;
    float echo_fb_ = 0.0f, echo_mix_ = 0.0f, echo_norm_ = 1.0f;
    Allpass diff_[kDiffusers];
    Delay onset_;                     // the diffused input, tapped twice to start the late reverb
    int onset_len_[2] = { 1, 1 };
    int tail_start_ = 1;              // where the lines are fed from
    Line lines_[kLines];
    float mix_[kLines][kLines] = {};  // feedback matrix: diffusion turns it from a ring into a full mix
    float inject_gain_[kLines] = {};  // how the input enters the lines
    float tail_gain_ = 0.0f;          // the recirculating tail against the onset
    float raw_mix_ = 0.0f, diffused_mix_ = 1.0f;
    float mod_depth_ = 0.0f;          // samples
    float mod_step_sin_ = 0.0f, mod_step_cos_ = 1.0f;
    bool lf_on_ = false;
    // Output mixes: (left, right) of each part to (left, right) out, gains, pan and width folded in.
    dsp::Smoother early_mix_[4], late_mix_[4];
};

} // namespace fastplay::audio
