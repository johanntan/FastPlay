#include "effects.h"
#include "globals.h"
#include "accessibility.h"
#include "app_ui.h"
#include "bass_fx.h"
#include "tempo_processor.h"
#include "center_cancel.h"
#include "convolution.h"
#include "reverb/reverb.h"
#include "reverb/efx_reverb.h"
#ifdef USE_STEAM_AUDIO
#include "spatial_audio.h"
#endif
#include <cstdio>
#include <vector>
#include <cmath>
#include <mutex>
#include <algorithm>

// Clamp helper
template<typename T> T clamp_val(T val, T minVal, T maxVal) {
    if (val < minVal) return minVal;
    if (val > maxVal) return maxVal;
    return val;
}

// Parameter definitions
static const ParamDef g_paramDefs[] = {
    // Stream effects (DSPEffectType cast to -1 means not a DSP effect)
    {ParamId::Volume,      "Volume",       "%",          0.0f,   4.0f,   0.02f, 1.0f,  (DSPEffectType)-1},
    {ParamId::Pitch,       "Pitch",        " semitones", -12.0f, 12.0f,  1.0f,  0.0f,  (DSPEffectType)-1},
    {ParamId::Tempo,       "Tempo",        "%",          -75.0f, 200.0f, 5.0f,  0.0f,  (DSPEffectType)-1},
    {ParamId::Rate,        "Rate",         "x",          0.25f,  4.0f,   0.01f, 1.0f,  (DSPEffectType)-1},
    // Simple reverb parameters (algorithm 1). The preset comes first: choosing one sets the rest,
    // and presets and settings are loaded in this order.
    {ParamId::ReverbPreset,   "Reverb Room",      "",     0.0f,    21.0f,    1.0f,    0.0f,     DSPEffectType::Reverb},
    {ParamId::ReverbMix,      "Reverb Mix",       "%",    0.0f,    100.0f,   5.0f,    30.0f,    DSPEffectType::Reverb},
    {ParamId::ReverbRoom,     "Reverb Size",      "%",    0.0f,    100.0f,   5.0f,    50.0f,    DSPEffectType::Reverb},
    {ParamId::ReverbDamp,     "Reverb Damping",   "%",    0.0f,    100.0f,   5.0f,    25.0f,    DSPEffectType::Reverb},
    {ParamId::ReverbWidth,    "Reverb Width",     "%",    0.0f,    100.0f,   10.0f,   100.0f,   DSPEffectType::Reverb},
    {ParamId::ReverbPreDelay, "Reverb Pre-delay", " ms",  0.0f,    250.0f,   5.0f,    0.0f,     DSPEffectType::Reverb},
    {ParamId::ReverbLowCut,   "Reverb Low Cut",   " Hz",  0.0f,    1000.0f,  20.0f,   0.0f,     DSPEffectType::Reverb},
    {ParamId::ReverbHighCut,  "Reverb High Cut",  " Hz",  1000.0f, 20000.0f, 1000.0f, 20000.0f, DSPEffectType::Reverb},
    // Advanced reverb parameters (algorithm 2); defaults are the Generic environment
    {ParamId::AdvReverbPreset,      "Reverb Environment",       "",     0.0f,   25.0f,  1.0f,  0.0f,   DSPEffectType::Reverb},
    {ParamId::AdvReverbMix,         "Reverb Mix",               "%",    0.0f,   100.0f, 5.0f,  30.0f,  DSPEffectType::Reverb},
    {ParamId::AdvReverbDecay,       "Reverb Decay",             " s",   0.1f,   20.0f,  0.1f,  1.49f,  DSPEffectType::Reverb},
    {ParamId::AdvReverbHFRatio,     "Reverb High Decay",        "x",    0.1f,   2.0f,   0.05f, 0.83f,  DSPEffectType::Reverb},
    {ParamId::AdvReverbDensity,     "Reverb Density",           "%",    0.0f,   100.0f, 5.0f,  100.0f, DSPEffectType::Reverb},
    {ParamId::AdvReverbDiffusion,   "Reverb Diffusion",         "%",    0.0f,   100.0f, 5.0f,  100.0f, DSPEffectType::Reverb},
    {ParamId::AdvReverbReflections, "Reverb Reflections",       " dB",  -60.0f, 10.0f,  1.0f,  -26.0f, DSPEffectType::Reverb},
    {ParamId::AdvReverbLate,        "Reverb Tail",              " dB",  -60.0f, 20.0f,  1.0f,  2.0f,   DSPEffectType::Reverb},
    {ParamId::AdvReverbReflDelay,   "Reverb Reflections Delay", " ms",  0.0f,   300.0f, 5.0f,  7.0f,   DSPEffectType::Reverb},
    {ParamId::AdvReverbLateDelay,   "Reverb Tail Delay",        " ms",  0.0f,   100.0f, 5.0f,  11.0f,  DSPEffectType::Reverb},
    // Echo parameters
    {ParamId::EchoDelay,   "Echo Delay",   "ms",         10.0f,  2000.0f, 50.0f, 300.0f, DSPEffectType::Echo},
    {ParamId::EchoFeedback,"Echo Feedback","%",          0.0f,   90.0f,  5.0f,  40.0f, DSPEffectType::Echo},
    {ParamId::EchoMix,     "Echo Mix",     "%",          0.0f,   100.0f, 5.0f,  30.0f, DSPEffectType::Echo},
    // EQ parameters (in dB)
    {ParamId::EQPreamp,    "EQ Preamp",    "dB",         -15.0f, 0.0f,   1.0f,  0.0f,  DSPEffectType::EQ},
    {ParamId::EQBass,      "EQ Bass",      "dB",         -15.0f, 15.0f,  1.0f,  0.0f,  DSPEffectType::EQ},
    {ParamId::EQMid,       "EQ Mid",       "dB",         -15.0f, 15.0f,  1.0f,  0.0f,  DSPEffectType::EQ},
    {ParamId::EQTreble,    "EQ Treble",    "dB",         -15.0f, 15.0f,  1.0f,  0.0f,  DSPEffectType::EQ},
    // Compressor parameters
    {ParamId::CompThreshold, "Comp Threshold", "dB",     -60.0f, 0.0f,   3.0f,  -20.0f, DSPEffectType::Compressor},
    {ParamId::CompRatio,     "Comp Ratio",     ":1",     1.0f,   20.0f,  1.0f,  4.0f,   DSPEffectType::Compressor},
    {ParamId::CompAttack,    "Comp Attack",    "ms",     0.01f,  500.0f, 10.0f, 20.0f,  DSPEffectType::Compressor},
    {ParamId::CompRelease,   "Comp Release",   "ms",     10.0f,  2000.0f,50.0f, 200.0f, DSPEffectType::Compressor},
    {ParamId::CompGain,      "Comp Gain",      "dB",     -20.0f, 20.0f,  1.0f,  0.0f,   DSPEffectType::Compressor},
    // Stereo width parameter (0% = mono, 100% = normal, 200% = extra wide)
    {ParamId::StereoWidth,   "Stereo Width",   "%",      0.0f,   200.0f, 10.0f, 100.0f, DSPEffectType::StereoWidth},
    // Center cancel parameter (-100% = extract center, 0% = off, +100% = cancel center)
    {ParamId::CenterCancel,  "Center Cancel",  "%",      -100.0f, 100.0f, 10.0f, 0.0f, DSPEffectType::CenterCancel},
    // Convolution reverb parameters
    {ParamId::ConvolutionMix,  "Conv Mix",   "%",      0.0f, 100.0f, 5.0f, 50.0f, DSPEffectType::Convolution},
    {ParamId::ConvolutionGain, "Conv Gain",  "dB",    -20.0f, 20.0f, 1.0f, 0.0f, DSPEffectType::Convolution},
    // 3D audio parameters
    {ParamId::SpatialBlend,      "3D Blend",       "%",    0.0f,   100.0f,  5.0f,  100.0f, DSPEffectType::SpatialAudio},
    {ParamId::SpatialWidth,      "3D Width",       " deg", 15.0f,  90.0f,   5.0f,  45.0f,  DSPEffectType::SpatialAudio},
    {ParamId::SpatialRotation,   "3D Rotation",    " deg",-180.0f, 180.0f,  5.0f,  0.0f,   DSPEffectType::SpatialAudio},
    {ParamId::SpatialMode,       "3D Mode",        "",     0.0f,   1.0f,    1.0f,  0.0f,   DSPEffectType::SpatialAudio},
    {ParamId::SpatialRearCenter, "3D Rear Speaker","",     0.0f,   1.0f,    1.0f,  1.0f,   DSPEffectType::SpatialAudio},
    {ParamId::SpatialX,          "3D Listener X",  "",    -50.0f,  50.0f,   1.0f,  0.0f,   DSPEffectType::SpatialAudio},
    {ParamId::SpatialY,          "3D Listener Y",  "",    -50.0f,  50.0f,   1.0f,  0.0f,   DSPEffectType::SpatialAudio},
    {ParamId::SpatialZ,          "3D Listener Z",  "",    -50.0f,  50.0f,   1.0f,  0.0f,   DSPEffectType::SpatialAudio},
};
static const int g_paramDefCount = sizeof(g_paramDefs) / sizeof(g_paramDefs[0]);

// DSP effect handles
static HFX g_hfxEcho = 0;
static HFX g_hfxEQPreamp = 0;
static HFX g_hfxEQBass = 0;
static HFX g_hfxEQMid = 0;
static HFX g_hfxEQTreble = 0;
static HFX g_hfxCompressor = 0;
static HDSP g_hdspReverb = 0;       // Custom DSP for the reverbs
static HDSP g_hdspStereoWidth = 0;  // Custom DSP for stereo width
static HDSP g_hdspCenterCancel = 0; // Custom DSP for center cancel/extract
static HDSP g_hdspConvolution = 0;  // Custom DSP for convolution reverb
static HDSP g_hdspSpatialAudio = 0;  // Custom DSP for 3D audio (Steam Audio)
static HDSP g_hdspVolume = 0;       // Custom DSP for volume (runs LAST, after encoder)

// DSP effect enabled states
static bool g_dspEnabled[(int)DSPEffectType::COUNT] = {false, false, false, false, false, false, false, false};

// Parameter values
static float g_paramValues[(int)ParamId::COUNT];

// Current parameter index for cycling
static int g_currentParamIndex = 0;

// ---------------------------------------------------------------------------
// Reverb: the two reverbs in src/reverb, run as one custom DSP on the stream.
// Simple is a 16 line FDN reverb; Advanced is an EFX model reverb with the EFX environments.
// ---------------------------------------------------------------------------

