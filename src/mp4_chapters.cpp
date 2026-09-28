// The chapters of an MP4 file: the QuickTime chapter track and the Nero chpl box.
// See mp4_chapters.h. Only the moov box (the file's index) is read, plus the chapter
// titles themselves, so a long audiobook costs no more than a short one.

#include "mp4_chapters.h"
#include "mp4_index.h"
#include "utils.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace mp4;

namespace {

const size_t kMaxChapters = 10000;
const size_t kMaxTitleBytes = 1024;

struct FileCloser {
    FILE* f;
    ~FileCloser() { fclose(f); }
};

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
    if (!track.timescale) return false;
    SampleTable table;
    if (!ReadSamples(track, kMaxChapters, table)) return false;
    std::vector<uint32_t> durations = table.Durations();
    std::vector<Chapter> found;
    uint64_t time = 0;
    for (size_t i = 0; i < table.sizes.size(); i++) {
        Chapter ch;
        ch.position = static_cast<double>(time) / track.timescale;
        time += durations[i];
        if (!ReadTitle(f, table.offsets[i], table.sizes[i], ch.name)) return false;
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

    std::vector<uint8_t> moov;
    if (!ReadMoov(f, moov)) return false;
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
