#import <AppKit/AppKit.h>

#include "DockMenu.h"

// The Dock menu items' target: a Dock menu has no responder chain to send
// their actions to.
@interface KonsollDockMenuTarget : NSObject
- (void)newTab:(id)sender;
- (void)newWindow:(id)sender;
@end

@implementation KonsollDockMenuTarget

- (void)newTab:(id)sender
{
    Q_UNUSED(sender);
    // The Dock does not make the app active.
    [NSApp activate];
    emit konsoll::DockMenu::shared().newTabRequested();
}

- (void)newWindow:(id)sender
{
    Q_UNUSED(sender);
    [NSApp activate];
    emit konsoll::DockMenu::shared().newWindowRequested();
}

@end

namespace konsoll
{

DockMenu &DockMenu::shared()
{
    static DockMenu instance;
    return instance;
}

DockMenu *DockMenu::create(QQmlEngine *qmlEngine, QJSEngine *jsEngine)
{
    Q_UNUSED(qmlEngine);
    Q_UNUSED(jsEngine);
    DockMenu &dockMenu = shared();
    // A function-local static: the engine must not delete it.
    QJSEngine::setObjectOwnership(&dockMenu, QJSEngine::CppOwnership);
    return &dockMenu;
}

NSMenu *DockMenu::menu()
{
    // Both live as long as the process: AppKit may still use them while the
    // app quits. Menu items keep their target weak.
    static NSMenu *const dockMenu = [] {
        KonsollDockMenuTarget *const target = [[KonsollDockMenuTarget alloc] init];
        NSMenu *const built = [[NSMenu alloc] initWithTitle:@""];
        built.autoenablesItems = NO;
        NSMenuItem *const tab = [built addItemWithTitle:@"New Tab" action:@selector(newTab:) keyEquivalent:@""];
        tab.target = target;
        NSMenuItem *const window = [built addItemWithTitle:@"New Window"
                                                    action:@selector(newWindow:)
                                             keyEquivalent:@""];
        window.target = target;
        return built;
    }();
    return dockMenu;
}

} // namespace konsoll
