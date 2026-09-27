// Systems FastPlay cannot update itself on: updates are found and downloaded, but
// installing them is left to the user.

#include "updater.h"
#include "updater_internal.h"
#include "app_ui.h"
#include "paths.h"

std::wstring UpdateZipPath() {
    return GetTempDir() + L"FastPlay-update.zip";
}

std::wstring UpdateInstallerPath() {
    return GetTempDir() + L"FastPlay-update.bin";
}

void ApplyUpdate() {
    ShowMessage(L"FastPlay cannot install updates on this system yet. The update was downloaded to " +
                    UpdateZipPath() + L".",
                L"Update", MessageIcon::Info);
}
