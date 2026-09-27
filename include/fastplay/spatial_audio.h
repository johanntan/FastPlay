#pragma once
#ifndef FASTPLAY_SPATIAL_AUDIO_H
#define FASTPLAY_SPATIAL_AUDIO_H

#include "types.h"
#include "spatial/hrtf.h"

#include <mutex>
#include <string>
#include <vector>

// 3D Audio: the stereo signal played through virtual speakers around the listener
// and rendered binaurally with an HRTF, for headphones. Binaural mode uses two
// speakers; 5.1 mode upmixes to five (or six, with the rear center speaker).
class SpatialAudio {
public:
    static constexpr int FRAME_SIZE = fastplay::audio::kHrtfBlock;
    static constexpr int MAX_QUEUE = 16384;  // Must handle largest BASS callback (~500ms @ 48kHz = ~24000 frames)
    static constexpr int MAX_SPEAKERS = 6;

    SpatialAudio();
    ~SpatialAudio();

    bool Initialize(int sampleRate);
    void Shutdown();
    // Interleaved stereo float; blend is the share of the 3D signal (0..1).
    void Process(float* buffer, int frameCount, float blend);
    bool IsInitialized() const { return m_initialized; }

    void SetMode(SpatialMode mode);
    SpatialMode GetMode() const { return m_mode; }
    void SetRearCenter(bool enabled) { m_rearCenter = enabled; }
    bool GetRearCenter() const { return m_rearCenter; }

    const wchar_t* GetLastError() const { return m_lastError.c_str(); }

    // Scratch space for converting 16 bit audio to float before Process().
    float* GetConversionBuffer(int samples);

private:
    void ProcessBinauralFrame(float* frameL, float* frameR);
    void ProcessSurroundFrame(float* frameL, float* frameR);
    // Render one virtual speaker at angleDeg (clockwise from straight ahead, on a
    // unit circle around the origin) into the current block, heard from the
    // listener position.
    void RenderSpeaker(int speaker, const float* signal, float gain, float angleDeg,
                       float lx, float ly, float lz);
    void ResetVoices();

    fastplay::audio::HrtfDatabase m_hrtf;
    fastplay::audio::HrtfRenderer m_renderer;
    fastplay::audio::HrtfVoiceState m_voices[MAX_SPEAKERS];
    fastplay::audio::AlignedFloats m_voiceInput;  // one speaker's gain-scaled block

    // Surround upmix: FL, FR, C, SL, SR, RC, one block each
    std::vector<float> m_upmix;
    std::vector<float> m_outL, m_outR;

    // Int16-to-float conversion buffer
    std::vector<float> m_convBuf;

    // Input carry (remainder from previous callback, < FRAME_SIZE samples)
    float m_carryL[FRAME_SIZE * 2];  // Extra margin for safety
    float m_carryR[FRAME_SIZE * 2];
    int m_carryCount = 0;

    // Output queue (pre-filled to absorb deficit)
    std::vector<float> m_queueL, m_queueR;
    int m_queueCount = 0;

    int m_sampleRate = 0;
    bool m_initialized = false;
    SpatialMode m_mode = SpatialMode::Binaural;
    bool m_rearCenter = true;
    std::wstring m_lastError;
    std::mutex m_mutex;
};

SpatialAudio* GetSpatialAudio();
void FreeSpatialAudio();

#endif
