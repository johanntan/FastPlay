// Watching folders on macOS: one FSEvents stream for all of them, reporting each
// file, on a queue of its own.

#include "folder_watch.h"
#include "utils.h"

#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>

namespace {

class MacFolderWatch : public FolderWatch {
public:
    MacFolderWatch(const std::vector<std::wstring>& roots, Handler handler) : m_handler(std::move(handler)) {
        CFMutableArrayRef paths = CFArrayCreateMutable(nullptr, 0, &kCFTypeArrayCallBacks);
        for (const std::wstring& root : roots) {
            std::string utf8 = WideToUtf8(root);
            while (utf8.size() > 1 && utf8.back() == '/') utf8.pop_back();
            m_roots.push_back(utf8);
            CFStringRef path = CFStringCreateWithCString(nullptr, utf8.c_str(), kCFStringEncodingUTF8);
            if (path) {
                CFArrayAppendValue(paths, path);
                CFRelease(path);
            }
        }
        FSEventStreamContext context = {0, this, nullptr, nullptr, nullptr};
        m_stream = FSEventStreamCreate(nullptr, &MacFolderWatch::OnEvents, &context, paths,
                                       kFSEventStreamEventIdSinceNow, 1.0,
                                       kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer |
                                           kFSEventStreamCreateFlagWatchRoot);
        CFRelease(paths);
        if (!m_stream) return;
        m_queue = dispatch_queue_create("FastPlay folder watch", DISPATCH_QUEUE_SERIAL);
        FSEventStreamSetDispatchQueue(m_stream, m_queue);
        FSEventStreamStart(m_stream);
    }

    ~MacFolderWatch() override {
        if (m_stream) {
            FSEventStreamStop(m_stream);
            FSEventStreamInvalidate(m_stream);
            FSEventStreamRelease(m_stream);
        }
        if (m_queue) {
            dispatch_sync_f(m_queue, nullptr, [](void*) {});  // anything still being handed on is done
            dispatch_release(m_queue);
        }
    }

private:
    static void OnEvents(ConstFSEventStreamRef, void* info, size_t count, void* eventPaths,
                         const FSEventStreamEventFlags flags[], const FSEventStreamEventId[]) {
        auto* self = static_cast<MacFolderWatch*>(info);
        char** paths = static_cast<char**>(eventPaths);
        // By root: the paths under it, and whether it must be looked at whole
        std::vector<std::vector<std::wstring>> changed(self->m_roots.size());
        std::vector<bool> everything(self->m_roots.size(), false);
        const FSEventStreamEventFlags lost = kFSEventStreamEventFlagMustScanSubDirs |
                                             kFSEventStreamEventFlagUserDropped |
                                             kFSEventStreamEventFlagKernelDropped |
                                             kFSEventStreamEventFlagRootChanged;
        for (size_t i = 0; i < count; i++) {
            std::string path = paths[i];
            for (size_t r = 0; r < self->m_roots.size(); r++) {
                const std::string& root = self->m_roots[r];
                if (path.compare(0, root.size(), root) != 0) continue;
                if (path.size() > root.size() && path[root.size()] != '/') continue;
                if (flags[i] & lost) {
                    everything[r] = true;
                } else {
                    changed[r].push_back(Utf8ToWide(path));
                }
                break;
            }
        }
        for (size_t r = 0; r < self->m_roots.size(); r++) {
            if (everything[r] || !changed[r].empty()) {
                self->m_handler(Utf8ToWide(self->m_roots[r]), changed[r], everything[r]);
            }
        }
    }

    Handler m_handler;
    std::vector<std::string> m_roots;
    FSEventStreamRef m_stream = nullptr;
    dispatch_queue_t m_queue = nullptr;
};

}  // namespace

std::unique_ptr<FolderWatch> FolderWatch::Start(const std::vector<std::wstring>& roots, Handler handler) {
    if (roots.empty()) return nullptr;
    return std::make_unique<MacFolderWatch>(roots, std::move(handler));
}