// Rooms for the simple reverb.
struct SimpleReverbPreset {
    const char* name;
    float room, damping, preDelayMs, width, lowCutHz, highCutHz;
    float level;  // the preset's wet level relative to the default (1 = default)
};
static const SimpleReverbPreset g_simpleReverbPresets[] = {
    {"Generic",          0.5f,  0.25f, 0,   1.0f, 0,   0,    1.0f},
    {"Small room",       0.0f,  0.6f,  4,   1.0f, 0,   0,    1.0f},
    {"Medium room",      0.39f, 0.5f,  8,   1.0f, 0,   0,    1.0f},
    {"Large room",       0.67f, 0.45f, 14,  1.0f, 0,   0,    1.0f},
    {"Bathroom",         0.57f, 0.1f,  3,   1.0f, 0,   0,    1.0f},
    {"Living room",      0.0f,  0.8f,  6,   1.0f, 0,   6000, 0.75f},
    {"Stone room",       0.74f, 0.2f,  10,  1.0f, 0,   0,    1.0f},
    {"Hallway",          0.6f,  0.4f,  7,   0.7f, 0,   0,    1.0f},
    {"Carpeted hallway", 0.0f,  0.9f,  7,   1.0f, 0,   4000, 0.6f},
    {"Auditorium",       0.84f, 0.4f,  20,  1.0f, 0,   0,    1.0f},
    {"Concert hall",     0.88f, 0.35f, 24,  1.0f, 0,   0,    1.0f},
    {"Cave",             0.82f, 0.15f, 15,  1.0f, 0,   0,    1.0f},
    {"Arena",            0.97f, 0.3f,  30,  1.0f, 0,   0,    1.0f},
    {"Hangar",           1.0f,  0.35f, 30,  1.0f, 0,   0,    1.0f},
    {"Forest",           0.6f,  0.8f,  40,  1.0f, 0,   5000, 0.45f},
    {"City",             0.6f,  0.7f,  20,  1.0f, 0,   6000, 0.45f},
    {"Mountains",        0.6f,  0.7f,  120, 1.0f, 0,   0,    0.3f},
    {"Parking lot",      0.64f, 0.5f,  25,  1.0f, 0,   0,    0.6f},
    {"Sewer pipe",       0.81f, 0.2f,  10,  0.4f, 150, 5000, 1.0f},
    {"Underwater",       0.6f,  1.0f,  5,   1.0f, 0,   1200, 1.0f},
    {"Cathedral",        0.95f, 0.3f,  35,  1.0f, 0,   0,    1.0f},
    {"Plate",            0.71f, 0.2f,  0,   1.0f, 0,   0,    1.0f},
};
static const int g_simpleReverbPresetCount = sizeof(g_simpleReverbPresets) / sizeof(g_simpleReverbPresets[0]);

// Environments for the advanced reverb: the EFX default presets (OpenAL Soft's efx-presets.h),
// field for field in EfxReverbParams order.
struct AdvancedReverbPreset {
    const char* name;
    fastplay::audio::EfxReverbParams params;
};
static const AdvancedReverbPreset g_advancedReverbPresets[] = {
    {"Generic", { 1.0000f, 1.0000f, 0.3162f, 0.8913f, 1.0000f, 1.4900f, 0.8300f, 1.0000f, 0.0500f, 0.0070f, { 0.0000f, 0.0000f, 0.0000f }, 1.2589f, 0.0110f, { 0.0000f, 0.0000f, 0.0000f }, 0.2500f, 0.0000f, 0.2500f, 0.0000f, 0.9943f, 5000.0000f, 250.0000f, 0.0000f, true }},
    {"Padded cell", { 0.1715f, 1.0000f, 0.3162f, 0.0010f, 1.0000f, 0.1700f, 0.1000f, 1.0000f, 0.2500f, 0.0010f, { 0.0000f, 0.0000f, 0.0000f }, 1.2691f, 0.0020f, { 0.0000f, 0.0000f, 0.0000f }, 0.2500f, 0.0000f, 0.2500f, 0.0000f, 0.9943f, 5000.0000f, 250.0000f, 0.0000f, true }},
    {"Room", { 0.4287f, 1.0000f, 0.3162f, 0.5929f, 1.0000f, 0.4000f, 0.8300f, 1.0000f, 0.1503f, 0.0020f, { 0.0000f, 0.0000f, 0.0000f }, 1.0629f, 0.0030f, { 0.0000f, 0.0000f, 0.0000f }, 0.2500f, 0.0000f, 0.2500f, 0.0000f, 0.9943f, 5000.0000f, 250.0000f, 0.0000f, true }},
    {"Bathroom", { 0.1715f, 1.0000f, 0.3162f, 0.2512f, 1.0000f, 1.4900f, 0.5400f, 1.0000f, 0.6531f, 0.0070f, { 0.0000f, 0.0000f, 0.0000f }, 3.2734f, 0.0110f, { 0.0000f, 0.0000f, 0.0000f }, 0.2500f, 0.0000f, 0.2500f, 0.0000f, 0.9943f, 5000.0000f, 250.0000f, 0.0000f, true }},
    {"Living room", { 0.9766f, 1.0000f, 0.3162f, 0.0010f, 1.0000f, 0.5000f, 0.1000f, 1.0000f, 0.2051f, 0.0030f, { 0.0000f, 0.0000f, 0.0000f }, 0.2805f, 0.0040f, { 0.0000f, 0.0000f, 0.0000f }, 0.2500f, 0.0000f, 0.2500f, 0.0000f, 0.9943f, 5000.0000f, 250.0000f, 0.0000f, true }},
    {"Stone room", { 1.0000f, 1.0000f, 0.3162f, 0.7079f, 1.0000f, 2.3100f, 0.6400f, 1.0000f, 0.4411f, 0.0120f, { 0.0000f, 0.0000f, 0.0000f }, 1.1003f, 0.0170f, { 0.0000f, 0.0000f, 0.0000f }, 0.2500f, 0.0000f, 0.2500f, 0.0000f, 0.9943f, 5000.0000f, 250.0000f, 0.0000f, true }},
    {"Auditorium", { 1.0000f, 1.0000f, 0.3162f, 0.5781f, 1.0000f, 4.3200f, 0.5900f, 1.0000f, 0.4032f, 0.0200f, { 0.0000f, 0.0000f, 0.0000f }, 0.7170f, 0.0300f, { 0.0000f, 0.0000f, 0.0000f }, 0.2500f, 0.0000f, 0.2500f, 0.0000f, 0.9943f, 5000.0000f, 250.0000f, 0.0000f, true }},
    {"Concert hall", { 1.0000f, 1.0000f, 0.3162f, 0.5623f, 1.0000f, 3.9200f, 0.7000f, 1.0000f, 0.2427f, 0.0200f, { 0.0000f, 0.0000f, 0.0000f }, 0.9977f, 0.0290f, { 0.0000f, 0.0000f, 0.0000f }, 0.2500f, 0.0000f, 0.2500f, 0.0000f, 0.9943f, 5000.0000f, 250.0000f, 0.0000f, true }},
    {"Cave", { 1.0000f, 1.0000f, 0.3162f, 1.0000f, 1.0000f, 2.9100f, 1.3000f, 1.0000f, 0.5000f, 0.0150f, { 0.0000f, 0.0000f, 0.0000f }, 0.7063f, 0.0220f, { 0.0000f, 0.0000f, 0.0000f }, 0.2500f, 0.0000f, 0.2500f, 0.0000f, 0.9943f, 5000.0000f, 250.0000f, 0.0000f, false }},
    {"Arena", { 1.0000f, 1.0000f, 0.3162f, 0.4477f, 1.0000f, 7.2400f, 0.3300f, 1.0000f, 0.2612f, 0.0200f, { 0.0000f, 0.0000f, 0.0000f }, 1.0186f, 0.0300f, { 0.0000f, 0.0000f, 0.0000f }, 0.2500f, 0.0000f, 0.2500f, 0.0000f, 0.9943f, 5000.0000f, 250.0000f, 0.0000f, true }},
    {"Hangar", { 1.0000f, 1.0000f, 0.3162f, 0.3162f, 1.0000f, 10.0500f, 0.2300f, 1.0000f, 0.5000f, 0.0200f, { 0.0000f, 0.0000f, 0.0000f }, 1.2560f, 0.0300f, { 0.0000f, 0.0000f, 0.0000f }, 0.2500f, 0.0000f, 0.2500f, 0.0000f, 0.9943f, 5000.0000f, 250.0000f, 0.0000f, true }},
    {"Carpeted hallway", { 0.4287f, 1.0000f, 0.3162f, 0.0100f, 1.0000f, 0.3000f, 0.1000f, 1.0000f, 0.1215f, 0.0020f, { 0.0000f, 0.0000f, 0.0000f }, 0.1531f, 0.0300f, { 0.0000f, 0.0000f, 0.0000f }, 0.2500f, 0.0000f, 0.2500f, 0.0000f, 0.9943f, 5000.0000f, 250.0000f, 0.0000f, true }},
    {"Hallway", { 0.3645f, 1.0000f, 0.3162f, 0.7079f, 1.0000f, 1.4900f, 0.5900f, 1.0000f, 0.2458f, 0.0070f, { 0.0000f, 0.0000f, 0.0000f }, 1.6615f, 0.0110f, { 0.0000f, 0.0000f, 0.0000f }, 0.2500f, 0.0000f, 0.2500f, 0.0000f, 0.9943f, 5000.0000f, 250.0000f, 0.0000f, true }},
    {"Stone corridor", { 1.0000f, 1.0000f, 0.3162f, 0.7612f, 1.0000f, 2.7000f, 0.7900f, 1.0000f, 0.2472f, 0.0130f, { 0.0000f, 0.0000f, 0.0000f }, 1.5758f, 0.0200f, { 0.0000f, 0.0000f, 0.0000f }, 0.2500f, 0.0000f, 0.2500f, 0.0000f, 0.9943f, 5000.0000f, 250.0000f, 0.0000f, true }},
    {"Alley", { 1.0000f, 0.3000f, 0.3162f, 0.7328f, 1.0000f, 1.4900f, 0.8600f, 1.0000f, 0.2500f, 0.0070f, { 0.0000f, 0.0000f, 0.0000f }, 0.9954f, 0.0110f, { 0.0000f, 0.0000f, 0.0000f }, 0.1250f, 0.9500f, 0.2500f, 0.0000f, 0.9943f, 5000.0000f, 250.0000f, 0.0000f, true }},
    {"Forest", { 1.0000f, 0.3000f, 0.3162f, 0.0224f, 1.0000f, 1.4900f, 0.5400f, 1.0000f, 0.0525f, 0.1620f, { 0.0000f, 0.0000f, 0.0000f }, 0.7682f, 0.0880f, { 0.0000f, 0.0000f, 0.0000f }, 0.1250f, 1.0000f, 0.2500f, 0.0000f, 0.9943f, 5000.0000f, 250.0000f, 0.0000f, true }},
    {"City", { 1.0000f, 0.5000f, 0.3162f, 0.3981f, 1.0000f, 1.4900f, 0.6700f, 1.0000f, 0.0730f, 0.0070f, { 0.0000f, 0.0000f, 0.0000f }, 0.1427f, 0.0110f, { 0.0000f, 0.0000f, 0.0000f }, 0.2500f, 0.0000f, 0.2500f, 0.0000f, 0.9943f, 5000.0000f, 250.0000f, 0.0000f, true }},
    {"Mountains", { 1.0000f, 0.2700f, 0.3162f, 0.0562f, 1.0000f, 1.4900f, 0.2100f, 1.0000f, 0.0407f, 0.3000f, { 0.0000f, 0.0000f, 0.0000f }, 0.1919f, 0.1000f, { 0.0000f, 0.0000f, 0.0000f }, 0.2500f, 1.0000f, 0.2500f, 0.0000f, 0.9943f, 5000.0000f, 250.0000f, 0.0000f, false }},
    {"Quarry", { 1.0000f, 1.0000f, 0.3162f, 0.3162f, 1.0000f, 1.4900f, 0.8300f, 1.0000f, 0.0000f, 0.0610f, { 0.0000f, 0.0000f, 0.0000f }, 1.7783f, 0.0250f, { 0.0000f, 0.0000f, 0.0000f }, 0.1250f, 0.7000f, 0.2500f, 0.0000f, 0.9943f, 5000.0000f, 250.0000f, 0.0000f, true }},
    {"Plain", { 1.0000f, 0.2100f, 0.3162f, 0.1000f, 1.0000f, 1.4900f, 0.5000f, 1.0000f, 0.0585f, 0.1790f, { 0.0000f, 0.0000f, 0.0000f }, 0.1089f, 0.1000f, { 0.0000f, 0.0000f, 0.0000f }, 0.2500f, 1.0000f, 0.2500f, 0.0000f, 0.9943f, 5000.0000f, 250.0000f, 0.0000f, true }},
    {"Parking lot", { 1.0000f, 1.0000f, 0.3162f, 1.0000f, 1.0000f, 1.6500f, 1.5000f, 1.0000f, 0.2082f, 0.0080f, { 0.0000f, 0.0000f, 0.0000f }, 0.2652f, 0.0120f, { 0.0000f, 0.0000f, 0.0000f }, 0.2500f, 0.0000f, 0.2500f, 0.0000f, 0.9943f, 5000.0000f, 250.0000f, 0.0000f, false }},
    {"Sewer pipe", { 0.3071f, 0.8000f, 0.3162f, 0.3162f, 1.0000f, 2.8100f, 0.1400f, 1.0000f, 1.6387f, 0.0140f, { 0.0000f, 0.0000f, 0.0000f }, 3.2471f, 0.0210f, { 0.0000f, 0.0000f, 0.0000f }, 0.2500f, 0.0000f, 0.2500f, 0.0000f, 0.9943f, 5000.0000f, 250.0000f, 0.0000f, true }},
    {"Underwater", { 0.3645f, 1.0000f, 0.3162f, 0.0100f, 1.0000f, 1.4900f, 0.1000f, 1.0000f, 0.5963f, 0.0070f, { 0.0000f, 0.0000f, 0.0000f }, 7.0795f, 0.0110f, { 0.0000f, 0.0000f, 0.0000f }, 0.2500f, 0.0000f, 1.1800f, 0.3480f, 0.9943f, 5000.0000f, 250.0000f, 0.0000f, true }},
    {"Drugged", { 0.4287f, 0.5000f, 0.3162f, 1.0000f, 1.0000f, 8.3900f, 1.3900f, 1.0000f, 0.8760f, 0.0020f, { 0.0000f, 0.0000f, 0.0000f }, 3.1081f, 0.0300f, { 0.0000f, 0.0000f, 0.0000f }, 0.2500f, 0.0000f, 0.2500f, 1.0000f, 0.9943f, 5000.0000f, 250.0000f, 0.0000f, false }},
    {"Dizzy", { 0.3645f, 0.6000f, 0.3162f, 0.6310f, 1.0000f, 17.2300f, 0.5600f, 1.0000f, 0.1392f, 0.0200f, { 0.0000f, 0.0000f, 0.0000f }, 0.4937f, 0.0300f, { 0.0000f, 0.0000f, 0.0000f }, 0.2500f, 1.0000f, 0.8100f, 0.3100f, 0.9943f, 5000.0000f, 250.0000f, 0.0000f, false }},
    {"Psychotic", { 0.0625f, 0.5000f, 0.3162f, 0.8404f, 1.0000f, 7.5600f, 0.9100f, 1.0000f, 0.4864f, 0.0200f, { 0.0000f, 0.0000f, 0.0000f }, 2.4378f, 0.0300f, { 0.0000f, 0.0000f, 0.0000f }, 0.2500f, 0.0000f, 4.0000f, 1.0000f, 0.9943f, 5000.0000f, 250.0000f, 0.0000f, false }},
};
static const int g_advancedReverbPresetCount = sizeof(g_advancedReverbPresets) / sizeof(g_advancedReverbPresets[0]);

