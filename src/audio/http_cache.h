#pragma once
#ifndef FASTPLAY_HTTP_CACHE_H
#define FASTPLAY_HTTP_CACHE_H

// A file played over HTTP, read through a local copy that fills in as it
// downloads. Without it every seek is a new request to the server, and the wait
// for its answer (most of a second, or two); with it, a seek to anywhere already
// downloaded, behind or ahead, is at once. A seek past what has come so far makes
// the download go there first, and it fills in the rest after.
//
// The copy is a temporary file, deleted when the file is closed.

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

struct AVIOContext;

namespace audio {

class HttpCache {
public:
    // Takes over `source` (open at byte `position` of a file of `size` bytes), and
    // downloads from there on a thread of its own. `arm` is called before each
    // read from the network (the decoder's timeout). Null if no temporary file
    // could be made; `source` is then left as it was.
    static std::unique_ptr<HttpCache> Create(AVIOContext* source, int64_t size, int64_t position,
                                             std::function<void()> arm);
    ~HttpCache();

    // What the demuxer reads from instead of `source`, at `position`
    AVIOContext* Io() const { return m_io; }
    // Gives up waiting on the network (the decoder is going)
    void Abort();

private:
    HttpCache() = default;
    static int Read(void* opaque, uint8_t* buffer, int size);
    static int64_t Seek(void* opaque, int64_t offset, int whence);
    void Download();
    int64_t RunEnd(int64_t pos) const;   // end of the downloaded run at pos (pos if none)
    int64_t NextGap(int64_t from) const;  // the first byte from `from` not downloaded (size if none)
    void Add(int64_t start, int64_t end);

    AVIOContext* m_source = nullptr;
    AVIOContext* m_io = nullptr;
    std::function<void()> m_arm;
    int64_t m_size = 0;
    int64_t m_start = 0;
    std::wstring m_path;
    FILE* m_file = nullptr;

    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::map<int64_t, int64_t> m_have;  // downloaded runs: start -> end
    int64_t m_pos = 0;                  // where the demuxer reads
    int64_t m_want = -1;                // where it waits for the download to get to
    bool m_failed = false, m_abort = false;
    std::thread m_thread;
};

}  // namespace audio

#endif  // FASTPLAY_HTTP_CACHE_H
