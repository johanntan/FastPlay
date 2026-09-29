// Watching folders where FastPlay has no way to: nothing is watched (the library
// is brought up to date when FastPlay starts and when asked).

#include "folder_watch.h"

std::unique_ptr<FolderWatch> FolderWatch::Start(const std::vector<std::wstring>&, Handler) {
    return nullptr;
}
