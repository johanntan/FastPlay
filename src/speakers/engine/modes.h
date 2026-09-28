#pragma once
#include <vector>

#include "../core/dsp.h"
#include "../core/vec3.h"
#include "../model/room.h"

namespace speakers {

// The room's standing waves.
//
// Below a couple of hundred hertz a small space does not behave like a room at
// all. Sound at those frequencies has wavelengths comparable to the space, so
// instead of reflections you get a handful of standing waves at frequencies
// fixed by the dimensions -- and where you happen to be sitting decides whether
// each one is a peak or a hole.
//
// This is most of why a car sounds like a car. A saloon cabin is 2.6 m long,
// which puts its first mode at 66 Hz, right where the bass lives; move your
// head half a metre and a note that was booming goes thin. Nothing else in the
// engine produces that, and without it a car sounds like a small dead room
// rather than like a car.
//
// Each mode is one peaking filter. Its centre frequency comes from the
// dimensions, its Q from how long the room rings down there, and its gain from
// the mode shape evaluated at both the listener and the source -- so a sub in
// the boot drives the length modes hard and the vertical ones barely at all.

class RoomModes {
public:
    void Init(float sampleRate);
    void Reset();

    // Works out which modes this room has. Call it when the room changes.
    void Configure(const RoomSpec &room);

    // Recomputes each mode's gain for where you are standing and where the
    // bass is coming from. Cheap enough per block, and skipped entirely when
    // neither has moved.
    void Update(const RoomSpec &room, Vec3 listener, Vec3 source);

    void Process(float *left, float *right, int frames);

    bool Active() const { return !m_modes.empty(); }
    int Count() const { return (int)m_modes.size(); }
    float Frequency(int index) const;
    // Gain in dB this mode is currently contributing, for diagnostics.
    float GainDb(int index) const;

private:
    struct Mode {
        int nx = 0, ny = 0, nz = 0;
        float freq = 0.0f;
        float q = 5.0f;
        float gainDb = 0.0f;
        dsp::Biquad left, right;
    };

    float m_sampleRate = 48000.0f;
    std::vector<Mode> m_modes;
    float m_strengthDb = 0.0f;
    Vec3 m_lastListener{1e9f, 1e9f, 1e9f};
    Vec3 m_lastSource{1e9f, 1e9f, 1e9f};
};

}  // namespace speakers
