#include "modes.h"

#include <algorithm>
#include <cmath>

namespace speakers {

namespace {

constexpr float kSpeedOfSound = 343.0f;

// Above this there are too many modes too close together to hear as separate
// resonances; that region is the reverberation tank's job, not this one.
constexpr float kHighestModeHz = 280.0f;
constexpr int kMaxModes = 12;

// The pressure shape of a mode along one axis, evaluated at a position given
// as a fraction of the way across the room. It is one at both walls and
// alternates in between, which is why a corner excites everything and the
// middle of a room excites nothing odd-numbered.
float ModeShape(int n, float fraction) {
    if (n == 0) return 1.0f;
    return std::cos((float)n * dsp::kPi * dsp::Clampf(fraction, 0.0f, 1.0f));
}

} // namespace

void RoomModes::Init(float sampleRate) {
    m_sampleRate = sampleRate;
    m_modes.clear();
}

void RoomModes::Reset() {
    for (auto &m : m_modes) {
        m.left.Reset();
        m.right.Reset();
    }
    m_lastListener = {1e9f, 1e9f, 1e9f};
    m_lastSource = {1e9f, 1e9f, 1e9f};
}



void RoomModes::Configure(const RoomSpec &room) {
    m_modes.clear();
    m_lastListener = {1e9f, 1e9f, 1e9f};
    m_lastSource = {1e9f, 1e9f, 1e9f};

    if (room.kind == RoomKind::Outdoor) return; // no walls, no modes

    // Absorption falls off at low frequency, so the modes ring for longer than
    // the broadband reverberation time suggests -- much longer in a car, where
    // the broadband figure is dominated by seats and carpet that do nothing at
    // 60 Hz.
    float rt60 = std::max(room.Rt60(), 0.02f);
    float lowRt60 = rt60 * (room.kind == RoomKind::Vehicle ? 2.5f : 1.6f);
    lowRt60 = std::max(lowRt60, 0.15f);

    m_strengthDb = room.kind == RoomKind::Vehicle ? 8.0f : 5.0f;

    for (int nx = 0; nx <= 2; ++nx) {
        for (int ny = 0; ny <= 2; ++ny) {
            for (int nz = 0; nz <= 1; ++nz) {
                if (nx == 0 && ny == 0 && nz == 0) continue;
                float fx = (float)nx / std::max(room.width, 0.1f);
                float fy = (float)ny / std::max(room.depth, 0.1f);
                float fz = (float)nz / std::max(room.height, 0.1f);
                float freq = 0.5f * kSpeedOfSound * std::sqrt(fx * fx + fy * fy + fz * fz);
                if (freq < 18.0f || freq > kHighestModeHz) continue;

                Mode mode;
                mode.nx = nx;
                mode.ny = ny;
                mode.nz = nz;
                mode.freq = freq;
                // Q of a resonance that decays to -60 dB in lowRt60 seconds.
                mode.q = dsp::Clampf(0.4548f * freq * lowRt60, 2.0f, 18.0f);
                m_modes.push_back(mode);
            }
        }
    }

    // Keep the lowest ones: those are the ones you hear individually.
    std::sort(m_modes.begin(), m_modes.end(),
              [](const Mode &a, const Mode &b) { return a.freq < b.freq; });
    if ((int)m_modes.size() > kMaxModes) m_modes.resize(kMaxModes);
}

void RoomModes::Update(const RoomSpec &room, Vec3 listener, Vec3 source) {
    if (m_modes.empty()) return;
    // Neither end has moved, so nothing about the coupling has changed.
    if (Length(listener - m_lastListener) < 0.005f && Length(source - m_lastSource) < 0.005f)
        return;
    m_lastListener = listener;
    m_lastSource = source;

    // Positions as a fraction of the way across the room. x and y are centred
    // on the origin; z runs up from the floor.
    auto fractions = [&room](Vec3 p) {
        return Vec3{(p.x + room.width * 0.5f) / std::max(room.width, 0.1f),
                    (p.y + room.depth * 0.5f) / std::max(room.depth, 0.1f),
                    p.z / std::max(room.height, 0.1f)};
    };
    Vec3 l = fractions(listener);
    Vec3 s = fractions(source);

    for (auto &mode : m_modes) {
        // How strongly the source drives this mode, and how much of it reaches
        // you. Both can be negative, and a mode you are sitting in the null of
        // is a hole rather than a peak.
        float atListener = ModeShape(mode.nx, l.x) * ModeShape(mode.ny, l.y) *
                           ModeShape(mode.nz, l.z);
        float atSource = ModeShape(mode.nx, s.x) * ModeShape(mode.ny, s.y) *
                         ModeShape(mode.nz, s.z);
        float coupling = dsp::Clampf(atListener * atSource, -1.0f, 1.0f);

        mode.gainDb = m_strengthDb * coupling;
        mode.left.SetPeaking(m_sampleRate, mode.freq, mode.q, mode.gainDb);
        mode.right.SetPeaking(m_sampleRate, mode.freq, mode.q, mode.gainDb);
    }
}

void RoomModes::Process(float *left, float *right, int frames) {
    for (auto &mode : m_modes) {
        for (int i = 0; i < frames; ++i) {
            left[i] = mode.left.Process(left[i]);
            right[i] = mode.right.Process(right[i]);
        }
    }
}

float RoomModes::Frequency(int index) const {
    if (index < 0 || index >= (int)m_modes.size()) return 0.0f;
    return m_modes[(size_t)index].freq;
}

float RoomModes::GainDb(int index) const {
    if (index < 0 || index >= (int)m_modes.size()) return 0.0f;
    return m_modes[(size_t)index].gainDb;
}

}  // namespace speakers
