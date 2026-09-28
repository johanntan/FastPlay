// Menu shortcuts that macOS shows but does not act on.
//
// wxWidgets offers every key press to the menu bar before the window's accelerator
// table sees it, and Cocoa does the same for Command keys of its own accord. A
// shortcut in a menu label is a key equivalent, so the key performs the menu item,
// and VoiceOver announces the item's title on every press. A menu whose delegate
// answers that it has no key equivalent for the event keeps its shortcuts on show,
// and reads them when browsed, but lets the key reach the accelerator table, which
// runs the command in silence, as on Windows.

#include "platform.h"

#import <AppKit/AppKit.h>
#include <wx/menu.h>

// Declines every key press and passes everything else on to the delegate wxWidgets
// gave the menu (which updates the items as the menu opens).
@interface FPMenuKeyFilter : NSObject <NSMenuDelegate>
@property (nonatomic, strong) id<NSMenuDelegate> next;
@end

@implementation FPMenuKeyFilter

- (BOOL)menuHasKeyEquivalent:(NSMenu*)menu forEvent:(NSEvent*)event target:(id*)target action:(SEL*)action {
    return NO;
}

- (BOOL)respondsToSelector:(SEL)selector {
    return [super respondsToSelector:selector] || [self.next respondsToSelector:selector];
}

- (id)forwardingTargetForSelector:(SEL)selector {
    return self.next;
}

@end

void KeepMenuShortcutsSilent(wxMenu* menu) {
    // A menu's delegate is a weak reference, so the filters live here.
    static NSMutableArray<FPMenuKeyFilter*>* filters = [NSMutableArray new];
    NSMenu* nsMenu = menu->GetHMenu();
    if (!nsMenu || [nsMenu.delegate isKindOfClass:[FPMenuKeyFilter class]]) return;
    FPMenuKeyFilter* filter = [FPMenuKeyFilter new];
    filter.next = nsMenu.delegate;
    nsMenu.delegate = filter;
    [filters addObject:filter];
}
