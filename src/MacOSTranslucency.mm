#import <AppKit/AppKit.h>
#import <objc/runtime.h>

#include <QDebug>
#include "MacOSTranslucency.h"

namespace
{
char effectViewAssociationKey;

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

void setWindowBlurEnabled(WId winId, bool enabled, qreal alpha)
{
    NSWindow *const nsWindow = windowFromId(winId);
    if (nsWindow == nil) {
        return;
    }

    NSVisualEffectView *effectView = objc_getAssociatedObject(nsWindow, &effectViewAssociationKey);

    if (enabled) {
        if (effectView == nil) {
            NSView *const contentView = [nsWindow contentView];
            NSView *const parentView = [contentView superview];
            if (parentView == nil) {
                return;
            }

            NSRect const effectFrame = [contentView convertRect:[contentView bounds] toView:parentView];
            effectView = [[NSVisualEffectView alloc] initWithFrame:effectFrame];
            [effectView setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
            [effectView setBlendingMode:NSVisualEffectBlendingModeBehindWindow];
            [effectView setMaterial:NSVisualEffectMaterialUnderWindowBackground];
            [effectView setState:NSVisualEffectStateActive];
            objc_setAssociatedObject(nsWindow, &effectViewAssociationKey, effectView, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
            [parentView addSubview:effectView positioned:NSWindowBelow relativeTo:contentView];
        }
        [effectView setAlphaValue:alpha];
    } else if (effectView != nil) {
        [effectView removeFromSuperview];
        objc_setAssociatedObject(nsWindow, &effectViewAssociationKey, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }
}

void setNativeWindowMovable(WId winId, bool movable)
{
    NSWindow *const nsWindow = windowFromId(winId);
    if (nsWindow != nil) {
        [nsWindow setMovable:movable];
    }
}
