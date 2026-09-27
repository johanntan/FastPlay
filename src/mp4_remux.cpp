// Fragmented MP4 to ordinary MP4. A fragmented file has an empty sample table in its
// moov box and describes the samples in moof boxes spread through the file; the
// ordinary file gathers all of that into one sample table (stts/stsc/stsz/stco) in
// front of a single mdat holding the audio.

#include "mp4_remux.h"
#include "utils.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;

constexpr uint32_t Tag(char a, char b, char c, char d) {
    return (static_cast<uint32_t>(static_cast<uint8_t>(a)) << 24) | (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 8) | static_cast<uint32_t>(static_cast<uint8_t>(d));
}

uint32_t Read32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

uint64_t Read64(const uint8_t* p) {
    return (static_cast<uint64_t>(Read32(p)) << 32) | Read32(p + 4);
}

void Put32(Bytes& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v >> 24));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

void Set32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

void Set64(uint8_t* p, uint64_t v) {
    Set32(p, static_cast<uint32_t>(v >> 32));
    Set32(p + 4, static_cast<uint32_t>(v));
}

struct Box {
    uint32_t type = 0;
    bool container = false;
    Bytes data;                // a leaf box's payload
    std::vector<Box> children; // a container box's boxes
};

bool IsContainer(uint32_t type) {
    switch (type) {
        case Tag('m', 'o', 'o', 'v'): case Tag('t', 'r', 'a', 'k'): case Tag('m', 'd', 'i', 'a'):
        case Tag('m', 'i', 'n', 'f'): case Tag('s', 't', 'b', 'l'): case Tag('m', 'v', 'e', 'x'):
        case Tag('m', 'o', 'o', 'f'): case Tag('t', 'r', 'a', 'f'):
            return true;
        default:
            return false;
    }
}

bool ParseBoxes(const uint8_t* p, const uint8_t* end, std::vector<Box>& out) {
    while (end - p >= 8) {
        uint64_t size = Read32(p);
        uint32_t type = Read32(p + 4);
        size_t header = 8;
        if (size == 1) {
            if (end - p < 16) return false;
            size = Read64(p + 8);
            header = 16;
        } else if (size == 0) {
            size = static_cast<uint64_t>(end - p);
        }
        if (size < header || size > static_cast<uint64_t>(end - p)) return false;
        Box box;
        box.type = type;
        if (IsContainer(type)) {
            box.container = true;
            if (!ParseBoxes(p + header, p + size, box.children)) return false;
        } else {
            box.data.assign(p + header, p + size);
        }
        out.push_back(std::move(box));
        p += size;
    }
    return true;
}

void Serialize(const Box& box, Bytes& out) {
    size_t start = out.size();
    Put32(out, 0);
    Put32(out, box.type);
    if (box.container) {
        for (const auto& child : box.children) Serialize(child, out);
    } else {
        out.insert(out.end(), box.data.begin(), box.data.end());
    }
    Set32(&out[start], static_cast<uint32_t>(out.size() - start));
}

Box* Find(std::vector<Box>& boxes, uint32_t type) {
    for (auto& box : boxes) {
        if (box.type == type) return &box;
    }
    return nullptr;
}

Box Leaf(uint32_t type, Bytes data) {
    Box box;
    box.type = type;
    box.data = std::move(data);
    return box;
}

// The duration field of mvhd, tkhd and mdhd, which sits at a version-dependent offset.
void SetDuration(Box& box, uint64_t duration) {
    Bytes& d = box.data;
    if (d.empty()) return;
    bool v1 = d[0] == 1;
    size_t offset;
    switch (box.type) {
        case Tag('t', 'k', 'h', 'd'): offset = v1 ? 28 : 20; break;  // after the track ID and a reserved word
        default: offset = v1 ? 24 : 16; break;                      // mvhd, mdhd: after the timescale
    }
    if (d.size() < offset + (v1 ? 8 : 4)) return;
    if (v1) {
        Set64(&d[offset], duration);
    } else {
        Set32(&d[offset], duration > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<uint32_t>(duration));
    }
}

