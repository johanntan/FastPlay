#include "bass_text.h"

#ifdef _WIN32
#include <windows.h>
#endif

std::wstring BassTextToWide(const char* text) {
    if (!text || !*text) return std::wstring();
#ifdef _WIN32
    // BASS returns ANSI code page text on Windows
    int len = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
    if (len <= 0) return std::wstring();
    std::wstring wide(len - 1, L'\0');
    MultiByteToWideChar(CP_ACP, 0, text, -1, &wide[0], len);
    return wide;
#else
    return Utf8ToWide(text);
#endif
}
