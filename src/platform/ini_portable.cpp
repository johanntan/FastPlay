// INI files for systems without the Windows profile functions. Reads and writes the
// same format they do (sections, key=value lines, ';' comments, surrounding quotes
// stripped from values), stored as UTF-8.

#include "ini.h"
#include "utils.h"

#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace {

std::mutex g_iniMutex;

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Section and key names match without regard to (ASCII) case.
bool SameName(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

std::filesystem::path ToPath(const wchar_t* path) {
    return std::filesystem::path(std::wstring(path));
}

std::vector<std::string> ReadLines(const wchar_t* path) {
    std::vector<std::string> lines;
    std::ifstream in(ToPath(path), std::ios::binary);
    if (!in) return lines;
    std::string line;
    bool first = true;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (first) {
            // A UTF-8 byte order mark is not part of the first line.
            if (line.size() >= 3 && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
            first = false;
        }
        lines.push_back(line);
    }
    return lines;
}

bool WriteLines(const wchar_t* path, const std::vector<std::string>& lines) {
    std::ofstream out(ToPath(path), std::ios::binary | std::ios::trunc);
    if (!out) return false;
    for (const auto& line : lines) {
        out << line << "\n";
    }
    return static_cast<bool>(out);
}

// "[name]" -> name; empty if the line is not a section header.
bool SectionName(const std::string& line, std::string& name) {
    std::string t = Trim(line);
    if (t.size() < 2 || t.front() != '[') return false;
    size_t close = t.find(']');
    if (close == std::string::npos) return false;
    name = Trim(t.substr(1, close - 1));
    return true;
}

// "key=value" -> key and value (both trimmed); false for comments and other lines.
bool KeyValue(const std::string& line, std::string& key, std::string& value) {
    std::string t = Trim(line);
    if (t.empty() || t[0] == ';') return false;
    size_t eq = t.find('=');
    if (eq == std::string::npos) return false;
    key = Trim(t.substr(0, eq));
    value = Trim(t.substr(eq + 1));
    return true;
}

// Index range [begin, end) of the lines belonging to `section` (after its header),
// with begin = the header's index; false if the section is not there.
bool FindSection(const std::vector<std::string>& lines, const std::string& section, size_t& header, size_t& end) {
    for (size_t i = 0; i < lines.size(); i++) {
        std::string name;
        if (SectionName(lines[i], name) && SameName(name, section)) {
            header = i;
            end = i + 1;
            while (end < lines.size() && !SectionName(lines[end], name)) end++;
            return true;
        }
    }
    return false;
}

bool FindValue(const wchar_t* section, const wchar_t* key, const wchar_t* path, std::string& value) {
    std::vector<std::string> lines = ReadLines(path);
    size_t header, end;
    if (!FindSection(lines, WideToUtf8(section), header, end)) return false;
    std::string wanted = WideToUtf8(key);
    for (size_t i = header + 1; i < end; i++) {
        std::string k, v;
        if (KeyValue(lines[i], k, v) && SameName(k, wanted)) {
            // Matching quotes around a value are not part of it.
            if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front()) {
                v = v.substr(1, v.size() - 2);
            }
            value = v;
            return true;
        }
    }
    return false;
}

}  // namespace

int IniGetInt(const wchar_t* section, const wchar_t* key, int defaultValue, const wchar_t* path) {
    std::lock_guard<std::mutex> lock(g_iniMutex);
    std::string value;
    if (!FindValue(section, key, path, value) || value.empty()) return defaultValue;
    // Like the Windows call: the leading number, 0 if there is none.
    return static_cast<int>(std::strtol(value.c_str(), nullptr, 10));
}

unsigned IniGetString(const wchar_t* section, const wchar_t* key, const wchar_t* defaultValue,
                      wchar_t* out, unsigned size, const wchar_t* path) {
    if (!out || size == 0) return 0;
    std::wstring result;
    {
        std::lock_guard<std::mutex> lock(g_iniMutex);
        std::string value;
        if (FindValue(section, key, path, value)) {
            result = Utf8ToWide(value);
        } else if (defaultValue) {
            result = defaultValue;
        }
    }
    size_t n = result.size() < size - 1 ? result.size() : size - 1;
    std::wmemcpy(out, result.c_str(), n);
    out[n] = L'\0';
    return static_cast<unsigned>(n);
}

bool IniWriteString(const wchar_t* section, const wchar_t* key, const wchar_t* value, const wchar_t* path) {
    std::lock_guard<std::mutex> lock(g_iniMutex);
    std::vector<std::string> lines = ReadLines(path);
    std::string sectionName = WideToUtf8(section);
    size_t header = 0, end = 0;
    bool haveSection = FindSection(lines, sectionName, header, end);

    if (!key) {
        // Remove the whole section.
        if (haveSection) lines.erase(lines.begin() + header, lines.begin() + end);
        return WriteLines(path, lines);
    }

    std::string keyName = WideToUtf8(key);
    if (haveSection) {
        size_t lastKey = header;
        for (size_t i = header + 1; i < end; i++) {
            std::string k, v;
            if (!KeyValue(lines[i], k, v)) continue;
            lastKey = i;
            if (SameName(k, keyName)) {
                if (value) {
                    lines[i] = k + "=" + WideToUtf8(value);
                } else {
                    lines.erase(lines.begin() + i);
                }
                return WriteLines(path, lines);
            }
        }
        if (!value) return true;  // nothing to remove
        lines.insert(lines.begin() + lastKey + 1, keyName + "=" + WideToUtf8(value));
        return WriteLines(path, lines);
    }

    if (!value) return true;
    lines.push_back("[" + sectionName + "]");
    lines.push_back(keyName + "=" + WideToUtf8(value));
    return WriteLines(path, lines);
}

bool IniClearSection(const wchar_t* section, const wchar_t* path) {
    std::lock_guard<std::mutex> lock(g_iniMutex);
    std::vector<std::string> lines = ReadLines(path);
    size_t header, end;
    if (FindSection(lines, WideToUtf8(section), header, end)) {
        lines.erase(lines.begin() + header + 1, lines.begin() + end);
    } else {
        lines.push_back("[" + WideToUtf8(section) + "]");
    }
    return WriteLines(path, lines);
}