// High cut at its maximum means off.
static const float kReverbHighCutOff = 20000.0f;

struct ReverbEngine {
    std::mutex mutex;  // params are set from the UI thread, processing runs on BASS's
    fastplay::audio::Reverb simple;
    fastplay::audio::EfxReverb advanced;
    fastplay::audio::ReverbParams simpleParams;
    fastplay::audio::EfxReverbParams advancedParams;
    int algorithm = 0;
    int sampleRate = 0;
    float dry = 1.0f;          // target dry gain
    float dryCurrent = 1.0f;   // ramped towards dry once per block
    std::vector<float> inL, inR, outL, outR;
};
static ReverbEngine g_reverbEngine;

// Mix: 0% is dry only, 50% is the dry signal at full level with the reverb at its natural level,
// 100% is the reverb only.
static void ReverbMixGains(float mixPercent, float& dry, float& wet) {
    float m = clamp_val(mixPercent / 100.0f, 0.0f, 1.0f);
    dry = std::min(1.0f, 2.0f * (1.0f - m));
    wet = std::min(1.0f, 2.0f * m);
}

static int ReverbPresetIndex(ParamId id, int count) {
    int i = static_cast<int>(std::lround(g_paramValues[(int)id]));
    return clamp_val(i, 0, count - 1);
}

// Copy a preset's values into the parameters the user can adjust.
static void ApplySimpleReverbPreset(int index) {
    const SimpleReverbPreset& p = g_simpleReverbPresets[clamp_val(index, 0, g_simpleReverbPresetCount - 1)];
    g_paramValues[(int)ParamId::ReverbRoom] = p.room * 100.0f;
    g_paramValues[(int)ParamId::ReverbDamp] = p.damping * 100.0f;
    g_paramValues[(int)ParamId::ReverbWidth] = p.width * 100.0f;
    g_paramValues[(int)ParamId::ReverbPreDelay] = p.preDelayMs;
    g_paramValues[(int)ParamId::ReverbLowCut] = p.lowCutHz;
    g_paramValues[(int)ParamId::ReverbHighCut] = p.highCutHz > 0 ? p.highCutHz : kReverbHighCutOff;
}

static float GainToDb(float gain, float minDb, float maxDb) {
    float db = gain > 0.0f ? 20.0f * std::log10(gain) : minDb;
    return clamp_val(std::round(db), minDb, maxDb);
}

static void ApplyAdvancedReverbPreset(int index) {
    const fastplay::audio::EfxReverbParams& p = g_advancedReverbPresets[clamp_val(index, 0, g_advancedReverbPresetCount - 1)].params;
    g_paramValues[(int)ParamId::AdvReverbDecay] = p.decay_time;
    g_paramValues[(int)ParamId::AdvReverbHFRatio] = p.decay_hf_ratio;
    g_paramValues[(int)ParamId::AdvReverbDensity] = p.density * 100.0f;
    g_paramValues[(int)ParamId::AdvReverbDiffusion] = p.diffusion * 100.0f;
    g_paramValues[(int)ParamId::AdvReverbReflections] = GainToDb(p.reflections_gain, -60.0f, 10.0f);
    g_paramValues[(int)ParamId::AdvReverbLate] = GainToDb(p.late_reverb_gain, -60.0f, 20.0f);
    g_paramValues[(int)ParamId::AdvReverbReflDelay] = p.reflections_delay * 1000.0f;
    g_paramValues[(int)ParamId::AdvReverbLateDelay] = p.late_reverb_delay * 1000.0f;
}

// Rebuild both reverbs' settings from the parameter values.
static void UpdateReverbParams() {
    const float* v = g_paramValues;
    float dry, wet;

    fastplay::audio::ReverbParams sp;
    const SimpleReverbPreset& room = g_simpleReverbPresets[ReverbPresetIndex(ParamId::ReverbPreset, g_simpleReverbPresetCount)];
    ReverbMixGains(v[(int)ParamId::ReverbMix], dry, wet);
    float simpleDry = dry;
    sp.room_size = v[(int)ParamId::ReverbRoom] / 100.0f;
    sp.damping = v[(int)ParamId::ReverbDamp] / 100.0f;
    sp.width = v[(int)ParamId::ReverbWidth] / 100.0f;
    sp.pre_delay_ms = v[(int)ParamId::ReverbPreDelay];
    sp.low_cut_hz = v[(int)ParamId::ReverbLowCut];
    sp.high_cut_hz = v[(int)ParamId::ReverbHighCut] >= kReverbHighCutOff ? 0.0f : v[(int)ParamId::ReverbHighCut];
    sp.wet = wet * room.level / 3.0f;  // the reverb's wet is scaled so 1/3 is unity
    sp.dry = 0.0f;                     // the dry signal is mixed here, not by the reverb

    fastplay::audio::EfxReverbParams ap = g_advancedReverbPresets[ReverbPresetIndex(ParamId::AdvReverbPreset, g_advancedReverbPresetCount)].params;
    ReverbMixGains(v[(int)ParamId::AdvReverbMix], dry, wet);
    float advancedDry = dry;
    ap.decay_time = v[(int)ParamId::AdvReverbDecay];
    ap.decay_hf_ratio = v[(int)ParamId::AdvReverbHFRatio];
    ap.density = v[(int)ParamId::AdvReverbDensity] / 100.0f;
    ap.diffusion = v[(int)ParamId::AdvReverbDiffusion] / 100.0f;
    ap.reflections_gain = std::pow(10.0f, v[(int)ParamId::AdvReverbReflections] / 20.0f);
    ap.late_reverb_gain = std::pow(10.0f, v[(int)ParamId::AdvReverbLate] / 20.0f);
    ap.reflections_delay = v[(int)ParamId::AdvReverbReflDelay] / 1000.0f;
    ap.late_reverb_delay = v[(int)ParamId::AdvReverbLateDelay] / 1000.0f;
    ap.gain *= wet;

    std::lock_guard<std::mutex> lock(g_reverbEngine.mutex);
    g_reverbEngine.algorithm = g_reverbAlgorithm;
    g_reverbEngine.simpleParams = sp;
    g_reverbEngine.advancedParams = ap;
    g_reverbEngine.dry = g_reverbAlgorithm == 2 ? advancedDry : simpleDry;
    if (g_reverbEngine.sampleRate > 0) {
        g_reverbEngine.simple.set_params(sp);
        g_reverbEngine.advanced.set_params(ap);
    }
}

// Start the reverbs fresh at a stream's sample rate (no tail carried over from the last track).
static void ResetReverbEngine(int sampleRate) {
    std::lock_guard<std::mutex> lock(g_reverbEngine.mutex);
    if (sampleRate != g_reverbEngine.sampleRate) {
        g_reverbEngine.simple.init(sampleRate);
        g_reverbEngine.advanced.init(sampleRate);
        g_reverbEngine.sampleRate = sampleRate;
    }
    g_reverbEngine.simple.set_params(g_reverbEngine.simpleParams);
    g_reverbEngine.advanced.set_params(g_reverbEngine.advancedParams);
    g_reverbEngine.simple.reset();
    g_reverbEngine.advanced.reset();
    g_reverbEngine.dryCurrent = g_reverbEngine.dry;
}

