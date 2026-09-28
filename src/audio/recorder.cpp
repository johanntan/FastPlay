// Recording: see recorder.h.

#include "recorder.h"
#include "utils.h"

#include <lame.h>
#include <vorbis/vorbisenc.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <thread>
#include <vector>

namespace audio {
namespace {

const int kChannels = 2;

// The queue and the encoding thread; each format supplies Encode() and Finish().
class ThreadedRecorder : public Recorder {
public:
    void Write(const float* samples, int frames) override {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_queue.insert(m_queue.end(), samples, samples + static_cast<size_t>(frames) * kChannels);
        }
        m_wake.notify_one();
    }

protected:
    void Run() { m_thread = std::thread([this] { Loop(); }); }

    // Stops the thread after it has encoded everything; then Finish().
    void Close() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop = true;
        }
        m_wake.notify_one();
        if (m_thread.joinable()) m_thread.join();
        Finish();
    }

    virtual void Encode(const float* samples, int frames) = 0;
    virtual void Finish() = 0;

private:
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::vector<float> m_queue;
    bool m_stop = false;
    std::thread m_thread;

    void Loop() {
        std::vector<float> work;
        while (true) {
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_wake.wait(lock, [this] { return m_stop || !m_queue.empty(); });
                if (m_queue.empty() && m_stop) return;
                work.swap(m_queue);
                m_queue.clear();
            }
            Encode(work.data(), static_cast<int>(work.size() / kChannels));
        }
    }
};

int16_t ToPcm16(float v) {
    v = std::clamp(v, -1.0f, 1.0f);
    return static_cast<int16_t>(std::lround(v * 32767.0f));
}

// ---------------------------------------------------------------------------
// WAV: 16-bit PCM, the sizes filled in at the end
// ---------------------------------------------------------------------------

class WavRecorder : public ThreadedRecorder {
public:
    ~WavRecorder() override { Close(); }

    bool Open(const std::wstring& path, int sampleRate, std::wstring& error) {
        m_file = FileOpen(path, "wb");
        if (!m_file) {
            error = L"The file could not be created: " + path;
            return false;
        }
        m_rate = sampleRate;
        WriteHeader(0);
        Run();
        return true;
    }

protected:
    void Encode(const float* samples, int frames) override {
        m_pcm.resize(static_cast<size_t>(frames) * kChannels);
        for (size_t i = 0; i < m_pcm.size(); i++) m_pcm[i] = ToPcm16(samples[i]);
        m_bytes += fwrite(m_pcm.data(), sizeof(int16_t), m_pcm.size(), m_file) * sizeof(int16_t);
    }

    void Finish() override {
        if (!m_file) return;
        fseek(m_file, 0, SEEK_SET);
        WriteHeader(m_bytes);
        fclose(m_file);
        m_file = nullptr;
    }

private:
    FILE* m_file = nullptr;
    int m_rate = 0;
    uint64_t m_bytes = 0;
    std::vector<int16_t> m_pcm;

    void WriteHeader(uint64_t dataBytes) {
        // RIFF sizes are 32-bit: a longer recording says the most they can
        uint32_t data = static_cast<uint32_t>(std::min<uint64_t>(dataBytes, 0xFFFFFFFFull - 36));
        uint8_t header[44];
        auto put32 = [&](int at, uint32_t v) {
            for (int i = 0; i < 4; i++) header[at + i] = static_cast<uint8_t>(v >> (8 * i));
        };
        auto put16 = [&](int at, uint16_t v) {
            header[at] = static_cast<uint8_t>(v);
            header[at + 1] = static_cast<uint8_t>(v >> 8);
        };
        std::memcpy(header, "RIFF", 4);
        put32(4, data + 36);
        std::memcpy(header + 8, "WAVEfmt ", 8);
        put32(16, 16);
        put16(20, 1);  // PCM
        put16(22, kChannels);
        put32(24, static_cast<uint32_t>(m_rate));
        put32(28, static_cast<uint32_t>(m_rate * kChannels * 2));
        put16(32, kChannels * 2);
        put16(34, 16);
        std::memcpy(header + 36, "data", 4);
        put32(40, data);
        fwrite(header, 1, sizeof(header), m_file);
    }
};

// ---------------------------------------------------------------------------
// MP3: LAME, constant bitrate
// ---------------------------------------------------------------------------

class Mp3Recorder : public ThreadedRecorder {
public:
    ~Mp3Recorder() override {
        Close();
        if (m_lame) lame_close(m_lame);
    }

