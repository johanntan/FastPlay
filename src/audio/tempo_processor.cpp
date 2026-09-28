// The tempo stage: see tempo_processor.h.
//
// The base class does what is common to every way of stretching:
//   - the output queue the mix thread reads,
//   - draining at the end of the source, so the last part of a file is heard,
//   - the direct path while tempo and pitch are both 0, and moving between it and
//     the stretcher when they change,
//   - rate and sample rate: a resampler after the stretcher,
//   - position: a map from output frames to source time.
// Speedy and Signalsmith Stretch each supply the stretching itself.

#include "tempo_processor.h"
#include "globals.h"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

#ifdef USE_SPEEDY
extern "C" {
#include "sonic2.h"
}
#endif

#ifdef USE_SIGNALSMITH
#include "signalsmith-stretch.h"
#endif

namespace audio {
namespace {

const int kChannels = 2;
const int kBlockFrames = 1024;  // frames read from the source at a time

class ProcessorBase : public TempoProcessor {
public:
    ProcessorBase(int sourceRate, int outputRate) : m_sampleRate(static_cast<float>(sourceRate)), m_outputRate(outputRate) {}
    ~ProcessorBase() override { swr_free(&m_swr); }

    void Restart(double seconds) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.clear();
        m_queueRead = 0;
        m_inputEnded = false;
        m_drained = false;
        m_runStart = seconds;
        m_runFrames = 0;
        m_bypass = WantBypass();
        m_needStart = !m_bypass;
        m_bypassFrames = 0;
        swr_free(&m_swr);  // made afresh, with nothing held from before
        ResetMap(seconds);
    }

    int Fill(PcmSource& source, float* out, int frames, bool& ended) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        ended = false;
        const size_t wanted = static_cast<size_t>(frames) * kChannels;
        size_t written = 0;
        while (written < wanted) {
            size_t available = m_queue.size() - m_queueRead;
            if (available > 0) {
                size_t n = std::min(wanted - written, available);
                std::memcpy(out + written, m_queue.data() + m_queueRead, n * sizeof(float));
                written += n;
                m_queueRead += n;
                continue;
            }
            m_queue.clear();
            m_queueRead = 0;
            if (m_drained) {
                ended = true;
                break;
            }
            if (!Produce(source)) break;  // the source is behind
        }
        return static_cast<int>(written / kChannels);
    }

    double PositionAt(uint64_t played) override {
        std::lock_guard<std::mutex> lock(m_mapMutex);
        m_lastPlayed = played;
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

    void SetTempo(float percent) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_tempo = percent;
        OnParameters();
    }

    void SetPitch(float semitones) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pitch = semitones;
        OnParameters();
    }

    void SetRate(float rate) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_rate = rate > 0 ? rate : 1.0f;
    }

protected:
    float m_sampleRate;
    const int m_outputRate;
    float m_tempo = 0.0f;  // percent
    float m_pitch = 0.0f;  // semitones
    float m_rate = 1.0f;

    std::vector<float> m_input;  // interleaved input of the current block
    double m_runStart = 0.0;     // source seconds where this run began
    uint64_t m_runFrames = 0;    // source frames read since then

    double Speed() const {
        double speed = (100.0 + m_tempo) / 100.0;
        return std::clamp(speed, 0.1, 6.0);
    }

    double RunTime(double frames) const { return m_runStart + frames / m_sampleRate; }

    // Reads up to `frames` from the source into m_input.
    int ReadSource(PcmSource& source, int frames) {
        m_input.resize(static_cast<size_t>(frames) * kChannels);
        int got = source.Read(m_input.data(), frames);
        m_runFrames += static_cast<uint64_t>(got);
        return got;
    }

    // Stretcher output, mapped to source time up to `srcEnd`: on to the resampler.
    void Emit(const float* interleaved, int frames, double srcEnd) {
        if (frames <= 0) {
            m_pendingEnd = std::max(m_pendingEnd, srcEnd);
            return;
        }
        Resample(interleaved, frames, std::max(m_pendingEnd, srcEnd));
    }

    // The stretching, supplied by each processor. Called with m_mutex held.
    virtual bool EngineCreate() = 0;
    // Starts at the source's current place. May need input first (pre-roll):
    // false if it must wait for more.
    virtual bool EngineStart(PcmSource& source) = 0;
    virtual void EngineFeed(int frames) = 0;
    virtual void EngineDrain() = 0;
    // Tempo or pitch changed (m_mutex held).
    virtual void EngineParameters() {}

