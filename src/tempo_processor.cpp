#include "tempo_processor.h"
#include "globals.h"
#include "bass_fx.h"
#include <cmath>
#include <memory>
#include <vector>
#include <mutex>
#include <deque>
#include <algorithm>
#include <cstring>

// Include Speedy/Sonic
#ifdef USE_SPEEDY
extern "C" {
#include "sonic2.h"
}
#endif

// Include Signalsmith Stretch
#ifdef USE_SIGNALSMITH
#include "signalsmith-stretch.h"
#endif

// Global algorithm preference
static TempoAlgorithm g_algorithm = TempoAlgorithm::SoundTouch;
static std::unique_ptr<TempoProcessor> g_tempoProcessor;

// Algorithm metadata
const char* GetAlgorithmName(TempoAlgorithm algo) {
    switch (algo) {
        case TempoAlgorithm::SoundTouch: return "SoundTouch (BASS_FX)";
        case TempoAlgorithm::Speedy: return "Speedy (Google)";
        case TempoAlgorithm::Signalsmith: return "Signalsmith Stretch";
        default: return "Unknown";
    }
}

const char* GetAlgorithmDescription(TempoAlgorithm algo) {
    switch (algo) {
        case TempoAlgorithm::SoundTouch:
            return "Fast processing, good for speech and general use";
        case TempoAlgorithm::Speedy:
            return "Nonlinear speech speedup, preserves consonants";
        case TempoAlgorithm::Signalsmith:
            return "High quality pitch/time, low latency";
        default:
            return "";
    }
}

// ============================================================================
// SoundTouch (BASS_FX) Implementation
// ============================================================================
class SoundTouchProcessor : public TempoProcessor {
private:
    HSTREAM m_sourceStream = 0;
    HSTREAM m_fxStream = 0;
    float m_sampleRate = 44100.0f;
    float m_tempo = 0.0f;   // percentage
    float m_pitch = 0.0f;   // semitones
    float m_rate = 1.0f;    // multiplier

public:
    ~SoundTouchProcessor() override {
        Shutdown();
    }

    HSTREAM Initialize(HSTREAM sourceStream, float sampleRate) override {
        m_sourceStream = sourceStream;
        m_sampleRate = sampleRate;

        // Create tempo stream wrapping the source (use float for DSP effects)
        m_fxStream = BASS_FX_TempoCreate(sourceStream, BASS_FX_FREESOURCE | BASS_SAMPLE_FLOAT);
        if (!m_fxStream) {
            return 0;
        }

        // Apply SoundTouch algorithm settings
        BASS_ChannelSetAttribute(m_fxStream, BASS_ATTRIB_TEMPO_OPTION_USE_AA_FILTER, g_stAntiAliasFilter ? 1.0f : 0.0f);
        BASS_ChannelSetAttribute(m_fxStream, BASS_ATTRIB_TEMPO_OPTION_AA_FILTER_LENGTH, static_cast<float>(g_stAAFilterLength));
        BASS_ChannelSetAttribute(m_fxStream, BASS_ATTRIB_TEMPO_OPTION_USE_QUICKALGO, g_stQuickAlgorithm ? 1.0f : 0.0f);
        BASS_ChannelSetAttribute(m_fxStream, BASS_ATTRIB_TEMPO_OPTION_SEQUENCE_MS, static_cast<float>(g_stSequenceMs));
        BASS_ChannelSetAttribute(m_fxStream, BASS_ATTRIB_TEMPO_OPTION_SEEKWINDOW_MS, static_cast<float>(g_stSeekWindowMs));
        BASS_ChannelSetAttribute(m_fxStream, BASS_ATTRIB_TEMPO_OPTION_OVERLAP_MS, static_cast<float>(g_stOverlapMs));
        BASS_ChannelSetAttribute(m_fxStream, BASS_ATTRIB_TEMPO_OPTION_PREVENT_CLICK, g_stPreventClick ? 1.0f : 0.0f);

        // Apply current tempo/pitch/rate settings
        BASS_ChannelSetAttribute(m_fxStream, BASS_ATTRIB_TEMPO, m_tempo);
        BASS_ChannelSetAttribute(m_fxStream, BASS_ATTRIB_TEMPO_PITCH, m_pitch);
        BASS_ChannelSetAttribute(m_fxStream, BASS_ATTRIB_TEMPO_FREQ, m_sampleRate * m_rate);

        return m_fxStream;
    }

