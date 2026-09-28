#include "platform.h"

#include <sys/sysctl.h>

void PlatformStartup() {
    // Nothing to do: everything FastPlay uses is linked into the app.
}

std::string GetSystemDescription() {
    std::string name = "macOS";
    char version[64] = {};
    size_t size = sizeof(version);
    if (sysctlbyname("kern.osproductversion", version, &size, nullptr, 0) == 0 && version[0]) {
        name += " ";
        name += version;
    }
#if defined(__arm64__) || defined(__aarch64__)
    return name + "; arm64";
#else
    return name + "; x86_64";
#endif
}
