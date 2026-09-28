#pragma once
#include <cmath>

namespace speakers {

// Simple 3D vector math for speaker / listener geometry.
//
// World axes, as used by every room preset:
//   +x = right, +y = forward, +z = up.  Metres throughout.
// A listener yaw of 0 faces +y; yaw increases turning to the right.

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }

inline float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float Length(Vec3 v) { return std::sqrt(Dot(v, v)); }

inline Vec3 Normalized(Vec3 v) {
    float len = Length(v);
    return len > 1e-9f ? v * (1.0f / len) : Vec3{0.0f, 1.0f, 0.0f};
}

// Unit vector for a yaw/pitch pair in degrees, matching the axes above.
inline Vec3 DirectionFromAngles(float yawDeg, float pitchDeg) {
    const float kDegToRad = 3.14159265358979f / 180.0f;
    float yaw = yawDeg * kDegToRad, pitch = pitchDeg * kDegToRad;
    float cp = std::cos(pitch);
    return {std::sin(yaw) * cp, std::cos(yaw) * cp, std::sin(pitch)};
}

}  // namespace speakers