    void Shutdown() override {
        // BASS_FX_FREESOURCE flag means freeing fxStream frees source too
        if (m_fxStream) {
            BASS_StreamFree(m_fxStream);
            m_fxStream = 0;
            m_sourceStream = 0;
        }
    }

    void SetTempo(float tempoPercent) override {
        m_tempo = tempoPercent;
        if (m_fxStream) {
            BASS_ChannelSetAttribute(m_fxStream, BASS_ATTRIB_TEMPO, m_tempo);
        }
    }

    void SetPitch(float semitones) override {
        m_pitch = semitones;
        if (m_fxStream) {
            BASS_ChannelSetAttribute(m_fxStream, BASS_ATTRIB_TEMPO_PITCH, m_pitch);
        }
    }

    void SetRate(float rate) override {
        m_rate = rate;
        if (m_fxStream) {
            BASS_ChannelSetAttribute(m_fxStream, BASS_ATTRIB_TEMPO_FREQ, m_sampleRate * m_rate);
        }
    }

    float GetTempo() const override { return m_tempo; }
    float GetPitch() const override { return m_pitch; }
    float GetRate() const override { return m_rate; }
    bool IsActive() const override { return m_fxStream != 0; }
    TempoAlgorithm GetAlgorithm() const override { return TempoAlgorithm::SoundTouch; }

    double GetLength() const override {
        if (!m_fxStream) return 0.0;
        QWORD bytes = BASS_ChannelGetLength(m_fxStream, BASS_POS_BYTE);
        if (bytes == (QWORD)-1) return 0.0;
        return BASS_ChannelBytes2Seconds(m_fxStream, bytes);
    }

    double GetPosition() const override {
        if (!m_fxStream) return 0.0;
        QWORD bytes = BASS_ChannelGetPosition(m_fxStream, BASS_POS_BYTE);
        if (bytes == (QWORD)-1) return 0.0;
        return BASS_ChannelBytes2Seconds(m_fxStream, bytes);
    }

    void SetPosition(double seconds) override {
        if (!m_fxStream) return;
        QWORD bytes = BASS_ChannelSeconds2Bytes(m_fxStream, seconds);
        BASS_ChannelSetPosition(m_fxStream, bytes, BASS_POS_BYTE | BASS_POS_FLUSH);
    }

    HSTREAM GetSourceStream() const override {
        return m_sourceStream;
    }
};

// ============================================================================
// Push-based processors (Speedy, Signalsmith)
// ============================================================================
// These decode the source themselves and feed a BASS user stream. The base class owns everything
// that is common to both and that BASS_FX does for SoundTouch internally:
//   - the output FIFO feeding the STREAMPROC,
//   - draining the engine at the end of the source, so the last part of the file is heard,
//   - seeking: reposition the source, restart the engine, and throw away the audio already
//     sitting in the output stream's playback buffer, so the new position is heard at once,
//   - position: a map from output frames to source time, read at the output stream's playback
//     position, so the reported position is what is heard rather than how far the decoder has
//     read (which runs ahead by the playback buffer times the speed).
#if defined(USE_SPEEDY) || defined(USE_SIGNALSMITH)

class PushTempoProcessor : public TempoProcessor {
protected:
    HSTREAM m_sourceStream = 0;
    HSTREAM m_outputStream = 0;
    float m_sampleRate = 44100.0f;
    int m_channels = 2;
    float m_tempo = 0.0f;   // percentage
    float m_pitch = 0.0f;   // semitones
    float m_rate = 1.0f;    // multiplier

    // Engine and FIFO state, shared between the STREAMPROC and the UI thread.
    mutable std::mutex m_mutex;
    std::vector<float> m_decodeBuffer;
    std::vector<float> m_queue;         // interleaved output not yet handed to BASS
    size_t m_queueRead = 0;
    bool m_inputEnded = false;          // the source has no more data
    bool m_drained = false;             // the engine's tail has been flushed into the queue

    // Source time of the start of the current run (set on seek) and frames of real source
    // audio decoded since then.
    double m_runStart = 0.0;
    QWORD m_runFrames = 0;

    static constexpr int DECODE_BLOCK_FRAMES = 1024;

