#pragma once
#ifndef FASTPLAY_BASS_TEXT_H
#define FASTPLAY_BASS_TEXT_H

// Text crossing into and out of BASS. On Windows BASS takes file names as UTF-16
// (with the BASS_UNICODE flag) and returns text in the ANSI code page; elsewhere
// both are UTF-8. With BASS_UNICODE an encoder's options string is UTF-16 too, so
// BassFileName(options).get() passes options alongside a BassFileName path.

#include "bass.h"
#include "utils.h"
#include <string>

class BassFileName {
public:
    explicit BassFileName(const std::wstring& path)
#ifdef _WIN32
        : m_path(path) {}
    const char* get() const { return reinterpret_cast<const char*>(m_path.c_str()); }
    DWORD flags() const { return BASS_UNICODE; }
private:
    std::wstring m_path;
#else
        : m_path(WideToUtf8(path)) {}
    const char* get() const { return m_path.c_str(); }
    DWORD flags() const { return 0; }
private:
    std::string m_path;
#endif
};

// Text returned by BASS (device names and the like).
std::wstring BassTextToWide(const char* text);

#endif // FASTPLAY_BASS_TEXT_H