static void CALLBACK ReverbDSPProc(HDSP handle, DWORD channel, void* buffer, DWORD length, void* user) {
    BASS_CHANNELINFO info;
    if (!BASS_ChannelGetInfo(channel, &info) || !(info.flags & BASS_SAMPLE_FLOAT) || info.chans == 0) return;

    ReverbEngine& e = g_reverbEngine;
    std::lock_guard<std::mutex> lock(e.mutex);
    if (e.algorithm == 0 || e.sampleRate <= 0) return;

    float* samples = static_cast<float*>(buffer);
    const int chans = static_cast<int>(info.chans);
    const int frames = static_cast<int>(length / (sizeof(float) * chans));
    if (frames <= 0) return;

    e.inL.resize(frames); e.inR.resize(frames);
    e.outL.assign(frames, 0.0f); e.outR.assign(frames, 0.0f);
    for (int i = 0; i < frames; i++) {
        e.inL[i] = samples[i * chans];
        e.inR[i] = samples[i * chans + (chans > 1 ? 1 : 0)];
    }

    if (e.algorithm == 2) {
        e.advanced.process(e.inL.data(), e.inR.data(), e.outL.data(), e.outR.data(), frames);
    } else {
        e.simple.process(e.inL.data(), e.inR.data(), e.outL.data(), e.outR.data(), frames);
    }

    // Dry gain ramps across the block so moving the mix does not click.
    const float dry0 = e.dryCurrent, dryStep = (e.dry - e.dryCurrent) / frames;
    for (int i = 0; i < frames; i++) {
        const float dry = dry0 + dryStep * (i + 1);
        float* frame = samples + static_cast<size_t>(i) * chans;
        if (chans == 1) {
            frame[0] = frame[0] * dry + 0.5f * (e.outL[i] + e.outR[i]);
            continue;
        }
        frame[0] = frame[0] * dry + e.outL[i];
        frame[1] = frame[1] * dry + e.outR[i];
        for (int c = 2; c < chans; c++) frame[c] *= dry;
    }
    e.dryCurrent = e.dry;
}

// Initialize parameter values to defaults
bool InitEffects() {
    for (int i = 0; i < g_paramDefCount; i++) {
        g_paramValues[(int)g_paramDefs[i].id] = g_paramDefs[i].defaultValue;
    }
    // Note: g_tempo, g_pitch, g_rate are loaded from settings in LoadSettings()
    // Only set defaults if they haven't been loaded yet (all zero means uninitialized)
    // Actually, these are loaded before InitEffects, so don't overwrite them
    UpdateReverbParams();
    return true;
}

void FreeEffects() {
    RemoveDSPEffects();
    FreeCenterCancelProcessor();
#ifdef USE_STEAM_AUDIO
    FreeSpatialAudio();
#endif
}

// Helper to check if a reverb param matches current algorithm
static bool IsReverbParamForCurrentAlgorithm(ParamId id) {
    switch (id) {
        // Simple reverb params (algorithm 1)
        case ParamId::ReverbPreset:
        case ParamId::ReverbMix:
        case ParamId::ReverbRoom:
        case ParamId::ReverbDamp:
        case ParamId::ReverbWidth:
        case ParamId::ReverbPreDelay:
        case ParamId::ReverbLowCut:
        case ParamId::ReverbHighCut:
            return g_reverbAlgorithm == 1;
        // Advanced reverb params (algorithm 2)
        case ParamId::AdvReverbPreset:
        case ParamId::AdvReverbMix:
        case ParamId::AdvReverbDecay:
        case ParamId::AdvReverbHFRatio:
        case ParamId::AdvReverbDensity:
        case ParamId::AdvReverbDiffusion:
        case ParamId::AdvReverbReflections:
        case ParamId::AdvReverbLate:
        case ParamId::AdvReverbReflDelay:
        case ParamId::AdvReverbLateDelay:
            return g_reverbAlgorithm == 2;
        default:
            return true;  // Not a reverb param
    }
}

// Build list of available parameters based on enabled stream effects and DSP effects
static std::vector<ParamId> GetAvailableParams() {
    std::vector<ParamId> params;

    for (int i = 0; i < g_paramDefCount; i++) {
        const ParamDef& def = g_paramDefs[i];

        // Check if this is a stream effect parameter
        if ((int)def.dspEffect == -1) {
            // Stream effect - check if enabled in g_effectEnabled
            int effectIdx = (int)def.id;
            if (effectIdx < 4 && g_effectEnabled[effectIdx]) {
                params.push_back(def.id);
            }
        } else if (def.dspEffect == DSPEffectType::Reverb) {
            // Reverb parameter - check algorithm and matching params
            if (g_reverbAlgorithm > 0 && IsReverbParamForCurrentAlgorithm(def.id)) {
                params.push_back(def.id);
            }
        } else {
            // Other DSP effect parameter - check if the DSP effect is enabled
            if (g_dspEnabled[(int)def.dspEffect]) {
                params.push_back(def.id);
            }
        }
    }

    return params;
}

int GetAvailableParamCount() {
    return (int)GetAvailableParams().size();
}

// Toggle stream effect (Volume=0, Pitch=1, Tempo=2, Rate=3)
void ToggleStreamEffect(int effectIndex) {
    if (effectIndex < 0 || effectIndex >= 4) return;

    g_effectEnabled[effectIndex] = !g_effectEnabled[effectIndex];

    const char* names[] = {"Volume", "Pitch", "Tempo", "Rate"};
    std::string msg = std::string(names[effectIndex]) +
                      (g_effectEnabled[effectIndex] ? " enabled" : " disabled");
    Speak(msg);
}

bool IsStreamEffectEnabled(int effectIndex) {
    if (effectIndex < 0 || effectIndex >= 4) return false;
    return g_effectEnabled[effectIndex];
}

// Toggle DSP effect
void ToggleDSPEffect(DSPEffectType type) {
    if ((int)type < 0 || (int)type >= (int)DSPEffectType::COUNT) return;

    // Reverb uses algorithm selection, not simple toggle
    if (type == DSPEffectType::Reverb) {
        // Cycle through reverb algorithms: Off -> Simple -> Advanced -> Off
        int newAlgo = (g_reverbAlgorithm + 1) % (int)ReverbAlgorithm::COUNT;
        SetReverbAlgorithm(newAlgo);
        const char* algoNames[] = {"Off", "Simple", "Advanced"};
        Speak(std::string("Reverb: ") + algoNames[newAlgo]);
        return;
    }

    bool newState = !g_dspEnabled[(int)type];
    EnableDSPEffect(type, newState);

    const char* names[] = {"Reverb", "Echo", "EQ", "Compressor", "Stereo Width", "Center Cancel", "Convolution", "3D Audio"};
    std::string msg = std::string(names[(int)type]) +
                      (newState ? " enabled" : " disabled");
    Speak(msg);
}

// Set reverb algorithm (0=Off, 1=Simple, 2=Advanced)
void SetReverbAlgorithm(int algorithm) {
    if (algorithm < 0 || algorithm >= (int)ReverbAlgorithm::COUNT) return;

    // Remove existing reverb effect if any
    if (g_hdspReverb && g_fxStream) {
        BASS_ChannelRemoveDSP(g_fxStream, g_hdspReverb);
        g_hdspReverb = 0;
    }

    g_reverbAlgorithm = algorithm;
    UpdateReverbParams();

    // Apply new reverb if enabled and stream exists
    if (algorithm > 0 && g_fxStream) {
        ApplyDSPEffects();
    }
}

// Enable or disable a DSP effect
void EnableDSPEffect(DSPEffectType type, bool enable) {
    if ((int)type < 0 || (int)type >= (int)DSPEffectType::COUNT) return;

    bool wasEnabled = g_dspEnabled[(int)type];
    g_dspEnabled[(int)type] = enable;

    // If stream exists, apply/remove effect immediately
    if (g_fxStream) {
        if (enable && !wasEnabled) {
            ApplyDSPEffects();
        } else if (!enable && wasEnabled) {
            // Remove just this effect
            switch (type) {
                case DSPEffectType::Reverb:
                    if (g_hdspReverb) { BASS_ChannelRemoveDSP(g_fxStream, g_hdspReverb); g_hdspReverb = 0; }
                    break;
                case DSPEffectType::Echo:
                    if (g_hfxEcho) { BASS_ChannelRemoveFX(g_fxStream, g_hfxEcho); g_hfxEcho = 0; }
                    break;
                case DSPEffectType::EQ:
                    if (g_hfxEQPreamp) { BASS_ChannelRemoveFX(g_fxStream, g_hfxEQPreamp); g_hfxEQPreamp = 0; }
                    if (g_hfxEQBass) { BASS_ChannelRemoveFX(g_fxStream, g_hfxEQBass); g_hfxEQBass = 0; }
                    if (g_hfxEQMid) { BASS_ChannelRemoveFX(g_fxStream, g_hfxEQMid); g_hfxEQMid = 0; }
                    if (g_hfxEQTreble) { BASS_ChannelRemoveFX(g_fxStream, g_hfxEQTreble); g_hfxEQTreble = 0; }
                    break;
                case DSPEffectType::Compressor:
                    if (g_hfxCompressor) { BASS_ChannelRemoveFX(g_fxStream, g_hfxCompressor); g_hfxCompressor = 0; }
                    break;
                case DSPEffectType::StereoWidth:
                    if (g_hdspStereoWidth) { BASS_ChannelRemoveDSP(g_fxStream, g_hdspStereoWidth); g_hdspStereoWidth = 0; }
                    break;
                case DSPEffectType::CenterCancel:
                    if (g_hdspCenterCancel) { BASS_ChannelRemoveDSP(g_fxStream, g_hdspCenterCancel); g_hdspCenterCancel = 0; }
                    break;
                case DSPEffectType::Convolution:
                    if (g_hdspConvolution) { BASS_ChannelRemoveDSP(g_fxStream, g_hdspConvolution); g_hdspConvolution = 0; }
                    break;
                case DSPEffectType::SpatialAudio:
                    if (g_hdspSpatialAudio) { BASS_ChannelRemoveDSP(g_fxStream, g_hdspSpatialAudio); g_hdspSpatialAudio = 0; }
                    break;
                default:
                    break;
            }
        }
    }
}

// Stereo width DSP callback - uses Mid/Side processing
// Width 0% = mono, 100% = normal stereo, 200% = extra wide
static void CALLBACK StereoWidthDSPProc(HDSP handle, DWORD channel, void* buffer, DWORD length, void* user) {
    // Get channel info to check format
    BASS_CHANNELINFO info;
    if (!BASS_ChannelGetInfo(channel, &info)) return;

    // Only process stereo content
    if (info.chans != 2) return;

    // Get width value (0-200, where 100 is normal)
    float width = g_paramValues[(int)ParamId::StereoWidth] / 100.0f;

    // Check if float format (BASS_FX tempo streams use float)
    if (info.flags & BASS_SAMPLE_FLOAT) {
        float* samples = static_cast<float*>(buffer);
        DWORD frameCount = length / (sizeof(float) * 2);  // 2 channels

        for (DWORD i = 0; i < frameCount; i++) {
            float left = samples[i * 2];
            float right = samples[i * 2 + 1];

            // Convert to Mid/Side
            float mid = (left + right) * 0.5f;
            float side = (left - right) * 0.5f;

            // Apply width to side signal
            side *= width;

            // Convert back to Left/Right
            samples[i * 2] = mid + side;
            samples[i * 2 + 1] = mid - side;
        }
    } else {
        // 16-bit format
        short* samples = static_cast<short*>(buffer);
        DWORD frameCount = length / (sizeof(short) * 2);  // 2 channels

        for (DWORD i = 0; i < frameCount; i++) {
            float left = samples[i * 2] / 32768.0f;
            float right = samples[i * 2 + 1] / 32768.0f;

            // Convert to Mid/Side
            float mid = (left + right) * 0.5f;
            float side = (left - right) * 0.5f;

            // Apply width to side signal
            side *= width;

            // Convert back to Left/Right and clamp
            float outL = mid + side;
            float outR = mid - side;
            if (outL > 1.0f) outL = 1.0f; else if (outL < -1.0f) outL = -1.0f;
            if (outR > 1.0f) outR = 1.0f; else if (outR < -1.0f) outR = -1.0f;

            samples[i * 2] = static_cast<short>(outL * 32767.0f);
            samples[i * 2 + 1] = static_cast<short>(outR * 32767.0f);
        }
    }
}

