#import <AppKit/AppKit.h>
#import <objc/runtime.h>

#include <array>
#include <algorithm>

#include <QDebug>
#include "MacOSTranslucency.h"

namespace
{
const char effectViewAssociationKey{};
const char toolbarAssociationKey{};
const char inputForwardingViewAssociationKey{};

NSWindow *windowFromId(WId winId)
{
    NSView *const nsView = reinterpret_cast<NSView *>(winId);
    return (nsView != nullptr) ? [nsView window] : nil;
}
} // namespace

@interface KonsollInputForwardingView : NSView
@property(nonatomic, assign) NSWindow *targetWindow;
@property(nonatomic, assign) NSView *targetView;
@end

@implementation KonsollInputForwardingView

- (BOOL)isOpaque
{
    return NO;
}

- (void)drawRect:(NSRect)dirtyRect
{
    Q_UNUSED(dirtyRect);
}

- (NSView *)hitTest:(NSPoint)point
{
    NSView *const parentView = [self superview];
    NSWindow *const window = self.targetWindow;
    NSView *const targetView = self.targetView;
    if (parentView == nil || window == nil || targetView == nil) {
        return nil;
    }

    constexpr std::array buttonTypes{
        NSWindowCloseButton,
        NSWindowMiniaturizeButton,
        NSWindowZoomButton,
    };
    auto const buttonType = std::ranges::find_if(buttonTypes, [window, parentView, point](NSWindowButton type) {
        NSButton *const button = [window standardWindowButton:type];
        if (button == nil) {
            return false;
        }
        NSRect const buttonRect = [button convertRect:[button bounds] toView:parentView];
        return NSPointInRect(point, buttonRect);
    });
    if (buttonType != buttonTypes.end()) {
        return [window standardWindowButton:*buttonType];
    }

    NSRect const forwardingRect = [self convertRect:[self bounds] toView:parentView];
    if (!NSPointInRect(point, NSInsetRect(forwardingRect, 12.0, 12.0))) {
        return nil;
    }
    return targetView;
}

@end

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
            objc_setAssociatedObject(
                nsWindow,
                &effectViewAssociationKey,
                effectView,
                OBJC_ASSOCIATION_RETAIN_NONATOMIC
            );
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

void installEmptyWindowToolbar(WId winId, bool visible)
{
    NSWindow *const nsWindow = windowFromId(winId);
    if (nsWindow == nil) {
        return;
    }

    NSToolbar *toolbar = objc_getAssociatedObject(nsWindow, &toolbarAssociationKey);
    if (toolbar == nil) {
        toolbar = [[NSToolbar alloc] initWithIdentifier:@"KonsollEmptyToolbar"];
        [toolbar setDisplayMode:NSToolbarDisplayModeIconOnly];
        [toolbar setSizeMode:NSToolbarSizeModeRegular];
        [toolbar setShowsBaselineSeparator:NO];
        objc_setAssociatedObject(nsWindow, &toolbarAssociationKey, toolbar, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        [nsWindow setToolbar:toolbar];
    }
    [toolbar setVisible:visible];

    if (!visible) {
        NSView *const inputView = objc_getAssociatedObject(nsWindow, &inputForwardingViewAssociationKey);
        [inputView removeFromSuperview];
        objc_setAssociatedObject(nsWindow, &inputForwardingViewAssociationKey, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        return;
    }

    // QML uses the expanded client area for its own titlebar controls. Keep
    // the Qt content view above AppKit's toolbar view so it receives input.
    NSView *const contentView = [nsWindow contentView];
    NSView *const parentView = [contentView superview];
    if (parentView != nil) {
        [parentView addSubview:contentView positioned:NSWindowAbove relativeTo:nil];

        constexpr std::array buttonTypes{
            NSWindowCloseButton,
            NSWindowMiniaturizeButton,
            NSWindowZoomButton,
        };
        std::ranges::for_each(buttonTypes, [nsWindow, parentView](NSWindowButton buttonType) {
            NSButton *const button = [nsWindow standardWindowButton:buttonType];
            NSView *buttonContainer = button;
            while ([buttonContainer superview] != nil && [buttonContainer superview] != parentView) {
                buttonContainer = [buttonContainer superview];
            }
            if ([buttonContainer superview] == parentView) {
                [parentView addSubview:buttonContainer positioned:NSWindowAbove relativeTo:nil];
            }
        });

        KonsollInputForwardingView *inputView = objc_getAssociatedObject(
            nsWindow,
            &inputForwardingViewAssociationKey
        );
        if (inputView == nil) {
            inputView = [[KonsollInputForwardingView alloc] initWithFrame:[parentView bounds]];
            [inputView setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
            objc_setAssociatedObject(
                nsWindow,
                &inputForwardingViewAssociationKey,
                inputView,
                OBJC_ASSOCIATION_RETAIN_NONATOMIC
            );
        }
        [inputView setTargetWindow:nsWindow];
        [inputView setTargetView:contentView];
        [parentView addSubview:inputView positioned:NSWindowAbove relativeTo:nil];
    }
}
