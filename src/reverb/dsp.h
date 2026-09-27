// Small inline DSP building blocks shared by the audio engine.
// Everything here is allocation free and safe to use on the audio thread.
#pragma once
#include <cmath>
#include <cstdint>
#include <algorithm>

namespace fastplay::dsp {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 6.28318530717958647692f;

inline float db_to_gain(float db) { return std::pow(10.0f, db * 0.05f); }
inline float gain_to_db(float gain) { return gain <= 1e-9f ? -180.0f : 20.0f * std::log10(gain); }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

// Flush denormals to zero to keep filters from slowing down when idle.
inline float undenormal(float v) { return std::fabs(v) < 1e-30f ? 0.0f : v; }

// Pan laws. pan is -1 (left) .. 0 (centre) .. +1 (right).
struct StereoGain { float left, right; };

// BASS style linear pan: the louder channel stays at unity. Matches old NVGT/BGT.
inline StereoGain pan_linear(float pan) {
    pan = clampf(pan, -1.0f, 1.0f);
    return { pan > 0.0f ? 1.0f - pan : 1.0f, pan < 0.0f ? 1.0f + pan : 1.0f };
}

// NVGT/BGT/BASS "log curve" pan: pan is in dB units -100..100; the channel
// opposite to the pan direction is attenuated by |pan| dB, the near channel
// stays at unity. This is the default pan law of the engine.
inline StereoGain pan_db(float pan) {
    pan = clampf(pan, -100.0f, 100.0f);
    if (pan > 0.0f) return { db_to_gain(-pan), 1.0f };
    if (pan < 0.0f) return { 1.0f, db_to_gain(pan) };
    return { 1.0f, 1.0f };
}

// Constant power pan (-3 dB centre).
inline StereoGain pan_constant_power(float pan) {
    pan = clampf(pan, -1.0f, 1.0f);
    const float angle = (pan + 1.0f) * 0.25f * kPi; // 0 .. pi/2
    return { std::cos(angle), std::sin(angle) };
}

// Linear parameter smoother: ramps towards a target over a fixed number of
// samples to avoid zipper noise when gains change.
class Smoother {
public:
    void reset(float value) { current_ = target_ = value; remaining_ = 0; step_ = 0.0f; }
    void set_target(float target, int ramp_samples) {
        target_ = target;
        if (ramp_samples <= 0 || std::fabs(target - current_) < 1e-7f) { current_ = target; remaining_ = 0; step_ = 0.0f; return; }
        remaining_ = ramp_samples;
        step_ = (target - current_) / static_cast<float>(ramp_samples);
    }
    inline float next() {
        if (remaining_ > 0) { current_ += step_; if (--remaining_ == 0) current_ = target_; }
        return current_;
    }
    inline void skip(int n) {
        if (remaining_ <= 0) return;
        if (n >= remaining_) { current_ = target_; remaining_ = 0; }
        else { current_ += step_ * n; remaining_ -= n; }
    }
    float current() const { return current_; }
    float target() const { return target_; }
    bool is_ramping() const { return remaining_ > 0; }
private:
    float current_ = 0.0f, target_ = 0.0f, step_ = 0.0f;
    int remaining_ = 0;
};

// One pole low pass. Cheap and click free: ideal for "behind the listener"
// muffling and occlusion.
class OnePole {
public:
    void set_lowpass(float cutoff_hz, float sample_rate) {
        const float x = std::exp(-kTwoPi * clampf(cutoff_hz, 1.0f, sample_rate * 0.499f) / sample_rate);
        a0_ = 1.0f - x; b1_ = x;
    }
    inline float process(float in) { z_ = undenormal(a0_ * in + b1_ * z_); return z_; }
    inline float process_highpass(float in) { return in - process(in); }
    void reset() { z_ = 0.0f; }
private:
    float a0_ = 1.0f, b1_ = 0.0f, z_ = 0.0f;
};

// RBJ biquad, transposed direct form II. Coefficients computed in double.
class Biquad {
public:
    enum class Type { LowPass, HighPass, BandPass, Notch, Peak, LowShelf, HighShelf, AllPass };
    void set(Type type, float freq_hz, float q, float gain_db, float sample_rate) {
        const double w0 = 2.0 * 3.14159265358979323846 * clampf(freq_hz, 1.0f, sample_rate * 0.499f) / sample_rate;
        const double cw = std::cos(w0), sw = std::sin(w0);
        const double Q = q < 0.01f ? 0.01 : q;
        const double A = std::pow(10.0, gain_db / 40.0);
        const double alpha = sw / (2.0 * Q);
        double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
        switch (type) {
        case Type::LowPass:  b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = (1 - cw) / 2; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; break;
        case Type::HighPass: b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; break;
        case Type::BandPass: b0 = alpha; b1 = 0; b2 = -alpha; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; break;
        case Type::Notch:    b0 = 1; b1 = -2 * cw; b2 = 1; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; break;
        case Type::AllPass:  b0 = 1 - alpha; b1 = -2 * cw; b2 = 1 + alpha; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha; break;
        case Type::Peak:     b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A; a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A; break;
        case Type::LowShelf: {
            const double s = 2 * std::sqrt(A) * alpha;
            b0 = A * ((A + 1) - (A - 1) * cw + s); b1 = 2 * A * ((A - 1) - (A + 1) * cw); b2 = A * ((A + 1) - (A - 1) * cw - s);
            a0 = (A + 1) + (A - 1) * cw + s; a1 = -2 * ((A - 1) + (A + 1) * cw); a2 = (A + 1) + (A - 1) * cw - s; break; }
        case Type::HighShelf: {
            const double s = 2 * std::sqrt(A) * alpha;
            b0 = A * ((A + 1) + (A - 1) * cw + s); b1 = -2 * A * ((A - 1) + (A + 1) * cw); b2 = A * ((A + 1) + (A - 1) * cw - s);
            a0 = (A + 1) - (A - 1) * cw + s; a1 = 2 * ((A - 1) - (A + 1) * cw); a2 = (A + 1) - (A - 1) * cw - s; break; }
        }
        b0_ = static_cast<float>(b0 / a0); b1_ = static_cast<float>(b1 / a0); b2_ = static_cast<float>(b2 / a0);
        a1_ = static_cast<float>(a1 / a0); a2_ = static_cast<float>(a2 / a0);
    }
    inline float process(float in) {
        const float out = b0_ * in + z1_;
        z1_ = b1_ * in - a1_ * out + z2_;
        z2_ = undenormal(b2_ * in - a2_ * out);
        return out;
    }
    void reset() { z1_ = z2_ = 0.0f; }
private:
    float b0_ = 1, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0, z1_ = 0, z2_ = 0;
};

// 4 point cubic Hermite interpolation between y1 and y2 (y0, y3 neighbours).
inline float hermite4(float frac, float y0, float y1, float y2, float y3) {
    const float c0 = y1;
    const float c1 = 0.5f * (y2 - y0);
    const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * frac + c2) * frac + c1) * frac + c0;
}

struct Xorshift32 {
    uint32_t s = 0x9E3779B9u;
    inline uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    inline float next_unit() { return (next() >> 8) * (1.0f / 16777216.0f); }
};

} // namespace fastplay::dsp
