#include "platform.h"

#include <windows.h>

void PlatformStartup() {
    // The BASS DLLs are delay-loaded from the lib folder next to the executable.
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(exePath, L'\\');
    if (lastSlash) {
        *(lastSlash + 1) = L'\0';
        wcscat_s(exePath, MAX_PATH, L"lib");
        SetDllDirectoryW(exePath);
    }
}