private:
    struct Segment {
        uint64_t outStart, outEnd;  // output frames since the last Restart()
        double srcStart, srcEnd;    // source seconds
    };

    std::mutex m_mutex;
    std::vector<float> m_queue;  // output not yet read, interleaved
    size_t m_queueRead = 0;
    bool m_inputEnded = false;
    bool m_drained = false;
    bool m_bypass = true;       // tempo and pitch at 0: no stretching
    bool m_needStart = false;   // the stretcher has to be started before use
    uint64_t m_bypassFrames = 0;
    bool m_created = false;
    double m_pendingEnd = 0.0;

    SwrContext* m_swr = nullptr;
    int m_swrInRate = 0;
    std::vector<float> m_resampled;

    std::mutex m_mapMutex;  // separate, so a position is never kept waiting on processing
    std::deque<Segment> m_segments;
    uint64_t m_outProduced = 0;
    double m_mapEnd = 0.0;
    uint64_t m_lastPlayed = 0;

    bool WantBypass() const { return m_tempo == 0.0f && m_pitch == 0.0f; }

    void OnParameters() {
        if (m_created) EngineParameters();
    }

    void ResetMap(double start) {
        std::lock_guard<std::mutex> lock(m_mapMutex);
        m_segments.clear();
        m_outProduced = 0;
        m_mapEnd = start;
        m_lastPlayed = 0;
        m_pendingEnd = start;
    }

    // Makes more output. False if the source has nothing for now.
    bool Produce(PcmSource& source) {
        // Moving between the direct path and the stretcher: what the stretcher holds
        // comes out first, then the other starts where the input has got to.
        if (!m_inputEnded && WantBypass() != m_bypass) {
            if (!m_bypass && !m_needStart) EngineDrain();
            m_runStart = RunTime(static_cast<double>(m_runFrames));
            m_runFrames = 0;
            m_bypass = WantBypass();
            m_needStart = !m_bypass;
            m_bypassFrames = 0;
        }
        if (!m_bypass && m_needStart) {
            if (!m_created) {
                if (!EngineCreate()) {
                    m_bypass = true;  // no stretcher: play at normal speed
                    m_needStart = false;
                    return true;
                }
                m_created = true;
                EngineParameters();
            }
            if (!EngineStart(source)) {
                if (!source.Ended()) return false;
            }
            m_needStart = false;
            return true;
        }
        if (m_inputEnded) {
            if (!m_bypass) EngineDrain();
            FlushResampler();
            m_drained = true;
            return true;
        }
        int frames = ReadSource(source, kBlockFrames);
        if (frames <= 0) {
            if (source.Ended()) {
                m_inputEnded = true;
                return true;
            }
            return false;
        }
        if (m_bypass) {
            m_bypassFrames += static_cast<uint64_t>(frames);
            Emit(m_input.data(), frames, RunTime(static_cast<double>(m_bypassFrames)));
        } else {
            EngineFeed(frames);
        }
        return true;
    }

    // Rate and sample rate: the stretcher's output resampled to the device's rate,
    // as if played at the source's rate times the rate setting.
    void Resample(const float* in, int frames, double srcEnd) {
        int inRate = static_cast<int>(std::lround(m_sampleRate * m_rate));
        if (inRate <= 0) inRate = static_cast<int>(m_sampleRate);
        if (!m_swr || inRate != m_swrInRate) {
            // A new rate: finish what the old one holds before changing.
            if (m_swr) FlushResampler();
            swr_free(&m_swr);
            AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
            if (swr_alloc_set_opts2(&m_swr, &stereo, AV_SAMPLE_FMT_FLT, m_outputRate, &stereo, AV_SAMPLE_FMT_FLT,
                                    inRate, 0, nullptr) < 0 ||
                swr_init(m_swr) < 0) {
                swr_free(&m_swr);
            }
            m_swrInRate = inRate;
        }
        if (!m_swr) return;
        int capacity = swr_get_out_samples(m_swr, frames);
        if (capacity <= 0) return;
        m_resampled.resize(static_cast<size_t>(capacity) * kChannels);
        uint8_t* outPlanes[1] = {reinterpret_cast<uint8_t*>(m_resampled.data())};
        const uint8_t* inPlanes[1] = {reinterpret_cast<const uint8_t*>(in)};
        int got = swr_convert(m_swr, outPlanes, capacity, inPlanes, frames);
        Queue(got, srcEnd);
    }

    void FlushResampler() {
        if (!m_swr) return;
        int capacity = swr_get_out_samples(m_swr, 0);
        if (capacity <= 0) return;
        m_resampled.resize(static_cast<size_t>(capacity) * kChannels);
        uint8_t* outPlanes[1] = {reinterpret_cast<uint8_t*>(m_resampled.data())};
        int got = swr_convert(m_swr, outPlanes, capacity, nullptr, 0);
        Queue(got, m_pendingEnd);
    }

    // Queues `frames` of m_resampled and maps them to source time up to `srcEnd`.
    void Queue(int frames, double srcEnd) {
        m_pendingEnd = std::max(m_pendingEnd, srcEnd);
        if (frames <= 0) return;
        m_queue.insert(m_queue.end(), m_resampled.begin(), m_resampled.begin() + static_cast<size_t>(frames) * kChannels);

        std::lock_guard<std::mutex> lock(m_mapMutex);
        Segment seg;
        seg.outStart = m_outProduced;
        seg.outEnd = m_outProduced + static_cast<uint64_t>(frames);
        seg.srcStart = m_mapEnd;
        seg.srcEnd = std::max(m_pendingEnd, m_mapEnd);
        m_segments.push_back(seg);
        m_outProduced = seg.outEnd;
        m_mapEnd = seg.srcEnd;
        // Drop what has been played; keep a bound if nobody is asking.
        while (m_segments.size() > 1 && (m_segments.front().outEnd <= m_lastPlayed || m_segments.size() > 4096)) {
            m_segments.pop_front();
        }
    }
};

