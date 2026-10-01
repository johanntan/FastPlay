// A file played over HTTP, read through a local copy: see http_cache.h.

#include "http_cache.h"
#include "paths.h"
#include "utils.h"

extern "C" {
#include <libavformat/avio.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

namespace audio {
namespace {

const int kIoBuffer = 32 * 1024;
const int kChunk = 64 * 1024;
// A wanted byte this little ahead of the download is waited for, not jumped to
const int64_t kNear = 1024 * 1024;

int SeekFile(FILE* file, int64_t pos) {
#ifdef _WIN32
    return _fseeki64(file, pos, SEEK_SET);
#else
    return fseeko(file, static_cast<off_t>(pos), SEEK_SET);
#endif
}

}  // namespace

std::unique_ptr<HttpCache> HttpCache::Create(AVIOContext* source, int64_t size, int64_t position,
                                             std::function<void()> arm) {
    static std::atomic<int> counter{0};
    // Copies left behind by a FastPlay that did not get to delete them (one that
    // crashed): any a day old, once a run
    static std::once_flag tidied;
    std::call_once(tidied, []() {
        std::error_code ec;
        const auto old = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24);
        for (auto it = std::filesystem::directory_iterator(std::filesystem::path(GetTempDir()), ec);
             !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
            const std::wstring name = it->path().filename().wstring();
            if (name.rfind(L"fastplay-", 0) != 0 || it->path().extension() != L".download") continue;
            if (it->last_write_time(ec) < old) std::filesystem::remove(it->path(), ec);
        }
    });
    std::unique_ptr<HttpCache> cache(new HttpCache());
    cache->m_path = GetTempDir() + L"fastplay-" + std::to_wstring(getpid()) + L"-" + std::to_wstring(++counter) +
                    L".download";
    cache->m_file = FileOpen(cache->m_path, "w+b");
    if (!cache->m_file) return nullptr;
    unsigned char* buffer = static_cast<unsigned char*>(av_malloc(kIoBuffer));
    cache->m_io = buffer ? avio_alloc_context(buffer, kIoBuffer, 0, cache.get(), &HttpCache::Read, nullptr,
                                              &HttpCache::Seek)
                         : nullptr;
    if (!cache->m_io) {
        av_free(buffer);
        return nullptr;  // the destructor removes the file
    }
    cache->m_io->seekable = AVIO_SEEKABLE_NORMAL;
    cache->m_source = source;
    cache->m_size = size;
    cache->m_start = position;
    cache->m_pos = position;
    cache->m_arm = std::move(arm);
    // The demuxer carries on from where it was. (Not by avio_seek: a short way on,
    // it reads its way there instead, and that would wait on the download.)
    cache->m_io->pos = position;
    cache->m_thread = std::thread([c = cache.get()]() { c->Download(); });
    return cache;
}

HttpCache::~HttpCache() {
    Abort();
    if (m_thread.joinable()) m_thread.join();
    if (m_io) {
        av_freep(&m_io->buffer);
        avio_context_free(&m_io);
    }
    if (m_source) avio_closep(&m_source);
    if (m_file) fclose(m_file);
    std::error_code ec;
    if (!m_path.empty()) std::filesystem::remove(std::filesystem::path(m_path), ec);
}

void HttpCache::Abort() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_abort = true;
    m_wake.notify_all();
}

int64_t HttpCache::RunEnd(int64_t pos) const {
    auto it = m_have.upper_bound(pos);
    if (it == m_have.begin()) return pos;
    --it;
    return it->second > pos ? it->second : pos;
}

int64_t HttpCache::NextGap(int64_t from) const {
    int64_t pos = from;
    for (;;) {
        int64_t end = RunEnd(pos);
        if (end == pos) return pos;
        pos = end;
        if (pos >= m_size) return m_size;
    }
}

void HttpCache::Add(int64_t start, int64_t end) {
    // Merged with the runs it touches
    auto it = m_have.upper_bound(start);
    if (it != m_have.begin()) {
        auto before = std::prev(it);
        if (before->second >= start) {
            start = before->first;
            end = std::max(end, before->second);
            it = m_have.erase(before);
        }
    }
    while (it != m_have.end() && it->first <= end) {
        end = std::max(end, it->second);
        it = m_have.erase(it);
    }
    m_have[start] = end;
}

int HttpCache::Read(void* opaque, uint8_t* buffer, int size) {
    auto* self = static_cast<HttpCache*>(opaque);
    std::unique_lock<std::mutex> lock(self->m_mutex);
    if (self->m_pos >= self->m_size) return AVERROR_EOF;
    for (;;) {
        const int64_t end = self->RunEnd(self->m_pos);
        if (end > self->m_pos) {
            const int n = static_cast<int>(std::min<int64_t>(size, end - self->m_pos));
            if (SeekFile(self->m_file, self->m_pos) != 0) return AVERROR(EIO);
            const size_t got = fread(buffer, 1, static_cast<size_t>(n), self->m_file);
            if (got == 0) return AVERROR(EIO);
            self->m_pos += static_cast<int64_t>(got);
            return static_cast<int>(got);
        }
        if (self->m_abort) return AVERROR_EXIT;
        if (self->m_failed) return AVERROR(EIO);
        // Not here yet: the download goes here first
        self->m_want = self->m_pos;
        self->m_wake.notify_all();
        self->m_wake.wait_for(lock, std::chrono::milliseconds(50));
    }
}

int64_t HttpCache::Seek(void* opaque, int64_t offset, int whence) {
    auto* self = static_cast<HttpCache*>(opaque);
    if (whence & AVSEEK_SIZE) return self->m_size;
    whence &= ~AVSEEK_FORCE;
    std::lock_guard<std::mutex> lock(self->m_mutex);
    int64_t pos;
    switch (whence) {
        case SEEK_SET: pos = offset; break;
        case SEEK_CUR: pos = self->m_pos + offset; break;
        case SEEK_END: pos = self->m_size + offset; break;
        default: return AVERROR(EINVAL);
    }
    if (pos < 0) return AVERROR(EINVAL);
    self->m_pos = pos;
    return pos;
}

void HttpCache::Download() {
    std::vector<uint8_t> chunk(kChunk);
    int64_t at = m_start;  // where `source` is
    for (;;) {
        int64_t target;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_abort) return;
            target = at;
            // Where the demuxer waits, unless the download is about to get there anyway
            if (m_want >= 0) {
                if (RunEnd(m_want) == m_want && (m_want < at || m_want > at + kNear)) target = m_want;
                m_want = -1;
            }
            // On from there to the next part not yet downloaded, and when the end
            // is reached, back for what was skipped
            target = NextGap(target);
            if (target >= m_size) target = NextGap(0);
            if (target >= m_size) return;  // all of it
        }
        if (target != at) {
            m_arm();
            if (avio_seek(m_source, target, SEEK_SET) < 0) break;
            at = target;
        }
        m_arm();
        const int n = avio_read(m_source, chunk.data(), static_cast<int>(std::min<int64_t>(kChunk, m_size - at)));
        if (n <= 0) break;
        std::lock_guard<std::mutex> lock(m_mutex);
        if (SeekFile(m_file, at) != 0 || fwrite(chunk.data(), 1, static_cast<size_t>(n), m_file) != static_cast<size_t>(n)) {
            break;
        }
        Add(at, at + n);
        at += n;
        m_wake.notify_all();
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_failed = true;
    m_wake.notify_all();
}

}  // namespace audio
