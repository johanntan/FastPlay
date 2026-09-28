// Decoding through FFmpeg: every file format FastPlay plays except xHE-AAC, and
// internet streams (HTTP with Shoutcast/Icecast titles, HTTPS, HLS).
//
// The decoded audio is converted to interleaved float stereo at the source's
// own rate: a mono source is copied to both sides, more channels are mixed down.
// Seeking is exact to the sample: FFmpeg seeks to a packet at or before the time,
// and the audio before it is decoded and dropped.

#include "audio.h"
#include "audio_internal.h"
#include "http.h"
#include "utils.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/version.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>

namespace audio {
namespace {

// FFmpeg would log to stderr, which a windowed program has no use for
void QuietLogging() {
    static const bool quiet = (av_log_set_level(AV_LOG_QUIET), true);
    (void)quiet;
}

// Seconds a network read may stall before the stream is given up
const int kNetworkTimeoutSeconds = 20;

std::string Upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    return s;
}

// FFmpeg's names for the common tags FastPlay asks for
const char* FfmpegKey(const std::string& name) {
    if (name == "TRACKNUMBER" || name == "TRACK") return "TRACK";
    if (name == "YEAR" || name == "DATE") return "DATE";
    return nullptr;
}

// "StreamTitle='Artist - Title';StreamUrl='';" -> "Artist - Title". The value ends
// at "';" rather than the first quote: titles have apostrophes ("Don't Stop").
std::string IcyField(const std::string& meta, const char* key) {
    std::string search = std::string(key) + "='";
    size_t start = meta.find(search);
    if (start == std::string::npos) return "";
    start += search.size();
    size_t end = meta.find("';", start);
    if (end == std::string::npos) {
        end = meta.size();
        while (end > start && (meta[end - 1] == '\0' || meta[end - 1] == ';' || meta[end - 1] == ' ')) end--;
        if (end > start && meta[end - 1] == '\'') end--;
    }
    return meta.substr(start, end - start);
}

class FfmpegDecoder : public Decoder {
public:
    ~FfmpegDecoder() override {
        swr_free(&m_swr);
        av_frame_free(&m_frame);
        av_packet_free(&m_packet);
        avcodec_free_context(&m_codec);
        avformat_close_input(&m_format);
    }