    double Speed() const {
        double speed = (100.0 + m_tempo) / 100.0 * m_rate;
        if (speed < 0.1) speed = 0.1;
        if (speed > 6.0) speed = 6.0;
        return speed;
    }

    double RunTime(double frames) const {
        return m_runStart + frames / m_sampleRate;
    }

    // Read up to `frames` interleaved frames from the source into m_decodeBuffer.
    // Returns frames read; sets m_inputEnded at the end of the source.
    int DecodeSource(int frames) {
        m_decodeBuffer.resize(static_cast<size_t>(frames) * m_channels);
        DWORD want = static_cast<DWORD>(m_decodeBuffer.size() * sizeof(float));
        DWORD got = 0;
        while (got < want) {
            DWORD r = BASS_ChannelGetData(m_sourceStream,
                reinterpret_cast<BYTE*>(m_decodeBuffer.data()) + got, (want - got) | BASS_DATA_FLOAT);
            if (r == (DWORD)-1) { m_inputEnded = true; break; }
            if (r == 0) break;  // nothing available right now
            got += r;
        }
        int read = static_cast<int>(got / (sizeof(float) * m_channels));
        m_runFrames += read;
        return read;
    }

    // Engine hooks, called with m_mutex held.
    // Start the engine at the source's current position (m_runStart). May decode pre-roll.
    virtual void EngineStart() = 0;
    // Process `frames` interleaved frames from m_decodeBuffer and Emit() the output.
    virtual void EngineFeed(int frames) = 0;
    // The source has ended: Emit() whatever the engine still holds.
    virtual void EngineDrain() = 0;

    // Append output to the FIFO and map it to source time up to `srcEnd` seconds.
    void Emit(const float* interleaved, int frames, double srcEnd) {
        if (frames <= 0) return;
        m_queue.insert(m_queue.end(), interleaved, interleaved + static_cast<size_t>(frames) * m_channels);

        std::lock_guard<std::mutex> lock(m_mapMutex);
        Segment seg;
        seg.outStart = m_outProduced;
        seg.outEnd = m_outProduced + frames;
        seg.srcStart = m_mapEnd;
        seg.srcEnd = srcEnd > m_mapEnd ? srcEnd : m_mapEnd;
        m_segments.push_back(seg);
        m_outProduced = seg.outEnd;
        m_mapEnd = seg.srcEnd;
        // Drop segments that have been played; keep a bound if nobody is asking.
        QWORD played = m_lastPlayedFrame;
        while (m_segments.size() > 1 && (m_segments.front().outEnd <= played || m_segments.size() > 4096)) {
            m_segments.pop_front();
        }
    }

private:
    struct Segment {
        QWORD outStart, outEnd;     // output frames since the last reset
        double srcStart, srcEnd;    // source seconds
    };
    mutable std::mutex m_mapMutex;  // separate from m_mutex so GetPosition never waits on processing
    std::deque<Segment> m_segments;
    QWORD m_outProduced = 0;
    double m_mapEnd = 0.0;
    mutable QWORD m_lastPlayedFrame = 0;

    void ResetMap(double start) {
        std::lock_guard<std::mutex> lock(m_mapMutex);
        m_segments.clear();
        m_outProduced = 0;
        m_mapEnd = start;
        m_lastPlayedFrame = 0;
    }

    // Restart the run at the source's current position. m_mutex held.
    void Restart() {
        m_queue.clear();
        m_queueRead = 0;
        m_inputEnded = false;
        m_drained = false;
        QWORD pos = BASS_ChannelGetPosition(m_sourceStream, BASS_POS_BYTE);
        m_runStart = (pos == (QWORD)-1) ? 0.0 : BASS_ChannelBytes2Seconds(m_sourceStream, pos);
        m_runFrames = 0;
        ResetMap(m_runStart);
        EngineStart();
    }

