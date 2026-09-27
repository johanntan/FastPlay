// INI files through the Windows profile functions.

#include "ini.h"

#include <windows.h>

int IniGetInt(const wchar_t* section, const wchar_t* key, int defaultValue, const wchar_t* path) {
    return static_cast<int>(GetPrivateProfileIntW(section, key, defaultValue, path));
}

unsigned IniGetString(const wchar_t* section, const wchar_t* key, const wchar_t* defaultValue,
                      wchar_t* out, unsigned size, const wchar_t* path) {
    return GetPrivateProfileStringW(section, key, defaultValue, out, size, path);
}

bool IniWriteString(const wchar_t* section, const wchar_t* key, const wchar_t* value, const wchar_t* path) {
    return WritePrivateProfileStringW(section, key, value, path) != FALSE;
}

bool IniClearSection(const wchar_t* section, const wchar_t* path) {
    return WritePrivateProfileSectionW(section, L"", path) != FALSE;
}