uint32_t Timescale(const Box& box) {
    const Bytes& d = box.data;
    if (d.empty()) return 0;
    size_t offset = d[0] == 1 ? 20 : 12;
    return d.size() >= offset + 4 ? Read32(&d[offset]) : 0;
}

// A run of consecutive samples in the input file (one trun).
struct Run {
    uint64_t offset = 0;
    std::vector<uint32_t> sizes;
    std::vector<uint32_t> durations;
};

struct Defaults {
    uint32_t duration = 0;
    uint32_t size = 0;
};

// Collect the samples described by one moof box, which starts at `moofStart`.
bool ReadFragment(std::vector<Box>& moof, uint64_t moofStart, uint32_t trackId, const Defaults& trex,
                  std::vector<Run>& runs) {
    for (auto& traf : moof) {
        if (traf.type != Tag('t', 'r', 'a', 'f')) continue;
        Box* tfhd = Find(traf.children, Tag('t', 'f', 'h', 'd'));
        if (!tfhd || tfhd->data.size() < 8) return false;
        const uint8_t* t = tfhd->data.data();
        uint32_t flags = Read32(t) & 0xFFFFFF;
        if (Read32(t + 4) != trackId) continue;
        size_t pos = 8;
        uint64_t base = moofStart;
        Defaults defaults = trex;
        auto need = [&](size_t n) { return tfhd->data.size() >= pos + n; };
        if (flags & 0x01) {
            if (!need(8)) return false;
            base = Read64(t + pos);
            pos += 8;
        }
        if (flags & 0x02) pos += 4;  // sample description index
        if (flags & 0x08) {
            if (!need(4)) return false;
            defaults.duration = Read32(t + pos);
            pos += 4;
        }
        if (flags & 0x10) {
            if (!need(4)) return false;
            defaults.size = Read32(t + pos);
            pos += 4;
        }

        uint64_t next = base;  // where a trun without a data offset starts
        for (auto& trun : traf.children) {
            if (trun.type != Tag('t', 'r', 'u', 'n')) continue;
            const Bytes& d = trun.data;
            if (d.size() < 8) return false;
            uint32_t tflags = Read32(d.data()) & 0xFFFFFF;
            uint32_t count = Read32(d.data() + 4);
            size_t p = 8;
            Run run;
            run.offset = next;
            if (tflags & 0x01) {
                if (d.size() < p + 4) return false;
                run.offset = base + static_cast<int32_t>(Read32(d.data() + p));
                p += 4;
            }
            if (tflags & 0x04) p += 4;  // first sample flags
            size_t perSample = ((tflags & 0x100) ? 4 : 0) + ((tflags & 0x200) ? 4 : 0) +
                               ((tflags & 0x400) ? 4 : 0) + ((tflags & 0x800) ? 4 : 0);
            if (d.size() < p + static_cast<uint64_t>(count) * perSample) return false;
            uint64_t total = 0;
            for (uint32_t i = 0; i < count; i++) {
                uint32_t duration = defaults.duration, size = defaults.size;
                if (tflags & 0x100) { duration = Read32(d.data() + p); p += 4; }
                if (tflags & 0x200) { size = Read32(d.data() + p); p += 4; }
                if (tflags & 0x400) p += 4;
                if (tflags & 0x800) p += 4;  // composition offsets: none in audio
                run.durations.push_back(duration);
                run.sizes.push_back(size);
                total += size;
            }
            next = run.offset + total;
            if (count) runs.push_back(std::move(run));
        }
    }
    return true;
}