    DWORD Fill(float* out, DWORD length) {
        std::lock_guard<std::mutex> lock(m_mutex);
        const size_t samplesNeeded = length / sizeof(float);
        size_t written = 0;

        while (written < samplesNeeded) {
            size_t avail = m_queue.size() - m_queueRead;
            if (avail > 0) {
                size_t n = std::min(samplesNeeded - written, avail);
                memcpy(out + written, m_queue.data() + m_queueRead, n * sizeof(float));
                written += n;
                m_queueRead += n;
                continue;
            }
            m_queue.clear();
            m_queueRead = 0;
            if (m_drained) break;
            if (m_inputEnded) {
                EngineDrain();
                m_drained = true;
                continue;
            }
            int frames = DecodeSource(DECODE_BLOCK_FRAMES);
            if (frames > 0) {
                EngineFeed(frames);
            } else if (!m_inputEnded) {
                break;  // source has nothing right now; try again on the next call
            }
        }

        // Keep the FIFO from growing without bound through repeated partial reads.
        if (m_queueRead > 0 && m_queueRead * 2 > m_queue.size()) {
            m_queue.erase(m_queue.begin(), m_queue.begin() + m_queueRead);
            m_queueRead = 0;
        }

        DWORD bytes = static_cast<DWORD>(written * sizeof(float));
        if (m_drained && m_queue.size() == m_queueRead) {
            bytes |= BASS_STREAMPROC_END;
        }
        return bytes;
    }

    static DWORD CALLBACK StreamProc(HSTREAM, void* buffer, DWORD length, void* user) {
        PushTempoProcessor* proc = static_cast<PushTempoProcessor*>(user);
        if (!proc) return BASS_STREAMPROC_END;
        return proc->Fill(static_cast<float*>(buffer), length);
    }

public:
    ~PushTempoProcessor() override = default;

    HSTREAM Initialize(HSTREAM sourceStream, float sampleRate) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        BASS_CHANNELINFO info;
        if (!BASS_ChannelGetInfo(sourceStream, &info) || info.chans == 0) {
            return 0;
        }
        m_sourceStream = sourceStream;
        m_sampleRate = sampleRate;
        m_channels = static_cast<int>(info.chans);
        if (!EngineCreate()) {
            m_sourceStream = 0;
            return 0;
        }
        Restart();

        m_outputStream = BASS_StreamCreate(static_cast<DWORD>(m_sampleRate), m_channels,
                                           BASS_SAMPLE_FLOAT, StreamProc, this);
        if (!m_outputStream) {
            EngineDestroy();
            m_sourceStream = 0;
            return 0;
        }
        return m_outputStream;
    }

    void Shutdown() override {
        // Free the output stream before taking the lock: BASS_StreamFree waits for a running
        // STREAMPROC, which itself needs the lock.
        HSTREAM out = m_outputStream;
        m_outputStream = 0;
        if (out) BASS_StreamFree(out);

        std::lock_guard<std::mutex> lock(m_mutex);
        EngineDestroy();
        m_sourceStream = 0;
        m_queue.clear();
        m_queueRead = 0;
    }

    float GetTempo() const override { return m_tempo; }
    float GetPitch() const override { return m_pitch; }
    float GetRate() const override { return m_rate; }
    bool IsActive() const override { return m_outputStream != 0; }

    double GetLength() const override {
        if (!m_sourceStream) return 0.0;
        QWORD bytes = BASS_ChannelGetLength(m_sourceStream, BASS_POS_BYTE);
        if (bytes == (QWORD)-1) return 0.0;
        return BASS_ChannelBytes2Seconds(m_sourceStream, bytes);
    }

    double GetPosition() const override {
        if (!m_outputStream) return 0.0;
        QWORD bytes = BASS_ChannelGetPosition(m_outputStream, BASS_POS_BYTE);
        QWORD played = (bytes == (QWORD)-1) ? 0 : bytes / (sizeof(float) * m_channels);

        std::lock_guard<std::mutex> lock(m_mapMutex);
        m_lastPlayedFrame = played;
        if (m_segments.empty()) return m_mapEnd;
        const Segment& first = m_segments.front();
        if (played <= first.outStart) return first.srcStart;
        for (const Segment& seg : m_segments) {
            if (played < seg.outEnd) {
                double t = static_cast<double>(played - seg.outStart) / static_cast<double>(seg.outEnd - seg.outStart);
                return seg.srcStart + (seg.srcEnd - seg.srcStart) * t;
            }
        }
        return m_segments.back().srcEnd;
    }

    void SetPosition(double seconds) override {
        if (!m_sourceStream || !m_outputStream) return;

        // Keep the STREAMPROC out while the source, engine and playback buffer are reset together.
        BASS_ChannelLock(m_outputStream, TRUE);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            QWORD bytes = BASS_ChannelSeconds2Bytes(m_sourceStream, seconds);
            if (BASS_ChannelSetPosition(m_sourceStream, bytes, BASS_POS_BYTE)) {
                Restart();
            } else {
                // Past the end (or unseekable): end the stream so playback moves on.
                m_queue.clear();
                m_queueRead = 0;
                m_inputEnded = true;
                m_drained = true;
                ResetMap(seconds);
            }
        }
        // Resetting a user stream to 0 discards its playback buffer, so the new position is heard
        // straight away instead of after the buffer of old audio plays out.
        BASS_ChannelSetPosition(m_outputStream, 0, BASS_POS_BYTE);
        BASS_ChannelLock(m_outputStream, FALSE);
    }

    HSTREAM GetSourceStream() const override { return m_sourceStream; }