// Center cancel/extract DSP callback - FFT-based spectral processing
// -100% = extract center (isolate vocals), 0% = no effect, +100% = cancel center (remove vocals)
static void CALLBACK CenterCancelDSPProc(HDSP handle, DWORD channel, void* buffer, DWORD length, void* user) {
    // Get channel info to check format
    BASS_CHANNELINFO info;
    if (!BASS_ChannelGetInfo(channel, &info)) return;

    // Only process stereo content
    if (info.chans != 2) return;

    // Get center cancel value (-100 to +100, where 0 is no effect)
    float amount = g_paramValues[(int)ParamId::CenterCancel] / 100.0f;

    // Get or initialize the processor
    CenterCancelProcessor* processor = GetCenterCancelProcessor();
    if (!processor) {
        InitCenterCancelProcessor((int)info.freq);
        processor = GetCenterCancelProcessor();
    }
    if (!processor || !processor->IsInitialized()) return;

    // Update the amount
    processor->SetAmount(amount);

    // If amount is 0, processor will passthrough
    if (info.flags & BASS_SAMPLE_FLOAT) {
        float* samples = static_cast<float*>(buffer);
        int frameCount = length / (sizeof(float) * 2);
        int outputFrames = 0;

        // Process in-place
        std::vector<float> tempOut(frameCount * 2);
        processor->ProcessFloat(samples, frameCount, tempOut.data(), outputFrames);

        // Copy output back (outputFrames should equal frameCount for steady-state)
        for (int i = 0; i < outputFrames * 2; i++) {
            samples[i] = tempOut[i];
        }
    } else {
        // 16-bit format
        short* samples = static_cast<short*>(buffer);
        int frameCount = length / (sizeof(short) * 2);
        int outputFrames = 0;

        std::vector<short> tempOut(frameCount * 2);
        processor->ProcessInt16(samples, frameCount, tempOut.data(), outputFrames);

        for (int i = 0; i < outputFrames * 2; i++) {
            samples[i] = tempOut[i];
        }
    }
}

// Convolution reverb DSP callback
static void CALLBACK ConvolutionDSPProc(HDSP handle, DWORD channel, void* buffer, DWORD length, void* user) {
    BASS_CHANNELINFO info;
    if (!BASS_ChannelGetInfo(channel, &info)) return;

    // Only process stereo content
    if (info.chans != 2) return;

    ConvolutionReverb* conv = GetConvolutionReverb();
    if (!conv) return;

    // Initialize if IR is loaded but not yet initialized
    if (conv->IsLoaded() && !conv->IsInitialized()) {
        conv->Init((int)info.freq);
    }

    conv->SetMix(g_paramValues[(int)ParamId::ConvolutionMix]);
    conv->SetGain(g_paramValues[(int)ParamId::ConvolutionGain]);

    // Handle both float and 16-bit formats
    if (info.flags & BASS_SAMPLE_FLOAT) {
        float* samples = static_cast<float*>(buffer);
        int frameCount = length / (sizeof(float) * 2);
        conv->Process(samples, frameCount);
    } else {
        // Convert 16-bit to float, process, convert back
        short* samples = static_cast<short*>(buffer);
        int frameCount = length / (sizeof(short) * 2);

        std::vector<float> floatBuf(frameCount * 2);
        for (int i = 0; i < frameCount * 2; i++) {
            floatBuf[i] = samples[i] / 32768.0f;
        }

        conv->Process(floatBuf.data(), frameCount);

        for (int i = 0; i < frameCount * 2; i++) {
            float val = floatBuf[i];
            if (val > 1.0f) val = 1.0f;
            if (val < -1.0f) val = -1.0f;
            samples[i] = static_cast<short>(val * 32767.0f);
        }
    }
}

// 3D Audio DSP callback - HRTF binaural rendering via Steam Audio
#ifdef USE_STEAM_AUDIO
static volatile int g_spatialCrashStep = 0;

static void SpatialAudioDSPProcInner(HDSP handle, DWORD channel, void* buffer, DWORD length) {
    g_spatialCrashStep = 1;  // entered callback
    BASS_CHANNELINFO info;
    if (!BASS_ChannelGetInfo(channel, &info)) return;
    if (info.chans != 2) return;

    g_spatialCrashStep = 2;  // got channel info
    SpatialAudio* spatial = GetSpatialAudio();
    if (!spatial || !spatial->IsInitialized()) return;

    g_spatialCrashStep = 3;  // spatial ready
    float blend = g_paramValues[(int)ParamId::SpatialBlend] / 100.0f;
    if (blend <= 0.0f) return;

    g_spatialCrashStep = 4;  // about to process
    if (info.flags & BASS_SAMPLE_FLOAT) {
        float* samples = static_cast<float*>(buffer);
        int frameCount = length / (sizeof(float) * 2);
        g_spatialCrashStep = 5;  // float path, calling Process
        spatial->Process(samples, frameCount, blend);
        g_spatialCrashStep = 6;  // Process returned OK
    } else {
        short* samples = static_cast<short*>(buffer);
        int frameCount = length / (sizeof(short) * 2);
        int totalSamples = frameCount * 2;
        g_spatialCrashStep = 7;  // int16 path, getting conv buffer
        float* floatBuf = spatial->GetConversionBuffer(totalSamples);
        if (!floatBuf) return;
        g_spatialCrashStep = 8;  // converting to float
        for (int i = 0; i < totalSamples; i++)
            floatBuf[i] = samples[i] / 32768.0f;
        g_spatialCrashStep = 9;  // calling Process (int16)
        spatial->Process(floatBuf, frameCount, blend);
        g_spatialCrashStep = 10; // converting back
        for (int i = 0; i < totalSamples; i++) {
            float v = floatBuf[i];
            if (v > 1.0f) v = 1.0f; else if (v < -1.0f) v = -1.0f;
            samples[i] = static_cast<short>(v * 32767.0f);
        }
        g_spatialCrashStep = 11; // int16 done
    }
}

// Wrap in SEH to catch delay-load failures or access violations from Steam Audio
static void CALLBACK SpatialAudioDSPProc(HDSP handle, DWORD channel, void* buffer, DWORD length, void* user) {
    DWORD exCode = 0;
    __try {
        SpatialAudioDSPProcInner(handle, channel, buffer, length);
    } __except(exCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        // Steam Audio crashed - disable the effect to prevent repeated crashes
        g_dspEnabled[(int)DSPEffectType::SpatialAudio] = false;
        if (g_hdspSpatialAudio) {
            BASS_ChannelRemoveDSP(channel, g_hdspSpatialAudio);
            g_hdspSpatialAudio = 0;
        }
        int innerStep = 0;
        SpatialAudio* sp = GetSpatialAudio();
        if (sp) innerStep = sp->m_debugStep;
        char msg[256];
        snprintf(msg, sizeof(msg), "3D Audio crashed at step %d dot %d with exception 0x%08X. "
                 "120 is fill, 130 is deinterleave, 150 is surround, 160 is binaural, 170 is done.",
                 g_spatialCrashStep, innerStep, (unsigned int)exCode);
        Speak(msg);
    }
}
#endif

// Volume DSP - runs LAST (very low priority) so encoder captures full volume
// This allows recording at full volume while playback respects g_volume/g_muted
// Only used when legacy volume mode is disabled
static void CALLBACK VolumeDSPProc(HDSP handle, DWORD channel, void* buffer, DWORD length, void* user) {
    (void)handle; (void)channel; (void)user;

    // Skip if using legacy volume (handled by BASS_ATTRIB_VOL instead)
    if (g_legacyVolume) return;

    float volume = g_muted ? 0.0f : g_volume;

    // Apply perceptual volume curve (quadratic) to match BASS_ATTRIB_VOL behavior
    // This makes lower volumes feel more gradual and natural, then fold in the
    // ReplayGain multiplier so loudness normalization applies in normal volume mode.
    float curvedVolume = volume * volume * g_replayGainScale;
    if (curvedVolume == 1.0f) return; // No processing needed

    BASS_CHANNELINFO info;
    if (!BASS_ChannelGetInfo(channel, &info)) return;

    if (info.flags & BASS_SAMPLE_FLOAT) {
        float* samples = static_cast<float*>(buffer);
        int sampleCount = length / sizeof(float);
        for (int i = 0; i < sampleCount; i++) {
            samples[i] *= curvedVolume;
        }
    } else {
        short* samples = static_cast<short*>(buffer);
        int sampleCount = length / sizeof(short);
        for (int i = 0; i < sampleCount; i++) {
            samples[i] = static_cast<short>(samples[i] * curvedVolume);
        }
    }
}

bool IsDSPEffectEnabled(DSPEffectType type) {
    if ((int)type < 0 || (int)type >= (int)DSPEffectType::COUNT) return false;
    // Reverb uses g_reverbAlgorithm instead of g_dspEnabled
    if (type == DSPEffectType::Reverb) {
        return g_reverbAlgorithm > 0;
    }
    return g_dspEnabled[(int)type];
}