// ============================================================================
// Speedy (Google)
// ============================================================================
#ifdef USE_SPEEDY

class SpeedyProcessor : public ProcessorBase {
public:
    using ProcessorBase::ProcessorBase;
    ~SpeedyProcessor() override { Destroy(); }

protected:
    bool EngineCreate() override { return true; }

    bool EngineStart(PcmSource&) override {
        // A fresh stream each time: Sonic and Speedy have no reset of their own.
        Destroy();
        m_nonlinear = g_speedyNonlinear;
        m_sonic = sonicCreateStream(static_cast<int>(m_sampleRate), kChannels);
        if (m_sonic && m_nonlinear) sonicEnableNonlinearSpeedup(m_sonic, 1.0f);
        EngineParameters();
        m_aligned = 0;
        return true;
    }

    void EngineParameters() override {
        if (!m_sonic) return;
        sonicSetSpeed(m_sonic, static_cast<float>(Speed()));
        sonicIntSetPitch(m_sonic, powf(2.0f, m_pitch / 12.0f));
    }

    void EngineFeed(int frames) override {
        if (!m_sonic) return;
        // Speedy's shim converts to 16 bit with a bare cast, which wraps at full scale.
        float* samples = m_input.data();
        const size_t count = static_cast<size_t>(frames) * kChannels;
        for (size_t i = 0; i < count; i++) samples[i] = std::clamp(samples[i], -1.0f, 0.99996f);
        sonicWriteFloatToStream(m_sonic, samples, frames);

        // Sonic and Speedy hold input back before it comes out: Speedy looks ahead
        // 10 frames of 10 ms plus a 15 ms window, and Sonic keeps up to two pitch
        // periods. Output is mapped to the input this far behind what was written.
        double latency = m_sampleRate * (m_nonlinear ? 0.19 : 0.03);
        double aligned = static_cast<double>(m_runFrames) - latency;
        if (aligned > static_cast<double>(m_aligned)) m_aligned = static_cast<uint64_t>(aligned);
        ReadOutput(RunTime(static_cast<double>(m_aligned)));
    }