    bool Open(const std::wstring& path, int bitrate, int sampleRate, std::wstring& error) {
        m_lame = lame_init();
        if (!m_lame) {
            error = L"The MP3 encoder could not be started.";
            return false;
        }
        lame_set_in_samplerate(m_lame, sampleRate);
        lame_set_num_channels(m_lame, kChannels);
        lame_set_VBR(m_lame, vbr_off);
        lame_set_brate(m_lame, std::clamp(bitrate, 32, 320));
        lame_set_quality(m_lame, 2);
        if (lame_init_params(m_lame) < 0) {
            error = L"The MP3 encoder does not take these settings.";
            return false;
        }
        m_file = FileOpen(path, "wb");
        if (!m_file) {
            error = L"The file could not be created: " + path;
            return false;
        }
        Run();
        return true;
    }

protected:
    void Encode(const float* samples, int frames) override {
        m_out.resize(static_cast<size_t>(frames) * 5 / 4 + 7200);
        int bytes = lame_encode_buffer_interleaved_ieee_float(m_lame, samples, frames, m_out.data(),
                                                              static_cast<int>(m_out.size()));
        if (bytes > 0) fwrite(m_out.data(), 1, static_cast<size_t>(bytes), m_file);
    }

    void Finish() override {
        if (!m_file) return;
        m_out.resize(7200);
        int bytes = lame_encode_flush(m_lame, m_out.data(), static_cast<int>(m_out.size()));
        if (bytes > 0) fwrite(m_out.data(), 1, static_cast<size_t>(bytes), m_file);
        // The Info frame at the start: the length, for players to show
        unsigned char tag[2880];
        size_t tagBytes = lame_get_lametag_frame(m_lame, tag, sizeof(tag));
        if (tagBytes > 0 && tagBytes <= sizeof(tag) && fseek(m_file, 0, SEEK_SET) == 0) {
            fwrite(tag, 1, tagBytes, m_file);
        }
        fclose(m_file);
        m_file = nullptr;
    }

private:
    lame_t m_lame = nullptr;
    FILE* m_file = nullptr;
    std::vector<unsigned char> m_out;
};

// ---------------------------------------------------------------------------
// OGG Vorbis: libvorbis at a nominal bitrate
// ---------------------------------------------------------------------------

class OggRecorder : public ThreadedRecorder {
public:
    ~OggRecorder() override {
        Close();
        if (m_started) {
            ogg_stream_clear(&m_stream);
            vorbis_block_clear(&m_block);
            vorbis_dsp_clear(&m_dsp);
            vorbis_comment_clear(&m_comment);
        }
        vorbis_info_clear(&m_info);
    }

    bool Open(const std::wstring& path, int bitrate, int sampleRate, std::wstring& error) {
        vorbis_info_init(&m_info);
        if (vorbis_encode_init(&m_info, kChannels, sampleRate, -1, std::clamp(bitrate, 32, 500) * 1000, -1) != 0) {
            error = L"The OGG encoder does not take these settings.";
            return false;
        }
        m_file = FileOpen(path, "wb");
        if (!m_file) {
            error = L"The file could not be created: " + path;
            return false;
        }
        vorbis_comment_init(&m_comment);
        vorbis_comment_add_tag(&m_comment, "ENCODER", "FastPlay");
        vorbis_analysis_init(&m_dsp, &m_info);
        vorbis_block_init(&m_dsp, &m_block);
        ogg_stream_init(&m_stream, static_cast<int>(time(nullptr) & 0x7FFFFFFF));
        m_started = true;

        ogg_packet header, comments, codebooks;
        vorbis_analysis_headerout(&m_dsp, &m_comment, &header, &comments, &codebooks);
        ogg_stream_packetin(&m_stream, &header);
        ogg_stream_packetin(&m_stream, &comments);
        ogg_stream_packetin(&m_stream, &codebooks);
        ogg_page page;
        while (ogg_stream_flush(&m_stream, &page) != 0) WritePage(page);
        Run();
        return true;
    }

protected:
    void Encode(const float* samples, int frames) override {
        if (frames <= 0) return;
        float** buffer = vorbis_analysis_buffer(&m_dsp, frames);
        for (int i = 0; i < frames; i++) {
            buffer[0][i] = samples[i * 2];
            buffer[1][i] = samples[i * 2 + 1];
        }
        vorbis_analysis_wrote(&m_dsp, frames);
        Drain();
    }

    void Finish() override {
        if (!m_file) return;
        vorbis_analysis_wrote(&m_dsp, 0);  // the end
        Drain();
        ogg_page page;
        while (ogg_stream_flush(&m_stream, &page) != 0) WritePage(page);  // the last page
        fclose(m_file);
        m_file = nullptr;
    }

private:
    FILE* m_file = nullptr;
    bool m_started = false;
    vorbis_info m_info;
    vorbis_comment m_comment;
    vorbis_dsp_state m_dsp;
    vorbis_block m_block;
    ogg_stream_state m_stream;

    void WritePage(const ogg_page& page) {
        fwrite(page.header, 1, static_cast<size_t>(page.header_len), m_file);
        fwrite(page.body, 1, static_cast<size_t>(page.body_len), m_file);
    }

    void Drain() {
        ogg_packet packet;
        ogg_page page;
        while (vorbis_analysis_blockout(&m_dsp, &m_block) == 1) {
            vorbis_analysis(&m_block, nullptr);
            vorbis_bitrate_addblock(&m_block);
            while (vorbis_bitrate_flushpacket(&m_dsp, &packet)) {
                ogg_stream_packetin(&m_stream, &packet);
                while (ogg_stream_pageout(&m_stream, &page) != 0) WritePage(page);
            }
        }
    }
};