// Apply DSP effects to current stream
void ApplyDSPEffects() {
    if (!g_fxStream) return;

    // Reverb (based on selected algorithm)
    if (g_reverbAlgorithm > 0 && !g_hdspReverb) {
        BASS_CHANNELINFO info;
        if (BASS_ChannelGetInfo(g_fxStream, &info)) {
            ResetReverbEngine((int)info.freq);
            g_hdspReverb = BASS_ChannelSetDSP(g_fxStream, ReverbDSPProc, nullptr, 0);
        }
    }

    // Echo
    if (g_dspEnabled[(int)DSPEffectType::Echo] && !g_hfxEcho) {
        g_hfxEcho = BASS_ChannelSetFX(g_fxStream, BASS_FX_BFX_ECHO4, 0);
        if (g_hfxEcho) {
            BASS_BFX_ECHO4 echo;
            echo.fDryMix = 1.0f - (g_paramValues[(int)ParamId::EchoMix] / 100.0f);
            echo.fWetMix = g_paramValues[(int)ParamId::EchoMix] / 100.0f;
            echo.fFeedback = g_paramValues[(int)ParamId::EchoFeedback] / 100.0f;
            echo.fDelay = g_paramValues[(int)ParamId::EchoDelay] / 1000.0f;  // Convert ms to seconds
            echo.bStereo = TRUE;
            echo.lChannel = BASS_BFX_CHANALL;
            BASS_FXSetParameters(g_hfxEcho, &echo);
        }
    }

    // EQ (3-band using peaking EQ)
    if (g_dspEnabled[(int)DSPEffectType::EQ]) {
        // Preamp (gain reduction to prevent clipping)
        if (!g_hfxEQPreamp) {
            g_hfxEQPreamp = BASS_ChannelSetFX(g_fxStream, BASS_FX_BFX_VOLUME, 0);
            if (g_hfxEQPreamp) {
                BASS_BFX_VOLUME vol = {0};
                vol.lChannel = BASS_BFX_CHANALL;
                // Convert dB to linear: linear = 10^(dB/20)
                vol.fVolume = powf(10.0f, g_paramValues[(int)ParamId::EQPreamp] / 20.0f);
                BASS_FXSetParameters(g_hfxEQPreamp, &vol);
            }
        }
        // Bass (60 Hz)
        if (!g_hfxEQBass) {
            g_hfxEQBass = BASS_ChannelSetFX(g_fxStream, BASS_FX_BFX_PEAKEQ, 0);
            if (g_hfxEQBass) {
                BASS_BFX_PEAKEQ eq = {0};
                eq.lBand = 0;
                eq.fBandwidth = 2.5f;  // Octaves
                eq.fQ = 0.0f;
                eq.fCenter = g_eqBassFreq;  // Bass center frequency
                eq.fGain = g_paramValues[(int)ParamId::EQBass];
                eq.lChannel = BASS_BFX_CHANALL;
                BASS_FXSetParameters(g_hfxEQBass, &eq);
            }
        }
        // Mid (1000 Hz)
        if (!g_hfxEQMid) {
            g_hfxEQMid = BASS_ChannelSetFX(g_fxStream, BASS_FX_BFX_PEAKEQ, 0);
            if (g_hfxEQMid) {
                BASS_BFX_PEAKEQ eq = {0};
                eq.lBand = 0;
                eq.fBandwidth = 2.5f;  // Octaves
                eq.fQ = 0.0f;
                eq.fCenter = g_eqMidFreq;   // Mid center frequency
                eq.fGain = g_paramValues[(int)ParamId::EQMid];
                eq.lChannel = BASS_BFX_CHANALL;
                BASS_FXSetParameters(g_hfxEQMid, &eq);
            }
        }
        // Treble (8000 Hz)
        if (!g_hfxEQTreble) {
            g_hfxEQTreble = BASS_ChannelSetFX(g_fxStream, BASS_FX_BFX_PEAKEQ, 0);
            if (g_hfxEQTreble) {
                BASS_BFX_PEAKEQ eq = {0};
                eq.lBand = 0;
                eq.fBandwidth = 2.5f;  // Octaves
                eq.fQ = 0.0f;
                eq.fCenter = g_eqTrebleFreq; // Treble center frequency
                eq.fGain = g_paramValues[(int)ParamId::EQTreble];
                eq.lChannel = BASS_BFX_CHANALL;
                BASS_FXSetParameters(g_hfxEQTreble, &eq);
            }
        }
    }

    // Compressor
    if (g_dspEnabled[(int)DSPEffectType::Compressor] && !g_hfxCompressor) {
        g_hfxCompressor = BASS_ChannelSetFX(g_fxStream, BASS_FX_BFX_COMPRESSOR2, 0);
        if (g_hfxCompressor) {
            BASS_BFX_COMPRESSOR2 comp = {0};
            comp.fGain = g_paramValues[(int)ParamId::CompGain];
            comp.fThreshold = g_paramValues[(int)ParamId::CompThreshold];
            comp.fRatio = g_paramValues[(int)ParamId::CompRatio];
            comp.fAttack = g_paramValues[(int)ParamId::CompAttack];
            comp.fRelease = g_paramValues[(int)ParamId::CompRelease];
            comp.lChannel = BASS_BFX_CHANALL;
            BASS_FXSetParameters(g_hfxCompressor, &comp);
        }
    }

    // Stereo Width (custom DSP)
    if (g_dspEnabled[(int)DSPEffectType::StereoWidth] && !g_hdspStereoWidth) {
        g_hdspStereoWidth = BASS_ChannelSetDSP(g_fxStream, StereoWidthDSPProc, nullptr, 0);
    }

    // Center Cancel/Extract (custom DSP)
    if (g_dspEnabled[(int)DSPEffectType::CenterCancel] && !g_hdspCenterCancel) {
        g_hdspCenterCancel = BASS_ChannelSetDSP(g_fxStream, CenterCancelDSPProc, nullptr, 0);
    }

    // Convolution Reverb (custom DSP)
    if (g_dspEnabled[(int)DSPEffectType::Convolution] && !g_hdspConvolution) {
        ConvolutionReverb* conv = GetConvolutionReverb();
        if (conv && conv->IsLoaded()) {
            BASS_CHANNELINFO info;
            if (BASS_ChannelGetInfo(g_fxStream, &info)) {
                conv->Init((int)info.freq);
            }
        }
        g_hdspConvolution = BASS_ChannelSetDSP(g_fxStream, ConvolutionDSPProc, nullptr, 0);
    }

    // 3D Audio (Steam Audio HRTF)
#ifdef USE_STEAM_AUDIO
    if (g_dspEnabled[(int)DSPEffectType::SpatialAudio] && !g_hdspSpatialAudio) {
        bool initOk = false;
        SpatialAudio* spatial = GetSpatialAudio();
        if (spatial) {
            BASS_CHANNELINFO info;
            if (BASS_ChannelGetInfo(g_fxStream, &info)) {
                initOk = spatial->Initialize((int)info.freq);
                if (!initOk) {
                    const wchar_t* err = spatial->GetLastError();
                    if (err && err[0]) {
                        ShowMessage(err, L"3D Audio Error", MessageIcon::Error);
                    }
                    g_dspEnabled[(int)DSPEffectType::SpatialAudio] = false;
                }
            }
        }
        if (initOk) {
            g_hdspSpatialAudio = BASS_ChannelSetDSP(g_fxStream, SpatialAudioDSPProc, nullptr, 0);
        }
    }
#endif

    // Legacy volume mode - apply volume directly to stream attribute
    // This must be done every time a new stream is created
    if (g_legacyVolume) {
        float curvedVolume = (g_muted ? 0.0f : (g_volume * g_volume)) * g_replayGainScale;
        BASS_ChannelSetAttribute(g_fxStream, BASS_ATTRIB_VOL, curvedVolume);
    }

    // Volume DSP - added with very low priority so it runs LAST (after encoder)
    // This ensures encoders capture full-volume audio, while playback is adjusted
    // Only used when legacy volume mode is disabled
    // Priority -2000000000 ensures it runs after encoder (priority 0)
    if (!g_legacyVolume && !g_hdspVolume) {
        g_hdspVolume = BASS_ChannelSetDSP(g_fxStream, VolumeDSPProc, nullptr, -2000000000);
    }
}

// Remove all DSP effects from stream
void RemoveDSPEffects() {
    if (g_hdspReverb) { if (g_fxStream) BASS_ChannelRemoveDSP(g_fxStream, g_hdspReverb); g_hdspReverb = 0; }
    if (g_hfxEcho) { if (g_fxStream) BASS_ChannelRemoveFX(g_fxStream, g_hfxEcho); g_hfxEcho = 0; }
    if (g_hfxEQPreamp) { if (g_fxStream) BASS_ChannelRemoveFX(g_fxStream, g_hfxEQPreamp); g_hfxEQPreamp = 0; }
    if (g_hfxEQBass) { if (g_fxStream) BASS_ChannelRemoveFX(g_fxStream, g_hfxEQBass); g_hfxEQBass = 0; }
    if (g_hfxEQMid) { if (g_fxStream) BASS_ChannelRemoveFX(g_fxStream, g_hfxEQMid); g_hfxEQMid = 0; }
    if (g_hfxEQTreble) { if (g_fxStream) BASS_ChannelRemoveFX(g_fxStream, g_hfxEQTreble); g_hfxEQTreble = 0; }
    if (g_hfxCompressor) { if (g_fxStream) BASS_ChannelRemoveFX(g_fxStream, g_hfxCompressor); g_hfxCompressor = 0; }
    if (g_hdspStereoWidth) { if (g_fxStream) BASS_ChannelRemoveDSP(g_fxStream, g_hdspStereoWidth); g_hdspStereoWidth = 0; }
    if (g_hdspCenterCancel) { if (g_fxStream) BASS_ChannelRemoveDSP(g_fxStream, g_hdspCenterCancel); g_hdspCenterCancel = 0; }
    if (g_hdspConvolution) { if (g_fxStream) BASS_ChannelRemoveDSP(g_fxStream, g_hdspConvolution); g_hdspConvolution = 0; }
    if (g_hdspSpatialAudio) { if (g_fxStream) BASS_ChannelRemoveDSP(g_fxStream, g_hdspSpatialAudio); g_hdspSpatialAudio = 0; }
    if (g_hdspVolume) { if (g_fxStream) BASS_ChannelRemoveDSP(g_fxStream, g_hdspVolume); g_hdspVolume = 0; }
}

// Get parameter definition
const ParamDef* GetParamDef(ParamId id) {
    for (int i = 0; i < g_paramDefCount; i++) {
        if (g_paramDefs[i].id == id) return &g_paramDefs[i];
    }
    return nullptr;
}

float GetParamValue(ParamId id) {
    // For stream effects, return the actual global values
    switch (id) {
        case ParamId::Volume: return g_volume;
        case ParamId::Pitch: return g_pitch;
        case ParamId::Tempo: return g_tempo;
        case ParamId::Rate: return g_rate;
        default: break;
    }
    // For DSP effect parameters, use the stored values
    if ((int)id < 0 || (int)id >= (int)ParamId::COUNT) return 0.0f;
    return g_paramValues[(int)id];
}

const char* GetParamName(ParamId id) {
    const ParamDef* def = GetParamDef(id);
    return def ? def->name : "Unknown";
}

const char* GetParamUnit(ParamId id) {
    const ParamDef* def = GetParamDef(id);
    return def ? def->unit : "";
}

