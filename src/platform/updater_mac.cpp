// Installing an update on macOS. The downloaded zip holds a new FastPlay.app. It is
// unpacked now, so a bad download is reported while FastPlay is still open; then a
// small script waits for FastPlay to quit, puts the new app where the old one was
// (keeping the old one until the new one is in place), and opens it.

#include "updater.h"
#include "updater_internal.h"
#include "app_ui.h"
#include "paths.h"
#include "subprocess.h"
#include "utils.h"

#include <cstdio>
#include <filesystem>
#include <spawn.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

extern char** environ;

namespace fs = std::filesystem;

namespace {

// A path as a single-quoted shell word.
std::string ShellQuote(const std::string& text) {
    std::string quoted = "'";
    for (char c : text) {
        if (c == '\'') {
            quoted += "'\\''";
        } else {
            quoted += c;
        }
    }
    return quoted + "'";
}

// The FastPlay.app FastPlay is running from, or empty when it is not in an app bundle.
std::string RunningBundle() {
    fs::path macos = fs::path(WideToUtf8(GetExecutableDir())).parent_path();  // .../Contents/MacOS
    fs::path bundle = macos.parent_path().parent_path();
    if (macos.filename() != "MacOS" || bundle.extension() != ".app") return "";
    return bundle.string();
}

void Fail(const std::wstring& message) {
    ShowMessage(message, L"Update Error", MessageIcon::Error);
}

}  // namespace

std::wstring UpdateZipPath() {
    return GetTempDir() + L"FastPlay-update.zip";
}

std::wstring UpdateInstallerPath() {
    return GetTempDir() + L"FastPlay-update.pkg";  // there is no Mac installer; never downloaded
}

void ApplyUpdate() {
    std::string zip = WideToUtf8(UpdateZipPath());
    std::error_code ec;
    if (!fs::is_regular_file(zip, ec)) {
        Fail(L"Update file not found. The download may have failed.");
        return;
    }

    std::string bundle = RunningBundle();
    if (bundle.empty()) {
        Fail(L"FastPlay is not running from FastPlay.app, so it cannot update itself. "
             L"Download the new version from the FastPlay releases page.");
        return;
    }
    std::string parent = fs::path(bundle).parent_path().string();
    if (access(parent.c_str(), W_OK) != 0) {
        Fail(Utf8ToWide("FastPlay cannot replace itself in " + parent +
                        " (no permission). Download the new version from the FastPlay releases page."));
        return;
    }

    // Unpack the new app next to the zip
    std::string staging = WideToUtf8(GetTempDir()) + "FastPlay-update";
    fs::remove_all(staging, ec);
    std::string output;
    int exitCode = -1;
    RunProcessCapture(L"/usr/bin/ditto", {L"-x", L"-k", Utf8ToWide(zip), Utf8ToWide(staging)}, output, nullptr,
                      &exitCode);
    std::string newApp = staging + "/FastPlay.app";
    if (exitCode != 0 || !fs::is_directory(newApp + "/Contents/MacOS", ec)) {
        fs::remove_all(staging, ec);
        Fail(L"The downloaded update could not be unpacked.");
        return;
    }

    // The script that swaps the apps once FastPlay has quit
    std::string script = WideToUtf8(GetTempDir()) + "FastPlay-update.sh";
    FILE* f = fopen(script.c_str(), "w");
    if (!f) {
        Fail(L"Failed to create the update script.");
        return;
    }
    std::string oldApp = ShellQuote(bundle), backup = ShellQuote(bundle + ".old");
    std::string text =
        "#!/bin/sh\n"
        "while kill -0 " + std::to_string(getpid()) + " 2>/dev/null; do sleep 0.2; done\n"
        "rm -rf " + backup + "\n"
        "if mv " + oldApp + " " + backup + "; then\n"
        "  if mv " + ShellQuote(newApp) + " " + oldApp + "; then\n"
        "    rm -rf " + backup + "\n"
        "  else\n"
        "    mv " + backup + " " + oldApp + "\n"
        "  fi\n"
        "fi\n"
        "xattr -dr com.apple.quarantine " + oldApp + " 2>/dev/null\n"
        "rm -rf " + ShellQuote(staging) + " " + ShellQuote(zip) + "\n"
        "open " + oldApp + "\n"
        "rm -f \"$0\"\n";
    fputs(text.c_str(), f);
    fclose(f);
    chmod(script.c_str(), 0755);

    // Started in a session of its own, so it outlives FastPlay.
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
    std::string shell = "/bin/sh";
    char* argv[] = {&shell[0], &script[0], nullptr};
    pid_t pid = 0;
    int err = posix_spawn(&pid, "/bin/sh", nullptr, &attr, argv, environ);
    posix_spawnattr_destroy(&attr);
    if (err != 0) {
        Fail(L"Failed to launch the update script.");
        return;
    }
    CloseMainWindow();
}
