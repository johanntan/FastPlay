// The audio engine: see audio.h.
//
// Threads and what they own:
//   - decode thread: the decoder. Reads ahead into m_pcm (stereo float at the
//     source's rate) and carries out seeks.
//   - mix thread: the tempo processor, the effects and the tap. Fills the output
//     ring (stereo float at the device's rate) while it has room.
//   - device callback (miniaudio): plays the output ring, counts the frames
//     played (the position), applies the gain, and notices the end.
// A seek stops the mix thread at a safe point (m_mixMutex), has the decode thread
// reposition the decoder, restarts the tempo processor and empties the ring.

#include "audio.h"
#include "audio_internal.h"
#include "app_ui.h"
#include "tempo_processor.h"
#include "utils.h"

#include "miniaudio.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace audio {
namespace {

const int kChannels = 2;
const int kMixBlockFrames = 512;
// Decoded audio read ahead: a little for files, several seconds for streams
const double kFileReadAhead = 2.0;
const double kStreamReadAhead = 10.0;
// A stream starts (and restarts after running dry) once this much is buffered
const double kStreamPrebuffer = 2.0;

// ---------------------------------------------------------------------------
// The decoded audio buffered between the decode and mix threads
// ---------------------------------------------------------------------------

class PcmBuffer : public PcmSource {
public:
    void Reset(size_t capacityFrames) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_data.assign(capacityFrames * kChannels, 0.0f);
        m_read = m_write = m_count = 0;
        m_ended = false;
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_read = m_write = m_count = 0;
        m_ended = false;
    }

    size_t Space() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_data.size() / kChannels - m_count;
    }

    void Write(const float* in, size_t frames) {
        std::lock_guard<std::mutex> lock(m_mutex);
        size_t capacity = m_data.size() / kChannels;
        frames = std::min(frames, capacity - m_count);
        for (size_t i = 0; i < frames; i++) {
            m_data[m_write * kChannels] = in[i * kChannels];
            m_data[m_write * kChannels + 1] = in[i * kChannels + 1];
            m_write = (m_write + 1) % capacity;
        }
        m_count += frames;
    }

    void SetEnded() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_ended = true;
    }

    int Available() override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return static_cast<int>(m_count);
    }

    int Read(float* out, int frames) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        size_t capacity = m_data.size() / kChannels;
        size_t n = std::min<size_t>(static_cast<size_t>(frames), m_count);
        for (size_t i = 0; i < n; i++) {
            out[i * kChannels] = m_data[m_read * kChannels];
            out[i * kChannels + 1] = m_data[m_read * kChannels + 1];
            m_read = (m_read + 1) % capacity;
        }
        m_count -= n;
        return static_cast<int>(n);
    }

    bool Ended() override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_ended && m_count == 0;
    }

    bool DecoderEnded() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_ended;
    }

private:
    std::mutex m_mutex;
    std::vector<float> m_data;
    size_t m_read = 0, m_write = 0, m_count = 0;
    bool m_ended = false;
};

struct Dsp {
    int id;
    DspProc proc;
    void* user;
    int priority;
};

// ---------------------------------------------------------------------------
// Engine state
// ---------------------------------------------------------------------------

struct Engine {
    // Device
    ma_context context;
    bool contextReady = false;
    ma_device device;
    bool deviceReady = false;
    std::wstring deviceName;
    bool defaultDevice = true;
    int outputRate = 48000;
    size_t ringFrames = 0;

    // The output ring: written by the mix thread, read by the device callback.
    // m_ringMutex is only ever tried by the callback, so it never waits.
    ma_pcm_rb ring;
    bool ringReady = false;
    std::mutex ringMutex;

    // What is loaded
    std::unique_ptr<Decoder> decoder;
    std::unique_ptr<TempoProcessor> processor;
    PcmBuffer pcm;
    std::atomic<bool> loaded{false};
    std::atomic<State> state{State::Empty};
    std::atomic<bool> live{false};
    double length = 0;
    size_t prebufferFrames = 0;

