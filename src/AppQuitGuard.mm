#import <AppKit/AppKit.h>

#include "AppQuitGuard.h"
#include "NativeDialogs.h"
#include "WindowRegistry.h"
#include "WindowStateStore.h"

// ObjC delegate that routes the macOS app-quit decision into the C++ logic.
// It is installed after the Qt launch has completed.
@interface KonsollAppQuitDelegate : NSObject <NSApplicationDelegate>
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender;
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender;
@end

@implementation KonsollAppQuitDelegate

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender
{
    Q_UNUSED(sender);
    if (konsoll::NativeDialogs::consumeQuitApproval()) {
        konsoll::WindowStateStore::setTerminating(true);
        return NSTerminateNow;
    }
    const auto &registry = konsoll::WindowRegistry::instance();
    const bool activeProcess = registry.anySessionHasActiveProcess();
    const int sessions = registry.totalSessionCount();
    if (activeProcess || sessions > 1) {
        konsoll::NativeDialogs dialog;
        const bool confirmed = dialog.confirmQuit();
        if (!confirmed) {
            return NSTerminateCancel;
        }
    }
    konsoll::WindowStateStore::setTerminating(true);
    return NSTerminateNow;
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender
{
    Q_UNUSED(sender);
    // Qt owns last-window shutdown through setQuitOnLastWindowClosed(). Letting
    // AppKit terminate here as well races Qt's event-loop and QML teardown.
    return NO;
}

@end

namespace konsoll
{

void installQuitGuard()
{
    KonsollAppQuitDelegate *const delegate = [[KonsollAppQuitDelegate alloc] init];
    [NSApp setDelegate:delegate];
}

} // namespace konsoll
