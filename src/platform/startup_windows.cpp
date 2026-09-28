#include "platform.h"

#include <windows.h>

#include <string>

void PlatformStartup() {
    // The screen reader client DLLs load from the lib folder next to the executable.
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(exePath, L'\\');
    if (lastSlash) {
        *(lastSlash + 1) = L'\0';
        wcscat_s(exePath, MAX_PATH, L"lib");
        SetDllDirectoryW(exePath);
    }
}

std::string GetSystemDescription() {
    // GetVersionEx reports what the manifest claims to support; RtlGetVersion does not.
    RTL_OSVERSIONINFOW version = {sizeof(version)};
    using RtlGetVersionFn = LONG(WINAPI*)(RTL_OSVERSIONINFOW*);
    auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(
        ::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    if (!rtlGetVersion || rtlGetVersion(&version) != 0) return "Windows";

    unsigned major = version.dwMajorVersion, minor = version.dwMinorVersion, build = version.dwBuildNumber;
    std::string name = "Windows";
    if (major == 10) name += build >= 22000 ? " 11" : " 10";
    else if (major == 6 && minor == 3) name += " 8.1";
    else if (major == 6 && minor == 2) name += " 8";
    else if (major == 6 && minor == 1) name += " 7";
    name += " " + std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(build);
#if defined(_M_ARM64)
    return name + "; ARM64";
#elif defined(_M_X64)
    return name + "; x64";
#else
    return name + "; x86";
#endif
}