    // Threads
    std::thread decodeThread, mixThread;
    std::atomic<bool> running{false};
    std::mutex decodeMutex;
    std::condition_variable decodeWake;
    std::mutex decoderMutex;       // held while the decoder is in use, and to replace it
    bool seekRequested = false;
    double seekTarget = 0;
    bool seekDone = false;
    bool seekOk = false;
    std::condition_variable seekFinished;
    std::mutex mixMutex;           // held by the mix thread while it works
    std::atomic<bool> mixWaiting{true};  // a stream waiting for its prebuffer
    std::atomic<bool> producerEnded{false};

    // Position: frames played from the ring since the last seek
    std::atomic<uint64_t> played{0};
    std::atomic<bool> endReported{false};

    // Gain, applied in the callback (ramped from block to block)
    std::atomic<float> gain{1.0f};
    float appliedGain = 1.0f;

    // Tempo settings, kept for the processor
    float tempo = 0, pitch = 0, rate = 1;

    // Effects chain and tap
    std::mutex dspMutex;
    std::vector<Dsp> dsps;
    int nextDspId = 1;
    TapProc tap = nullptr;
    void* tapUser = nullptr;
    std::vector<float> mixBlock;

    // UI handlers
    std::function<void()> endHandler;
    std::function<void()> titleHandler;
};

Engine g;

// ---------------------------------------------------------------------------
// Device callback
// ---------------------------------------------------------------------------

void DataCallback(ma_device*, void* output, const void*, ma_uint32 frameCount) {
    float* out = static_cast<float*>(output);
    std::memset(out, 0, static_cast<size_t>(frameCount) * kChannels * sizeof(float));
    if (g.state.load() != State::Playing) return;
    std::unique_lock<std::mutex> lock(g.ringMutex, std::try_to_lock);
    if (!lock.owns_lock() || !g.ringReady) return;  // being emptied for a seek

    ma_uint32 done = 0;
    while (done < frameCount) {
        ma_uint32 frames = frameCount - done;
        void* buffer = nullptr;
        if (ma_pcm_rb_acquire_read(&g.ring, &frames, &buffer) != MA_SUCCESS || frames == 0) break;
        std::memcpy(out + static_cast<size_t>(done) * kChannels, buffer,
                    static_cast<size_t>(frames) * kChannels * sizeof(float));
        ma_pcm_rb_commit_read(&g.ring, frames);
        done += frames;
    }
    g.played += done;

    // The gain, ramped across the block so a change never clicks
    float target = g.gain.load();
    float start = g.appliedGain;
    if (done > 0) {
        float step = (target - start) / static_cast<float>(done);
        for (ma_uint32 i = 0; i < done; i++) {
            float gain = start + step * static_cast<float>(i + 1);
            out[i * 2] *= gain;
            out[i * 2 + 1] *= gain;
        }
    }
    g.appliedGain = target;

    // The end: everything produced and played
    if (done < frameCount && g.producerEnded.load() && !g.endReported.exchange(true)) {
        RunOnUiThread([]() {
            if (g.endHandler) g.endHandler();
        });
    }
}

// ---------------------------------------------------------------------------
// Decode thread
// ---------------------------------------------------------------------------

void DecodeLoop() {
    std::vector<float> block(4096 * kChannels);
    while (g.running) {
        {
            std::unique_lock<std::mutex> lock(g.decodeMutex);
            if (g.seekRequested) {
                g.seekRequested = false;
                double target = g.seekTarget;
                lock.unlock();
                bool ok;
                {
                    std::lock_guard<std::mutex> use(g.decoderMutex);
                    ok = g.decoder && g.decoder->Seek(target);
                }
                g.pcm.Clear();
                lock.lock();
                g.seekOk = ok;
                g.seekDone = true;
                g.seekFinished.notify_all();
                continue;
            }
            if (!g.decoder || g.pcm.DecoderEnded() || g.pcm.Space() < 4096) {
                g.decodeWake.wait_for(lock, std::chrono::milliseconds(20));
                continue;
            }
        }
        bool titleChanged;
        {
            // Written while the decoder is still held, so a block read from a
            // decoder being unloaded never lands in the next one's buffer.
            std::lock_guard<std::mutex> use(g.decoderMutex);
            if (!g.decoder) continue;
            int got = g.decoder->Read(block.data(), 4096);
            if (got > 0) {
                g.pcm.Write(block.data(), static_cast<size_t>(got));
            } else {
                g.pcm.SetEnded();
            }
            titleChanged = TakeStreamTitleChange(g.decoder.get());
        }
        if (titleChanged) {
            RunOnUiThread([]() {
                if (g.titleHandler) g.titleHandler();
            });
        }
    }
}

