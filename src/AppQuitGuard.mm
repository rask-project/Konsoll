#import <AppKit/AppKit.h>

#include "AppController.h"
#include "AppQuitGuard.h"
#include "NativeDialogs.h"
#include "WindowRegistry.h"

// ObjC delegate that routes the macOS app-quit decision and the dock-click
// reopen into the C++ logic. It is installed after the Qt launch has
// completed, so only the quit- and reopen-related methods need to be
// provided here.
@interface KonsollAppQuitDelegate : NSObject <NSApplicationDelegate>
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender;
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender;
- (BOOL)applicationShouldHandleReopen:(NSApplication *)sender hasVisibleWindows:(BOOL)hasVisibleWindows;
@end

@implementation KonsollAppQuitDelegate

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender
{
    Q_UNUSED(sender);
    if (konsoll::NativeDialogs::consumeQuitApproval()) {
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
    return NSTerminateNow;
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender
{
    Q_UNUSED(sender);
    return NO;
}

- (BOOL)applicationShouldHandleReopen:(NSApplication *)sender hasVisibleWindows:(BOOL)hasVisibleWindows
{
    Q_UNUSED(sender);
    if (!hasVisibleWindows) {
        konsoll::AppController::requestOpenWindow();
        return YES;
    }
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
