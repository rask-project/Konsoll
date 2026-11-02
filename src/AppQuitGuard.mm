#import <AppKit/AppKit.h>

#include "AppQuitGuard.h"
#include "CloseConfirmDialog.h"
#include "TerminalManager.h"

// ObjC delegate that routes the macOS app-quit decision to the C++ logic.
// It is installed after the Qt launch has completed, so only the quit-related
// methods need to be provided here.
@interface KonsollAppQuitDelegate : NSObject <NSApplicationDelegate>
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender;
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender;
@end

@implementation KonsollAppQuitDelegate

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender
{
    Q_UNUSED(sender);
    konsoll::TerminalManager *const manager = konsoll::TerminalManager::instance();
    if ((manager != nullptr) && (manager->anySessionHasRunningCommand() || manager->sessionCount() > 1))
    {
        konsoll::CloseConfirmDialog dialog;
        if (!dialog.confirmClose())
        {
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

@end

namespace konsoll
{

void installQuitGuard()
{
    KonsollAppQuitDelegate *const delegate = [[KonsollAppQuitDelegate alloc] init];
    [NSApp setDelegate:delegate];
}

} // namespace konsoll
