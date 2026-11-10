#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/runtime.h>

#include <algorithm>
#include <array>

#include <QDebug>
#include "MacOSTranslucency.h"

namespace
{
const char effectViewAssociationKey{};
const char toolbarAssociationKey{};
const char inputForwardingViewAssociationKey{};
const char glassHostAssociationKey{};

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

    // QML draws its own title bar (the tab strip) in the expanded client area.
    // The window title only feeds the Window menu and the window switcher, so
    // AppKit must not paint it over the first tab.
    [nsWindow setTitleVisibility:NSWindowTitleHidden];
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

            // The superview is the window's frame view, which also contains
            // the native title bar/toolbar area. Using contentView's bounds
            // leaves that area transparent but unblurred.
            NSRect const effectFrame = [parentView bounds];
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
            // Stay below the glass host (if any) so the glass samples the blur.
            NSView *const glassHost = objc_getAssociatedObject(nsWindow, &glassHostAssociationKey);
            NSView *const anchorView = ([glassHost superview] == parentView) ? glassHost : contentView;
            [parentView addSubview:effectView positioned:NSWindowBelow relativeTo:anchorView];
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

        KonsollInputForwardingView *inputView = objc_getAssociatedObject(nsWindow, &inputForwardingViewAssociationKey);
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

namespace
{
NSView *glassHostContentView(NSWindow *nsWindow, bool create) API_AVAILABLE(macos(26.0))
{
    if (nsWindow == nil) {
        return nil;
    }
    NSGlassEffectContainerView *host = objc_getAssociatedObject(nsWindow, &glassHostAssociationKey);
    if (host != nil || !create) {
        return [host contentView];
    }

    NSView *const contentView = [nsWindow contentView];
    NSView *const parentView = [contentView superview];
    if (parentView == nil) {
        return nil;
    }

    // Like the blur view, the host lives in the frame view so its coordinate
    // space covers the expanded title-bar area. It sits directly below the Qt
    // content view and therefore above the blur view.
    NSRect const bounds = [parentView bounds];
    host = [[[NSGlassEffectContainerView alloc] initWithFrame:bounds] autorelease];
    [host setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    NSView *const hostContent = [[[NSView alloc] initWithFrame:bounds] autorelease];
    [hostContent setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [host setContentView:hostContent];
    objc_setAssociatedObject(nsWindow, &glassHostAssociationKey, host, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    [parentView addSubview:host positioned:NSWindowBelow relativeTo:contentView];
    return hostContent;
}

NSGlassEffectView *findGlassView(NSView *hostContent, const QString &identifier) API_AVAILABLE(macos(26.0))
{
    if (hostContent == nil) {
        return nil;
    }
    NSString *const nsIdentifier = identifier.toNSString();
    NSArray<NSView *> *const subviews = [hostContent subviews];
    NSUInteger const index = [subviews indexOfObjectPassingTest:^BOOL(NSView *view, NSUInteger, BOOL *) {
      return [[view identifier] isEqualToString:nsIdentifier];
    }];
    if (index == NSNotFound) {
        return nil;
    }
    return static_cast<NSGlassEffectView *>(subviews[index]);
}
} // namespace

bool isNativeGlassSupported()
{
    if (@available(macOS 26.0, *)) {
        return true;
    }
    return false;
}

bool createNativeGlassView(WId winId, const QString &identifier)
{
    if (@available(macOS 26.0, *)) {
        NSView *const hostContent = glassHostContentView(windowFromId(winId), true);
        if (hostContent == nil) {
            return false;
        }
        if (findGlassView(hostContent, identifier) != nil) {
            return true;
        }
        NSGlassEffectView *const glass = [[[NSGlassEffectView alloc] initWithFrame:NSZeroRect] autorelease];
        [glass setIdentifier:identifier.toNSString()];
        [glass setHidden:YES];
        [hostContent addSubview:glass];
        return true;
    }
    return false;
}

void updateNativeGlassView(
    WId winId,
    const QString &identifier,
    const QRectF &sceneRect,
    qreal cornerRadius,
    const QColor &tint,
    qreal alpha,
    bool clearStyle
)
{
    if (@available(macOS 26.0, *)) {
        NSWindow *const nsWindow = windowFromId(winId);
        NSView *const hostContent = glassHostContentView(nsWindow, false);
        NSGlassEffectView *const glass = findGlassView(hostContent, identifier);
        if (glass == nil) {
            return;
        }

        // QML scene coordinates are the Qt content view's coordinates;
        // convertRect handles the flipped/unflipped difference.
        NSRect const frame = [[nsWindow contentView] convertRect:sceneRect.toCGRect() toView:hostContent];
        NSColor *const tintColor = tint.alpha() > 0 ? [NSColor colorWithRed:tint.redF()
                                                                      green:tint.greenF()
                                                                       blue:tint.blueF()
                                                                      alpha:tint.alphaF()]
                                                    : nil;

        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        [glass setFrame:frame];
        [glass setCornerRadius:cornerRadius];
        [glass setStyle:clearStyle ? NSGlassEffectViewStyleClear : NSGlassEffectViewStyleRegular];
        [glass setTintColor:tintColor];
        [glass setAlphaValue:alpha];
        [glass setHidden:alpha <= 0.0];
        [CATransaction commit];
    }
}

void destroyNativeGlassView(WId winId, const QString &identifier)
{
    if (@available(macOS 26.0, *)) {
        NSView *const hostContent = glassHostContentView(windowFromId(winId), false);
        [findGlassView(hostContent, identifier) removeFromSuperview];
    }
}