// udta/meta/ilst with the title and artist, as iTunes writes them.
Box MakeTags(const std::wstring& title, const std::wstring& artist) {
    auto item = [](uint32_t type, const std::wstring& text) {
        std::string utf8 = WideToUtf8(text);
        Bytes data;
        Put32(data, 1);  // UTF-8 text
        Put32(data, 0);  // locale
        data.insert(data.end(), utf8.begin(), utf8.end());
        Box value = Leaf(Tag('d', 'a', 't', 'a'), std::move(data));
        Box box;
        box.type = type;
        box.container = true;
        box.children.push_back(std::move(value));
        return box;
    };
    Box ilst;
    ilst.type = Tag('i', 'l', 's', 't');
    ilst.container = true;
    if (!title.empty()) ilst.children.push_back(item(Tag('\xA9', 'n', 'a', 'm'), title));
    if (!artist.empty()) ilst.children.push_back(item(Tag('\xA9', 'A', 'R', 'T'), artist));

    Bytes hdlr = {0, 0, 0, 0, 0, 0, 0, 0, 'm', 'd', 'i', 'r', 'a', 'p', 'p', 'l', 0, 0, 0, 0, 0, 0, 0, 0, 0};
    Bytes meta = {0, 0, 0, 0};  // version and flags: meta is a full box
    Serialize(Leaf(Tag('h', 'd', 'l', 'r'), hdlr), meta);
    Serialize(ilst, meta);

    Box udta;
    udta.type = Tag('u', 'd', 't', 'a');
    udta.container = true;
    udta.children.push_back(Leaf(Tag('m', 'e', 't', 'a'), std::move(meta)));
    return udta;
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
    ~FileCloser() {
        if (f) fclose(f);
    }
};

}  // namespace

