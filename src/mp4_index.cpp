// Reading an MP4 file's index. See mp4_index.h.

#include "mp4_index.h"

#include <algorithm>
#include <cstring>

namespace mp4 {

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

bool FindPath(Span s, std::initializer_list<uint32_t> path, Span& out) {
    for (uint32_t type : path) {
        Span next;
        if (!FindBox(s, type, next)) return false;
        s = next;
    }
    out = s;
    return true;
}

bool Seek(FILE* f, uint64_t offset) {
#ifdef _WIN32
    return _fseeki64(f, static_cast<__int64>(offset), SEEK_SET) == 0;
#else
    return fseeko(f, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}

bool ReadMoov(FILE* f, std::vector<uint8_t>& moov, uint64_t maxBytes) {
    moov.clear();
    uint64_t pos = 0;
    for (;;) {
        uint8_t header[16];
        if (!Seek(f, pos) || fread(header, 1, 8, f) != 8) return false;
        uint64_t size = Read32(header);
        uint32_t type = Read32(header + 4);
        size_t headerSize = 8;
        if (size == 1) {
            if (fread(header + 8, 1, 8, f) != 8) return false;
            size = Read64(header + 8);
            headerSize = 16;
        } else if (size == 0) {
            return false;  // runs to the end of the file
        }
        if (size < headerSize) return false;
        if (type == Tag('m', 'o', 'o', 'v')) {
            if (size - headerSize > maxBytes) return false;
            moov.resize(static_cast<size_t>(size - headerSize));
            if (fread(moov.data(), 1, moov.size(), f) != moov.size()) {
                moov.clear();
                return false;
            }
            return !moov.empty();
        }
        pos += size;
    }
}

std::vector<Track> ReadTracks(Span moov) {
    std::vector<Track> tracks;
    uint32_t movieTimescale = 0;
    Span mvhd;
    if (FindBox(moov, Tag('m', 'v', 'h', 'd'), mvhd)) {
        size_t offset = Version(mvhd) == 1 ? 20 : 12;
        if (mvhd.size() >= offset + 4) movieTimescale = Read32(mvhd.p + offset);
    }
    ForEachBox(moov, [&](uint32_t type, Span trak) {
        if (type != Tag('t', 'r', 'a', 'k')) return;
        Track track;
        track.movieTimescale = movieTimescale;
        Span tkhd, mdhd, tref, elst;
        if (FindPath(trak, {Tag('e', 'd', 't', 's'), Tag('e', 'l', 's', 't')}, elst) && elst.size() >= 8 &&
            Read32(elst.p + 4) == 1) {
            const uint8_t* e = elst.p + 8;
            if (Version(elst) == 1 && elst.size() >= 8 + 20) {
                track.editDuration = Read64(e);
                track.editStart = static_cast<int64_t>(Read64(e + 8));
            } else if (Version(elst) == 0 && elst.size() >= 8 + 12) {
                track.editDuration = Read32(e);
                track.editStart = static_cast<int32_t>(Read32(e + 4));
            }
            if (track.editStart < 0) track.editStart = 0;  // an empty edit: nothing to skip
        }
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
        if (FindPath(trak, {Tag('m', 'd', 'i', 'a'), Tag('m', 'i', 'n', 'f'), Tag('s', 't', 'b', 'l')}, track.stbl)) {
            FindBox(track.stbl, Tag('s', 't', 's', 'd'), track.stsd);
        }
        tracks.push_back(track);
    });
    return tracks;
}

std::vector<uint32_t> SampleTable::Durations() const {
    std::vector<uint32_t> out;
    out.reserve(sizes.size());
    for (const auto& run : timing) out.insert(out.end(), run.first, run.second);
    out.resize(sizes.size(), timing.empty() ? 0 : timing.back().second);
    return out;
}

namespace {

// stts: the samples' durations, as runs.
bool ReadTiming(Span stbl, size_t maxSamples, SampleTable& out) {
    Span stts;
    if (!FindBox(stbl, Tag('s', 't', 't', 's'), stts) || stts.size() < 8) return false;
    uint32_t entries = Read32(stts.p + 4);
    if (stts.size() < 8 + entries * 8ull) return false;
    uint64_t total = 0;
    for (uint32_t i = 0; i < entries; i++) {
        uint32_t count = Read32(stts.p + 8 + i * 8);
        uint32_t delta = Read32(stts.p + 12 + i * 8);
        total += count;
        if (total > maxSamples) return false;
        out.timing.emplace_back(count, delta);
    }
    return total > 0;
}

// stsz, or the compact stz2: the size of each sample.
bool ReadSizes(Span stbl, size_t maxSamples, SampleTable& out) {
    Span stsz;
    if (FindBox(stbl, Tag('s', 't', 's', 'z'), stsz)) {
        if (stsz.size() < 12) return false;
        uint32_t fixed = Read32(stsz.p + 4);
        uint32_t count = Read32(stsz.p + 8);
        if (count == 0 || count > maxSamples) return false;
        if (fixed) {
            out.sizes.assign(count, fixed);
            return true;
        }
        if (stsz.size() < 12 + count * 4ull) return false;
        out.sizes.reserve(count);
        for (uint32_t i = 0; i < count; i++) out.sizes.push_back(Read32(stsz.p + 12 + i * 4));
        return true;
    }
    Span stz2;
    if (!FindBox(stbl, Tag('s', 't', 'z', '2'), stz2) || stz2.size() < 12) return false;
    uint32_t bits = stz2.p[7];  // 4, 8 or 16 bits per size
    uint32_t count = Read32(stz2.p + 8);
    if (count == 0 || count > maxSamples || (bits != 4 && bits != 8 && bits != 16)) return false;
    if (stz2.size() < 12 + (static_cast<uint64_t>(count) * bits + 7) / 8) return false;
    const uint8_t* p = stz2.p + 12;
    out.sizes.reserve(count);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t size;
        if (bits == 16) size = Read16(p + i * 2);
        else if (bits == 8) size = p[i];
        else size = (i & 1) ? (p[i / 2] & 0x0F) : (p[i / 2] >> 4);
        out.sizes.push_back(size);
    }
    return true;
}

// stsc and stco/co64: where each sample starts in the file. The chunk offsets
// place each chunk, and stsc says how many samples each chunk holds, as runs.
bool ReadOffsets(Span stbl, SampleTable& out) {
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
    const std::vector<uint32_t>& sizes = out.sizes;
    out.offsets.reserve(sizes.size());
    size_t sample = 0;
    uint32_t entry = 0;
    for (uint32_t chunk = 1; chunk <= chunks && sample < sizes.size(); chunk++) {
        // The stsc entry in force: the last whose first chunk is at or before this one
        while (entry + 1 < entries && Read32(stsc.p + 8 + (entry + 1) * 12) <= chunk) entry++;
        uint32_t perChunk = Read32(stsc.p + 12 + entry * 12);
        uint64_t offset = wide ? Read64(stco.p + 8 + (chunk - 1) * 8) : Read32(stco.p + 8 + (chunk - 1) * 4);
        for (uint32_t i = 0; i < perChunk && sample < sizes.size(); i++, sample++) {
            out.offsets.push_back(offset);
            offset += sizes[sample];
        }
    }
    return out.offsets.size() == sizes.size();
}

// One ilst item's text, from its data box (a type, a locale, then the value).
std::string ItemText(Span item) {
    Span data;
    if (!FindBox(item, Tag('d', 'a', 't', 'a'), data) || data.size() < 8) return "";
    uint32_t type = Read32(data.p) & 0xFFFFFF;
    const uint8_t* value = data.p + 8;
    size_t n = data.size() - 8;
    if (type == 1) return std::string(reinterpret_cast<const char*>(value), n);  // UTF-8
    if (type == 0 && n >= 4) {  // binary: trkn / disk hold a padded pair of numbers
        return std::to_string(Read16(value + 2));
    }
    return "";
}

}  // namespace

// stss: the samples decoding can start at, 1-based in the file.
void ReadSync(Span stbl, SampleTable& out) {
    Span stss;
    if (!FindBox(stbl, Tag('s', 't', 's', 's'), stss) || stss.size() < 8) return;
    uint32_t count = Read32(stss.p + 4);
    if (stss.size() < 8 + count * 4ull) return;
    out.sync.reserve(count);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t sample = Read32(stss.p + 8 + i * 4);
        if (sample >= 1 && sample <= out.sizes.size()) out.sync.push_back(sample - 1);
    }
    std::sort(out.sync.begin(), out.sync.end());
}

