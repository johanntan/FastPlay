#include "reverb.h"
#include <cmath>
#include <algorithm>

namespace fastplay::audio {

namespace {

// Delay line lengths in samples at 48 kHz: primes from 30 to 82 ms, so no two lines share a period.
constexpr int kLineTuning[Reverb::kLines] = { 1423, 1597, 1777, 1889, 2029, 2213, 2381, 2539, 2713, 2887, 3049, 3221, 3389, 3571, 3761, 3947 };
// Each line's modulation rate (Hz), all different so the lines never sway together.
constexpr float kModRate[Reverb::kLines] = { 0.13f, 0.71f, 0.29f, 0.87f, 0.41f, 0.19f, 0.61f, 0.37f, 0.83f, 0.23f, 0.53f, 0.91f, 0.31f, 0.67f, 0.47f, 0.79f };
// Input diffusers (samples at 48 kHz, gain), a slightly different set for the right channel.
constexpr int kDiffTuningL[Reverb::kDiffusers] = { 229, 173, 611, 447 };
constexpr int kDiffTuningR[Reverb::kDiffusers] = { 241, 163, 587, 463 };
constexpr float kDiffGain[Reverb::kDiffusers] = { 0.75f, 0.75f, 0.625f, 0.625f };
// Which way round each line feeds each output channel: two rows of the Hadamard matrix, so the two
// channels hear the same tail through different mixes and come out decorrelated.
constexpr float kOutL[Reverb::kLines] = { 1, -1, 1, -1, 1, -1, 1, -1, 1, -1, 1, -1, 1, -1, 1, -1 };
constexpr float kOutR[Reverb::kLines] = { 1, 1, -1, -1, 1, 1, -1, -1, 1, 1, -1, -1, 1, 1, -1, -1 };

// Decay time for a room size: 0.6 s at 0, 1.3 s at 0.5, 11 s at 1.
constexpr double kRefPass = 1378.0 / 44100.0;    // seconds, the pass the scale was defined on
double decay_seconds(float room_size) {
    const double fb = room_size * 0.28 + 0.7;
    return 3.0 * kRefPass / -std::log10(fb);
}
// Treble loss per reference pass, as a one pole lowpass coefficient, for a damping setting.
float damping_coefficient(float damping) { return damping * 0.8f; }
// Level calibration, measured on noise: the same settings give the same tail
// level they always have. A longer room comes out a little quieter from a network this dense, so it gets a
// little more input.
constexpr float kInputGain = 0.173f;
constexpr float kRoomGainDb = 4.7f;
constexpr float kOutputScale = 0.25f;
constexpr float kScaleWet = 3.0f;
constexpr float kScaleDry = 2.0f;

int scaled(int rate, int value) { return std::max(1, static_cast<int>(static_cast<double>(value) * rate / 48000.0)); }
int next_pow2(int v) { int p = 1; while (p < v) p <<= 1; return p; }

// In place 16 point fast Walsh-Hadamard transform, scaled to keep energy.
inline void hadamard16(float* v) {
    for (int h = 1; h < 16; h <<= 1) {
        for (int i = 0; i < 16; i += h << 1) {
            for (int j = i; j < i + h; ++j) {
                const float a = v[j], b = v[j + h];
                v[j] = a + b;
                v[j + h] = a - b;
            }
        }
    }
    for (int i = 0; i < 16; ++i) v[i] *= 0.25f;
}

} // namespace

void Reverb::init(int sample_rate) {
    rate_ = sample_rate;
    mod_depth_ = 6.0f * static_cast<float>(rate_) / 48000.0f;
    for (int i = 0; i < kLines; ++i) {
        Line& l = lines_[i];
        l.length = static_cast<float>(scaled(rate_, kLineTuning[i]));
        const int size = next_pow2(static_cast<int>(l.length + mod_depth_) + 4);
        l.buf.assign(static_cast<size_t>(size), 0.0f);
        l.mask = size - 1;
        const double step = 2.0 * 3.14159265358979323846 * kModRate[i] / rate_;
        l.mod_step_sin = static_cast<float>(std::sin(step));
        l.mod_step_cos = static_cast<float>(std::cos(step));
    }
    for (int i = 0; i < kDiffusers; ++i) {
        diff_l_[i].buf.assign(static_cast<size_t>(scaled(rate_, kDiffTuningL[i])), 0.0f);
        diff_r_[i].buf.assign(static_cast<size_t>(scaled(rate_, kDiffTuningR[i])), 0.0f);
        diff_l_[i].gain = diff_r_[i].gain = kDiffGain[i];
    }
    // The longest way from the input to the output: every diffuser, then the longest line at its furthest sway.
    int diffusion_l = 0, diffusion_r = 0;
    for (int i = 0; i < kDiffusers; ++i) { diffusion_l += static_cast<int>(diff_l_[i].buf.size()); diffusion_r += static_cast<int>(diff_r_[i].buf.size()); }
    settle_frames_ = std::max(diffusion_l, diffusion_r) + static_cast<int>(lines_[kLines - 1].length + mod_depth_) + 2;
    const int pre_size = next_pow2(static_cast<int>(0.25f * rate_) + 8);
    pre_mask_ = pre_size - 1;
    pre_l_.assign(static_cast<size_t>(pre_size), 0.0f);
    pre_r_.assign(static_cast<size_t>(pre_size), 0.0f);
    reset();
    update_derived();
    wet1_.reset(wet1_.target());
    wet2_.reset(wet2_.target());
    dry_.reset(dry_.target());
}

void Reverb::reset() {
    for (int i = 0; i < kLines; ++i) {
        Line& l = lines_[i];
        std::fill(l.buf.begin(), l.buf.end(), 0.0f);
        l.write = 0;
        l.lp = 0.0f;
        // Start every line's sway at a different point, so they are spread from the first sample.
        const double phase = 2.0 * 3.14159265358979323846 * i / kLines;
        l.mod_sin = static_cast<float>(std::sin(phase));
        l.mod_cos = static_cast<float>(std::cos(phase));
    }
    for (int i = 0; i < kDiffusers; ++i) {
        for (Diffuser* d : { &diff_l_[i], &diff_r_[i] }) { std::fill(d->buf.begin(), d->buf.end(), 0.0f); d->idx = 0; }
    }
    std::fill(pre_l_.begin(), pre_l_.end(), 0.0f);
    std::fill(pre_r_.begin(), pre_r_.end(), 0.0f);
    pre_write_ = 0;
    silent_frames_ = 0;
    quiet_frames_ = 0;
    for (auto& f : low_cut_) f.reset();
    for (auto& f : high_cut_) f.reset();
    idle_ = true;
}

void Reverb::set_params(const ReverbParams& p) {
    p_ = p;
    p_.room_size = dsp::clampf(p_.room_size, 0.0f, 1.0f);
    p_.damping = dsp::clampf(p_.damping, 0.0f, 1.0f);
    p_.width = dsp::clampf(p_.width, 0.0f, 1.0f);
    p_.input_width = std::max(0.0f, p_.input_width);
    p_.wet = std::max(0.0f, p_.wet);
    p_.dry = std::max(0.0f, p_.dry);
    p_.pre_delay_ms = dsp::clampf(p_.pre_delay_ms, 0.0f, 250.0f);
    update_derived();
}

void Reverb::update_derived() {
    const double decay = decay_seconds(p_.room_size);
    const float ref_damp = damping_coefficient(p_.damping);
    // The treble loss of one reference pass at Nyquist, in dB; each line loses its share by its own length.
    const double ref_loss_db = 20.0 * std::log10(std::max(1e-6, (1.0 - ref_damp) / (1.0 + ref_damp)));
    const double ref_samples = kRefPass * rate_;
    for (int i = 0; i < kLines; ++i) {
        Line& l = lines_[i];
        if (p_.freeze) { l.gain = 1.0f; l.damp = 0.0f; continue; }
        l.gain = static_cast<float>(std::pow(10.0, -3.0 * l.length / (decay * rate_)));
        const double h = std::pow(10.0, ref_loss_db * l.length / ref_samples / 20.0);
        l.damp = static_cast<float>((1.0 - h) / (1.0 + h));
    }
    input_gain_ = p_.freeze ? 0.0f : kInputGain * dsp::db_to_gain(kRoomGainDb * p_.room_size);
    const float wet = p_.wet * kScaleWet;
    const int ramp = rate_ / 50; // 20 ms, so live changes do not click
    wet1_.set_target(wet * (p_.width / 2.0f + 0.5f), ramp);
    wet2_.set_target(wet * ((1.0f - p_.width) / 2.0f), ramp);
    dry_.set_target(p_.dry * kScaleDry, ramp);
    pre_len_ = std::min(static_cast<int>(p_.pre_delay_ms * rate_ / 1000.0f), pre_mask_);
    for (int c = 0; c < 2; ++c) {
        if (p_.low_cut_hz > 0.0f) low_cut_[c].set(dsp::Biquad::Type::HighPass, p_.low_cut_hz, 0.707f, 0.0f, static_cast<float>(rate_));
        if (p_.high_cut_hz > 0.0f) high_cut_[c].set(dsp::Biquad::Type::LowPass, p_.high_cut_hz, 0.707f, 0.0f, static_cast<float>(rate_));
    }
}

void Reverb::process(const float* in_l, const float* in_r, float* out_l, float* out_r, int frames) {
    float in_peak = 0.0f;
    for (int i = 0; i < frames; ++i) in_peak = std::max(in_peak, std::max(std::fabs(in_l[i]), std::fabs(in_r[i])));
    if (in_peak > 1e-7f) {
        idle_ = false;
        silent_frames_ = 0;
    } else {
        silent_frames_ = std::min(silent_frames_ + frames, 1 << 30);
        if (idle_ && !p_.freeze) {
            if (dry_.current() != 0.0f || dry_.is_ramping())
                for (int i = 0; i < frames; ++i) { const float d = dry_.next(); out_l[i] += in_l[i] * d; out_r[i] += in_r[i] * d; }
            return;
        }
    }

    // Stereo input: summed to mono, or kept as mid/side scaled by input_width.
    const bool stereo_in = p_.input_width > 0.0f;
    const float tmp = 1.0f / std::max(1.0f + p_.input_width, 2.0f);
    const float coef_mid = tmp, coef_side = p_.input_width * tmp;
    const bool low = p_.low_cut_hz > 0.0f, high = p_.high_cut_hz > 0.0f;
    float tail_peak = 0.0f;
    float v[kLines];

    for (int n = 0; n < frames; ++n) {
        // Optional pre-delay in front of the whole reverb.
        pre_l_[pre_write_] = in_l[n];
        pre_r_[pre_write_] = in_r[n];
        const int read = (pre_write_ - pre_len_) & pre_mask_;
        const float xl = pre_l_[read], xr = pre_r_[read];
        pre_write_ = (pre_write_ + 1) & pre_mask_;

        float input_left, input_right;
        if (stereo_in) {
            const float mid = (xl + xr) * coef_mid;
            const float side = (xr - xl) * coef_side;
            input_left = (mid - side) * (input_gain_ * 2.0f);
            input_right = (mid + side) * (input_gain_ * 2.0f);
        } else {
            input_left = input_right = (xl + xr) * input_gain_;
        }
        for (int i = 0; i < kDiffusers; ++i) {
            input_left = diff_l_[i].process(input_left);
            input_right = diff_r_[i].process(input_right);
        }

        // Read every line at its swaying delay, and take the outputs from what comes back.
        float ol = 0.0f, orr = 0.0f;
        for (int i = 0; i < kLines; ++i) {
            Line& l = lines_[i];
            const float s = l.mod_sin * l.mod_step_cos + l.mod_cos * l.mod_step_sin;
            l.mod_cos = l.mod_cos * l.mod_step_cos - l.mod_sin * l.mod_step_sin;
            l.mod_sin = s;
            const float pos = static_cast<float>(l.write) - l.length - mod_depth_ * s;
            const float fl = std::floor(pos);
            const int i0 = static_cast<int>(fl) & l.mask;
            const float frac = pos - fl;
            const float a = l.buf[static_cast<size_t>(i0)], b = l.buf[static_cast<size_t>((i0 + 1) & l.mask)];
            const float out = dsp::undenormal(a + (b - a) * frac);
            ol += out * kOutL[i];
            orr += out * kOutR[i];
            // Lose this pass's share of energy and treble.
            l.lp = dsp::undenormal(out + (l.lp - out) * l.damp);
            v[i] = l.lp * l.gain;
        }
        hadamard16(v);
        for (int i = 0; i < kLines; ++i) {
            Line& l = lines_[i];
            l.buf[static_cast<size_t>(l.write)] = v[i] + ((i & 1) ? input_right : input_left);
            l.write = (l.write + 1) & l.mask;
        }
        ol *= kOutputScale;
        orr *= kOutputScale;

        const float w1 = wet1_.next(), w2 = wet2_.next();
        float wl = ol * w1 + orr * w2;
        float wr = orr * w1 + ol * w2;
        if (low) { wl = low_cut_[0].process(wl); wr = low_cut_[1].process(wr); }
        if (high) { wl = high_cut_[0].process(wl); wr = high_cut_[1].process(wr); }
        tail_peak = std::max(tail_peak, std::max(std::fabs(wl), std::fabs(wr)));

        const float d = dry_.next();
        out_l[n] += in_l[n] * d + wl;
        out_r[n] += in_r[n] * d + wr;
    }
    // Keep the swaying on the unit circle.
    for (Line& l : lines_) {
        const float r = 1.0f / std::sqrt(l.mod_sin * l.mod_sin + l.mod_cos * l.mod_cos);
        l.mod_sin *= r;
        l.mod_cos *= r;
    }

    // Idle once the input has been silent long enough to have passed the pre-delay, the diffusers and the longest
    // line, and the tail has stayed below -140 dB for as long. Nothing comes out before the shortest line (about
    // 30 ms), so a click shorter than that is gone from the input while the output is still silent: going idle then
    // dropped its whole tail.
    quiet_frames_ = tail_peak < 1e-7f ? std::min(quiet_frames_ + frames, 1 << 30) : 0;
    if (in_peak <= 1e-7f && silent_frames_ > pre_len_ + settle_frames_ && quiet_frames_ > settle_frames_ && !p_.freeze) idle_ = true;
}

} // namespace fastplay::audio
