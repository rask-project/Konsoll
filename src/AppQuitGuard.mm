#import <AppKit/AppKit.h>

#include "AppQuitGuard.h"
#include "BackgroundJobs.h"
#include "DockMenu.h"
#include "EditorMode.h"
#include "NativeDialogs.h"
#include "WindowRegistry.h"
#include "WindowStateStore.h"

// ObjC delegate that routes the macOS app-quit decision into the C++ logic.
// It is installed after the Qt launch has completed.
@interface KonsollAppQuitDelegate : NSObject <NSApplicationDelegate>
// Qt's own delegate, which this one replaces; Qt keeps it alive.
@property(nonatomic, assign) id<NSApplicationDelegate> qtDelegate;
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender;
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender;
- (void)application:(NSApplication *)sender openFiles:(NSArray<NSString *> *)filenames;
- (NSMenu *)applicationDockMenu:(NSApplication *)sender;
@end

@implementation KonsollAppQuitDelegate

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender
{
    Q_UNUSED(sender);
    if (konsoll::NativeDialogs::consumeQuitApproval()) {
        konsoll::WindowStateStore::setTerminating(true);
        return NSTerminateNow;
    }
    // Same rule as closing a window, across every window: ask when quitting
    // would terminate more than one session or interrupt a running command.
    // A single idle tab in a single window quits without prompting.
    // Commands running in the background count as sessions too.
    const auto &registry = konsoll::WindowRegistry::instance();
    // The editor asks each nvim to quit (it asks about unsaved buffers); the
    // app ends with its last window. A second quit while one still runs
    // stops waiting, after the usual confirmation.
    if (konsoll::EditorMode::isActive() && registry.totalSessionCount() > 0) {
        if (!konsoll::EditorMode::shared().requestQuit()) {
            return NSTerminateCancel;
        }
        konsoll::NativeDialogs dialog;
        if (!dialog.confirmQuit(registry.totalSessionCount())) {
            return NSTerminateCancel;
        }
        konsoll::WindowStateStore::setTerminating(true);
        return NSTerminateNow;
    }
    const konsoll::BackgroundJobs *background = konsoll::BackgroundJobs::instance();
    const int backgroundRunning = background != nullptr ? background->runningCount() : 0;
    const int sessions = registry.totalSessionCount() + backgroundRunning;
    if (sessions <= 1 && !registry.anySessionHasRunningCommand() && backgroundRunning == 0) {
        konsoll::WindowStateStore::setTerminating(true);
        return NSTerminateNow;
    }
    konsoll::NativeDialogs dialog;
    const bool confirmed = dialog.confirmQuit(sessions);
    if (!confirmed) {
        return NSTerminateCancel;
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

- (void)application:(NSApplication *)sender openFiles:(NSArray<NSString *> *)filenames
{
    // Qt turns them into QFileOpenEvents (a folder dropped on the Dock icon
    // opens a tab there: FinderIntegration).
    if ([self.qtDelegate respondsToSelector:_cmd]) {
        [self.qtDelegate application:sender openFiles:filenames];
    }
}

- (NSMenu *)applicationDockMenu:(NSApplication *)sender
{
    Q_UNUSED(sender);
    // New Tab / New Window; AppKit adds the window list above them.
    return konsoll::DockMenu::menu();
}

@end

namespace konsoll
{

void installQuitGuard()
{
    KonsollAppQuitDelegate *const delegate = [[KonsollAppQuitDelegate alloc] init];
    delegate.qtDelegate = [NSApp delegate];
    [NSApp setDelegate:delegate];
}

} // namespace konsoll
