// Menu shortcuts that macOS shows but does not act on.
//
// wxWidgets offers every key press to the menu bar before the window's accelerator
// table sees it, and Cocoa does the same for Command keys of its own accord. A
// shortcut in a menu label is a key equivalent, so the key performs the menu item,
// and VoiceOver announces the item's title on every press. (A menu delegate that
// denies having a key equivalent does not stop that: macOS matches the items
// anyway.) So the keys are caught before the system dispatches them at all, with a
// local event monitor, and handed to the window's own shortcut table. The menus
// keep showing and reading their shortcuts, and never see the keys.

#include "platform.h"
#include "keycodes.h"
#include "system_keys.h"

#import <AppKit/AppKit.h>
#include <wx/frame.h>

namespace {

id g_shortcutMonitor = nil;

}  // namespace

void StartShortcutMonitor(wxFrame* frame, bool (*handler)(unsigned modifiers, unsigned vk)) {
    StopShortcutMonitor();
    NSWindow* window = frame->GetWXWindow();
    g_shortcutMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                                              handler:^NSEvent*(NSEvent* event) {
        // Only keys typed into this window; a dialog's keys are its own.
        if (event.window != window) return event;
        NSEventModifierFlags flags = event.modifierFlags;
        unsigned modifiers = 0;
        if (flags & NSEventModifierFlagCommand) modifiers |= MOD_CONTROL;
        if (flags & NSEventModifierFlagShift) modifiers |= MOD_SHIFT;
        if (flags & NSEventModifierFlagOption) modifiers |= MOD_ALT;
        if (flags & NSEventModifierFlagControl) modifiers |= MOD_WIN;
        unsigned vk = MacKeyCodeToVirtualKey(event.keyCode);
        if (vk && handler(modifiers, vk)) return nil;  // swallowed: the menu never sees it
        return event;
    }];
}

void StopShortcutMonitor() {
    if (g_shortcutMonitor) {
        [NSEvent removeMonitor:g_shortcutMonitor];
        g_shortcutMonitor = nil;
    }
}
