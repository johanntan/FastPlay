#include "download_manager.h"
#include "globals.h"
#include "accessibility.h"
#include "app_ui.h"
#include "http.h"
#include <cstdio>
#include <filesystem>
#include <thread>

// Download one file (runs on its own thread) and report back on the UI thread.
static void DownloadWorker(int id, std::wstring url, std::wstring destPath, std::wstring headers,
                           std::shared_ptr<std::atomic<bool>> cancel) {
    // Create directory if needed
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::path(destPath).parent_path();
    if (!dir.empty()) std::filesystem::create_directory(dir, ec);

    HttpOptions options;
    options.saveTo = destPath;
    options.timeoutMs = 60000;  // don't hang on a stalled server
    size_t start = 0;
    while (start < headers.size()) {
        size_t end = headers.find(L"\r\n", start);
        if (end == std::wstring::npos) end = headers.size();
        if (end > start) options.headers.push_back(headers.substr(start, end - start));
        start = end + 2;
    }
    options.progress = [cancel](uint64_t, uint64_t) { return !*cancel; };

    HttpResult response = HttpGet(url, options);
    bool success = response.completed && !response.cancelled;
    if (response.cancelled) {
        std::filesystem::remove(std::filesystem::path(destPath), ec);
    }

    // Finish on the UI thread
    RunOnUiThread([id, success]() { DownloadManager::Instance().ProcessCompletion(id, success); });
}

DownloadManager& DownloadManager::Instance() {
    static DownloadManager instance;
    return instance;
}

DownloadManager::~DownloadManager() {
    CancelAll();
}

void DownloadManager::Enqueue(const std::wstring& url, const std::wstring& destPath, const std::wstring& title,
                              const std::wstring& headers) {
    std::unique_lock<std::recursive_mutex> lock(m_mutex);

    // Check if already queued or downloading
    for (const auto& item : m_queue) {
        if (item.url == url) {
            return;
        }
    }
    for (const auto& pair : m_active) {
        if (pair.second.url == url) {
            return;
        }
    }

    // Check if file already exists
    std::error_code ec;
    if (std::filesystem::exists(std::filesystem::path(destPath), ec)) {
        return;
    }

    DownloadItem item;
    item.id = m_nextId++;
    item.url = url;
    item.destPath = destPath;
    item.title = title;
    item.headers = headers;

    m_queue.push_back(item);

    // Reset batch tracking for single download
    m_batchTotal = 1;
    m_batchSuccess = 0;
    m_batchFailed = 0;

    lock.unlock();

    if (onQueueChanged) onQueueChanged();
    ProcessQueue();
}

void DownloadManager::EnqueueMultiple(const std::vector<std::tuple<std::wstring, std::wstring, std::wstring>>& items,
                                      const std::wstring& headers) {
    std::unique_lock<std::recursive_mutex> lock(m_mutex);

    // Reset batch tracking
    m_batchTotal = 0;
    m_batchSuccess = 0;
    m_batchFailed = 0;

    int addedCount = 0;
    for (const auto& tuple : items) {
        const std::wstring& url = std::get<0>(tuple);
        const std::wstring& destPath = std::get<1>(tuple);
        const std::wstring& title = std::get<2>(tuple);

        // Check if already queued or downloading
        bool exists = false;
        for (const auto& item : m_queue) {
            if (item.url == url) { exists = true; break; }
        }
        for (const auto& pair : m_active) {
            if (pair.second.url == url) { exists = true; break; }
        }
        if (exists) continue;

        // Check if file already exists
        std::error_code ec;
        if (std::filesystem::exists(std::filesystem::path(destPath), ec)) continue;

        DownloadItem item;
        item.id = m_nextId++;
        item.url = url;
        item.destPath = destPath;
        item.title = title;
        item.headers = headers;

        m_queue.push_back(item);
        addedCount++;
    }

    m_batchTotal = addedCount;

    lock.unlock();

    if (addedCount > 0) {
        if (onQueueChanged) onQueueChanged();
        ProcessQueue();
    }
}

void DownloadManager::CancelAll() {
    std::unique_lock<std::recursive_mutex> lock(m_mutex);

    // Cancel active downloads (each stops at its next chunk and removes its partial file)
    for (auto& pair : m_active) {
        if (pair.second.cancel) *pair.second.cancel = true;
    }
    m_active.clear();

    // Clear queue
    m_queue.clear();

    lock.unlock();

    if (onQueueChanged) onQueueChanged();
}

int DownloadManager::PendingCount() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return static_cast<int>(m_queue.size() + m_active.size());
}

int DownloadManager::ActiveCount() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return static_cast<int>(m_active.size());
}

int DownloadManager::QueuedCount() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return static_cast<int>(m_queue.size());
}

void DownloadManager::ProcessQueue() {
    std::unique_lock<std::recursive_mutex> lock(m_mutex);

    while (static_cast<int>(m_active.size()) < m_maxConcurrent && !m_queue.empty()) {
        DownloadItem item = m_queue.front();
        m_queue.erase(m_queue.begin());

        StartDownload(item);
        m_active[item.id] = item;
    }

    lock.unlock();
}

void DownloadManager::StartDownload(DownloadItem& item) {
    item.cancel = std::make_shared<std::atomic<bool>>(false);
    try {
        std::thread(DownloadWorker, item.id, item.url, item.destPath, item.headers, item.cancel).detach();
    } catch (const std::system_error&) {
        // Report the failure on the UI thread
        int id = item.id;
        RunOnUiThread([id]() { DownloadManager::Instance().ProcessCompletion(id, false); });
    }
}

void DownloadManager::ProcessCompletion(int id, bool success) {
    std::wstring title;

    std::unique_lock<std::recursive_mutex> lock(m_mutex);

    auto it = m_active.find(id);
    if (it != m_active.end()) {
        title = it->second.title;
        m_active.erase(it);
    }

    // Track batch stats
    if (success) {
        m_batchSuccess++;
    } else {
        m_batchFailed++;
    }

    bool allDone = m_active.empty() && m_queue.empty();
    int batchTotal = m_batchTotal;
    int batchSuccess = m_batchSuccess;
    int batchFailed = m_batchFailed;

    lock.unlock();

    // Fire callbacks
    if (onDownloadComplete && !title.empty()) {
        onDownloadComplete(title, success);
    }

    if (onQueueChanged) {
        onQueueChanged();
    }

    // Speak progress when all downloads complete
    if (allDone && batchTotal > 0) {
        char msg[128];
        if (batchTotal == 1) {
            // Single download
            Speak(success ? "Download complete" : "Download failed");
        } else {
            // Batch download
            if (batchFailed == 0) {
                snprintf(msg, sizeof(msg), "%d downloads complete", batchSuccess);
            } else {
                snprintf(msg, sizeof(msg), "%d complete, %d failed", batchSuccess, batchFailed);
            }
            Speak(msg);
        }
    }

    // Process next in queue
    ProcessQueue();

    // Check if all done
    if (allDone && onAllComplete) {
        onAllComplete();
    }
}