protected:
    virtual bool EngineCreate() = 0;
    virtual void EngineDestroy() = 0;
};

#endif // USE_SPEEDY || USE_SIGNALSMITH

// ============================================================================
// Speedy (Google)
// ============================================================================
#ifdef USE_SPEEDY

class SpeedyProcessor : public PushTempoProcessor {
private:
    sonicStream m_sonicStream = nullptr;
    bool m_nonlinearEnabled = true;
    std::vector<float> m_scratch;
    QWORD m_alignedFrames = 0;     // source frames of this run that the emitted output covers

    // Sonic and Speedy hold some input back before it comes out: Speedy looks ahead
    // kTemporalHysteresisFuture 10 ms frames plus its 15 ms analysis window, and Sonic keeps
    // up to two pitch periods. Output is mapped to the input this far behind what was written.
    double LatencyFrames() const {
        return m_sampleRate * (m_nonlinearEnabled ? 0.19 : 0.03);
    }

    void UpdateSonicParams() {
        if (!m_sonicStream) return;
        sonicSetSpeed(m_sonicStream, static_cast<float>(Speed()));
        // sonicSetPitch is not wrapped by sonic2.h, use internal function
        sonicIntSetPitch(m_sonicStream, powf(2.0f, m_pitch / 12.0f));
    }

    void ReadOutput(double srcEnd) {
        const int chunk = 4096;
        m_scratch.resize(static_cast<size_t>(chunk) * m_channels);
        int got;
        while ((got = sonicReadFloatFromStream(m_sonicStream, m_scratch.data(), chunk)) > 0) {
            Emit(m_scratch.data(), got, srcEnd);
        }
    }

protected:
    bool EngineCreate() override {
        m_nonlinearEnabled = g_speedyNonlinear;
        m_sonicStream = sonicCreateStream(static_cast<int>(m_sampleRate), m_channels);
        return m_sonicStream != nullptr;
    }

    void EngineDestroy() override {
        if (m_sonicStream) {
            sonicDestroyStream(m_sonicStream);
            m_sonicStream = nullptr;
        }
    }

    void EngineStart() override {
        // A fresh stream per run: Sonic and Speedy have no reset of their own.
        EngineDestroy();
        m_sonicStream = sonicCreateStream(static_cast<int>(m_sampleRate), m_channels);
        if (m_sonicStream && m_nonlinearEnabled) {
            sonicEnableNonlinearSpeedup(m_sonicStream, 1.0f);
        }
        UpdateSonicParams();
        m_alignedFrames = 0;
        if (!m_sonicStream) m_drained = true;  // nothing we can play
    }

    void EngineFeed(int frames) override {
        if (!m_sonicStream) return;
        // Speedy's shim converts to 16 bit with a bare cast, which wraps at full scale.
        float* samples = m_decodeBuffer.data();
        const size_t count = static_cast<size_t>(frames) * m_channels;
        for (size_t i = 0; i < count; i++) {
            if (samples[i] > 0.99996f) samples[i] = 0.99996f;
            else if (samples[i] < -1.0f) samples[i] = -1.0f;
        }
        sonicWriteFloatToStream(m_sonicStream, samples, frames);

        double aligned = static_cast<double>(m_runFrames) - LatencyFrames();
        if (aligned > static_cast<double>(m_alignedFrames)) m_alignedFrames = static_cast<QWORD>(aligned);
        ReadOutput(RunTime(static_cast<double>(m_alignedFrames)));
    }

