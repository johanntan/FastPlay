#include "paths.h"
#include "utils.h"

#include <mach-o/dyld.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <climits>
#include <cstdlib>
#include <string>
#include <vector>

const wchar_t kPathSeparator = L'/';

static std::string HomeDir() {
    if (const char* home = std::getenv("HOME")) {
        if (*home) return home;
    }
    if (struct passwd* pw = getpwuid(getuid())) {
        return pw->pw_dir;
    }
    return "/tmp";
}

std::wstring GetExecutableDir() {
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buf(size + 1, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return L"./";
    char resolved[PATH_MAX];
    std::string path = realpath(buf.data(), resolved) ? resolved : buf.data();
    size_t pos = path.find_last_of('/');
    return Utf8ToWide(pos != std::string::npos ? path.substr(0, pos + 1) : std::string("./"));
}

// The Mac app is never "installed" in the Windows sense; settings always live in
// Application Support.
bool IsInstalledMode() {
    return true;
}

std::wstring GetDataDirectory() {
    std::string dir = HomeDir() + "/Library/Application Support/FastPlay";
    mkdir(dir.c_str(), 0755);
    return Utf8ToWide(dir + "/");
}

std::wstring GetUserMusicDir() {
    return Utf8ToWide(HomeDir() + "/Music");
}

std::wstring GetUserDownloadsDir() {
    return Utf8ToWide(HomeDir() + "/Downloads");
}

std::wstring GetTempDir() {
    const char* tmp = std::getenv("TMPDIR");
    std::string dir = (tmp && *tmp) ? tmp : "/tmp/";
    if (dir.back() != '/') dir += '/';
    return Utf8ToWide(dir);
}

// Inside the app bundle: FastPlay.app/Contents/Frameworks, beside Contents/MacOS.
std::wstring GetLibraryDir() {
    return GetExecutableDir() + L"../Frameworks/";
}
