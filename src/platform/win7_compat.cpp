// Windows 7 support. Newer MSVC runtime libraries call CreateFile2, CopyFile2
// and GetSystemTimePreciseAsFileTime, which Windows 7 lacks, and an import it cannot
// resolve stops FastPlay from starting at all. This file defines those import slots
// itself, so the linker takes them from here instead of from kernel32: the real
// function where there is one, otherwise the nearest Windows 7 equivalent.

#include <windows.h>

namespace {

HANDLE WINAPI CreateFile2Compat(LPCWSTR fileName, DWORD access, DWORD shareMode, DWORD disposition,
                                LPCREATEFILE2_EXTENDED_PARAMETERS params) {
    using CreateFile2Fn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, DWORD, LPCREATEFILE2_EXTENDED_PARAMETERS);
    static const CreateFile2Fn real = reinterpret_cast<CreateFile2Fn>(
        ::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"), "CreateFile2"));
    if (real) return real(fileName, access, shareMode, disposition, params);

    DWORD flags = 0;
    LPSECURITY_ATTRIBUTES security = nullptr;
    HANDLE templateFile = nullptr;
    if (params) {
        flags = params->dwFileAttributes | params->dwFileFlags | params->dwSecurityQosFlags;
        security = params->lpSecurityAttributes;
        templateFile = params->hTemplateFile;
    }
    return ::CreateFileW(fileName, access, shareMode, security, disposition, flags, templateFile);
}

// The time to a millisecond or so instead of a fraction of a microsecond.
VOID WINAPI GetSystemTimePreciseAsFileTimeCompat(LPFILETIME time) {
    using PreciseFn = VOID(WINAPI*)(LPFILETIME);
    static const PreciseFn real = reinterpret_cast<PreciseFn>(
        ::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"), "GetSystemTimePreciseAsFileTime"));
    if (real) {
        real(time);
    } else {
        ::GetSystemTimeAsFileTime(time);
    }
}

HRESULT WINAPI CopyFile2Compat(PCWSTR from, PCWSTR to, COPYFILE2_EXTENDED_PARAMETERS* params) {
    using CopyFile2Fn = HRESULT(WINAPI*)(PCWSTR, PCWSTR, COPYFILE2_EXTENDED_PARAMETERS*);
    static const CopyFile2Fn real = reinterpret_cast<CopyFile2Fn>(
        ::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"), "CopyFile2"));
    if (real) return real(from, to, params);

    BOOL failIfExists = params && (params->dwCopyFlags & COPY_FILE_FAIL_IF_EXISTS);
    if (::CopyFileW(from, to, failIfExists)) return S_OK;
    return HRESULT_FROM_WIN32(::GetLastError());
}

}  // namespace

// The import slots the runtime library's calls go through (x64 names are undecorated).
extern "C" decltype(&CreateFile2Compat) __imp_CreateFile2 = &CreateFile2Compat;
extern "C" decltype(&CopyFile2Compat) __imp_CopyFile2 = &CopyFile2Compat;
extern "C" decltype(&GetSystemTimePreciseAsFileTimeCompat) __imp_GetSystemTimePreciseAsFileTime =
    &GetSystemTimePreciseAsFileTimeCompat;