    void EngineDrain() override {
        if (!m_sonic) return;
        if (m_nonlinear) {
            // Speedy's flush only passes on complete 10 ms buffers; top up the last
            // partial one with silence so the final few milliseconds are not lost.
            std::vector<float> silence(static_cast<size_t>(m_sampleRate / 100.0f + 1) * kChannels, 0.0f);
            sonicWriteFloatToStream(m_sonic, silence.data(), static_cast<int>(silence.size() / kChannels));
        }
        sonicFlushStream(m_sonic);
        m_aligned = m_runFrames;
        ReadOutput(RunTime(static_cast<double>(m_runFrames)));
        Destroy();
    }

private:
    sonicStream m_sonic = nullptr;
    bool m_nonlinear = true;
    std::vector<float> m_scratch;
    uint64_t m_aligned = 0;

    void Destroy() {
        if (m_sonic) sonicDestroyStream(m_sonic);
        m_sonic = nullptr;
    }

    void ReadOutput(double srcEnd) {
        const int chunk = 4096;
        m_scratch.resize(static_cast<size_t>(chunk) * kChannels);
        int got;
        bool any = false;
        while ((got = sonicReadFloatFromStream(m_sonic, m_scratch.data(), chunk)) > 0) {
            Emit(m_scratch.data(), got, srcEnd);
            any = true;
        }
        if (!any) Emit(nullptr, 0, srcEnd);
    }
};

#endif  // USE_SPEEDY

// ============================================================================
// Signalsmith Stretch
// ============================================================================
#ifdef USE_SIGNALSMITH

class SignalsmithProcessor : public ProcessorBase {
public:
    using ProcessorBase::ProcessorBase;

protected:
    bool EngineCreate() override {
        if (g_ssPreset == 1) {
            m_stretcher.presetCheaper(kChannels, m_sampleRate);
        } else {
            m_stretcher.presetDefault(kChannels, m_sampleRate);
        }
        m_channelIn.assign(kChannels, {});
        m_channelOut.assign(kChannels, {});
        m_inPtrs.assign(kChannels, nullptr);
        m_outPtrs.assign(kChannels, nullptr);
        return true;
    }

    void EngineParameters() override { m_stretcher.setTransposeSemitones(m_pitch, TonalityLimit()); }

    bool EngineStart(PcmSource& source) override {
        // Pre-roll: the stretcher is handed the audio just after the start so its
        // first output lines up with it. Without this the first ~150 ms after
        // every seek would be the stretcher's own latency (silence).
        const int seekLength = m_stretcher.outputSeekLength(static_cast<float>(Speed()));
        if (source.Available() < seekLength && !source.Ended()) return false;
        m_stretcher.reset();
        EngineParameters();
        int got = ReadSource(source, seekLength);
        m_input.resize(static_cast<size_t>(seekLength) * kChannels);
        std::fill(m_input.begin() + static_cast<size_t>(got) * kChannels, m_input.end(), 0.0f);
        Deinterleave(m_input.data(), seekLength);
        m_stretcher.outputSeek(m_inPtrs.data(), seekLength);
        m_aligned = 0;
        m_outFraction = 0.0;
        return true;
    }