    bool Open(const std::wstring& pathOrUrl, std::wstring& error) {
        const bool network = IsNetworkPath(pathOrUrl);
        std::string url = WideToUtf8(pathOrUrl);

        m_format = avformat_alloc_context();
        if (!m_format) return Fail(error, L"Out of memory.");
        m_format->interrupt_callback.callback = &FfmpegDecoder::Interrupted;
        m_format->interrupt_callback.opaque = this;

        AVDictionary* options = nullptr;
        if (network) {
            av_dict_set(&options, "user_agent", UserAgent().c_str(), 0);
            av_dict_set(&options, "icy", "1", 0);  // ask Shoutcast/Icecast for titles
            av_dict_set(&options, "reconnect", "1", 0);
            av_dict_set(&options, "reconnect_streamed", "1", 0);
            av_dict_set(&options, "reconnect_on_network_error", "1", 0);
            av_dict_set(&options, "reconnect_delay_max", "10", 0);
            av_dict_set(&options, "rw_timeout", std::to_string(kNetworkTimeoutSeconds * 1000000LL).c_str(), 0);
        }
        Arm();
        int rc = avformat_open_input(&m_format, url.c_str(), nullptr, &options);
        av_dict_free(&options);
        if (rc < 0) {
            m_format = nullptr;  // freed by avformat_open_input
            return Fail(error, network ? L"Could not open the stream: " + ErrorText(rc) : OpenErrorText(rc));
        }
        Arm();
        if (avformat_find_stream_info(m_format, nullptr) < 0 && network) {
            // Streams can still play; their details show up once audio flows.
        }
        m_stream = av_find_best_stream(m_format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (m_stream < 0) return Fail(error, L"There is no audio in it.");
        AVStream* stream = m_format->streams[m_stream];
        for (unsigned i = 0; i < m_format->nb_streams; i++) {
            if (static_cast<int>(i) != m_stream) m_format->streams[i]->discard = AVDISCARD_ALL;
        }

        const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!codec) return Fail(error, L"Its audio format is not supported.");
        m_codec = avcodec_alloc_context3(codec);
        if (!m_codec || avcodec_parameters_to_context(m_codec, stream->codecpar) < 0) {
            return Fail(error, L"Out of memory.");
        }
        m_codec->pkt_timebase = stream->time_base;
        if (avcodec_open2(m_codec, codec, nullptr) < 0) return Fail(error, L"Its audio could not be decoded.");
        if (m_codec->sample_rate <= 0) return Fail(error, L"Its audio has no sample rate.");

        m_rate = m_codec->sample_rate;
        m_sourceChannels = m_codec->ch_layout.nb_channels;
        m_sourceRate = m_codec->sample_rate;
        m_bits = stream->codecpar->bits_per_raw_sample > 0 ? stream->codecpar->bits_per_raw_sample
                                                            : stream->codecpar->bits_per_coded_sample;
        const AVCodecDescriptor* desc = avcodec_descriptor_get(stream->codecpar->codec_id);
        if (desc && (desc->props & AV_CODEC_PROP_LOSSY)) m_bits = 0;
        m_codecName = codec->name;

        m_packet = av_packet_alloc();
        m_frame = av_frame_alloc();
        if (!m_packet || !m_frame) return Fail(error, L"Out of memory.");

        // Length, and whether this is live (no length and no seeking).
        if (m_format->duration > 0) {
            m_length = m_format->duration / static_cast<double>(AV_TIME_BASE);
        } else if (stream->duration > 0) {
            m_length = stream->duration * av_q2d(stream->time_base);
        }
        const bool seekable = m_format->pb && (m_format->pb->seekable & AVIO_SEEKABLE_NORMAL);
        m_live = network && (m_length <= 0 || !seekable);
        if (m_live) m_length = 0;
        if (m_format->start_time != AV_NOPTS_VALUE) m_startTime = m_format->start_time / static_cast<double>(AV_TIME_BASE);

        m_nominalBitrate = static_cast<int>((stream->codecpar->bit_rate > 0 ? stream->codecpar->bit_rate
                                                                            : m_format->bit_rate) / 1000);
        ReadInfo();
        return true;
    }

    int SampleRate() const override { return m_rate; }

    int Read(float* out, int frames) override {
        int written = 0;
        while (written < frames) {
            size_t available = (m_pcm.size() - m_pcmRead) / 2;
            if (available > 0) {
                size_t n = std::min<size_t>(available, static_cast<size_t>(frames - written));
                std::memcpy(out + written * 2, m_pcm.data() + m_pcmRead, n * 2 * sizeof(float));
                m_pcmRead += n * 2;
                written += static_cast<int>(n);
                continue;
            }
            m_pcm.clear();
            m_pcmRead = 0;
            if (!DecodeMore()) break;
        }
        return written;
    }

    bool Seek(double seconds) override {
        if (m_live || !m_format) return false;
        if (seconds < 0) seconds = 0;
        int64_t target = static_cast<int64_t>((seconds + m_startTime) * AV_TIME_BASE);
        Arm();
        if (avformat_seek_file(m_format, -1, INT64_MIN, target, target, 0) < 0 &&
            av_seek_frame(m_format, -1, target, AVSEEK_FLAG_BACKWARD) < 0) {
            return false;
        }
        avcodec_flush_buffers(m_codec);
        if (m_swr) swr_init(m_swr);  // drops anything still inside it
        m_pcm.clear();
        m_pcmRead = 0;
        m_ended = false;
        m_flushing = false;
        m_skipUntil = seconds;  // decoded audio before this is dropped
        return true;
    }

    void Abort() override { m_abort = true; }

    double Length() const override { return m_length; }
    bool IsLive() const override { return m_live; }

    std::string Tag(const std::string& name) const override {
        std::lock_guard<std::mutex> lock(m_infoMutex);
        std::string key = Upper(name);
        auto it = m_tags.find(key);
        if (it == m_tags.end()) {
            if (const char* alias = FfmpegKey(key)) it = m_tags.find(alias);
        }
        return it == m_tags.end() ? "" : it->second;
    }

