#pragma once
#ifndef FASTPLAY_MP4_INDEX_H
#define FASTPLAY_MP4_INDEX_H

// Reading an MP4 file's index: the moov box, its tracks, and where each track's
// samples sit in the file. Shared by the chapter reader and the xHE-AAC decoder.
// (mp4_remux.cpp has a walker of its own that takes boxes apart to write them
// back; this one reads in place.)

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace mp4 {

constexpr uint32_t Tag(char a, char b, char c, char d) {
    return (static_cast<uint32_t>(static_cast<uint8_t>(a)) << 24) | (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 8) | static_cast<uint32_t>(static_cast<uint8_t>(d));
}

inline uint16_t Read16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

inline uint32_t Read32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

inline uint64_t Read64(const uint8_t* p) {
    return (static_cast<uint64_t>(Read32(p)) << 32) | Read32(p + 4);
}

// A stretch of the moov box in memory: a box's payload.
struct Span {
    const uint8_t* p = nullptr;
    const uint8_t* end = nullptr;
    size_t size() const { return static_cast<size_t>(end - p); }
    bool empty() const { return p == nullptr || end <= p; }
};

// Calls fn(type, payload) for each box in a span. Stops at the first malformed
// box (some writers pad udta with four zero bytes).
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
bool FindBox(Span s, uint32_t type, Span& out);

// The box at the end of a path of nested boxes.
bool FindPath(Span s, std::initializer_list<uint32_t> path, Span& out);

// A full box's version, its first payload byte (three flag bytes follow).
inline uint8_t Version(Span s) {
    return s.size() ? s.p[0] : 0;
}

// 64-bit fseek.
bool Seek(FILE* f, uint64_t offset);

// The moov box of an open file, wherever it sits among the top-level boxes
// (behind the audio in a file not written for streaming). False if there is
// none, or it is over `maxBytes`.
bool ReadMoov(FILE* f, std::vector<uint8_t>& moov, uint64_t maxBytes = 64 * 1024 * 1024);

struct Track {
    uint32_t id = 0;
    uint32_t timescale = 0;               // media time units per second
    uint32_t movieTimescale = 0;          // the movie's, which the edit list counts in
    std::vector<uint32_t> chapterTracks;  // the ids of the tracks holding its chapters
    Span stbl;                            // its sample table
    Span stsd;                            // its sample descriptions
    // The edit list's one entry, if it has one: where in the media the track
    // starts (media time units; an encoder's priming is skipped this way) and
    // how long it plays (movie time units, 0 for the whole media).
    int64_t editStart = 0;
    uint64_t editDuration = 0;
};

std::vector<Track> ReadTracks(Span moov);

// Where a track's samples are: the size and file offset of each, and their
// durations as stts holds them, in runs.
struct SampleTable {
    std::vector<uint32_t> sizes;
    std::vector<uint64_t> offsets;
    std::vector<std::pair<uint32_t, uint32_t>> timing;  // (count, duration) runs
    // The samples decoding can start at (stss), ascending. Empty: every sample.
    std::vector<uint32_t> sync;

    // Each sample's duration, media time units.
    std::vector<uint32_t> Durations() const;
};

// False if the table is malformed or has more than `maxSamples` samples.
bool ReadSamples(const Track& track, size_t maxSamples, SampleTable& out);

// The iTunes-style tags of the file (moov/udta/meta/ilst), as a BASS-style list
// of "NAME=value" strings: TITLE, ARTIST, ALBUM, YEAR, TRACK, GENRE, COMMENT,
// each followed by a null, the list ended by another. Empty if there are none.
std::string ReadTags(Span moov);

}  // namespace mp4

#endif // FASTPLAY_MP4_INDEX_H
