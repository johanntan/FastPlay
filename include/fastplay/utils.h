#pragma once
#ifndef FASTPLAY_UTILS_H
#define FASTPLAY_UTILS_H

#include <cstdint>
#include <cstdio>
#include <string>

// String conversion (wchar_t is UTF-16 on Windows and UTF-32 elsewhere; both are handled)
std::string WideToUtf8(const std::wstring& wide);
std::wstring Utf8ToWide(const std::string& utf8);
// UTF-16 code units (e.g. from an ID3 tag) to UTF-8, joining surrogate pairs.
std::string Utf16ToUtf8(const std::u16string& utf16);

// Extract filename from path
std::wstring GetFileName(const std::wstring& path);

// Format time as M:SS or H:MM:SS
std::wstring FormatTime(double seconds);

// Case-insensitive comparison (as _wcsicmp / _wcsnicmp)
int WStrICmp(const wchar_t* a, const wchar_t* b);
int WStrNICmp(const wchar_t* a, const wchar_t* b, size_t count);

// Room for a path in a fixed-size buffer
constexpr unsigned kMaxPathChars = 4096;

// Open a file by its wide path (fopen modes: "rb", "wb", "a"...)
FILE* FileOpen(const std::wstring& path, const char* mode);

// Milliseconds since some fixed point, for measuring intervals (wraps like GetTickCount)
uint32_t TickCountMs();

#endif // FASTPLAY_UTILS_H
