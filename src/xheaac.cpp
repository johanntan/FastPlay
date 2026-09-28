// xHE-AAC files, decoded with FDK AAC and served to BASS as a WAV. See xheaac.h.
//
// BASS_StreamCreateFileUser lets a file be read through callbacks. The callbacks
// here present a WAV: a 44 byte header, then the decoded 16-bit PCM, produced as
// BASS reads it. A byte position in that file is a sample position, so seeking is
// a matter of restarting the decoder at the access unit holding it.

#include "xheaac.h"
#include "mp4_index.h"
#include "utils.h"

#include "aacdecoder_lib.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

using namespace mp4;

namespace {

const size_t kMaxUnits = 8 * 1000 * 1000;  // access units: two days at 21 ms
const size_t kHeaderBytes = 44;
const int kMaxChannels = 8;
const int kMaxFrame = 4096;                 // samples per channel a unit can decode to
const uint32_t kAotUsac = 42;

// The MP4 audio track that is xHE-AAC, with its AudioSpecificConfig.
struct Source {
    Track track;
    SampleTable table;
    std::vector<uint8_t> asc;
    std::string tags;
};

// The AudioSpecificConfig inside an mp4a sample entry: through the esds box's
// ES_Descriptor and DecoderConfigDescriptor to the DecoderSpecificInfo.
bool ReadConfig(Span stsd, std::vector<uint8_t>& asc) {
    if (stsd.size() < 8) return false;
    Span entries{stsd.p + 8, stsd.end};  // after the version, flags and count
    Span mp4a;
    if (!FindBox(entries, Tag('m', 'p', '4', 'a'), mp4a)) return false;
    // 8 bytes of sample entry, then the audio sample entry, longer in later versions
    if (mp4a.size() < 28) return false;
    size_t version = Read16(mp4a.p + 8);
    size_t header = 28 + (version == 1 ? 16 : version == 2 ? 36 : 0);
    if (mp4a.size() < header) return false;
    Span esds;
    if (!FindBox(Span{mp4a.p + header, mp4a.end}, Tag('e', 's', 'd', 's'), esds) || esds.size() < 4) return false;

    const uint8_t* p = esds.p + 4;  // version and flags
    const uint8_t* end = esds.end;
    // A descriptor: a tag byte and a size in up to four bytes, seven bits each.
    auto descriptor = [&](uint8_t tag, size_t& size) {
        if (p >= end || *p++ != tag) return false;
        size = 0;
        for (int i = 0; i < 4 && p < end; i++) {
            uint8_t b = *p++;
            size = (size << 7) | (b & 0x7F);
            if (!(b & 0x80)) break;
        }
        return size <= static_cast<size_t>(end - p);
    };
    size_t size;
    if (!descriptor(0x03, size) || end - p < 3) return false;  // ES_Descriptor
    uint8_t flags = p[2];
    p += 3;
    if (flags & 0x80) p += 2;             // depends on another stream
    if (flags & 0x40) {                   // a URL
        if (p >= end) return false;
        p += 1 + *p;
    }
    if (flags & 0x20) p += 2;             // OCR stream
    if (!descriptor(0x04, size) || end - p < 13) return false;  // DecoderConfigDescriptor
    p += 13;
    if (!descriptor(0x05, size) || size == 0) return false;     // DecoderSpecificInfo
    asc.assign(p, p + size);
    return true;
}

// The audio object type an AudioSpecificConfig starts with.
uint32_t ObjectType(const std::vector<uint8_t>& asc) {
    if (asc.size() < 2) return 0;
    uint32_t aot = asc[0] >> 3;
    if (aot == 31) aot = 32 + (((asc[0] & 0x07) << 3) | (asc[1] >> 5));
    return aot;
}

bool FindSource(FILE* f, Source& out) {
    std::vector<uint8_t> moov;
    if (!ReadMoov(f, moov)) return false;
    Span all{moov.data(), moov.data() + moov.size()};
    for (const Track& track : ReadTracks(all)) {
        std::vector<uint8_t> asc;
        if (track.stsd.empty() || !ReadConfig(track.stsd, asc) || ObjectType(asc) != kAotUsac) continue;
        if (!ReadSamples(track, kMaxUnits, out.table)) return false;
        out.track = track;
        out.asc = std::move(asc);
        out.tags = ReadTags(all);
        return true;
    }
    return false;
}

struct FileCloser {
    FILE* f;
    ~FileCloser() { fclose(f); }
};

// One open file: the decoder, and the virtual WAV it is read through.
class Stream {
public:
    Stream(FILE* f, Source&& source) : m_file(f), m_source(std::move(source)) {}
    ~Stream() {
        if (m_decoder) aacDecoder_Close(m_decoder);
        fclose(m_file);
    }