    void EngineDrain() override {
        if (!m_sonicStream) return;
        if (m_nonlinearEnabled) {
            // Speedy's flush only passes on complete 10 ms buffers; top up the last partial one
            // with silence so the final few milliseconds are not dropped.
            std::vector<float> silence(static_cast<size_t>(m_sampleRate / 100.0f + 1) * m_channels, 0.0f);
            sonicWriteFloatToStream(m_sonicStream, silence.data(), static_cast<int>(silence.size() / m_channels));
        }
        sonicFlushStream(m_sonicStream);
        m_alignedFrames = m_runFrames;
        ReadOutput(RunTime(static_cast<double>(m_runFrames)));
    }

public:
    ~SpeedyProcessor() override { Shutdown(); }

    TempoAlgorithm GetAlgorithm() const override { return TempoAlgorithm::Speedy; }

    void SetTempo(float tempoPercent) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_tempo = tempoPercent;
        UpdateSonicParams();
    }

    void SetPitch(float semitones) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pitch = semitones;
        UpdateSonicParams();
    }

    void SetRate(float rate) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_rate = rate;
        UpdateSonicParams();
    }
};

#endif // USE_SPEEDY

// ============================================================================
// Signalsmith Stretch
// ============================================================================
#ifdef USE_SIGNALSMITH

class SignalsmithProcessor : public PushTempoProcessor {
private:
    signalsmith::stretch::SignalsmithStretch<float> m_stretcher;
    std::vector<std::vector<float>> m_channelIn, m_channelOut;
    std::vector<float*> m_inPtrs, m_outPtrs;
    std::vector<float> m_interleaved;
    QWORD m_alignedFrames = 0;   // source frames of this run already rendered to output
    double m_outFraction = 0.0;  // carries the fractional output frame between blocks

    float TonalityLimit() const {
        return g_ssTonalityLimit > 0 ? static_cast<float>(g_ssTonalityLimit) / m_sampleRate : 0.0f;
    }

    void Deinterleave(const float* in, int frames) {
        for (int ch = 0; ch < m_channels; ch++) {
            m_channelIn[ch].resize(frames);
            float* dst = m_channelIn[ch].data();
            for (int i = 0; i < frames; i++) dst[i] = in[static_cast<size_t>(i) * m_channels + ch];
            m_inPtrs[ch] = dst;
        }
    }

    void PrepareOutput(int frames) {
        for (int ch = 0; ch < m_channels; ch++) {
            m_channelOut[ch].resize(frames > 0 ? frames : 1);
            m_outPtrs[ch] = m_channelOut[ch].data();
        }
    }

    void EmitOutput(int frames, double srcEnd) {
        m_interleaved.resize(static_cast<size_t>(frames) * m_channels);
        for (int ch = 0; ch < m_channels; ch++) {
            const float* src = m_channelOut[ch].data();
            for (int i = 0; i < frames; i++) m_interleaved[static_cast<size_t>(i) * m_channels + ch] = src[i];
        }
        Emit(m_interleaved.data(), frames, srcEnd);
    }

    double RealTime(QWORD engineFrames) const {
        // Pre-roll padding past the end of a short file is not real audio.
        return RunTime(static_cast<double>(std::min(engineFrames, m_runFrames)));
    }

protected:
    bool EngineCreate() override {
        if (g_ssPreset == 1) {
            m_stretcher.presetCheaper(m_channels, m_sampleRate);
        } else {
            m_stretcher.presetDefault(m_channels, m_sampleRate);
        }
        m_stretcher.setTransposeSemitones(m_pitch, TonalityLimit());
        m_channelIn.assign(m_channels, {});
        m_channelOut.assign(m_channels, {});
        m_inPtrs.assign(m_channels, nullptr);
        m_outPtrs.assign(m_channels, nullptr);
        return true;
    }

    void EngineDestroy() override {
        m_channelIn.clear();
        m_channelOut.clear();
        m_inPtrs.clear();
        m_outPtrs.clear();
    }