// ---------------------------------------------------------------------------
// FLAC: FFmpeg's encoder and muxer, 16-bit
// ---------------------------------------------------------------------------

class FlacRecorder : public ThreadedRecorder {
public:
    ~FlacRecorder() override {
        Close();
        av_frame_free(&m_frame);
        av_packet_free(&m_packet);
        avcodec_free_context(&m_codec);
        if (m_format) {
            if (m_format->pb) avio_closep(&m_format->pb);
            avformat_free_context(m_format);
        }
    }

    bool Open(const std::wstring& path, int sampleRate, std::wstring& error) {
        std::string url = WideToUtf8(path);
        error = L"The FLAC encoder could not be started.";
        if (avformat_alloc_output_context2(&m_format, nullptr, "flac", url.c_str()) < 0 || !m_format) return false;
        const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_FLAC);
        AVStream* stream = codec ? avformat_new_stream(m_format, nullptr) : nullptr;
        m_codec = codec ? avcodec_alloc_context3(codec) : nullptr;
        if (!stream || !m_codec) return false;
        m_codec->sample_rate = sampleRate;
        m_codec->sample_fmt = AV_SAMPLE_FMT_S16;
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        av_channel_layout_copy(&m_codec->ch_layout, &stereo);
        m_codec->time_base = AVRational{1, sampleRate};
        if (avcodec_open2(m_codec, codec, nullptr) < 0) return false;
        if (avcodec_parameters_from_context(stream->codecpar, m_codec) < 0) return false;
        stream->time_base = m_codec->time_base;
        if (avio_open(&m_format->pb, url.c_str(), AVIO_FLAG_WRITE) < 0) {
            error = L"The file could not be created: " + path;
            return false;
        }
        if (avformat_write_header(m_format, nullptr) < 0) return false;
        m_frameSize = m_codec->frame_size > 0 ? m_codec->frame_size : 4608;
        m_frame = av_frame_alloc();
        m_packet = av_packet_alloc();
        if (!m_frame || !m_packet) return false;
        error.clear();
        Run();
        return true;
    }

protected:
    void Encode(const float* samples, int frames) override {
        for (int i = 0; i < frames * kChannels; i++) m_pending.push_back(ToPcm16(samples[i]));
        while (m_pending.size() >= static_cast<size_t>(m_frameSize) * kChannels) SendFrame(m_frameSize);
    }

    void Finish() override {
        if (!m_format || !m_format->pb) return;
        if (!m_pending.empty()) SendFrame(static_cast<int>(m_pending.size() / kChannels));
        avcodec_send_frame(m_codec, nullptr);
        Receive();
        av_write_trailer(m_format);  // goes back to fill in the length and checksum
    }

private:
    AVFormatContext* m_format = nullptr;
    AVCodecContext* m_codec = nullptr;
    AVFrame* m_frame = nullptr;
    AVPacket* m_packet = nullptr;
    int m_frameSize = 4608;
    int64_t m_pts = 0;
    std::vector<int16_t> m_pending;

    void SendFrame(int frames) {
        av_frame_unref(m_frame);
        m_frame->nb_samples = frames;
        m_frame->format = AV_SAMPLE_FMT_S16;
        m_frame->sample_rate = m_codec->sample_rate;
        av_channel_layout_copy(&m_frame->ch_layout, &m_codec->ch_layout);
        if (av_frame_get_buffer(m_frame, 0) < 0) return;
        std::memcpy(m_frame->data[0], m_pending.data(), static_cast<size_t>(frames) * kChannels * sizeof(int16_t));
        m_pending.erase(m_pending.begin(), m_pending.begin() + static_cast<size_t>(frames) * kChannels);
        m_frame->pts = m_pts;
        m_pts += frames;
        if (avcodec_send_frame(m_codec, m_frame) >= 0) Receive();
    }

    void Receive() {
        while (avcodec_receive_packet(m_codec, m_packet) == 0) {
            av_packet_rescale_ts(m_packet, m_codec->time_base, m_format->streams[0]->time_base);
            m_packet->stream_index = 0;
            av_interleaved_write_frame(m_format, m_packet);
        }
    }
};

}  // namespace

std::unique_ptr<Recorder> Recorder::Start(const std::wstring& path, RecordFormat format, int bitrateKbps,
                                          int sampleRate, std::wstring& error) {
    switch (format) {
        case RecordFormat::Mp3: {
            auto r = std::make_unique<Mp3Recorder>();
            if (r->Open(path, bitrateKbps, sampleRate, error)) return r;
            return nullptr;
        }
        case RecordFormat::Ogg: {
            auto r = std::make_unique<OggRecorder>();
            if (r->Open(path, bitrateKbps, sampleRate, error)) return r;
            return nullptr;
        }
        case RecordFormat::Flac: {
            auto r = std::make_unique<FlacRecorder>();
            if (r->Open(path, sampleRate, error)) return r;
            return nullptr;
        }
        default: {
            auto r = std::make_unique<WavRecorder>();
            if (r->Open(path, sampleRate, error)) return r;
            return nullptr;
        }
    }
}

}  // namespace audio
