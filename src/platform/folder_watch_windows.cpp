// Watching folders on Windows: ReadDirectoryChangesW on each, overlapped, from one
// thread that waits on all of them at once.

#include "folder_watch.h"

#include <windows.h>

#include <thread>
#include <vector>

namespace {

class WindowsFolderWatch : public FolderWatch {
public:
    WindowsFolderWatch(const std::vector<std::wstring>& roots, Handler handler) : m_handler(std::move(handler)) {
        m_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        // WaitForMultipleObjects takes 64 handles, one of them the stop event
        for (const std::wstring& root : roots) {
            if (m_folders.size() >= MAXIMUM_WAIT_OBJECTS - 1) break;
            HANDLE dir = CreateFileW(root.c_str(), FILE_LIST_DIRECTORY,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                     FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
            if (dir == INVALID_HANDLE_VALUE) continue;
            auto folder = std::make_unique<Folder>();
            folder->root = root;
            if (!folder->root.empty() && folder->root.back() != L'\\' && folder->root.back() != L'/') {
                folder->root += L'\\';
            }
            folder->handle = dir;
            folder->overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            folder->buffer.resize(64 * 1024);  // the most a network share takes
            m_folders.push_back(std::move(folder));
        }
        m_thread = std::thread([this]() { Run(); });
    }

    ~WindowsFolderWatch() override {
        SetEvent(m_stop);
        if (m_thread.joinable()) m_thread.join();
        for (auto& folder : m_folders) {
            CancelIoEx(folder->handle, &folder->overlapped);
            DWORD ignored;
            GetOverlappedResult(folder->handle, &folder->overlapped, &ignored, TRUE);
            CloseHandle(folder->overlapped.hEvent);
            CloseHandle(folder->handle);
        }
        CloseHandle(m_stop);
    }

private:
    struct Folder {
        std::wstring root;  // with a trailing separator
        HANDLE handle = INVALID_HANDLE_VALUE;
        OVERLAPPED overlapped = {};
        std::vector<DWORD> buffer;  // DWORD-aligned, as the notifications must be
        bool listening = false;
    };

    static const DWORD kWatched = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                  FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE;

    bool Listen(Folder& folder) {
        ResetEvent(folder.overlapped.hEvent);
        folder.listening = ReadDirectoryChangesW(folder.handle, folder.buffer.data(),
                                                 static_cast<DWORD>(folder.buffer.size() * sizeof(DWORD)), TRUE,
                                                 kWatched, nullptr, &folder.overlapped, nullptr) != 0;
        return folder.listening;
    }

    void Run() {
        std::vector<HANDLE> events;
        for (auto& folder : m_folders) Listen(*folder);
        for (;;) {
            events.assign(1, m_stop);
            std::vector<Folder*> waiting;
            for (auto& folder : m_folders) {
                if (!folder->listening) continue;
                events.push_back(folder->overlapped.hEvent);
                waiting.push_back(folder.get());
            }
            DWORD which = WaitForMultipleObjects(static_cast<DWORD>(events.size()), events.data(), FALSE, INFINITE);
            if (which == WAIT_OBJECT_0 || which == WAIT_FAILED) return;
            size_t index = which - WAIT_OBJECT_0 - 1;
            if (index >= waiting.size()) continue;
            Folder& folder = *waiting[index];
            DWORD bytes = 0;
            bool ok = GetOverlappedResult(folder.handle, &folder.overlapped, &bytes, FALSE) != 0;
            std::vector<std::wstring> paths;
            // Nothing in the buffer: more changed than it could hold
            bool everything = !ok || bytes == 0;
            if (!everything) {
                const BYTE* at = reinterpret_cast<const BYTE*>(folder.buffer.data());
                for (;;) {
                    const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(at);
                    paths.push_back(folder.root +
                                    std::wstring(info->FileName, info->FileNameLength / sizeof(wchar_t)));
                    if (info->NextEntryOffset == 0) break;
                    at += info->NextEntryOffset;
                }
            }
            // Listen again before handing on, so nothing is missed meanwhile
            std::wstring root = folder.root;
            Listen(folder);
            m_handler(root, paths, everything);
        }
    }

    Handler m_handler;
    HANDLE m_stop = nullptr;
    std::vector<std::unique_ptr<Folder>> m_folders;
    std::thread m_thread;
};

}  // namespace

std::unique_ptr<FolderWatch> FolderWatch::Start(const std::vector<std::wstring>& roots, Handler handler) {
    if (roots.empty()) return nullptr;
    return std::make_unique<WindowsFolderWatch>(roots, std::move(handler));
}
