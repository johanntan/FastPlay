#pragma once

// What an ear actually does with level.
//
// The normal equal-loudness-level contours, ISO 226:2003. The standard gives
// three coefficients per third-octave band and a formula that turns a loudness
// level in phons into the sound pressure level a pure tone needs in order to
// be that loud. It is the measured behaviour of an average young adult ear,
// not a curve anyone drew by eye.
//
// The reason it is here: the contours are not parallel. The bass end of the
// quiet ones sits far above the bass end of the loud ones, which is the
// familiar business of a system sounding thin until you turn it up. A
// simulation playing a loud car into a pair of headphones at ordinary
// headphone level gets this wrong unless it puts the difference back.

#include <cmath>

namespace speakers {

namespace loudness {

// ISO 226:2003, table 1. af is the exponent of loudness perception, Lu the
// transfer of the free field to the ear, Tf the threshold of hearing.
constexpr int kBands = 29;
constexpr float kFreq[kBands] = {20.0f,   25.0f,   31.5f,   40.0f,   50.0f,   63.0f,
                                 80.0f,   100.0f,  125.0f,  160.0f,  200.0f,  250.0f,
                                 315.0f,  400.0f,  500.0f,  630.0f,  800.0f,  1000.0f,
                                 1250.0f, 1600.0f, 2000.0f, 2500.0f, 3150.0f, 4000.0f,
                                 5000.0f, 6300.0f, 8000.0f, 10000.0f, 12500.0f};
constexpr float kAf[kBands] = {0.532f, 0.506f, 0.480f, 0.455f, 0.432f, 0.409f, 0.387f,
                               0.367f, 0.349f, 0.330f, 0.315f, 0.301f, 0.288f, 0.276f,
                               0.267f, 0.259f, 0.253f, 0.250f, 0.246f, 0.244f, 0.243f,
                               0.243f, 0.243f, 0.242f, 0.242f, 0.245f, 0.254f, 0.271f,
                               0.301f};
constexpr float kLu[kBands] = {-31.6f, -27.2f, -23.0f, -19.1f, -15.9f, -13.0f, -10.3f,
                               -8.1f,  -6.2f,  -4.5f,  -3.1f,  -2.0f,  -1.1f,  -0.4f,
                               0.0f,   0.3f,   0.5f,   0.0f,   -2.7f,  -4.1f,  -1.0f,
                               1.7f,   2.5f,   1.2f,   -2.1f,  -7.1f,  -11.2f, -10.7f,
                               -3.1f};
constexpr float kTf[kBands] = {78.5f, 68.7f, 59.5f, 51.1f, 44.0f, 37.5f, 31.5f,
                               26.5f, 22.1f, 17.9f, 14.4f, 11.4f, 8.6f,  6.2f,
                               4.4f,  3.0f,  2.2f,  2.4f,  3.5f,  1.7f,  -1.3f,
                               -4.2f, -6.0f, -5.4f, -1.5f, 6.0f,  12.6f, 13.9f,
                               12.3f};

// The level a tone in band i needs to be, to be as loud as phon dB at 1 kHz.
inline float BandSpl(int i, float phon) {
    float af = kAf[i];
    float a = 4.47e-3f * (std::pow(10.0f, 0.025f * phon) - 1.15f) +
              std::pow(0.4f * std::pow(10.0f, (kTf[i] + kLu[i]) * 0.1f - 9.0f), af);
    return (10.0f / af) * std::log10(a) - kLu[i] + 94.0f;
}

// The same, at any frequency, straight-lined across the octave in between.
inline float Spl(float hz, float phon) {
    if (hz <= kFreq[0]) return BandSpl(0, phon);
    if (hz >= kFreq[kBands - 1]) return BandSpl(kBands - 1, phon);
    int i = 0;
    while (i + 1 < kBands - 1 && kFreq[i + 1] < hz) ++i;
    float t = std::log(hz / kFreq[i]) / std::log(kFreq[i + 1] / kFreq[i]);
    return BandSpl(i, phon) * (1.0f - t) + BandSpl(i + 1, phon) * t;
}

// How much a tone at hz stands away from a kilohertz on the contour for a
// given loudness. Small when loud, large when quiet: that gap is the effect.
inline float ShapeDb(float hz, float phon) { return Spl(hz, phon) - Spl(1000.0f, phon); }

// What to add at hz so that a system heard at playedPhon carries the balance
// it would have had at intendedPhon. Positive below a kilohertz whenever the
// intended level is the louder of the two.
inline float TiltDb(float hz, float playedPhon, float intendedPhon) {
    return ShapeDb(hz, playedPhon) - ShapeDb(hz, intendedPhon);
}

} // namespace loudness

}  // namespace speakers
