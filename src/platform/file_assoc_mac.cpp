// File associations on macOS. The types FastPlay can open are declared in its
// Info.plist; registering makes FastPlay the default app for each of them, as
// Finder's "Change All..." would.

// Before FastPlay's headers: bass.h (through globals.h) defines Windows-style names.
#include <CoreServices/CoreServices.h>

#include "file_assoc.h"
#include "globals.h"
#include "utils.h"

// The UTType and Launch Services calls used here are deprecated, but remain the only
// C interface for setting the default app.
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

static CFStringRef CopyTypeForExtension(const wchar_t* ext) {
    std::string name = WideToUtf8(ext[0] == L'.' ? ext + 1 : ext);
    CFStringRef extension = CFStringCreateWithCString(nullptr, name.c_str(), kCFStringEncodingUTF8);
    if (!extension) return nullptr;
    CFStringRef type = UTTypeCreatePreferredIdentifierForTag(kUTTagClassFilenameExtension, extension, nullptr);
    CFRelease(extension);
    // A "dyn." type is made up on the spot for an extension no app declares.
    if (type && CFStringHasPrefix(type, CFSTR("dyn."))) {
        CFRelease(type);
        return nullptr;
    }
    return type;
}

void RegisterAllFileTypes() {
    CFStringRef bundleId = CFBundleGetIdentifier(CFBundleGetMainBundle());
    if (!bundleId) return;  // not running from FastPlay.app
    for (int i = 0; i < g_fileAssocCount; i++) {
        CFStringRef type = CopyTypeForExtension(g_fileAssocs[i].ext);
        if (!type) continue;
        LSSetDefaultRoleHandlerForContentType(type, kLSRolesViewer, bundleId);
        CFRelease(type);
    }
}

void UnregisterAllFileTypes() {
    // macOS has no "no default app": each type keeps FastPlay until another app is
    // chosen for it in Finder.
}