// ---------------------------------------------------------------------------
// Mix thread
// ---------------------------------------------------------------------------

void RunDsps(float* samples, int frames) {
    std::lock_guard<std::mutex> lock(g.dspMutex);
    for (const Dsp& dsp : g.dsps) dsp.proc(samples, frames, kChannels, g.outputRate, dsp.user);
    if (g.tap) g.tap(samples, frames, kChannels, g.outputRate, g.tapUser);
}

void MixLoop() {
    g.mixBlock.resize(static_cast<size_t>(kMixBlockFrames) * kChannels);
    while (g.running) {
        bool idle = true;
        {
            std::lock_guard<std::mutex> lock(g.mixMutex);
            if (g.processor && g.ringReady && !g.producerEnded) {
                // A stream fills its buffer before it plays (and after it runs dry).
                if (g.mixWaiting) {
                    if (g.pcm.Available() >= static_cast<int>(g.prebufferFrames) || g.pcm.DecoderEnded()) {
                        g.mixWaiting = false;
                    }
                }
                ma_uint32 space = ma_pcm_rb_available_write(&g.ring);
                if (!g.mixWaiting && space >= static_cast<ma_uint32>(kMixBlockFrames)) {
                    bool ended = false;
                    int frames = g.processor->Fill(g.pcm, g.mixBlock.data(), kMixBlockFrames, ended);
                    if (frames > 0) {
                        RunDsps(g.mixBlock.data(), frames);
                        ma_uint32 left = static_cast<ma_uint32>(frames);
                        const float* src = g.mixBlock.data();
                        while (left > 0) {
                            ma_uint32 n = left;
                            void* buffer = nullptr;
                            if (ma_pcm_rb_acquire_write(&g.ring, &n, &buffer) != MA_SUCCESS || n == 0) break;
                            std::memcpy(buffer, src, static_cast<size_t>(n) * kChannels * sizeof(float));
                            ma_pcm_rb_commit_write(&g.ring, n);
                            src += static_cast<size_t>(n) * kChannels;
                            left -= n;
                        }
                        idle = false;
                    }
                    if (ended) {
                        g.producerEnded = true;
                    } else if (frames < kMixBlockFrames && g.live && !g.pcm.DecoderEnded() &&
                               ma_pcm_rb_available_read(&g.ring) == 0) {
                        g.mixWaiting = true;  // ran dry: buffer again
                    }
                }
            }
        }
        g.decodeWake.notify_one();
        if (idle) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

// ---------------------------------------------------------------------------
// Device
// ---------------------------------------------------------------------------

bool FindDevice(const std::wstring& name, ma_device_id& id) {
    if (name.empty()) return false;
    ma_device_info* devices = nullptr;
    ma_uint32 count = 0;
    if (ma_context_get_devices(&g.context, &devices, &count, nullptr, nullptr) != MA_SUCCESS) return false;
    for (ma_uint32 i = 0; i < count; i++) {
        if (Utf8ToWide(devices[i].name) == name) {
            id = devices[i].id;
            return true;
        }
    }
    return false;
}

void CloseDevice() {
    if (g.deviceReady) {
        ma_device_uninit(&g.device);
        g.deviceReady = false;
    }
    std::lock_guard<std::mutex> lock(g.ringMutex);
    if (g.ringReady) {
        ma_pcm_rb_uninit(&g.ring);
        g.ringReady = false;
    }
}

bool OpenDevice(const std::wstring& name, int bufferMs) {
    ma_device_id id;
    bool found = FindDevice(name, id);
    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.pDeviceID = found ? &id : nullptr;
    config.playback.format = ma_format_f32;
    config.playback.channels = kChannels;
    config.sampleRate = 0;  // the device's own
    config.dataCallback = DataCallback;
    config.performanceProfile = ma_performance_profile_conservative;
    if (ma_device_init(&g.context, &config, &g.device) != MA_SUCCESS) {
        if (!found) return false;
        config.playback.pDeviceID = nullptr;  // fall back to the default
        found = false;
        if (ma_device_init(&g.context, &config, &g.device) != MA_SUCCESS) return false;
    }
    g.deviceReady = true;
    g.defaultDevice = !found;
    g.deviceName = Utf8ToWide(g.device.playback.name);
    g.outputRate = static_cast<int>(g.device.sampleRate);

    g.ringFrames = static_cast<size_t>(g.outputRate) * static_cast<size_t>(std::clamp(bufferMs, 50, 5000)) / 1000;
    {
        std::lock_guard<std::mutex> lock(g.ringMutex);
        if (ma_pcm_rb_init(ma_format_f32, kChannels, static_cast<ma_uint32>(g.ringFrames), nullptr, nullptr, &g.ring) !=
            MA_SUCCESS) {
            ma_device_uninit(&g.device);
            g.deviceReady = false;
            return false;
        }
        g.ringReady = true;
    }
    return ma_device_start(&g.device) == MA_SUCCESS;
}

// Empties the output ring and starts counting played frames again. The mix
// thread must be held (m_mixMutex).
void ResetOutput() {
    std::lock_guard<std::mutex> lock(g.ringMutex);
    if (g.ringReady) ma_pcm_rb_reset(&g.ring);
    g.played = 0;
    g.producerEnded = false;
    g.endReported = false;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------

std::vector<Device> ListDevices() {
    std::vector<Device> list;
    if (!g.contextReady) return list;
    ma_device_info* devices = nullptr;
    ma_uint32 count = 0;
    if (ma_context_get_devices(&g.context, &devices, &count, nullptr, nullptr) != MA_SUCCESS) return list;
    for (ma_uint32 i = 0; i < count; i++) {
        Device device;
        device.name = Utf8ToWide(devices[i].name);
        device.isDefault = devices[i].isDefault != 0;
        list.push_back(device);
    }
    return list;
}

bool Init(const std::wstring& deviceName, int bufferMs) {
    if (!g.contextReady) {
        if (ma_context_init(nullptr, 0, nullptr, &g.context) != MA_SUCCESS) return false;
        g.contextReady = true;
    }
    if (!OpenDevice(deviceName, bufferMs)) return false;
    if (!g.running) {
        g.running = true;
        g.decodeThread = std::thread(DecodeLoop);
        g.mixThread = std::thread(MixLoop);
    }
    return true;
}

void Shutdown() {
    Unload();
    if (g.running) {
        g.running = false;
        g.decodeWake.notify_all();
        if (g.decodeThread.joinable()) g.decodeThread.join();
        if (g.mixThread.joinable()) g.mixThread.join();
    }
    CloseDevice();
    if (g.contextReady) {
        ma_context_uninit(&g.context);
        g.contextReady = false;
    }
}

bool SwitchDevice(const std::wstring& deviceName, int bufferMs) {
    if (g.loaded) return false;
    std::lock_guard<std::mutex> mix(g.mixMutex);
    CloseDevice();
    return OpenDevice(deviceName, bufferMs);
}

std::wstring CurrentDeviceName() { return g.deviceName; }
bool UsingDefaultDevice() { return g.defaultDevice; }
int MixSampleRate() { return g.outputRate; }

bool Load(std::unique_ptr<Decoder> decoder, TempoAlgorithm algorithm) {
    Unload();
    if (!decoder || !g.deviceReady) return false;

    std::unique_lock<std::mutex> mix(g.mixMutex);
    std::lock_guard<std::mutex> decode(g.decodeMutex);
    g.live = decoder->IsLive();
    g.length = decoder->Length();
    const int rate = decoder->SampleRate();
    g.pcm.Reset(static_cast<size_t>(rate * (g.live ? kStreamReadAhead : kFileReadAhead)));
    g.prebufferFrames = g.live ? static_cast<size_t>(rate * kStreamPrebuffer) : 0;
    g.mixWaiting = g.live.load();
    g.processor = TempoProcessor::Create(algorithm, rate, g.outputRate);
    g.processor->SetTempo(g.live ? 0.0f : g.tempo);
    g.processor->SetPitch(g.pitch);
    g.processor->SetRate(g.live ? 1.0f : g.rate);
    g.processor->Restart(0.0);
    {
        std::lock_guard<std::mutex> use(g.decoderMutex);
        g.decoder = std::move(decoder);
    }
    g.seekRequested = false;
    ResetOutput();
    g.state = State::Paused;
    g.loaded = true;
    g.decodeWake.notify_one();
    return true;
}

void Unload() {
    if (!g.loaded) return;
    g.state = State::Empty;
    // A decoder blocked on the network is asked to give up, then the threads are
    // held while it goes.
    if (g.decoder) g.decoder->Abort();
    std::unique_lock<std::mutex> mix(g.mixMutex);
    std::lock_guard<std::mutex> decode(g.decodeMutex);
    g.loaded = false;
    g.processor.reset();
    {
        std::lock_guard<std::mutex> use(g.decoderMutex);
        g.decoder.reset();
    }
    g.pcm.Clear();
    ResetOutput();
    g.live = false;
    g.length = 0;
}

bool IsLoaded() { return g.loaded; }
const Decoder* Current() { return g.loaded ? g.decoder.get() : nullptr; }

void Play() {
    if (!g.loaded) return;
    g.state = State::Playing;
}

void Pause() {
    if (!g.loaded) return;
    g.state = State::Paused;
}

void Stop() {
    if (!g.loaded) return;
    g.state = State::Stopped;
}

State GetState() { return g.state.load(); }

double Position() {
    if (!g.loaded) return 0.0;
    std::lock_guard<std::mutex> mix(g.mixMutex);
    return g.processor ? g.processor->PositionAt(g.played.load()) : 0.0;
}

double Length() { return g.loaded ? g.length : 0.0; }
bool IsLive() { return g.loaded && g.live; }

bool Seek(double seconds) {
    if (!g.loaded || g.live) return false;
    if (g.length > 0) seconds = std::min(seconds, g.length);
    if (seconds < 0) seconds = 0;
    std::unique_lock<std::mutex> mix(g.mixMutex);
    bool ok;
    {
        std::unique_lock<std::mutex> lock(g.decodeMutex);
        g.seekTarget = seconds;
        g.seekRequested = true;
        g.seekDone = false;
        g.decodeWake.notify_all();
        if (!g.seekFinished.wait_for(lock, std::chrono::seconds(30), [] { return g.seekDone; })) return false;
        ok = g.seekOk;
    }
    if (g.processor) g.processor->Restart(seconds);
    ResetOutput();
    g.decodeWake.notify_all();
    return ok;
}

void SetTempo(float percent) {
    g.tempo = percent;
    std::lock_guard<std::mutex> mix(g.mixMutex);
    if (g.processor && !g.live) g.processor->SetTempo(percent);
}

void SetPitch(float semitones) {
    g.pitch = semitones;
    std::lock_guard<std::mutex> mix(g.mixMutex);
    if (g.processor) g.processor->SetPitch(semitones);
}

void SetRate(float rate) {
    g.rate = rate;
    std::lock_guard<std::mutex> mix(g.mixMutex);
    if (g.processor && !g.live) g.processor->SetRate(rate);
}

void SetGain(float linear) { g.gain = linear; }

int AddDsp(DspProc proc, void* user, int priority) {
    std::lock_guard<std::mutex> lock(g.dspMutex);
    Dsp dsp{g.nextDspId++, proc, user, priority};
    // After any of the same priority already there, as BASS did
    auto at = std::find_if(g.dsps.begin(), g.dsps.end(), [&](const Dsp& d) { return d.priority < priority; });
    g.dsps.insert(at, dsp);
    return dsp.id;
}

void RemoveDsp(int id) {
    std::lock_guard<std::mutex> lock(g.dspMutex);
    g.dsps.erase(std::remove_if(g.dsps.begin(), g.dsps.end(), [&](const Dsp& d) { return d.id == id; }),
                 g.dsps.end());
}

void SetTap(TapProc proc, void* user) {
    std::lock_guard<std::mutex> lock(g.dspMutex);
    g.tap = proc;
    g.tapUser = user;
}

void SetEndHandler(std::function<void()> handler) { g.endHandler = std::move(handler); }
void SetStreamTitleHandler(std::function<void()> handler) { g.titleHandler = std::move(handler); }

}  // namespace audio
