#import <AppKit/AppKit.h>

#include "macs_translucent.h"

void makeWindowTranslucent(WId winId, const QColor &color, qreal alpha)
{
    // On macOS, QWindow::winId() is the window's contentView (NSView*).
    NSView *nsView = reinterpret_cast<NSView *>(winId);
    if (!nsView) {
        return;
    }
    NSWindow *nsWindow = [nsView window];
    if (!nsWindow) {
        return;
    }

    // iTerm2 look: a non-opaque window whose background is the given color at
    // the given alpha. The desktop shows through, but the tint keeps the
    // terminal readable (unlike a fully clear background).
    [nsWindow setOpaque:NO];
    // NSColor expects 0.0-1.0 components; QColor::redF()/greenF()/blueF() return
    // that range (red()/green()/blue() return 0-255 ints and would clamp to white).
    [nsWindow setBackgroundColor:[NSColor colorWithRed:color.redF() green:color.greenF() blue:color.blueF() alpha:alpha]];
}
