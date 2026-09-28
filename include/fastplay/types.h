#pragma once
#ifndef FASTPLAY_TYPES_H
#define FASTPLAY_TYPES_H

#include <functional>
#include <string>

// File association info
struct FileAssoc {
    const wchar_t* ext;
    const wchar_t* desc;
};

// Seek amount definition
struct SeekAmount {
    double value;       // seconds or track count
    const char* label;
    bool isTrack;       // true if track-based navigation
};

// Global hotkey action
struct HotkeyAction {
    int commandId;
    const wchar_t* name;
};

// Global hotkey storage
struct GlobalHotkey {
    int id;         // Unique ID for RegisterHotKey
    unsigned modifiers; // MOD_ALT, MOD_CONTROL, MOD_SHIFT, MOD_WIN
    unsigned vk;        // Virtual key code
    int actionIdx;  // Index into g_hotkeyActions
};

// Hotkey dialog data
struct HotkeyDlgData {
    unsigned modifiers;
    unsigned vk;
    int actionIdx;
    bool isEdit;
};

// Stream effect types (tempo stream attributes)
enum class StreamEffect {
    Volume,
    Pitch,
    Tempo,
    Rate,
    COUNT
};

// Spatial audio mode
enum class SpatialMode {
    Binaural,     // Stereo HRTF for headphones (2 virtual speakers)
    Surround51,   // 5.1 virtual surround (5 virtual speakers rendered binaurally)
    Speakers,     // a room of simulated speakers (src/speakers/presets.h), heard from a seat in it
    COUNT
};

// DSP effect types (BASS_FX DSP effects + custom)
enum class DSPEffectType {
    Reverb,
    Echo,
    EQ,
    Compressor,
    StereoWidth,
    CenterCancel,  // Center channel canceler/extractor (vocal removal/isolation)
    Convolution,   // Convolution reverb using impulse response
    SpatialAudio,  // 3D audio via HRTF/binaural rendering
    COUNT
};

// Reverb algorithm types (see src/reverb)
enum class ReverbAlgorithm {
    Off,
    Simple,     // 16 line FDN reverb: room size, damping, width
    Advanced,   // EFX model reverb: the OpenAL EFX / EAX parameter set and environments
    COUNT
};

// All adjustable parameters (stream + DSP)
// The numeric values are stored in effect presets (Param<n>), so existing ones must not change:
// retired parameters leave gaps and new ones go at the end.
enum class ParamId {
    // Stream effects
    Volume,
    Pitch,
    Tempo,
    Rate,
    // Simple reverb parameters (the rest are after SpatialZ)
    ReverbMix,
    ReverbRoom,
    ReverbDamp,
    // 7 .. 13 were the DX8 and I3DL2 reverb parameters
    // Echo parameters
    EchoDelay = 14,
    EchoFeedback,
    EchoMix,
    // EQ parameters
    EQPreamp,
    EQBass,
    EQMid,
    EQTreble,
    // Compressor parameters
    CompThreshold,
    CompRatio,
    CompAttack,
    CompRelease,
    CompGain,
    // Stereo width parameter
    StereoWidth,
    // Center cancel parameter (-100% extract to +100% cancel)
    CenterCancel,
    // Convolution reverb parameters
    ConvolutionMix,
    ConvolutionGain,
    // 3D audio parameters
    SpatialBlend,
    SpatialWidth,
    SpatialRotation,
    SpatialMode,        // 0=Binaural, 1=5.1 Surround, 2 and up = the room presets
    SpatialRearCenter,  // 0=Off, 1=On (5.1 only)
    SpatialX,           // Listener X position
    SpatialY,           // Listener Y position
    SpatialZ,           // Listener Z position
    // Simple reverb parameters (continued)
    ReverbPreset,       // index into the simple reverb's rooms
    ReverbWidth,
    ReverbPreDelay,
    ReverbLowCut,       // 0 = off
    ReverbHighCut,      // maximum = off
    // Advanced reverb parameters
    AdvReverbPreset,    // index into the EFX environments
    AdvReverbMix,
    AdvReverbDecay,
    AdvReverbHFRatio,
    AdvReverbDensity,
    AdvReverbDiffusion,
    AdvReverbReflections,   // dB
    AdvReverbLate,          // dB
    AdvReverbReflDelay,     // ms
    AdvReverbLateDelay,     // ms
    // 3D audio room presets (listed with the other 3D parameters)
    SpatialSub,         // 0=Off, 1=On (presets with subwoofers)
    SpatialSubLevel,    // dB
    SpatialCrossover,   // Hz
    SpatialBassFeel,    // %
    COUNT
};

// Parameter definition
struct ParamDef {
    ParamId id;
    const char* name;
    const char* unit;
    float minValue;
    float maxValue;
    float step;
    float defaultValue;
    DSPEffectType dspEffect;  // Which DSP effect this belongs to (or -1 for stream effects)
};

// Legacy EffectType for backwards compatibility
enum class EffectType {
    Volume,
    Pitch,
    Tempo,
    Rate
};

// Legacy EffectParam for backwards compatibility
struct EffectParam {
    EffectType type;
    float minValue;
    float maxValue;
    float step;
    float defaultValue;
};

#endif // FASTPLAY_TYPES_H
