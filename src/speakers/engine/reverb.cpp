#include "reverb.h"

#include <algorithm>
#include <cmath>

namespace speakers {

namespace {

constexpr float kSpeedOfSound = 343.0f;
constexpr int kMaxLine = 16384;

// Spread of the line lengths around the base. Each is then nudged to the
// nearest prime so that no two lines ever share a period.
const float kLineRatios[12] = {0.55f, 0.66f, 0.76f, 0.87f, 0.98f, 1.09f,
                               1.22f, 1.37f, 1.53f, 1.70f, 1.88f, 2.07f};

// Alternating signs on the way in, so the lines do not all receive the same
// thing and immediately comb filter against each other.
const float kInject[12] = {1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f,
                           -1.0f, 1.0f, 1.0f, -1.0f, 1.0f, -1.0f};
const float kOutL[12] = {1.0f, -1.0f, 1.0f, 1.0f, -1.0f, 1.0f,
                         -1.0f, -1.0f, 1.0f, 1.0f, -1.0f, -1.0f};
const float kOutR[12] = {1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f,
                         -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f};

// Diffuser lengths, as a fraction of the base delay. Short, and deliberately
// unrelated to each other.
const float kDiffuserRatios[6] = {0.13f, 0.21f, 0.31f, 0.43f, 0.57f, 0.73f};

bool IsPrime(int n) {
    if (n < 2) return false;
    if (n % 2 == 0) return n == 2;
    for (int d = 3; d * d <= n; d += 2)
        if (n % d == 0) return false;
    return true;
}

int NearestPrime(int n) {
    if (n < 2) return 2;
    for (int offset = 0; offset < 200; ++offset) {
        if (IsPrime(n + offset)) return n + offset;
        if (n - offset > 1 && IsPrime(n - offset)) return n - offset;
    }
    return n | 1;
}

} // namespace

void Reverb::Init(float sampleRate) {
    m_sampleRate = sampleRate;
    for (int i = 0; i < kLines; ++i) m_lines[i].Init(kMaxLine);
    for (int i = 0; i < kDiffusers; ++i) m_diffusers[i].Init(4096);
    m_preDelay.Init(8192);
    Reset();
}

void Reverb::Reset() {
    for (int i = 0; i < kLines; ++i) {
        m_lines[i].Reset();
        m_damping[i].Reset();
        m_modPhase[i] = (float)i * 0.7853f;
    }
    for (int i = 0; i < kDiffusers; ++i) m_diffusers[i].Reset();
    m_preDelay.Reset();
    m_inputHp.Reset();
}

void Reverb::Configure(const RoomSpec &room) {
    float rt60 = room.Rt60();
    m_active = room.kind != RoomKind::Outdoor && rt60 > 0.05f;
    if (!m_active) {
        m_wet = 0.0f;
        return;
    }

    // How long one trip round a delay line should take. Short enough that the
    // sound goes round many times a second, so the echo density builds up
    // before the tail has died away. Getting this too long is what made the
    // first version of this sound like a metal box rather than a room.
    // The range at which the diffuse field matches the direct sound.
    m_criticalDistance = 0.057f * std::sqrt(std::max(room.Volume(), 0.1f) / std::max(rt60, 0.02f));

    float size = std::cbrt(std::max(room.Volume(), 0.5f));
    float baseMs = dsp::Clampf(5.0f + 2.6f * size, 8.0f, 80.0f);
    float baseSamples = baseMs * 0.001f * m_sampleRate;

    float meanLength = 0.0f;
    for (int i = 0; i < kLines; ++i) {
        int wanted = (int)(baseSamples * kLineRatios[i]);
        m_length[i] = std::min(NearestPrime(std::max(wanted, 16)), kMaxLine - 8);
        meanLength += (float)m_length[i];

        // A dead room swallows the treble in one bounce; bare concrete keeps
        // it for many.
        float dampHz = dsp::Clampf(15000.0f * (1.0f - room.absorption * 1.5f), 700.0f, 15000.0f);
        m_damping[i].SetCutoff(m_sampleRate, dampHz);

        // Between an eighth and a third of a hertz, all different, so the
        // wander never repeats across lines.
        m_modStep[i] = 2.0f * dsp::kPi * (0.11f + 0.037f * (float)i) / m_sampleRate;
    }
    meanLength /= (float)kLines;
    m_modDepth = dsp::Clampf(baseSamples * 0.004f, 0.5f, 6.0f);

    // Feedback that reaches -60 dB after RT60 seconds for a trip of the mean
    // line length.
    m_feedback = std::pow(10.0f, -3.0f * meanLength / (rt60 * m_sampleRate));
    m_feedback = dsp::Clampf(m_feedback, 0.0f, 0.95f);

    // Normalise by how much the tank builds up, so the wet level means the
    // same thing in a car and in a hall rather than scaling with RT60.
    m_outputScale = (1.0f - m_feedback) / std::sqrt((float)kLines);

    // How much reverberant field a room has is set by how much absorption is
    // in it, and falls as one over the square root of it. A bare garage has a
    // lot; a furnished living room has some; neither has as much as the direct
    // sound at any sensible listening distance.
    float absorbed = std::max(room.SurfaceArea() * std::max(room.absorption, 0.01f), 1.0f);
    m_wet = dsp::Clampf(1.6f / std::sqrt(absorbed), 0.05f, 0.6f);

    for (int i = 0; i < kDiffusers; ++i) {
        int len = (int)(baseSamples * kDiffuserRatios[i]);
        m_diffusers[i].length = std::min(NearestPrime(std::max(len, 8)), 4000);
        m_diffusers[i].g = 0.70f;
    }

    m_inputHp.SetHighpass(m_sampleRate, 110.0f, 0.707f);

    // Enough pre-delay to keep the tail behind the early reflections.
    m_preDelaySamples = std::min(6000, (int)(baseSamples * 0.5f));
}

float Reverb::SendFor(float distance) const {
    if (!m_active) return 0.0f;
    // The direct sound falls off with distance and the reverberant field does
    // not, so how much room you hear depends on where you are relative to the
    // critical distance -- the range at which the two are equal.
    //
    //   rc = 0.057 * sqrt(V / RT60)
    //
    // In a living room that is a metre or two and you sit well inside it. In a
    // car it is forty centimetres, and every seat is beyond it: at the driver's
    // ears the diffuse field is already louder than the speaker. That is the
    // reason a real car measures smooth at a seat while a model built only out
    // of direct paths comb filters itself to pieces -- there is nothing in it
    // to fill the nulls between four speakers playing the same thing.
    float ratio = distance / std::max(m_criticalDistance, 0.05f);
    // Trimmed so that a car comes out a shade drier than it was before this
    // was made room aware: a cabin that small has almost no tail to speak of,
    // and what little there is reads as a room rather than as a car the
    // moment there is too much of it.
    return dsp::Clampf(0.040f * ratio, 0.0f, 0.55f);
}

void Reverb::Process(const float *in, int frames, float *outL, float *outR) {
    if (!m_active || m_wet <= 0.0f) return;

    for (int n = 0; n < frames; ++n) {
        float x = m_inputHp.Process(in[n]);
        m_preDelay.Write(x);
        x = m_preDelay.ReadInt(m_preDelaySamples);

        // Smear it before the tank ever sees it.
        for (int i = 0; i < kDiffusers; ++i) x = m_diffusers[i].Process(x);

        float s[kLines];
        float sum = 0.0f;
        for (int i = 0; i < kLines; ++i) {
            m_modPhase[i] += m_modStep[i];
            if (m_modPhase[i] > 2.0f * dsp::kPi) m_modPhase[i] -= 2.0f * dsp::kPi;
            float delay = (float)m_length[i] + m_modDepth * std::sin(m_modPhase[i]);
            s[i] = m_damping[i].Process(m_lines[i].Read(delay));
            sum += s[i];
        }

        // Householder: v' = v - (2/N) * sum(v). Lossless and cheap, so the
        // tank neither runs away nor collapses.
        float correction = sum * (2.0f / (float)kLines);
        float l = 0.0f, r = 0.0f;
        for (int i = 0; i < kLines; ++i) {
            m_lines[i].Write((s[i] - correction) * m_feedback + x * kInject[i]);
            l += s[i] * kOutL[i];
            r += s[i] * kOutR[i];
        }

        float scale = m_wet * m_outputScale;
        outL[n] += l * scale;
        outR[n] += r * scale;
    }
}

}  // namespace speakers
