#pragma once
#ifndef FASTPLAY_BASIC_EFFECTS_H
#define FASTPLAY_BASIC_EFFECTS_H

// Echo, peaking EQ band and compressor: FastPlay's own versions of the BASS_FX
// effects it used (BFX_ECHO4, BFX_PEAKEQ, BFX_COMPRESSOR2), with the same
// parameters. Each works on interleaved stereo float and keeps its own state; a
// fresh one starts silent. Parameters are set from the UI thread while audio is
// processed on the mix thread, so each guards them with a mutex.

#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

namespace audio {

// A delay with feedback. With `stereo` each side's echo feeds back into the
// other, so repeats bounce between left and right.
class Echo {
public:
    struct Params {
        float dry = 1.0f, wet = 0.3f;
        float feedback = 0.4f;  // -1..1
        float delay = 0.3f;     // seconds
        bool stereo = true;
    };

    void Set(const Params& p) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_params = p;
    }

    void Process(float* samples, int frames, int sampleRate) {
        std::lock_guard<std::mutex> lock(m_mutex);
        const size_t length = std::max<size_t>(1, static_cast<size_t>(std::clamp(m_params.delay, 0.001f, 6.0f) * sampleRate));
        if (length != m_length || sampleRate != m_rate) {
            m_line.assign(length * 2, 0.0f);
            m_length = length;
            m_rate = sampleRate;
            m_pos = 0;
        }
        const float fb = std::clamp(m_params.feedback, -1.0f, 1.0f);
        for (int i = 0; i < frames; i++) {
            float* frame = samples + static_cast<size_t>(i) * 2;
            float* slot = m_line.data() + m_pos * 2;
            const float dl = slot[0], dr = slot[1];
            const float inL = frame[0], inR = frame[1];
            frame[0] = inL * m_params.dry + dl * m_params.wet;
            frame[1] = inR * m_params.dry + dr * m_params.wet;
            slot[0] = inL + fb * (m_params.stereo ? dr : dl);
            slot[1] = inR + fb * (m_params.stereo ? dl : dr);
            if (++m_pos >= m_length) m_pos = 0;
        }
    }

private:
    std::mutex m_mutex;
    Params m_params;
    std::vector<float> m_line;  // interleaved stereo
    size_t m_length = 0, m_pos = 0;
    int m_rate = 0;
};

// One peaking EQ band (the RBJ cookbook filter), `bandwidth` in octaves.
class PeakingEq {
public:
    void Set(float centerHz, float bandwidth, float gainDb) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_center = centerHz;
        m_bandwidth = bandwidth;
        m_gainDb = gainDb;
        m_rate = 0;  // coefficients again at the next block
    }

    float Gain() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_gainDb;
    }

    void Process(float* samples, int frames, int sampleRate) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_gainDb == 0.0f && m_z[0] == 0.0 && m_z[1] == 0.0 && m_z[2] == 0.0 && m_z[3] == 0.0) {
            m_rate = 0;
            return;  // flat and settled: nothing to do
        }
        if (sampleRate != m_rate) Design(sampleRate);
        for (int i = 0; i < frames; i++) {
            for (int c = 0; c < 2; c++) {
                double x = samples[i * 2 + c];
                double y = m_b0 * x + m_z[c * 2];
                m_z[c * 2] = m_b1 * x - m_a1 * y + m_z[c * 2 + 1];
                m_z[c * 2 + 1] = m_b2 * x - m_a2 * y;
                samples[i * 2 + c] = static_cast<float>(y);
            }
        }
        // Flush denormals
        for (double& z : m_z) {
            if (std::fabs(z) < 1e-20) z = 0.0;
        }
    }

private:
    std::mutex m_mutex;
    float m_center = 1000.0f, m_bandwidth = 2.5f, m_gainDb = 0.0f;
    int m_rate = 0;
    double m_b0 = 1, m_b1 = 0, m_b2 = 0, m_a1 = 0, m_a2 = 0;
    double m_z[4] = {0, 0, 0, 0};  // transposed direct form II state, two per channel

    void Design(int sampleRate) {
        m_rate = sampleRate;
        const double pi = 3.14159265358979323846;
        double f0 = std::clamp(static_cast<double>(m_center), 10.0, sampleRate * 0.45);
        double A = std::pow(10.0, m_gainDb / 40.0);
        double w0 = 2.0 * pi * f0 / sampleRate;
        double sw = std::sin(w0), cw = std::cos(w0);
        double alpha = sw * std::sinh(std::log(2.0) / 2.0 * std::max(0.05, static_cast<double>(m_bandwidth)) * w0 / sw);
        double a0 = 1.0 + alpha / A;
        m_b0 = (1.0 + alpha * A) / a0;
        m_b1 = -2.0 * cw / a0;
        m_b2 = (1.0 - alpha * A) / a0;
        m_a1 = -2.0 * cw / a0;
        m_a2 = (1.0 - alpha / A) / a0;
    }
};

// A plain gain (the EQ's preamp).
class Gain {
public:
    void Set(float linear) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_target = linear;
    }

    void Process(float* samples, int frames) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (frames <= 0) return;
        const float step = (m_target - m_current) / frames;
        for (int i = 0; i < frames; i++) {
            m_current += step;
            samples[i * 2] *= m_current;
            samples[i * 2 + 1] *= m_current;
        }
        m_current = m_target;
    }

private:
    std::mutex m_mutex;
    float m_target = 1.0f, m_current = 1.0f;
};

// A feed-forward compressor, both sides reduced together by the louder one.
class Compressor {
public:
    struct Params {
        float gainDb = 0.0f;         // makeup gain
        float thresholdDb = -20.0f;
        float ratio = 4.0f;
        float attackMs = 20.0f;
        float releaseMs = 200.0f;
    };

    void Set(const Params& p) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_params = p;
    }

    void Process(float* samples, int frames, int sampleRate) {
        std::lock_guard<std::mutex> lock(m_mutex);
        const float attack = std::exp(-1.0f / (std::max(0.01f, m_params.attackMs) * 0.001f * sampleRate));
        const float release = std::exp(-1.0f / (std::max(1.0f, m_params.releaseMs) * 0.001f * sampleRate));
        const float slope = 1.0f - 1.0f / std::max(1.0f, m_params.ratio);
        const float makeup = std::pow(10.0f, m_params.gainDb / 20.0f);
        for (int i = 0; i < frames; i++) {
            float* frame = samples + static_cast<size_t>(i) * 2;
            float peak = std::max(std::fabs(frame[0]), std::fabs(frame[1]));
            float levelDb = peak > 1e-6f ? 20.0f * std::log10(peak) : -120.0f;
            float over = levelDb - m_params.thresholdDb;
            float reduction = over > 0.0f ? over * slope : 0.0f;  // dB to take off
            float coeff = reduction > m_reduction ? attack : release;
            m_reduction = reduction + coeff * (m_reduction - reduction);
            float gain = std::pow(10.0f, -m_reduction / 20.0f) * makeup;
            frame[0] *= gain;
            frame[1] *= gain;
        }
    }

private:
    std::mutex m_mutex;
    Params m_params;
    float m_reduction = 0.0f;  // smoothed gain reduction in dB
};

}  // namespace audio

#endif  // FASTPLAY_BASIC_EFFECTS_H