    void EngineStart() override {
        // Pre-roll: hand the stretcher the audio just after the start point so that its first
        // output lines up with the start point. Without this the first ~150 ms after every seek
        // come out as the stretcher's own latency (silence).
        const double speed = Speed();
        const int seekLen = m_stretcher.outputSeekLength(static_cast<float>(speed));
        int got = DecodeSource(seekLen);
        m_decodeBuffer.resize(static_cast<size_t>(seekLen) * m_channels, 0.0f);
        std::fill(m_decodeBuffer.begin() + static_cast<size_t>(got) * m_channels, m_decodeBuffer.end(), 0.0f);
        Deinterleave(m_decodeBuffer.data(), seekLen);
        float** inputs = m_inPtrs.data();
        m_stretcher.outputSeek(inputs, seekLen);
        m_alignedFrames = 0;
        m_outFraction = 0.0;
    }

    void EngineFeed(int frames) override {
        const double speed = Speed();
        m_outFraction += frames / speed;
        int outFrames = static_cast<int>(m_outFraction);
        m_outFraction -= outFrames;

        Deinterleave(m_decodeBuffer.data(), frames);
        PrepareOutput(outFrames);
        m_stretcher.process(m_inPtrs.data(), frames, m_outPtrs.data(), outFrames);
        m_alignedFrames += frames;
        EmitOutput(outFrames, RealTime(m_alignedFrames));
    }

    void EngineDrain() override {
        // Everything fed but not yet rendered (the stretcher's latency) comes out here; without
        // it the end of every file was cut short.
        const double speed = Speed();
        QWORD realEnd = std::max(m_runFrames, m_alignedFrames);
        QWORD remaining = realEnd - m_alignedFrames;
        int outFrames = static_cast<int>(remaining / speed + m_outFraction + 0.5);
        if (outFrames <= 0) return;
        PrepareOutput(outFrames);
        m_stretcher.flush(m_outPtrs.data(), outFrames, static_cast<float>(speed));
        m_alignedFrames = realEnd;
        EmitOutput(outFrames, RealTime(realEnd));
    }

public:
    ~SignalsmithProcessor() override { Shutdown(); }

    TempoAlgorithm GetAlgorithm() const override { return TempoAlgorithm::Signalsmith; }

    void SetTempo(float tempoPercent) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_tempo = tempoPercent;  // applied through the input/output ratio of each block
    }

    void SetPitch(float semitones) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pitch = semitones;
        m_stretcher.setTransposeSemitones(semitones, TonalityLimit());
    }

    void SetRate(float rate) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_rate = rate;  // applied through the input/output ratio of each block
    }
};

#endif // USE_SIGNALSMITH

// ============================================================================
// Factory and Global Management
// ============================================================================
TempoProcessor* CreateTempoProcessor(TempoAlgorithm algorithm) {
    switch (algorithm) {
        case TempoAlgorithm::SoundTouch:
            return new SoundTouchProcessor();
#ifdef USE_SPEEDY
        case TempoAlgorithm::Speedy:
            return new SpeedyProcessor();
#endif
#ifdef USE_SIGNALSMITH
        case TempoAlgorithm::Signalsmith:
            return new SignalsmithProcessor();
#endif
        default:
            // Fall back to SoundTouch if algorithm not available
            return new SoundTouchProcessor();
    }
}

TempoAlgorithm GetCurrentAlgorithm() {
    return g_algorithm;
}

void SetCurrentAlgorithm(TempoAlgorithm algorithm) {
    // Check if the selected algorithm is available
    bool available = false;
    switch (algorithm) {
        case TempoAlgorithm::SoundTouch:
            available = true;
            break;
#ifdef USE_SPEEDY
        case TempoAlgorithm::Speedy:
            available = true;
            break;
#endif
#ifdef USE_SIGNALSMITH
        case TempoAlgorithm::Signalsmith:
            available = true;
            break;
#endif
        default:
            break;
    }

    if (!available) {
        algorithm = TempoAlgorithm::SoundTouch;
    }
    g_algorithm = algorithm;
}

void InitTempoProcessor() {
    if (!g_tempoProcessor) {
        g_tempoProcessor.reset(CreateTempoProcessor(g_algorithm));
    }
}

void FreeTempoProcessor() {
    if (g_tempoProcessor) {
        g_tempoProcessor->Shutdown();
        g_tempoProcessor.reset();
    }
}

TempoProcessor* GetTempoProcessor() {
    if (!g_tempoProcessor) {
        InitTempoProcessor();
    }
    return g_tempoProcessor.get();
}