void SetParamValue(ParamId id, float value) {
    const ParamDef* def = GetParamDef(id);
    if (!def) return;

    // For Volume, respect the allow amplify setting
    float maxVal = def->maxValue;
    if (id == ParamId::Volume) {
        maxVal = g_allowAmplify ? MAX_VOLUME_AMPLIFY : MAX_VOLUME_NORMAL;
    }

    value = clamp_val(value, def->minValue, maxVal);
    g_paramValues[(int)id] = value;

    // Apply the change
    switch (id) {
        case ParamId::Volume:
            g_volume = value;
            // In legacy mode, apply via BASS_ATTRIB_VOL
            // In normal mode, volume DSP automatically uses updated g_volume
            if (g_legacyVolume && g_fxStream) {
                float curvedVolume = (g_muted ? 0.0f : (g_volume * g_volume)) * g_replayGainScale;
                BASS_ChannelSetAttribute(g_fxStream, BASS_ATTRIB_VOL, curvedVolume);
            }
            break;
        case ParamId::Pitch:
            g_pitch = value;
            if (g_fxStream) {
                TempoProcessor* processor = GetTempoProcessor();
                if (processor && processor->IsActive()) {
                    processor->SetPitch(g_pitch);
                }
            }
            break;
        case ParamId::Tempo:
            g_tempo = value;
            // Skip applying tempo to live streams (not supported)
            if (g_fxStream && !g_isLiveStream) {
                TempoProcessor* processor = GetTempoProcessor();
                if (processor && processor->IsActive()) {
                    processor->SetTempo(g_tempo);
                }
            }
            break;
        case ParamId::Rate:
            g_rate = value;
            // Skip applying rate to live streams (not supported)
            if (g_fxStream && !g_isLiveStream) {
                // Use native BASS frequency attribute (changes speed and pitch together)
                BASS_ChannelSetAttribute(g_fxStream, BASS_ATTRIB_FREQ, g_originalFreq * g_rate);
            }
            break;
        // Reverb parameters: a preset sets the others, then both reverbs are updated
        case ParamId::ReverbPreset:
            ApplySimpleReverbPreset(ReverbPresetIndex(id, g_simpleReverbPresetCount));
            UpdateReverbParams();
            break;
        case ParamId::AdvReverbPreset:
            ApplyAdvancedReverbPreset(ReverbPresetIndex(id, g_advancedReverbPresetCount));
            UpdateReverbParams();
            break;
        case ParamId::ReverbMix:
        case ParamId::ReverbRoom:
        case ParamId::ReverbDamp:
        case ParamId::ReverbWidth:
        case ParamId::ReverbPreDelay:
        case ParamId::ReverbLowCut:
        case ParamId::ReverbHighCut:
        case ParamId::AdvReverbMix:
        case ParamId::AdvReverbDecay:
        case ParamId::AdvReverbHFRatio:
        case ParamId::AdvReverbDensity:
        case ParamId::AdvReverbDiffusion:
        case ParamId::AdvReverbReflections:
        case ParamId::AdvReverbLate:
        case ParamId::AdvReverbReflDelay:
        case ParamId::AdvReverbLateDelay:
            UpdateReverbParams();
            break;
        case ParamId::EchoDelay:
        case ParamId::EchoFeedback:
        case ParamId::EchoMix:
            if (g_hfxEcho) {
                BASS_BFX_ECHO4 echo;
                BASS_FXGetParameters(g_hfxEcho, &echo);
                echo.fDryMix = 1.0f - (g_paramValues[(int)ParamId::EchoMix] / 100.0f);
                echo.fWetMix = g_paramValues[(int)ParamId::EchoMix] / 100.0f;
                echo.fFeedback = g_paramValues[(int)ParamId::EchoFeedback] / 100.0f;
                echo.fDelay = g_paramValues[(int)ParamId::EchoDelay] / 1000.0f;
                BASS_FXSetParameters(g_hfxEcho, &echo);
            }
            break;
        case ParamId::EQPreamp:
            if (g_hfxEQPreamp) {
                BASS_BFX_VOLUME vol = {0};
                vol.lChannel = BASS_BFX_CHANALL;
                vol.fVolume = powf(10.0f, value / 20.0f);
                BASS_FXSetParameters(g_hfxEQPreamp, &vol);
            }
            break;
        case ParamId::EQBass:
            if (g_hfxEQBass) {
                BASS_BFX_PEAKEQ eq = {0};
                eq.lBand = 0;
                BASS_FXGetParameters(g_hfxEQBass, &eq);
                eq.fGain = value;
                BASS_FXSetParameters(g_hfxEQBass, &eq);
            }
            break;
        case ParamId::EQMid:
            if (g_hfxEQMid) {
                BASS_BFX_PEAKEQ eq = {0};
                eq.lBand = 0;
                BASS_FXGetParameters(g_hfxEQMid, &eq);
                eq.fGain = value;
                BASS_FXSetParameters(g_hfxEQMid, &eq);
            }
            break;
        case ParamId::EQTreble:
            if (g_hfxEQTreble) {
                BASS_BFX_PEAKEQ eq = {0};
                eq.lBand = 0;
                BASS_FXGetParameters(g_hfxEQTreble, &eq);
                eq.fGain = value;
                BASS_FXSetParameters(g_hfxEQTreble, &eq);
            }
            break;
        case ParamId::CompThreshold:
        case ParamId::CompRatio:
        case ParamId::CompAttack:
        case ParamId::CompRelease:
        case ParamId::CompGain:
            if (g_hfxCompressor) {
                BASS_BFX_COMPRESSOR2 comp = {0};
                BASS_FXGetParameters(g_hfxCompressor, &comp);
                comp.fThreshold = g_paramValues[(int)ParamId::CompThreshold];
                comp.fRatio = g_paramValues[(int)ParamId::CompRatio];
                comp.fAttack = g_paramValues[(int)ParamId::CompAttack];
                comp.fRelease = g_paramValues[(int)ParamId::CompRelease];
                comp.fGain = g_paramValues[(int)ParamId::CompGain];
                BASS_FXSetParameters(g_hfxCompressor, &comp);
            }
            break;
    #ifdef USE_STEAM_AUDIO
        case ParamId::SpatialMode: {
            SpatialAudio* spatial = GetSpatialAudio();
            if (spatial) spatial->SetMode(value >= 0.5f ? SpatialMode::Surround51 : SpatialMode::Binaural);
            break;
        }
        case ParamId::SpatialRearCenter: {
            SpatialAudio* spatial = GetSpatialAudio();
            if (spatial) spatial->SetRearCenter(value >= 0.5f);
            break;
        }
    #endif
        default:
            break;
    }
}

void CycleParam(int direction) {
    std::vector<ParamId> params = GetAvailableParams();

    if (params.empty()) {
        Speak("No parameters available");
        return;
    }

    // Find current param in available list
    int currentIdx = -1;
    for (int i = 0; i < (int)params.size(); i++) {
        if ((int)params[i] == g_currentParamIndex) {
            currentIdx = i;
            break;
        }
    }

    // If current not found, start at beginning
    if (currentIdx < 0) {
        currentIdx = 0;
    } else {
        currentIdx += direction;
        // Clamp to bounds (no wrapping)
        if (currentIdx < 0) currentIdx = 0;
        if (currentIdx >= (int)params.size()) currentIdx = (int)params.size() - 1;
    }

    g_currentParamIndex = (int)params[currentIdx];
    AnnounceCurrentParam();
}

void AdjustCurrentParam(int direction) {
    std::vector<ParamId> params = GetAvailableParams();
    if (params.empty()) return;

    // Check if current param is available
    bool found = false;
    for (const auto& p : params) {
        if ((int)p == g_currentParamIndex) {
            found = true;
            break;
        }
    }

    if (!found) {
        g_currentParamIndex = (int)params[0];
    }

    ParamId id = (ParamId)g_currentParamIndex;
    const ParamDef* def = GetParamDef(id);
    if (!def) return;

    // Block tempo and rate adjustments for live streams
    if (g_isLiveStream && (id == ParamId::Tempo || id == ParamId::Rate)) {
        Speak("Not available for live streams");
        return;
    }

    // For Volume, respect the allow amplify setting and use g_volumeStep
    float maxVal = def->maxValue;
    float step = def->step;
    if (id == ParamId::Volume) {
        maxVal = g_allowAmplify ? MAX_VOLUME_AMPLIFY : MAX_VOLUME_NORMAL;
        step = g_volumeStep;  // Use configurable volume step
    }

    float currentVal = GetParamValue(id);
    float newVal;

    // Handle Rate with semitone stepping if enabled
    if (id == ParamId::Rate && g_rateStepMode == 1) {
        // Semitone ratio = 2^(1/12) ≈ 1.0594630943592953
        const float semitoneRatio = 1.0594630943592953f;
        if (direction > 0) {
            newVal = currentVal * semitoneRatio;
        } else {
            newVal = currentVal / semitoneRatio;
        }
    } else {
        newVal = currentVal + (direction * step);
    }

    // 3D Rotation, Mode, and Rear Speaker wrap around instead of clamping
    // so the user can cycle through modes or rotate continuously.
    if (id == ParamId::SpatialRotation) {
        // Angular: ±180° meet, so full range = max - min
        float range = def->maxValue - def->minValue;
        while (newVal > def->maxValue) newVal -= range;
        while (newVal < def->minValue) newVal += range;
    } else if (id == ParamId::SpatialMode || id == ParamId::SpatialRearCenter ||
               id == ParamId::ReverbPreset || id == ParamId::AdvReverbPreset) {
        // Discrete choice: add step so past-max wraps to min
        float range = def->maxValue - def->minValue + def->step;
        while (newVal > def->maxValue) newVal -= range;
        while (newVal < def->minValue) newVal += range;
    } else {
        newVal = clamp_val(newVal, def->minValue, maxVal);
    }

    SetParamValue(id, newVal);
    AnnounceCurrentParam();
}

void ResetCurrentParam() {
    std::vector<ParamId> params = GetAvailableParams();
    if (params.empty()) return;

    // Check if current param is available
    bool found = false;
    for (const auto& p : params) {
        if ((int)p == g_currentParamIndex) {
            found = true;
            break;
        }
    }

    if (!found) {
        g_currentParamIndex = (int)params[0];
    }

    ParamId id = (ParamId)g_currentParamIndex;
    const ParamDef* def = GetParamDef(id);
    if (!def) return;

    // Block tempo and rate reset for live streams
    if (g_isLiveStream && (id == ParamId::Tempo || id == ParamId::Rate)) {
        Speak("Not available for live streams");
        return;
    }

    SetParamValue(id, def->defaultValue);
    AnnounceCurrentParam();
}

void SetCurrentParamToMin() {
    std::vector<ParamId> params = GetAvailableParams();
    if (params.empty()) return;

    // Check if current param is available
    bool found = false;
    for (const auto& p : params) {
        if ((int)p == g_currentParamIndex) {
            found = true;
            break;
        }
    }

    if (!found) {
        g_currentParamIndex = (int)params[0];
    }

    ParamId id = (ParamId)g_currentParamIndex;
    const ParamDef* def = GetParamDef(id);
    if (!def) return;

    // Block tempo and rate min for live streams
    if (g_isLiveStream && (id == ParamId::Tempo || id == ParamId::Rate)) {
        Speak("Not available for live streams");
        return;
    }

    SetParamValue(id, def->minValue);
    AnnounceCurrentParam();
}

void SetCurrentParamToMax() {
    std::vector<ParamId> params = GetAvailableParams();
    if (params.empty()) return;

    // Check if current param is available
    bool found = false;
    for (const auto& p : params) {
        if ((int)p == g_currentParamIndex) {
            found = true;
            break;
        }
    }

    if (!found) {
        g_currentParamIndex = (int)params[0];
    }

    ParamId id = (ParamId)g_currentParamIndex;
    const ParamDef* def = GetParamDef(id);
    if (!def) return;

    // Block tempo and rate max for live streams
    if (g_isLiveStream && (id == ParamId::Tempo || id == ParamId::Rate)) {
        Speak("Not available for live streams");
        return;
    }

    // Use maxValue, but respect g_allowAmplify for volume
    float maxVal = def->maxValue;
    if (id == ParamId::Volume && !g_allowAmplify && maxVal > 1.0f) {
        maxVal = 1.0f;
    }

    SetParamValue(id, maxVal);
    AnnounceCurrentParam();
}