bool DefragmentMp4(const std::wstring& inPath, const std::wstring& outPath,
                   const std::wstring& title, const std::wstring& artist) {
    FILE* in = FileOpen(inPath, "rb");
    if (!in) return false;
    FileCloser inCloser{in};

    // Walk the top-level boxes: keep moov, read each moof's samples, skip the rest.
    std::vector<Box> moov;
    std::vector<std::pair<uint64_t, std::vector<Box>>> fragments;  // moof start, its boxes
    uint64_t pos = 0;
    for (;;) {
        uint8_t header[16];
        if (!Seek(in, pos) || fread(header, 1, 8, in) != 8) break;
        uint64_t size = Read32(header);
        uint32_t type = Read32(header + 4);
        size_t headerSize = 8;
        if (size == 1) {
            if (fread(header + 8, 1, 8, in) != 8) return false;
            size = Read64(header + 8);
            headerSize = 16;
        } else if (size == 0) {
            break;  // runs to the end of the file (a final mdat)
        }
        if (size < headerSize) return false;
        if (type == Tag('m', 'o', 'o', 'v') || type == Tag('m', 'o', 'o', 'f')) {
            if (size > 64 * 1024 * 1024) return false;
            Bytes body(static_cast<size_t>(size - headerSize));
            if (fread(body.data(), 1, body.size(), in) != body.size()) return false;
            std::vector<Box> boxes;
            if (!ParseBoxes(body.data(), body.data() + body.size(), boxes)) return false;
            if (type == Tag('m', 'o', 'o', 'v')) {
                moov = std::move(boxes);
            } else {
                fragments.emplace_back(pos, std::move(boxes));
            }
        }
        pos += size;
    }
    if (moov.empty() || fragments.empty()) return false;  // not a fragmented MP4

    Box* trak = Find(moov, Tag('t', 'r', 'a', 'k'));
    Box* mvhd = Find(moov, Tag('m', 'v', 'h', 'd'));
    if (!trak || !mvhd) return false;
    Box* tkhd = Find(trak->children, Tag('t', 'k', 'h', 'd'));
    Box* mdia = Find(trak->children, Tag('m', 'd', 'i', 'a'));
    Box* mdhd = mdia ? Find(mdia->children, Tag('m', 'd', 'h', 'd')) : nullptr;
    Box* minf = mdia ? Find(mdia->children, Tag('m', 'i', 'n', 'f')) : nullptr;
    Box* stbl = minf ? Find(minf->children, Tag('s', 't', 'b', 'l')) : nullptr;
    Box* stsd = stbl ? Find(stbl->children, Tag('s', 't', 's', 'd')) : nullptr;
    if (!tkhd || !mdhd || !stsd || tkhd->data.size() < 16) return false;
    const Bytes& tk = tkhd->data;
    uint32_t trackId = Read32(&tk[tk[0] == 1 ? 20 : 12]);

    Defaults trex;
    if (Box* mvex = Find(moov, Tag('m', 'v', 'e', 'x'))) {
        for (auto& box : mvex->children) {
            if (box.type == Tag('t', 'r', 'e', 'x') && box.data.size() >= 24 && Read32(&box.data[4]) == trackId) {
                trex.duration = Read32(&box.data[12]);
                trex.size = Read32(&box.data[16]);
            }
        }
    }

    std::vector<Run> runs;
    for (auto& fragment : fragments) {
        if (!ReadFragment(fragment.second, fragment.first, trackId, trex, runs)) return false;
    }
    if (runs.empty()) return false;

    // The new sample table: one chunk per run, in order.
    uint64_t totalDuration = 0, totalBytes = 0;
    uint32_t sampleCount = 0;
    Bytes stts, stsc, stsz, stco;
    {
        std::vector<std::pair<uint32_t, uint32_t>> timeRuns;   // (count, duration)
        std::vector<std::pair<uint32_t, uint32_t>> chunkRuns;  // (first chunk, samples per chunk)
        uint32_t chunk = 1;
        for (const auto& run : runs) {
            for (size_t i = 0; i < run.sizes.size(); i++) {
                uint32_t duration = run.durations[i];
                if (!timeRuns.empty() && timeRuns.back().second == duration) {
                    timeRuns.back().first++;
                } else {
                    timeRuns.emplace_back(1, duration);
                }
                totalDuration += duration;
                totalBytes += run.sizes[i];
            }
            uint32_t perChunk = static_cast<uint32_t>(run.sizes.size());
            if (chunkRuns.empty() || chunkRuns.back().second != perChunk) chunkRuns.emplace_back(chunk, perChunk);
            sampleCount += perChunk;
            chunk++;
        }
        if (totalBytes > 0xF0000000u) return false;  // beyond 32-bit chunk offsets; no audio is this big

        Put32(stts, 0);
        Put32(stts, static_cast<uint32_t>(timeRuns.size()));
        for (const auto& t : timeRuns) {
            Put32(stts, t.first);
            Put32(stts, t.second);
        }
        Put32(stsc, 0);
        Put32(stsc, static_cast<uint32_t>(chunkRuns.size()));
        for (const auto& c : chunkRuns) {
            Put32(stsc, c.first);
            Put32(stsc, c.second);
            Put32(stsc, 1);  // sample description index
        }
        Put32(stsz, 0);
        Put32(stsz, 0);  // sizes vary
        Put32(stsz, sampleCount);
        for (const auto& run : runs) {
            for (uint32_t size : run.sizes) Put32(stsz, size);
        }
        Put32(stco, 0);
        Put32(stco, static_cast<uint32_t>(runs.size()));
        for (size_t i = 0; i < runs.size(); i++) Put32(stco, 0);  // filled in below
    }

    // Rebuild moov: durations set, the sample table replaced, no fragment or edit boxes.
    uint32_t mediaScale = Timescale(*mdhd), movieScale = Timescale(*mvhd);
    if (!mediaScale || !movieScale) return false;
    uint64_t movieDuration = totalDuration * movieScale / mediaScale;
    SetDuration(*mdhd, totalDuration);
    SetDuration(*tkhd, movieDuration);
    SetDuration(*mvhd, movieDuration);

    Box sampleDescriptions = *stsd;
    stbl->children.clear();
    stbl->children.push_back(std::move(sampleDescriptions));
    stbl->children.push_back(Leaf(Tag('s', 't', 't', 's'), std::move(stts)));
    stbl->children.push_back(Leaf(Tag('s', 't', 's', 'c'), std::move(stsc)));
    stbl->children.push_back(Leaf(Tag('s', 't', 's', 'z'), std::move(stsz)));
    stbl->children.push_back(Leaf(Tag('s', 't', 'c', 'o'), std::move(stco)));

    std::vector<Box> trakChildren;
    for (auto& box : trak->children) {
        if (box.type != Tag('e', 'd', 't', 's')) trakChildren.push_back(std::move(box));
    }
    trak->children = std::move(trakChildren);

    Box newMoov;
    newMoov.type = Tag('m', 'o', 'o', 'v');
    newMoov.container = true;
    for (auto& box : moov) {
        if (box.type == Tag('m', 'v', 'e', 'x') || box.type == Tag('u', 'd', 't', 'a')) continue;
        newMoov.children.push_back(box);
        if (box.type == Tag('t', 'r', 'a', 'k')) break;  // the one audio track
    }
    newMoov.children.push_back(MakeTags(title, artist));

    // Layout: ftyp, moov, mdat. The moov size does not depend on the offsets in it.
    Bytes ftyp;
    Serialize(Leaf(Tag('f', 't', 'y', 'p'), {'M', '4', 'A', ' ', 0, 0, 2, 0, 'M', '4', 'A', ' ', 'm', 'p', '4', '2',
                                               'i', 's', 'o', 'm'}),
              ftyp);
    Bytes moovBytes;
    Serialize(newMoov, moovBytes);
    uint64_t dataStart = ftyp.size() + moovBytes.size() + 8;

    // Fill in the chunk offsets: find stco inside the serialized moov and write them.
    {
        Box* newTrak = Find(newMoov.children, Tag('t', 'r', 'a', 'k'));
        Box* newStco = Find(Find(Find(Find(newTrak->children, Tag('m', 'd', 'i', 'a'))->children,
                                      Tag('m', 'i', 'n', 'f'))->children, Tag('s', 't', 'b', 'l'))->children,
                            Tag('s', 't', 'c', 'o'));
        uint64_t offset = dataStart;
        for (size_t i = 0; i < runs.size(); i++) {
            Set32(&newStco->data[8 + i * 4], static_cast<uint32_t>(offset));
            for (uint32_t size : runs[i].sizes) offset += size;
        }
        moovBytes.clear();
        Serialize(newMoov, moovBytes);
    }

    FILE* out = FileOpen(outPath, "wb");
    if (!out) return false;
    FileCloser outCloser{out};
    Bytes mdatHeader;
    Put32(mdatHeader, static_cast<uint32_t>(totalBytes + 8));
    Put32(mdatHeader, Tag('m', 'd', 'a', 't'));
    if (fwrite(ftyp.data(), 1, ftyp.size(), out) != ftyp.size() ||
        fwrite(moovBytes.data(), 1, moovBytes.size(), out) != moovBytes.size() ||
        fwrite(mdatHeader.data(), 1, mdatHeader.size(), out) != mdatHeader.size()) {
        return false;
    }

    std::vector<uint8_t> buffer(256 * 1024);
    for (const auto& run : runs) {
        uint64_t remaining = 0;
        for (uint32_t size : run.sizes) remaining += size;
        if (!Seek(in, run.offset)) return false;
        while (remaining > 0) {
            size_t n = static_cast<size_t>(remaining < buffer.size() ? remaining : buffer.size());
            if (fread(buffer.data(), 1, n, in) != n || fwrite(buffer.data(), 1, n, out) != n) return false;
            remaining -= n;
        }
    }
    return fflush(out) == 0;
}
