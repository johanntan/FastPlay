#include "utils.h"

#include <chrono>
#include <cwchar>
#include <cwctype>

static void AppendUtf8(std::string& utf8, uint32_t cp) {
    if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;  // unpaired surrogate
    if (cp < 0x80) {
        utf8 += static_cast<char>(cp);
    } else if (cp < 0x800) {
        utf8 += static_cast<char>(0xC0 | (cp >> 6));
        utf8 += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        utf8 += static_cast<char>(0xE0 | (cp >> 12));
        utf8 += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        utf8 += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x110000) {
        utf8 += static_cast<char>(0xF0 | (cp >> 18));
        utf8 += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        utf8 += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        utf8 += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        utf8 += "\xEF\xBF\xBD";  // U+FFFD
    }
}

// Append units[i] (with units[i + 1] when the two form a UTF-16 surrogate pair) and
// return how many units were used.
template <typename Units>
static size_t AppendUnit(std::string& utf8, const Units& units, size_t i, bool utf16) {
    uint32_t cp = static_cast<uint32_t>(units[i]);
    if (utf16 && cp >= 0xD800 && cp <= 0xDBFF && i + 1 < units.size()) {
        uint32_t lo = static_cast<uint32_t>(units[i + 1]);
        if (lo >= 0xDC00 && lo <= 0xDFFF) {
            AppendUtf8(utf8, 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00));
            return 2;
        }
    }
    AppendUtf8(utf8, cp);
    return 1;
}

std::string WideToUtf8(const std::wstring& wide) {
    std::string utf8;
    utf8.reserve(wide.size());
    // wchar_t holds UTF-16 on Windows and whole code points elsewhere
    for (size_t i = 0; i < wide.size();) {
        i += AppendUnit(utf8, wide, i, sizeof(wchar_t) == 2);
    }
    return utf8;
}

std::string Utf16ToUtf8(const std::u16string& utf16) {
    std::string utf8;
    utf8.reserve(utf16.size());
    for (size_t i = 0; i < utf16.size();) {
        i += AppendUnit(utf8, utf16, i, true);
    }
    return utf8;
}

std::wstring Utf8ToWide(const std::string& utf8) {
    std::wstring wide;
    wide.reserve(utf8.size());
    const unsigned char* s = reinterpret_cast<const unsigned char*>(utf8.data());
    size_t n = utf8.size();
    for (size_t i = 0; i < n;) {
        uint32_t cp;
        size_t len;
        unsigned char c = s[i];
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        else { cp = 0xFFFD; len = 1; }
        bool valid = len == 1 || i + len <= n;
        for (size_t k = 1; valid && k < len; k++) {
            if ((s[i + k] & 0xC0) != 0x80) valid = false;
            else cp = (cp << 6) | (s[i + k] & 0x3F);
        }
        // Invalid or overlong sequences become U+FFFD, one per bad byte
        if (!valid || (len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) ||
            cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            cp = 0xFFFD;
            len = 1;
        }
        i += len;
        if (sizeof(wchar_t) == 2 && cp >= 0x10000) {
            cp -= 0x10000;
            wide += static_cast<wchar_t>(0xD800 + (cp >> 10));
            wide += static_cast<wchar_t>(0xDC00 + (cp & 0x3FF));
        } else {
            wide += static_cast<wchar_t>(cp);
        }
    }
    return wide;
}

std::wstring GetFileName(const std::wstring& path) {
    size_t pos = path.find_last_of(L"\\/");
    if (pos != std::wstring::npos) {
        return path.substr(pos + 1);
    }
    return path;
}

std::wstring FormatTime(double seconds) {
    if (seconds < 0) seconds = 0;
    int totalSec = static_cast<int>(seconds);
    int h = totalSec / 3600;
    int m = (totalSec % 3600) / 60;
    int s = totalSec % 60;

    wchar_t buf[32];
    if (h > 0) {
        swprintf(buf, 32, L"%d:%02d:%02d", h, m, s);
    } else {
        swprintf(buf, 32, L"%d:%02d", m, s);
    }
    return buf;
}

int WStrNICmp(const wchar_t* a, const wchar_t* b, size_t count) {
    for (size_t i = 0; i < count; i++) {
        wint_t x = std::towlower(static_cast<wint_t>(a[i]));
        wint_t y = std::towlower(static_cast<wint_t>(b[i]));
        if (x != y) return x < y ? -1 : 1;
        if (a[i] == L'\0') return 0;
    }
    return 0;
}

int WStrICmp(const wchar_t* a, const wchar_t* b) {
    return WStrNICmp(a, b, static_cast<size_t>(-1));
}

FILE* FileOpen(const std::wstring& path, const char* mode) {
#ifdef _WIN32
    std::wstring wmode(mode, mode + std::char_traits<char>::length(mode));
    return _wfopen(path.c_str(), wmode.c_str());
#else
    return std::fopen(WideToUtf8(path).c_str(), mode);
#endif
}

uint32_t TickCountMs() {
    using namespace std::chrono;
    return static_cast<uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}