    std::string StreamTitle() const override {
        std::lock_guard<std::mutex> lock(m_infoMutex);
        return m_streamTitle;
    }

    std::vector<Chapter> Chapters() const override {
        std::lock_guard<std::mutex> lock(m_infoMutex);
        return m_chapters;
    }

    int Bitrate() const override {
        std::lock_guard<std::mutex> lock(m_infoMutex);
        return m_recentBitrate > 0 ? m_recentBitrate : m_nominalBitrate;
    }

    bool IsVbr() const override {
        std::lock_guard<std::mutex> lock(m_infoMutex);
        return m_vbr;
    }

    int SourceChannels() const override { return m_sourceChannels; }
    int SourceSampleRate() const override { return m_sourceRate; }
    int SourceBits() const override { return m_bits; }
    std::string CodecName() const override { return m_codecName; }

    // A new stream title since the last call (the decode thread asks after reads)
    bool TakeTitleChange() {
        std::lock_guard<std::mutex> lock(m_infoMutex);
        bool changed = m_titleChanged;
        m_titleChanged = false;
        return changed;
    }

private:
    static bool Fail(std::wstring& error, const std::wstring& message) {
        error = message;
        return false;
    }

    static std::wstring ErrorText(int rc) {
        char text[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(rc, text, sizeof(text));
        return Utf8ToWide(text);
    }

    static std::wstring OpenErrorText(int rc) {
        if (rc == AVERROR(ENOENT)) return L"The file was not found.";
        if (rc == AVERROR(EACCES)) return L"The file could not be opened.";
        if (rc == AVERROR_INVALIDDATA) return L"Unsupported file format.";
        return L"Could not open the file: " + ErrorText(rc);
    }

    // Starts the clock a blocking call may run for before it is given up.
    void Arm() {
        m_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kNetworkTimeoutSeconds * 2);
    }

    static int Interrupted(void* opaque) {
        auto* self = static_cast<FfmpegDecoder*>(opaque);
        return self->m_abort || std::chrono::steady_clock::now() > self->m_deadline ? 1 : 0;
    }

    // The tags, chapters and stream headers, read once the file is open.
    void ReadInfo() {
        std::lock_guard<std::mutex> lock(m_infoMutex);
        auto add = [this](AVDictionary* dict) {
            const AVDictionaryEntry* e = nullptr;
            while ((e = av_dict_get(dict, "", e, AV_DICT_IGNORE_SUFFIX))) {
                std::string key = Upper(e->key);
                if (m_tags.find(key) == m_tags.end()) m_tags[key] = e->value;
            }
        };
        add(m_format->metadata);
        add(m_format->streams[m_stream]->metadata);
        for (unsigned i = 0; i < m_format->nb_chapters; i++) {
            const AVChapter* c = m_format->chapters[i];
            Chapter chapter;
            // Chapter times count from the audible start already (after an MP3's
            // encoder delay, which start_time is), so they are taken as they are.
            chapter.position = c->start * av_q2d(c->time_base);
            if (chapter.position < 0) chapter.position = 0;
            const AVDictionaryEntry* title = av_dict_get(c->metadata, "title", nullptr, 0);
            if (title) chapter.name = Utf8ToWide(title->value);
            m_chapters.push_back(chapter);
        }
        std::sort(m_chapters.begin(), m_chapters.end(),
                  [](const Chapter& a, const Chapter& b) { return a.position < b.position; });
        // Shoutcast/Icecast headers ("icy-name: ...") come as the stream's metadata
        // too; keep their names lower case, as they are known by.
        for (const char* key : {"icy-name", "icy-genre", "icy-br", "icy-url", "icy-description"}) {
            auto it = m_tags.find(Upper(key));
            if (it != m_tags.end()) m_tags[key] = it->second;
        }
        auto st = m_tags.find("STREAMTITLE");
        if (st != m_tags.end()) m_streamTitle = st->second;
        PollIcyLocked();
    }

