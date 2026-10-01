// Menu shortcuts that macOS shows but does not act on.
//
// wxWidgets offers every key press to the menu bar before the window's accelerator
// table sees it, and Cocoa does the same for Command keys of its own accord. A
// shortcut in a menu label is a key equivalent, so the key performs the menu item,
// and VoiceOver announces the item's title on every press. (A menu delegate that
// denies having a key equivalent does not stop that: macOS matches the items
// anyway.) So the keys are caught before the system dispatches them at all, with a
// local event monitor, and handed to the window's own shortcut table. The menus
// keep showing and reading their shortcuts in the main window. In dialogs their
// key equivalents are removed, so wxWidgets' menu-first dispatch lets the keys
// reach the focused control instead.

#include "platform.h"
#include "keycodes.h"
#include "system_keys.h"

#import <AppKit/AppKit.h>
#include <wx/frame.h>

namespace {

id g_shortcutMonitor = nil;
id g_keyWindowObserver = nil;
NSMapTable<NSMenuItem*, NSString*>* g_menuShortcuts = nil;

void RemoveMenuShortcuts(NSMenu* menu) {
	for (NSMenuItem* item in menu.itemArray) {
		if (item.submenu) RemoveMenuShortcuts(item.submenu);
		if (item.keyEquivalent.length == 0) continue;
		[g_menuShortcuts setObject:item.keyEquivalent forKey:item];
		item.keyEquivalent = @"";
	}
}

void UpdateMenuShortcuts(bool mainWindow) {
	if (mainWindow) {
		for (NSMenuItem* item in g_menuShortcuts.keyEnumerator) {
			item.keyEquivalent = [g_menuShortcuts objectForKey:item];
		}
		[g_menuShortcuts removeAllObjects];
	} else {
		RemoveMenuShortcuts(NSApp.mainMenu);
	}
}

}  // namespace

void StartShortcutMonitor(wxFrame* frame, bool (*handler)(unsigned modifiers, unsigned vk),
                          bool (*holdHandler)(unsigned modifiers, unsigned vk, bool down, bool repeat)) {
    StopShortcutMonitor();
    NSWindow* window = frame->GetWXWindow();
	g_menuShortcuts = [NSMapTable strongToStrongObjectsMapTable];
	g_keyWindowObserver = [NSNotificationCenter.defaultCenter
		addObserverForName:NSWindowDidBecomeKeyNotification object:nil queue:nil
		usingBlock:^(NSNotification* notification) {
			UpdateMenuShortcuts(notification.object == window);
		}];
	UpdateMenuShortcuts(NSApp.keyWindow == window);
    g_shortcutMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown | NSEventMaskKeyUp
                                                              handler:^NSEvent*(NSEvent* event) {
		// Menus can be rebuilt while a dialog is active; remove new equivalents
		// before Cocoa or wxWidgets can offer this key to the menu bar.
		UpdateMenuShortcuts(event.window == window);
        // Only keys typed into this window; a dialog's keys are its own.
        if (event.window != window) return event;
        NSEventModifierFlags flags = event.modifierFlags;
        unsigned modifiers = 0;
        if (flags & NSEventModifierFlagCommand) modifiers |= MOD_CONTROL;
        if (flags & NSEventModifierFlagShift) modifiers |= MOD_SHIFT;
        if (flags & NSEventModifierFlagOption) modifiers |= MOD_ALT;
        if (flags & NSEventModifierFlagControl) modifiers |= MOD_WIN;
        unsigned vk = MacKeyCodeToVirtualKey(event.keyCode);
        if (!vk) return event;
        const bool down = event.type == NSEventTypeKeyDown;
        if (holdHandler && holdHandler(modifiers, vk, down, down && event.isARepeat)) return nil;
        if (down && handler(modifiers, vk)) return nil;  // swallowed: the menu never sees it
        return event;
    }];
}

void StopShortcutMonitor() {
	if (g_keyWindowObserver) {
		[NSNotificationCenter.defaultCenter removeObserver:g_keyWindowObserver];
		g_keyWindowObserver = nil;
	}
    if (g_shortcutMonitor) {
        [NSEvent removeMonitor:g_shortcutMonitor];
        g_shortcutMonitor = nil;
    }
	UpdateMenuShortcuts(true);
	g_menuShortcuts = nil;
}