    // Decodes the first unit to learn the format and lays out the WAV.
    bool Open() {
        if (!Restart(0) || !DecodeMore()) return false;
        const Track& track = m_source.track;
        uint64_t decoded = static_cast<uint64_t>(m_source.table.sizes.size()) * m_frameSize;
        // The edit list says where the track starts in the decoded audio (past
        // the encoder's priming) and how long it plays, in its own time units.
        uint64_t start = 0, frames = decoded;
        if (track.timescale && track.editStart > 0) {
            start = static_cast<uint64_t>(static_cast<double>(track.editStart) * m_rate / track.timescale);
        }
        if (track.movieTimescale && track.editDuration > 0) {
            frames = static_cast<uint64_t>(static_cast<double>(track.editDuration) * m_rate / track.movieTimescale);
        }
        if (start >= decoded) start = 0;
        frames = std::min(frames, decoded - start);
        m_startBytes = start * m_bytesPerFrame;
        m_dataBytes = frames * m_bytesPerFrame;
        m_endBytes = m_startBytes + m_dataBytes;
        uint64_t total = 0;
        for (uint32_t size : m_source.table.sizes) total += size;
        double seconds = static_cast<double>(frames) / m_rate;
        m_bitrate = seconds > 0 ? static_cast<int>(total * 8 / seconds / 1000 + 0.5) : 0;

        // The header. RIFF sizes are 32-bit; a file too long for them is served
        // with the largest they hold, and BASS goes by the file length anyway.
        uint32_t data = static_cast<uint32_t>(std::min<uint64_t>(m_dataBytes, 0xFFFFFFFFull - 36));
        auto put32 = [&](size_t at, uint32_t v) {
            for (int i = 0; i < 4; i++) m_header[at + i] = static_cast<uint8_t>(v >> (8 * i));
        };
        auto put16 = [&](size_t at, uint16_t v) {
            m_header[at] = static_cast<uint8_t>(v);
            m_header[at + 1] = static_cast<uint8_t>(v >> 8);
        };
        std::memcpy(m_header, "RIFF", 4);
        put32(4, data + 36);
        std::memcpy(m_header + 8, "WAVEfmt ", 8);
        put32(16, 16);
        put16(20, 1);  // PCM
        put16(22, static_cast<uint16_t>(m_channels));
        put32(24, static_cast<uint32_t>(m_rate));
        put32(28, static_cast<uint32_t>(m_rate * m_bytesPerFrame));
        put16(32, static_cast<uint16_t>(m_bytesPerFrame));
        put16(34, 16);
        std::memcpy(m_header + 36, "data", 4);
        put32(40, data);
        return Restart(0);
    }

    uint64_t Length() const { return kHeaderBytes + m_dataBytes; }
    const std::string& Tags() const { return m_source.tags; }
    int Bitrate() const { return m_bitrate; }

    bool SeekTo(uint64_t offset) {
        if (offset > Length()) return false;
        m_pos = offset;
        return true;
    }