    // A Shoutcast stream's title comes with the audio; see if it changed.
    void PollIcy() {
        std::lock_guard<std::mutex> lock(m_infoMutex);
        PollIcyLocked();
    }

    void PollIcyLocked() {
        if (!m_format || !m_format->pb) return;
        uint8_t* packet = nullptr;
        if (av_opt_get(m_format, "icy_metadata_packet", AV_OPT_SEARCH_CHILDREN, &packet) < 0 || !packet) return;
        std::string meta = reinterpret_cast<char*>(packet);
        av_free(packet);
        if (meta.empty() || meta == m_lastIcyPacket) return;
        m_lastIcyPacket = meta;
        std::string title = IcyField(meta, "StreamTitle");
        if (title != m_streamTitle) {
            m_streamTitle = title;
            m_titleChanged = true;
        }
    }

    // Averages the bitrate over the last few seconds of packets.
    void CountPacket(const AVPacket* packet) {
        if (packet->duration <= 0) return;
        double seconds = packet->duration * av_q2d(m_format->streams[m_stream]->time_base);
        if (seconds <= 0) return;
        int kbps = static_cast<int>(packet->size * 8 / seconds / 1000 + 0.5);
        m_window.push_back({packet->size, seconds});
        m_windowBytes += packet->size;
        m_windowSeconds += seconds;
        while (m_window.size() > 1 && m_windowSeconds - m_window.front().second > 2.0) {
            m_windowBytes -= m_window.front().first;
            m_windowSeconds -= m_window.front().second;
            m_window.pop_front();
        }
        std::lock_guard<std::mutex> lock(m_infoMutex);
        if (m_windowSeconds > 0) m_recentBitrate = static_cast<int>(m_windowBytes * 8 / m_windowSeconds / 1000 + 0.5);
        // Frames of different sizes: a variable bitrate (a constant one varies by
        // a padding byte at most).
        if (m_firstKbps == 0) {
            m_firstKbps = kbps;
        } else if (!m_vbr && std::abs(kbps - m_firstKbps) > std::max(8, m_firstKbps / 20)) {
            m_vbr = m_bits == 0;
        }
    }

    // Decodes more audio into m_pcm. False at the end.
    bool DecodeMore() {
        while (true) {
            int rc = avcodec_receive_frame(m_codec, m_frame);
            if (rc == 0) {
                Convert(m_frame);
                av_frame_unref(m_frame);
                if (!m_pcm.empty()) return true;
                continue;
            }
            if (rc == AVERROR_EOF) return false;
            if (rc != AVERROR(EAGAIN)) return false;
            if (m_flushing) return false;

            Arm();
            rc = av_read_frame(m_format, m_packet);
            if (rc < 0) {
                // The end (or a stream gone for good): drain the decoder.
                m_flushing = true;
                avcodec_send_packet(m_codec, nullptr);
                continue;
            }
            if (m_packet->stream_index == m_stream) {
                CountPacket(m_packet);
                rc = avcodec_send_packet(m_codec, m_packet);
                // A damaged packet is skipped; the audio carries on after it.
                (void)rc;
            }
            av_packet_unref(m_packet);
            PollIcy();
        }
    }

