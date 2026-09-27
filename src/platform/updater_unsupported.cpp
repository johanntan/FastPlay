// Updates where FastPlay has no updater yet: checking reports that, and nothing is
// downloaded or applied.

#include "updater.h"

UpdateInfo CheckForUpdates() {
    UpdateInfo info{};
    info.available = false;
    info.errorMessage = "Automatic updates are not available on this system yet.";
    return info;
}

bool DownloadUpdate(const std::string&, DownloadProgressCallback) {
    return false;
}

void ApplyUpdate() {}

void CheckForUpdatesOnStartup() {}