    void EngineFeed(int frames) override {
        const double speed = Speed();
        m_outFraction += frames / speed;
        int outFrames = static_cast<int>(m_outFraction);
        m_outFraction -= outFrames;

        Deinterleave(m_input.data(), frames);
        PrepareOutput(outFrames);
        m_stretcher.process(m_inPtrs.data(), frames, m_outPtrs.data(), outFrames);
        m_aligned += static_cast<uint64_t>(frames);
        EmitOutput(outFrames, RealTime(m_aligned));
    }

    void EngineDrain() override {
        // Everything fed but not yet rendered (the stretcher's latency) comes out
        // here; without it the end of every file was cut short.
        const double speed = Speed();
        uint64_t realEnd = std::max(m_runFrames, m_aligned);
        uint64_t remaining = realEnd - m_aligned;
        int outFrames = static_cast<int>(remaining / speed + m_outFraction + 0.5);
        m_aligned = realEnd;
        if (outFrames <= 0) return;
        PrepareOutput(outFrames);
        m_stretcher.flush(m_outPtrs.data(), outFrames, static_cast<float>(speed));
        EmitOutput(outFrames, RealTime(realEnd));
    }

private:
    signalsmith::stretch::SignalsmithStretch<float> m_stretcher;
    std::vector<std::vector<float>> m_channelIn, m_channelOut;
    std::vector<float*> m_inPtrs, m_outPtrs;
    std::vector<float> m_interleaved;
    uint64_t m_aligned = 0;       // source frames of this run already rendered
    double m_outFraction = 0.0;   // the fraction of an output frame carried between blocks

    float TonalityLimit() const {
        return g_ssTonalityLimit > 0 ? static_cast<float>(g_ssTonalityLimit) / m_sampleRate : 0.0f;
    }

    void Deinterleave(const float* in, int frames) {
        for (int ch = 0; ch < kChannels; ch++) {
            m_channelIn[ch].resize(frames);
            float* dst = m_channelIn[ch].data();
            for (int i = 0; i < frames; i++) dst[i] = in[static_cast<size_t>(i) * kChannels + ch];
            m_inPtrs[ch] = dst;
        }
    }

    void PrepareOutput(int frames) {
        for (int ch = 0; ch < kChannels; ch++) {
            m_channelOut[ch].resize(frames > 0 ? frames : 1);
            m_outPtrs[ch] = m_channelOut[ch].data();
        }
    }

    void EmitOutput(int frames, double srcEnd) {
        m_interleaved.resize(static_cast<size_t>(frames) * kChannels);
        for (int ch = 0; ch < kChannels; ch++) {
            const float* src = m_channelOut[ch].data();
            for (int i = 0; i < frames; i++) m_interleaved[static_cast<size_t>(i) * kChannels + ch] = src[i];
        }
        Emit(m_interleaved.data(), frames, srcEnd);
    }

    double RealTime(uint64_t engineFrames) const {
        // Pre-roll padding past the end of a short file is not real audio.
        return RunTime(static_cast<double>(std::min(engineFrames, m_runFrames)));
    }
};

#endif  // USE_SIGNALSMITH

// Only the direct path and resampling (no stretcher built in).
class DirectProcessor : public ProcessorBase {
public:
    using ProcessorBase::ProcessorBase;

protected:
    bool EngineCreate() override { return false; }
    bool EngineStart(PcmSource&) override { return true; }
    void EngineFeed(int) override {}
    void EngineDrain() override {}
};

}  // namespace

std::unique_ptr<TempoProcessor> TempoProcessor::Create(TempoAlgorithm algorithm, int sourceRate, int outputRate) {
    switch (algorithm) {
#ifdef USE_SPEEDY
        case TempoAlgorithm::Speedy:
            return std::make_unique<SpeedyProcessor>(sourceRate, outputRate);
#endif
#ifdef USE_SIGNALSMITH
        case TempoAlgorithm::Signalsmith:
            return std::make_unique<SignalsmithProcessor>(sourceRate, outputRate);
#endif
        default:
            return std::make_unique<DirectProcessor>(sourceRate, outputRate);
    }
}

}  // namespace audio