    // Converts a decoded frame to stereo float and appends it, dropping what lies
    // before a seek's target.
    void Convert(const AVFrame* frame) {
        if (frame->nb_samples <= 0) return;
        const int channels = frame->ch_layout.nb_channels;
        // The source layout can change mid-stream (a radio station's ad break):
        // the converter follows, and resamples to the rate the stream began at.
        if (!m_swr || channels != m_swrChannels || frame->sample_rate != m_swrRate ||
            frame->format != m_swrFormat) {
            swr_free(&m_swr);
            AVChannelLayout in = frame->ch_layout;
            AVChannelLayout layout;
            if (in.order == AV_CHANNEL_ORDER_UNSPEC || in.nb_channels <= 0) {
                av_channel_layout_default(&layout, channels > 0 ? channels : 2);
            } else {
                av_channel_layout_copy(&layout, &in);
            }
            // Mono stays one channel here and is copied to both sides below: a mix
            // to stereo would play it 3 dB quieter.
            AVChannelLayout outLayout;
            if (channels == 1) {
                av_channel_layout_default(&outLayout, 1);
            } else {
                av_channel_layout_default(&outLayout, 2);
            }
            if (swr_alloc_set_opts2(&m_swr, &outLayout, AV_SAMPLE_FMT_FLT, m_rate, &layout,
                                    static_cast<AVSampleFormat>(frame->format), frame->sample_rate, 0,
                                    nullptr) < 0 ||
                swr_init(m_swr) < 0) {
                swr_free(&m_swr);
                av_channel_layout_uninit(&layout);
                return;
            }
            av_channel_layout_uninit(&layout);
            m_swrChannels = channels;
            m_swrRate = frame->sample_rate;
            m_swrFormat = frame->format;
            m_swrOutChannels = outLayout.nb_channels;
        }

        int outCapacity = swr_get_out_samples(m_swr, frame->nb_samples);
        if (outCapacity <= 0) return;
        m_convert.resize(static_cast<size_t>(outCapacity) * m_swrOutChannels);
        uint8_t* outPlanes[1] = {reinterpret_cast<uint8_t*>(m_convert.data())};
        int got = swr_convert(m_swr, outPlanes, outCapacity, const_cast<const uint8_t**>(frame->extended_data),
                              frame->nb_samples);
        if (got <= 0) return;

        // Where this frame starts, to drop what precedes a seek's target
        int skip = 0;
        if (m_skipUntil >= 0) {
            int64_t pts = frame->best_effort_timestamp;
            if (pts != AV_NOPTS_VALUE) {
                double start = pts * av_q2d(m_format->streams[m_stream]->time_base) - m_startTime;
                double end = start + static_cast<double>(got) / m_rate;
                if (end <= m_skipUntil) return;  // wholly before it
                if (start < m_skipUntil) skip = static_cast<int>((m_skipUntil - start) * m_rate);
            }
            m_skipUntil = -1;
        }
        if (skip >= got) return;

        size_t base = m_pcm.size();
        m_pcm.resize(base + static_cast<size_t>(got - skip) * 2);
        float* dst = m_pcm.data() + base;
        if (m_swrOutChannels == 1) {
            for (int i = skip; i < got; i++) {
                *dst++ = m_convert[i];
                *dst++ = m_convert[i];
            }
        } else {
            std::memcpy(dst, m_convert.data() + static_cast<size_t>(skip) * 2,
                        static_cast<size_t>(got - skip) * 2 * sizeof(float));
        }
    }

    AVFormatContext* m_format = nullptr;
    AVCodecContext* m_codec = nullptr;
    SwrContext* m_swr = nullptr;
    AVPacket* m_packet = nullptr;
    AVFrame* m_frame = nullptr;
    int m_stream = -1;
    int m_rate = 0;
    int m_swrChannels = 0, m_swrRate = 0, m_swrFormat = -1, m_swrOutChannels = 2;
    std::vector<float> m_convert;
    std::vector<float> m_pcm;  // decoded, interleaved stereo, from m_pcmRead on
    size_t m_pcmRead = 0;
    bool m_ended = false;
    bool m_flushing = false;
    double m_skipUntil = -1;
    double m_startTime = 0;

    double m_length = 0;
    bool m_live = false;
    int m_sourceChannels = 0, m_sourceRate = 0, m_bits = 0;
    std::string m_codecName;

    std::atomic<bool> m_abort{false};
    std::chrono::steady_clock::time_point m_deadline;

    std::deque<std::pair<int, double>> m_window;  // recent packets: bytes, seconds
    double m_windowBytes = 0, m_windowSeconds = 0;
    int m_firstKbps = 0;

    mutable std::mutex m_infoMutex;
    std::map<std::string, std::string> m_tags;  // upper-case names
    std::vector<Chapter> m_chapters;
    std::string m_streamTitle, m_lastIcyPacket;
    bool m_titleChanged = false;
    int m_nominalBitrate = 0, m_recentBitrate = 0;
    bool m_vbr = false;
};

}  // namespace

bool IsNetworkPath(const std::wstring& path) {
    return WStrNICmp(path.c_str(), L"http://", 7) == 0 || WStrNICmp(path.c_str(), L"https://", 8) == 0;
}