    DWORD Read(uint8_t* out, DWORD length) {
        DWORD written = 0;
        while (written < length) {
            if (m_pos < kHeaderBytes) {
                size_t n = std::min<size_t>(length - written, kHeaderBytes - static_cast<size_t>(m_pos));
                std::memcpy(out + written, m_header + m_pos, n);
                m_pos += n;
                written += static_cast<DWORD>(n);
                continue;
            }
            if (m_pos - kHeaderBytes >= m_dataBytes) break;
            uint64_t at = m_pos - kHeaderBytes + m_startBytes;  // in the decoded audio
            if (!Reach(at)) break;
            size_t have = m_pcm.size() - static_cast<size_t>(at - m_pcmStart);
            size_t n = std::min<size_t>(length - written, have);
            n = std::min<uint64_t>(n, m_endBytes - at);
            std::memcpy(out + written, m_pcm.data() + (at - m_pcmStart), n);
            m_pos += n;
            written += static_cast<DWORD>(n);
        }
        return written;
    }

private:
    // Has the decoded bytes at `at` (an offset into the decoded audio) in m_pcm.
    bool Reach(uint64_t at) {
        uint64_t end = m_pcmStart + m_pcm.size();
        if (at >= m_pcmStart && at < end) return true;
        // A little ahead: decode on. Anywhere else: start again at the last
        // unit before it that decoding can start from.
        if (at < m_pcmStart || at > end + static_cast<uint64_t>(m_rate) * m_bytesPerFrame) {
            uint64_t frame = at / m_bytesPerFrame;
            size_t unit = static_cast<size_t>(frame / m_frameSize);
            const std::vector<uint32_t>& sync = m_source.table.sync;
            if (!sync.empty()) {
                auto next = std::upper_bound(sync.begin(), sync.end(), unit);
                unit = next == sync.begin() ? 0 : *(next - 1);
            }
            if (!Restart(unit)) return false;
        }
        while (m_pcmStart + m_pcm.size() <= at) {
            // Drop what is behind, keeping a block's worth in case of a step back.
            if (m_pcm.size() > static_cast<size_t>(m_bytesPerFrame) * m_frameSize * 4) {
                size_t drop = m_pcm.size() - static_cast<size_t>(m_bytesPerFrame) * m_frameSize * 2;
                m_pcm.erase(m_pcm.begin(), m_pcm.begin() + drop);
                m_pcmStart += drop;
            }
            if (!DecodeMore()) return false;
        }
        return true;
    }

    // Starts decoding afresh from access unit `unit`.
    bool Restart(size_t unit) {
        if (m_decoder) aacDecoder_Close(m_decoder);
        m_decoder = aacDecoder_Open(TT_MP4_RAW, 1);
        if (!m_decoder) return false;
        UCHAR* conf = m_source.asc.data();
        UINT length = static_cast<UINT>(m_source.asc.size());
        if (aacDecoder_ConfigRaw(m_decoder, &conf, &length) != AAC_DEC_OK) return false;
        // As encoded: no loudness normalization (the default would bring
        // everything to -24 dBFS), as with every other format FastPlay plays.
        aacDecoder_SetParam(m_decoder, AAC_DRC_REFERENCE_LEVEL, -1);
        m_next = unit;
        m_flushed = false;
        m_skip = -1;  // learned from the first decoded frame
        m_pcm.clear();
        m_pcmStart = static_cast<uint64_t>(unit) * m_frameSize * m_bytesPerFrame;
        return true;
    }

    // Decodes the next unit, or flushes the decoder's tail, or pads the end with
    // silence, appending to m_pcm. False only if nothing more can come.
    bool DecodeMore() {
        const SampleTable& table = m_source.table;
        INT_PCM out[kMaxFrame * kMaxChannels];
        while (m_next < table.sizes.size()) {
            size_t i = m_next++;
            m_unit.resize(table.sizes[i]);
            if (!Seek(m_file, table.offsets[i]) || fread(m_unit.data(), 1, m_unit.size(), m_file) != m_unit.size()) {
                return false;
            }
            UCHAR* data = m_unit.data();
            UINT size = static_cast<UINT>(m_unit.size()), left = size;
            if (aacDecoder_Fill(m_decoder, &data, &size, &left) != AAC_DEC_OK) return false;
            AAC_DECODER_ERROR err = aacDecoder_DecodeFrame(m_decoder, out, kMaxFrame * kMaxChannels, 0);
            if (err == AAC_DEC_NOT_ENOUGH_BITS) continue;
            if (err != AAC_DEC_OK) {
                // A damaged unit: its time passes in silence.
                if (m_frameSize) Append(nullptr, m_frameSize);
                continue;
            }
            const CStreamInfo* info = aacDecoder_GetStreamInfo(m_decoder);
            if (!info || info->numChannels <= 0 || info->frameSize <= 0) return false;
            if (m_frameSize == 0) {
                m_channels = std::min(info->numChannels, kMaxChannels);
                m_rate = info->sampleRate;
                m_frameSize = info->frameSize;
                m_bytesPerFrame = m_channels * 2;
                m_pcmStart = static_cast<uint64_t>(m_next - 1) * m_frameSize * m_bytesPerFrame;
            }
            if (m_skip < 0) m_skip = static_cast<int>(info->outputDelay);  // the filter banks filling
            Append(out, info->frameSize);
            return true;
        }
        // The end of the file: the decoder still holds its delay's worth.
        if (!m_flushed) {
            m_flushed = true;
            if (aacDecoder_DecodeFrame(m_decoder, out, kMaxFrame * kMaxChannels, AACDEC_FLUSH) == AAC_DEC_OK) {
                const CStreamInfo* info = aacDecoder_GetStreamInfo(m_decoder);
                if (info && info->frameSize > 0) Append(out, info->frameSize);
            }
        }
        uint64_t end = m_pcmStart + m_pcm.size();
        if (end >= m_endBytes) return false;
        Append(nullptr, static_cast<int>(std::min<uint64_t>((m_endBytes - end) / m_bytesPerFrame, m_frameSize)));
        return true;
    }

