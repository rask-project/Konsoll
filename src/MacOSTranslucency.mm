#import <AppKit/AppKit.h>

#include <QDebug>
#include "MacOSTranslucency.h"

namespace
{
NSWindow *windowFromId(WId winId)
{
    NSView *const nsView = reinterpret_cast<NSView *>(winId);
    return nsView ? [nsView window] : nil;
}
} // namespace

void makeWindowTranslucent(WId winId, const QColor &color, qreal alpha)
{
    NSWindow *const nsWindow = windowFromId(winId);
    if (nsWindow == nil) {
        return;
    }

    [nsWindow setOpaque:NO];
    [nsWindow setBackgroundColor:[NSColor colorWithRed:color.redF()
                                                 green:color.greenF()
                                                  blue:color.blueF()
                                                 alpha:alpha]];
}

void setNativeWindowMovable(WId winId, bool movable)
{
    NSWindow *const nsWindow = windowFromId(winId);
    if (nsWindow != nil) {
        [nsWindow setMovable:movable];
    }
}
