#pragma once
#include "../core/dsp.h"
#include "../model/room.h"

namespace speakers {

// Late reverberation: everything after the first few reflections, which the
// spatializer handles per source.
//
// Three stages, and the first two matter more than the tank does:
//
//   diffusion   six allpass sections in series. They smear the input into
//               something already dense before the tank ever sees it. Without
//               this, a feedback network repeats a recognisable copy of the
//               input over and over and sounds like a metal box.
//   the tank    twelve delay lines mixed through a Householder matrix. The
//               lengths are prime numbers so no two ever line up, and each is
//               slowly modulated by a fraction of a sample so that the
//               resonances never sit still long enough to ring.
//   damping     a one pole in each line, so the top end dies away sooner than
//               the bottom. That is the difference between a garage and a
//               room full of furniture.
//
// Line lengths come from the size of the room and the feedback from its Sabine
// RT60. The output is normalised by how much the tank builds up, so the wet
// level means the same thing whether the room rings for a tenth of a second or
// for three seconds.

class Reverb {
public:
    void Init(float sampleRate);
    void Reset();

    // Retunes for a room. Cheap, but not free: call it when the room changes,
    // not per block.
    void Configure(const RoomSpec &room);

    // Adds the reverberant field for `in` (a mono send) into the outputs.
    void Process(const float *in, int frames, float *outL, float *outR);

    // Send level for a source at this distance. Distant sources are heard more
    // through the room and less directly.
    float SendFor(float distance) const;

    bool Active() const { return m_active; }

private:
    static constexpr int kLines = 12;
    static constexpr int kDiffusers = 6;

    // A Schroeder allpass: passes everything but scrambles the phase, which is
    // exactly what is wanted for smearing an input without colouring it.
    struct Allpass {
        dsp::DelayLine line;
        int length = 1;
        float g = 0.62f;

        void Init(int maxLength) { line.Init(maxLength); }
        void Reset() { line.Reset(); }
        inline float Process(float x) {
            float delayed = line.ReadInt(length);
            float v = x + g * delayed;
            line.Write(v);
            return delayed - g * v;
        }
    };

    float m_sampleRate = 48000.0f;

    Allpass m_diffusers[kDiffusers];

    dsp::DelayLine m_lines[kLines];
    dsp::OnePole m_damping[kLines];
    int m_length[kLines] = {};
    // A slow wander of a fraction of a sample, different in every line, so the
    // tank never settles into a fixed set of resonances.
    float m_modPhase[kLines] = {};
    float m_modStep[kLines] = {};
    float m_modDepth = 0.0f;

    float m_feedback = 0.0f;
    float m_outputScale = 0.0f;
    float m_wet = 0.0f;
    float m_criticalDistance = 1.0f; // where the diffuse field matches the direct
    bool m_active = false;

    dsp::Biquad m_inputHp;
    dsp::DelayLine m_preDelay;
    int m_preDelaySamples = 0;
};

}  // namespace speakers