    // Appends `frames` frames of decoded audio (interleaved, or silence if null),
    // less any still to be skipped.
    void Append(const INT_PCM* samples, int frames) {
        int skip = std::min(std::max(m_skip, 0), frames);
        m_skip = std::max(m_skip - skip, 0);
        frames -= skip;
        if (frames <= 0) return;
        size_t bytes = static_cast<size_t>(frames) * m_bytesPerFrame;
        size_t start = m_pcm.size();
        m_pcm.resize(start + bytes);
        if (samples) {
            std::memcpy(m_pcm.data() + start, samples + static_cast<size_t>(skip) * m_channels, bytes);
        } else {
            std::memset(m_pcm.data() + start, 0, bytes);
        }
    }

    FILE* m_file;
    Source m_source;
    HANDLE_AACDECODER m_decoder = nullptr;
    int m_channels = 0, m_rate = 0, m_frameSize = 0, m_bytesPerFrame = 0;
    int m_bitrate = 0;
    uint64_t m_dataBytes = 0;   // what the WAV holds
    uint64_t m_startBytes = 0;  // where in the decoded audio it starts
    uint64_t m_endBytes = 0;    // and ends
    uint8_t m_header[kHeaderBytes] = {};

    uint64_t m_pos = 0;             // where BASS is reading in the virtual file
    std::vector<uint8_t> m_pcm;     // decoded PCM, from offset m_pcmStart of the decoded audio
    uint64_t m_pcmStart = 0;
    size_t m_next = 0;              // the next access unit to decode
    int m_skip = -1;                // frames of output still to drop after a restart
    bool m_flushed = false;
    std::vector<uint8_t> m_unit;
};

std::mutex g_mutex;
std::map<HSTREAM, Stream*> g_streams;

void CALLBACK OnClose(void* user) {
    auto* stream = static_cast<Stream*>(user);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto it = g_streams.begin(); it != g_streams.end(); ++it) {
            if (it->second == stream) {
                g_streams.erase(it);
                break;
            }
        }
    }
    delete stream;
}

QWORD CALLBACK OnLength(void* user) {
    return static_cast<Stream*>(user)->Length();
}

DWORD CALLBACK OnRead(void* buffer, DWORD length, void* user) {
    return static_cast<Stream*>(user)->Read(static_cast<uint8_t*>(buffer), length);
}

BOOL CALLBACK OnSeek(QWORD offset, void* user) {
    return static_cast<Stream*>(user)->SeekTo(offset) ? TRUE : FALSE;
}

Stream* Find(HSTREAM handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_streams.find(handle);
    return it == g_streams.end() ? nullptr : it->second;
}

}  // namespace

bool IsXheAacFile(const std::wstring& path) {
    FILE* f = FileOpen(path, "rb");
    if (!f) return false;
    FileCloser closer{f};
    Source source;
    return FindSource(f, source);
}

HSTREAM CreateXheAacStream(const std::wstring& path, DWORD flags) {
    FILE* f = FileOpen(path, "rb");
    if (!f) return 0;
    Source source;
    if (!FindSource(f, source)) {
        fclose(f);
        return 0;
    }
    auto stream = std::make_unique<Stream>(f, std::move(source));  // owns the file now
    if (!stream->Open()) return 0;

    BASS_FILEPROCS procs = {OnClose, OnLength, OnRead, OnSeek};
    Stream* raw = stream.release();  // BASS owns it, and closes it through OnClose
    HSTREAM handle = BASS_StreamCreateFileUser(STREAMFILE_NOBUFFER, flags, &procs, raw);
    if (!handle) {
        delete raw;
        return 0;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_streams[handle] = raw;
    return handle;
}

const char* XheAacTags(HSTREAM stream) {
    Stream* s = Find(stream);
    return s && !s->Tags().empty() ? s->Tags().c_str() : nullptr;
}

int XheAacBitrate(HSTREAM stream) {
    Stream* s = Find(stream);
    return s ? s->Bitrate() : 0;
}
