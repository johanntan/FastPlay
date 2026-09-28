// The chapters of an MP4 file: the QuickTime chapter track and the Nero chpl box.
// See mp4_chapters.h. Only the moov box (the file's index) is read, plus the chapter
// titles themselves, so a long audiobook costs no more than a short one.

#include "mp4_chapters.h"
#include "utils.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

namespace {

const uint64_t kMaxMoovBytes = 64 * 1024 * 1024;
const size_t kMaxChapters = 10000;
const size_t kMaxTitleBytes = 1024;

constexpr uint32_t Tag(char a, char b, char c, char d) {
    return (static_cast<uint32_t>(static_cast<uint8_t>(a)) << 24) | (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 8) | static_cast<uint32_t>(static_cast<uint8_t>(d));
}

uint16_t Read16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

uint32_t Read32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

uint64_t Read64(const uint8_t* p) {
    return (static_cast<uint64_t>(Read32(p)) << 32) | Read32(p + 4);
}

bool Seek(FILE* f, uint64_t offset) {
#ifdef _WIN32
    return _fseeki64(f, static_cast<__int64>(offset), SEEK_SET) == 0;
#else
    return fseeko(f, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}

struct FileCloser {
    FILE* f;
    ~FileCloser() { fclose(f); }
};

// A stretch of the moov box in memory: a box's payload.
struct Span {
    const uint8_t* p = nullptr;
    const uint8_t* end = nullptr;
    size_t size() const { return static_cast<size_t>(end - p); }
};

// Calls fn(type, payload) for each box in a span. Stops at the first malformed box
// (some writers pad udta with four zero bytes).
template <class F>
void ForEachBox(Span s, F fn) {
    const uint8_t* p = s.p;
    while (s.end - p >= 8) {
        uint64_t size = Read32(p);
        uint32_t type = Read32(p + 4);
        size_t header = 8;
        if (size == 1) {
            if (s.end - p < 16) return;
            size = Read64(p + 8);
            header = 16;
        } else if (size == 0) {
            size = static_cast<uint64_t>(s.end - p);
        }
        if (size < header || size > static_cast<uint64_t>(s.end - p)) return;
        fn(type, Span{p + header, p + size});
        p += size;
    }
}

// The first box of a type in a span.
bool FindBox(Span s, uint32_t type, Span& out) {
    bool found = false;
    ForEachBox(s, [&](uint32_t t, Span payload) {
        if (!found && t == type) {
            found = true;
            out = payload;
        }
    });
    return found;
}

// The box at the end of a path of nested boxes.
bool FindPath(Span s, std::initializer_list<uint32_t> path, Span& out) {
    for (uint32_t type : path) {
        Span next;
        if (!FindBox(s, type, next)) return false;
        s = next;
    }
    out = s;
    return true;
}

// A full box's version, its first payload byte (three flag bytes follow).
uint8_t Version(Span s) {
    return s.size() ? s.p[0] : 0;
}

struct Track {
    uint32_t id = 0;
    uint32_t timescale = 0;               // media time units per second
    std::vector<uint32_t> chapterTracks;  // the ids of the tracks holding its chapters
    Span stbl;                            // its sample table
};

std::vector<Track> ReadTracks(Span moov) {
    std::vector<Track> tracks;
    ForEachBox(moov, [&](uint32_t type, Span trak) {
        if (type != Tag('t', 'r', 'a', 'k')) return;
        Track track;
        Span tkhd, mdhd, tref;
        if (FindBox(trak, Tag('t', 'k', 'h', 'd'), tkhd)) {
            size_t offset = Version(tkhd) == 1 ? 20 : 12;  // after the creation and modification times
            if (tkhd.size() >= offset + 4) track.id = Read32(tkhd.p + offset);
        }
        if (FindPath(trak, {Tag('m', 'd', 'i', 'a'), Tag('m', 'd', 'h', 'd')}, mdhd)) {
            size_t offset = Version(mdhd) == 1 ? 20 : 12;
            if (mdhd.size() >= offset + 4) track.timescale = Read32(mdhd.p + offset);
        }
        if (FindBox(trak, Tag('t', 'r', 'e', 'f'), tref)) {
            ForEachBox(tref, [&](uint32_t refType, Span ids) {
                if (refType != Tag('c', 'h', 'a', 'p')) return;
                for (size_t i = 0; i + 4 <= ids.size(); i += 4) {
                    track.chapterTracks.push_back(Read32(ids.p + i));
                }
            });
        }
        FindPath(trak, {Tag('m', 'd', 'i', 'a'), Tag('m', 'i', 'n', 'f'), Tag('s', 't', 'b', 'l')}, track.stbl);
        tracks.push_back(track);
    });
    return tracks;
}

// stts: the duration of each sample, in media time units.
bool SampleDurations(Span stbl, std::vector<uint32_t>& out) {
    Span stts;
    if (!FindBox(stbl, Tag('s', 't', 't', 's'), stts) || stts.size() < 8) return false;
    uint32_t entries = Read32(stts.p + 4);
    if (stts.size() < 8 + entries * 8ull) return false;
    for (uint32_t i = 0; i < entries; i++) {
        uint32_t count = Read32(stts.p + 8 + i * 8);
        uint32_t delta = Read32(stts.p + 12 + i * 8);
        if (out.size() + count > kMaxChapters) return false;
        out.insert(out.end(), count, delta);
    }
    return !out.empty();
}

// stsz: the size of each of `count` samples.
bool SampleSizes(Span stbl, size_t count, std::vector<uint32_t>& out) {
    Span stsz;
    if (!FindBox(stbl, Tag('s', 't', 's', 'z'), stsz) || stsz.size() < 12) return false;
    uint32_t fixed = Read32(stsz.p + 4);
    if (Read32(stsz.p + 8) < count) return false;
    if (fixed) {
        out.assign(count, fixed);
        return true;
    }
    if (stsz.size() < 12 + count * 4ull) return false;
    for (size_t i = 0; i < count; i++) out.push_back(Read32(stsz.p + 12 + i * 4));
    return true;
}

// stsc and stco/co64: where each sample starts in the file. The chunk offsets place
// each chunk, and stsc says how many samples each chunk holds, as runs of chunks.
bool SampleOffsets(Span stbl, const std::vector<uint32_t>& sizes, std::vector<uint64_t>& out) {
    Span stsc, stco;
    bool wide = false;
    if (!FindBox(stbl, Tag('s', 't', 's', 'c'), stsc) || stsc.size() < 8) return false;
    if (!FindBox(stbl, Tag('s', 't', 'c', 'o'), stco)) {
        if (!FindBox(stbl, Tag('c', 'o', '6', '4'), stco)) return false;
        wide = true;
    }
    if (stco.size() < 8) return false;
    uint32_t chunks = Read32(stco.p + 4);
    uint32_t entries = Read32(stsc.p + 4);
    if (stco.size() < 8 + chunks * (wide ? 8ull : 4ull) || stsc.size() < 8 + entries * 12ull || entries == 0) return false;
    size_t sample = 0;
    uint32_t entry = 0;
    for (uint32_t chunk = 1; chunk <= chunks && sample < sizes.size(); chunk++) {
        // The stsc entry in force: the last whose first chunk is at or before this one
        while (entry + 1 < entries && Read32(stsc.p + 8 + (entry + 1) * 12) <= chunk) entry++;
        uint32_t perChunk = Read32(stsc.p + 12 + entry * 12);
        uint64_t offset = wide ? Read64(stco.p + 8 + (chunk - 1) * 8) : Read32(stco.p + 8 + (chunk - 1) * 4);
        for (uint32_t i = 0; i < perChunk && sample < sizes.size(); i++, sample++) {
            out.push_back(offset);
            offset += sizes[sample];
        }
    }
    return out.size() == sizes.size();
}

// One sample of a chapter track: a 16-bit length and the title in UTF-8 (a QuickTime
// text sample may lead with a byte order mark), then boxes that add nothing here.
bool ReadTitle(FILE* f, uint64_t offset, uint32_t size, std::wstring& title) {
    if (size < 2 || !Seek(f, offset)) return false;
    uint8_t length[2];
    if (fread(length, 1, 2, f) != 2) return false;
    size_t n = std::min<size_t>(Read16(length), size - 2);
    n = std::min(n, kMaxTitleBytes);
    std::string text(n, '\0');
    if (n && fread(&text[0], 1, n, f) != n) return false;
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);
    while (!text.empty() && text.back() == '\0') text.pop_back();
    title = Utf8ToWide(text);
    return true;
}

// The chapters a chapter track holds: one per sample, starting where the sample does.
bool ChaptersFromTrack(FILE* f, const Track& track, std::vector<Chapter>& chapters) {
    if (!track.timescale || !track.stbl.p) return false;
    std::vector<uint32_t> durations, sizes;
    std::vector<uint64_t> offsets;
    if (!SampleDurations(track.stbl, durations) || !SampleSizes(track.stbl, durations.size(), sizes) ||
        !SampleOffsets(track.stbl, sizes, offsets)) {
        return false;
    }
    std::vector<Chapter> found;
    uint64_t time = 0;
    for (size_t i = 0; i < durations.size(); i++) {
        Chapter ch;
        ch.position = static_cast<double>(time) / track.timescale;
        time += durations[i];
        if (!ReadTitle(f, offsets[i], sizes[i], ch.name)) return false;
        found.push_back(std::move(ch));
    }
    chapters = std::move(found);
    return true;
}

// Nero's chpl box: a count, then each chapter's start in units of 100 nanoseconds
// and its title behind a length byte.
bool ChaptersFromChpl(Span moov, std::vector<Chapter>& chapters) {
    Span chpl;
    if (!FindPath(moov, {Tag('u', 'd', 't', 'a'), Tag('c', 'h', 'p', 'l')}, chpl)) return false;
    size_t pos = Version(chpl) ? 8 : 4;  // version 1 adds four reserved bytes
    if (chpl.size() < pos + 1) return false;
    uint8_t count = chpl.p[pos++];
    std::vector<Chapter> found;
    for (uint8_t i = 0; i < count; i++) {
        if (chpl.size() < pos + 9) return false;
        uint64_t start = Read64(chpl.p + pos);
        uint8_t length = chpl.p[pos + 8];
        pos += 9;
        if (chpl.size() < pos + length) return false;
        Chapter ch;
        ch.position = static_cast<double>(start) / 10000000.0;
        ch.name = Utf8ToWide(std::string(reinterpret_cast<const char*>(chpl.p + pos), length));
        pos += length;
        found.push_back(std::move(ch));
    }
    if (found.empty()) return false;
    chapters = std::move(found);
    return true;
}

}  // namespace