void AnnounceCurrentParam() {
    if (!g_speechEffect) return;

    ParamId id = (ParamId)g_currentParamIndex;
    const ParamDef* def = GetParamDef(id);
    if (!def) return;

    float val = GetParamValue(id);
    char buf[64];

    // Format based on parameter type
    if (id == ParamId::SpatialMode) {
        snprintf(buf, sizeof(buf), "3D Mode: %s", val >= 0.5f ? "5.1 Surround" : "Binaural");
    } else if (id == ParamId::SpatialRearCenter) {
        snprintf(buf, sizeof(buf), "3D Rear Speaker: %s", val >= 0.5f ? "On" : "Off");
    } else if (id == ParamId::ReverbPreset) {
        snprintf(buf, sizeof(buf), "%s: %s", def->name,
                 g_simpleReverbPresets[ReverbPresetIndex(id, g_simpleReverbPresetCount)].name);
    } else if (id == ParamId::AdvReverbPreset) {
        snprintf(buf, sizeof(buf), "%s: %s", def->name,
                 g_advancedReverbPresets[ReverbPresetIndex(id, g_advancedReverbPresetCount)].name);
    } else if ((id == ParamId::ReverbLowCut && val <= 0.0f) ||
               (id == ParamId::ReverbHighCut && val >= kReverbHighCutOff)) {
        snprintf(buf, sizeof(buf), "%s Off", def->name);
    } else if (id == ParamId::AdvReverbDecay) {
        snprintf(buf, sizeof(buf), "%s %.1f%s", def->name, val, def->unit);
    } else if (id == ParamId::AdvReverbHFRatio) {
        snprintf(buf, sizeof(buf), "%s %.2f%s", def->name, val, def->unit);
    } else if (id == ParamId::AdvReverbReflections || id == ParamId::AdvReverbLate) {
        snprintf(buf, sizeof(buf), "%s %+.0f%s", def->name, val, def->unit);
    } else if (id == ParamId::Volume) {
        snprintf(buf, sizeof(buf), "%s %d%s", def->name, (int)(val * 100 + 0.5f), def->unit);
    } else if (id == ParamId::Rate) {
        snprintf(buf, sizeof(buf), "%s %.2f%s", def->name, val, def->unit);
    } else if (id == ParamId::Pitch || id == ParamId::EQBass || id == ParamId::EQMid || id == ParamId::EQTreble) {
        snprintf(buf, sizeof(buf), "%s %+.0f%s", def->name, val, def->unit);
    } else if (id == ParamId::EchoDelay) {
        snprintf(buf, sizeof(buf), "%s %.0f%s", def->name, val, def->unit);
    } else {
        snprintf(buf, sizeof(buf), "%s %.0f%s", def->name, val, def->unit);
    }

    Speak(buf);
}

void ResetEffects() {
    for (int i = 0; i < g_paramDefCount; i++) {
        SetParamValue(g_paramDefs[i].id, g_paramDefs[i].defaultValue);
    }
}

// Legacy compatibility functions
float GetEffectValue(EffectType type) {
    switch (type) {
        case EffectType::Volume: return g_volume;
        case EffectType::Pitch: return g_pitch;
        case EffectType::Tempo: return g_tempo;
        case EffectType::Rate: return g_rate;
        default: return 0.0f;
    }
}

const char* GetEffectName(EffectType type) {
    switch (type) {
        case EffectType::Volume: return "Volume";
        case EffectType::Pitch: return "Pitch";
        case EffectType::Tempo: return "Tempo";
        case EffectType::Rate: return "Rate";
        default: return "Unknown";
    }
}

const char* GetEffectUnit(EffectType type) {
    switch (type) {
        case EffectType::Volume: return "%";
        case EffectType::Pitch: return " semitones";
        case EffectType::Tempo: return "%";
        case EffectType::Rate: return "x";
        default: return "";
    }
}

void SetEffectValue(EffectType type, float value) {
    switch (type) {
        case EffectType::Volume: SetParamValue(ParamId::Volume, value); break;
        case EffectType::Pitch: SetParamValue(ParamId::Pitch, value); break;
        case EffectType::Tempo: SetParamValue(ParamId::Tempo, value); break;
        case EffectType::Rate: SetParamValue(ParamId::Rate, value); break;
        default: break;
    }
}

void CycleEffect(int direction) {
    CycleParam(direction);
}

void AdjustCurrentEffect(int direction) {
    AdjustCurrentParam(direction);
}

// ---------------------------------------------------------------------------
// Effect presets: save/load/delete named sets of all enabled effects + params.
// Stored in FastPlay.ini under [Presets] (index) and [Preset_<name>] (values).
// ---------------------------------------------------------------------------

static std::wstring PresetSectionName(const std::wstring& name) {
    return L"Preset_" + name;
}

static float GetPrivateProfileFloatW_Preset(const wchar_t* section, const wchar_t* key, float defaultVal) {
    wchar_t buf[64] = {0};
    GetPrivateProfileStringW(section, key, L"", buf, 64, g_configPath.c_str());
    if (buf[0] == L'\0') return defaultVal;
    return (float)_wtof(buf);
}

std::vector<std::wstring> GetEffectPresetNames() {
    std::vector<std::wstring> names;
    int count = GetPrivateProfileIntW(L"Presets", L"Count", 0, g_configPath.c_str());
    for (int i = 0; i < count; i++) {
        wchar_t key[32];
        swprintf(key, 32, L"Name%d", i);
        wchar_t buf[128] = {0};
        GetPrivateProfileStringW(L"Presets", key, L"", buf, 128, g_configPath.c_str());
        if (buf[0] != L'\0') names.push_back(buf);
    }
    return names;
}

static void WritePresetNameList(const std::vector<std::wstring>& names) {
    // Clear old name entries first
    int oldCount = GetPrivateProfileIntW(L"Presets", L"Count", 0, g_configPath.c_str());
    for (int i = 0; i < oldCount; i++) {
        wchar_t key[32];
        swprintf(key, 32, L"Name%d", i);
        WritePrivateProfileStringW(L"Presets", key, nullptr, g_configPath.c_str());
    }
    wchar_t buf[32];
    swprintf(buf, 32, L"%d", (int)names.size());
    WritePrivateProfileStringW(L"Presets", L"Count", buf, g_configPath.c_str());
    for (size_t i = 0; i < names.size(); i++) {
        wchar_t key[32];
        swprintf(key, 32, L"Name%zu", i);
        WritePrivateProfileStringW(L"Presets", key, names[i].c_str(), g_configPath.c_str());
    }
}

bool SaveEffectPreset(const std::wstring& name) {
    if (name.empty()) return false;

    std::wstring section = PresetSectionName(name);
    wchar_t buf[64];

    // Stream effect enabled flags
    for (int i = 0; i < 4; i++) {
        wchar_t key[32];
        swprintf(key, 32, L"StreamEnabled%d", i);
        WritePrivateProfileStringW(section.c_str(), key, g_effectEnabled[i] ? L"1" : L"0", g_configPath.c_str());
    }

    // Stream effect values (pitch, tempo, rate — volume intentionally excluded)
    swprintf(buf, 64, L"%.4f", g_pitch);
    WritePrivateProfileStringW(section.c_str(), L"Pitch", buf, g_configPath.c_str());
    swprintf(buf, 64, L"%.4f", g_tempo);
    WritePrivateProfileStringW(section.c_str(), L"Tempo", buf, g_configPath.c_str());
    swprintf(buf, 64, L"%.4f", g_rate);
    WritePrivateProfileStringW(section.c_str(), L"Rate", buf, g_configPath.c_str());

    // Reverb algorithm
    swprintf(buf, 64, L"%d", g_reverbAlgorithm);
    WritePrivateProfileStringW(section.c_str(), L"ReverbAlgorithm", buf, g_configPath.c_str());

    // DSP effect enabled flags
    for (int i = 0; i < (int)DSPEffectType::COUNT; i++) {
        wchar_t key[32];
        swprintf(key, 32, L"DSPEnabled%d", i);
        WritePrivateProfileStringW(section.c_str(), key,
            g_dspEnabled[i] ? L"1" : L"0", g_configPath.c_str());
    }

    // All param values (DSP params plus stream effect params)
    for (int i = 0; i < g_paramDefCount; i++) {
        const ParamDef& def = g_paramDefs[i];
        // Skip volume - presets shouldn't hijack playback volume
        if (def.id == ParamId::Volume) continue;
        wchar_t key[64];
        swprintf(key, 64, L"Param%d", (int)def.id);
        swprintf(buf, 64, L"%.6f", g_paramValues[(int)def.id]);
        WritePrivateProfileStringW(section.c_str(), key, buf, g_configPath.c_str());
    }

    // Add to name list if not already present
    auto names = GetEffectPresetNames();
    bool found = false;
    for (auto& n : names) { if (n == name) { found = true; break; } }
    if (!found) {
        names.push_back(name);
        WritePresetNameList(names);
    }
    return true;
}

bool LoadEffectPreset(const std::wstring& name) {
    if (name.empty()) return false;
    std::wstring section = PresetSectionName(name);

    // Quick existence check
    wchar_t test[8] = {0};
    GetPrivateProfileStringW(section.c_str(), L"Pitch", L"__MISSING__", test, 8, g_configPath.c_str());
    if (wcscmp(test, L"__MISSING__") == 0) return false;

    // Stream effect values
    wchar_t buf[64] = {0};
    GetPrivateProfileStringW(section.c_str(), L"Pitch", L"0", buf, 64, g_configPath.c_str());
    g_pitch = (float)_wtof(buf);
    GetPrivateProfileStringW(section.c_str(), L"Tempo", L"0", buf, 64, g_configPath.c_str());
    g_tempo = (float)_wtof(buf);
    GetPrivateProfileStringW(section.c_str(), L"Rate", L"1", buf, 64, g_configPath.c_str());
    g_rate = (float)_wtof(buf);

    // Stream effect enabled flags
    for (int i = 0; i < 4; i++) {
        wchar_t key[32];
        swprintf(key, 32, L"StreamEnabled%d", i);
        g_effectEnabled[i] = GetPrivateProfileIntW(section.c_str(), key,
            g_effectEnabled[i] ? 1 : 0, g_configPath.c_str()) != 0;
    }

    // Reverb algorithm
    int ra = GetPrivateProfileIntW(section.c_str(), L"ReverbAlgorithm", g_reverbAlgorithm, g_configPath.c_str());
    if (ra < 0) ra = 0;
    if (ra >= (int)ReverbAlgorithm::COUNT) ra = (int)ReverbAlgorithm::Advanced;  // old DX8 / I3DL2
    SetReverbAlgorithm(ra);

    // DSP effect enabled flags (apply via EnableDSPEffect so handlers hook up properly)
    for (int i = 0; i < (int)DSPEffectType::COUNT; i++) {
        if ((DSPEffectType)i == DSPEffectType::Reverb) continue;  // controlled by algorithm
        wchar_t key[32];
        swprintf(key, 32, L"DSPEnabled%d", i);
        bool en = GetPrivateProfileIntW(section.c_str(), key,
            g_dspEnabled[i] ? 1 : 0, g_configPath.c_str()) != 0;
        EnableDSPEffect((DSPEffectType)i, en);
    }

    // All param values (via SetParamValue so effects update live)
    for (int i = 0; i < g_paramDefCount; i++) {
        const ParamDef& def = g_paramDefs[i];
        if (def.id == ParamId::Volume) continue;
        wchar_t key[64];
        swprintf(key, 64, L"Param%d", (int)def.id);
        float val = GetPrivateProfileFloatW_Preset(section.c_str(), key, g_paramValues[(int)def.id]);
        SetParamValue(def.id, val);
    }

    return true;
}

bool DeleteEffectPreset(const std::wstring& name) {
    if (name.empty()) return false;
    std::wstring section = PresetSectionName(name);
    // Wipe the preset's entire section
    WritePrivateProfileStringW(section.c_str(), nullptr, nullptr, g_configPath.c_str());

    auto names = GetEffectPresetNames();
    bool found = false;
    for (auto it = names.begin(); it != names.end(); ) {
        if (*it == name) { it = names.erase(it); found = true; } else { ++it; }
    }
    if (found) WritePresetNameList(names);
    return found;
}
