// Windows 7 support. Newer MSVC runtime libraries call CreateFile2, which Windows 7
// lacks, and an import it cannot resolve stops FastPlay from starting at all. This
// file defines the import slot itself, so the linker takes it from here instead of
// from kernel32: the real CreateFile2 where there is one, otherwise the same call
// made through CreateFileW.

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

}  // namespace

// The import slot the runtime library's calls go through (x64 names are undecorated).
extern "C" decltype(&CreateFile2Compat) __imp_CreateFile2 = &CreateFile2Compat;