bool ReadMp4Chapters(const std::wstring& path, std::vector<Chapter>& chapters) {
    FILE* f = FileOpen(path, "rb");
    if (!f) return false;
    FileCloser closer{f};

    // The moov box, wherever it sits among the top-level boxes (behind the audio in
    // a file not written for streaming).
    std::vector<uint8_t> moov;
    uint64_t pos = 0;
    for (;;) {
        uint8_t header[16];
        if (!Seek(f, pos) || fread(header, 1, 8, f) != 8) break;
        uint64_t size = Read32(header);
        uint32_t type = Read32(header + 4);
        size_t headerSize = 8;
        if (size == 1) {
            if (fread(header + 8, 1, 8, f) != 8) break;
            size = Read64(header + 8);
            headerSize = 16;
        } else if (size == 0) {
            break;  // runs to the end of the file
        }
        if (size < headerSize) break;
        if (type == Tag('m', 'o', 'o', 'v')) {
            if (size - headerSize > kMaxMoovBytes) break;
            moov.resize(static_cast<size_t>(size - headerSize));
            if (fread(moov.data(), 1, moov.size(), f) != moov.size()) moov.clear();
            break;
        }
        pos += size;
    }
    if (moov.empty()) return false;
    Span all{moov.data(), moov.data() + moov.size()};

    // The chapter track first: chpl holds at most 255 chapters, and a tool that
    // writes both writes the same chapters twice.
    std::vector<Track> tracks = ReadTracks(all);
    std::vector<Chapter> found;
    for (const Track& track : tracks) {
        for (uint32_t id : track.chapterTracks) {
            for (const Track& chapterTrack : tracks) {
                if (chapterTrack.id == id && ChaptersFromTrack(f, chapterTrack, found)) break;
            }
            if (!found.empty()) break;
        }
        if (!found.empty()) break;
    }
    if (found.empty() && !ChaptersFromChpl(all, found)) return false;

    std::stable_sort(found.begin(), found.end(),
                     [](const Chapter& a, const Chapter& b) { return a.position < b.position; });
    for (size_t i = 0; i < found.size(); i++) {
        if (found[i].name.empty()) found[i].name = L"Chapter " + std::to_wstring(i + 1);
    }
    chapters = std::move(found);
    return true;
}