bool ReadSamples(const Track& track, size_t maxSamples, SampleTable& out) {
    out = SampleTable();
    if (track.stbl.empty()) return false;
    if (!ReadTiming(track.stbl, maxSamples, out) || !ReadSizes(track.stbl, maxSamples, out) ||
        !ReadOffsets(track.stbl, out)) {
        out = SampleTable();
        return false;
    }
    ReadSync(track.stbl, out);
    return true;
}

std::string ReadTags(Span moov) {
    Span udta, meta, ilst;
    if (!FindBox(moov, Tag('u', 'd', 't', 'a'), udta) || !FindBox(udta, Tag('m', 'e', 't', 'a'), meta)) return "";
    // meta is a full box: its boxes follow the version and flags
    if (meta.size() < 4) return "";
    meta.p += 4;
    if (!FindBox(meta, Tag('i', 'l', 's', 't'), ilst)) return "";

    struct Name {
        uint32_t item;
        const char* tag;
    };
    static const Name kNames[] = {
        {Tag('\xA9', 'n', 'a', 'm'), "TITLE"},  {Tag('\xA9', 'A', 'R', 'T'), "ARTIST"},
        {Tag('\xA9', 'a', 'l', 'b'), "ALBUM"},  {Tag('\xA9', 'd', 'a', 'y'), "YEAR"},
        {Tag('t', 'r', 'k', 'n'), "TRACK"},     {Tag('\xA9', 'g', 'e', 'n'), "GENRE"},
        {Tag('\xA9', 'c', 'm', 't'), "COMMENT"},
    };
    std::string tags;
    ForEachBox(ilst, [&](uint32_t type, Span item) {
        for (const Name& name : kNames) {
            if (name.item != type) continue;
            std::string text = ItemText(item);
            if (text.empty()) continue;
            tags += name.tag;
            tags += '=';
            tags += text;
            tags += '\0';
        }
    });
    if (!tags.empty()) tags += '\0';
    return tags;
}

}  // namespace mp4