std::unique_ptr<Decoder> OpenFfmpegDecoder(const std::wstring& pathOrUrl, std::wstring& error) {
    QuietLogging();
    auto decoder = std::make_unique<FfmpegDecoder>();
    if (!decoder->Open(pathOrUrl, error)) return nullptr;
    return decoder;
}

bool TakeStreamTitleChange(Decoder* decoder) {
    auto* ffmpeg = dynamic_cast<FfmpegDecoder*>(decoder);
    return ffmpeg && ffmpeg->TakeTitleChange();
}

bool DecodeWholeFile(const std::wstring& path, std::vector<float>& samples, int& channels, int& sampleRate,
                     std::wstring& error) {
    // Kept in the file's own channels: an impulse response's left and right differ.
    QuietLogging();
    AVFormatContext* format = nullptr;
    std::string url = WideToUtf8(path);
    if (avformat_open_input(&format, url.c_str(), nullptr, nullptr) < 0) {
        error = L"The file could not be opened.";
        return false;
    }
    struct Closer {
        AVFormatContext*& f;
        ~Closer() { avformat_close_input(&f); }
    } closeFormat{format};
    avformat_find_stream_info(format, nullptr);
    int index = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (index < 0) {
        error = L"There is no audio in it.";
        return false;
    }
    const AVCodec* codec = avcodec_find_decoder(format->streams[index]->codecpar->codec_id);
    AVCodecContext* ctx = codec ? avcodec_alloc_context3(codec) : nullptr;
    struct CodecCloser {
        AVCodecContext*& c;
        ~CodecCloser() { avcodec_free_context(&c); }
    } closeCodec{ctx};
    if (!ctx || avcodec_parameters_to_context(ctx, format->streams[index]->codecpar) < 0 ||
        avcodec_open2(ctx, codec, nullptr) < 0) {
        error = L"Its audio could not be decoded.";
        return false;
    }
    channels = ctx->ch_layout.nb_channels;
    sampleRate = ctx->sample_rate;
    if (channels <= 0 || sampleRate <= 0) {
        error = L"Its audio format is not supported.";
        return false;
    }

    SwrContext* swr = nullptr;
    AVChannelLayout layout;
    av_channel_layout_default(&layout, channels);
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    std::vector<float> convert;
    samples.clear();
    auto drain = [&]() {
        while (avcodec_receive_frame(ctx, frame) == 0) {
            if (!swr) {
                AVChannelLayout in = frame->ch_layout;
                if (in.order == AV_CHANNEL_ORDER_UNSPEC) av_channel_layout_default(&in, channels);
                swr_alloc_set_opts2(&swr, &layout, AV_SAMPLE_FMT_FLT, sampleRate, &in,
                                    static_cast<AVSampleFormat>(frame->format), frame->sample_rate, 0, nullptr);
                if (swr) swr_init(swr);
            }
            if (!swr) break;
            int capacity = swr_get_out_samples(swr, frame->nb_samples);
            convert.resize(static_cast<size_t>(std::max(capacity, 0)) * channels);
            uint8_t* out[1] = {reinterpret_cast<uint8_t*>(convert.data())};
            int got = swr_convert(swr, out, capacity, const_cast<const uint8_t**>(frame->extended_data),
                                  frame->nb_samples);
            if (got > 0) samples.insert(samples.end(), convert.begin(), convert.begin() + static_cast<size_t>(got) * channels);
            av_frame_unref(frame);
        }
    };
    while (av_read_frame(format, packet) >= 0) {
        if (packet->stream_index == index && avcodec_send_packet(ctx, packet) >= 0) drain();
        av_packet_unref(packet);
    }
    avcodec_send_packet(ctx, nullptr);
    drain();
    av_frame_free(&frame);
    av_packet_free(&packet);
    swr_free(&swr);
    av_channel_layout_uninit(&layout);
    if (samples.empty()) {
        error = L"No audio could be decoded from it.";
        return false;
    }
    return true;
}

std::string DecoderVersion() {
    return std::string("FFmpeg ") + av_version_info();
}

}  // namespace audio
