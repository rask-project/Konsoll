#pragma once

#include <QColor>
#include <QRectF>
#include <QString>
#include <QWindow>

// Makes the window see-through like iTerm2: the NSWindow becomes non-opaque and
// its background is the given color at the given alpha, so the desktop shows
// through while the terminal stays readable. The whole window (content + title
// bar) is tinted see-through — that is the iTerm2 look. macOS-specific.
void makeWindowTranslucent(WId winId, const QColor &color, qreal alpha);
void setWindowBlurEnabled(WId winId, bool enabled, qreal alpha);
void setNativeWindowMovable(WId winId, bool movable);
void installEmptyWindowToolbar(WId winId, bool visible);

// Liquid Glass (macOS 26+): native NSGlassEffectViews placed behind the Qt
// content view, above the window blur, so QML can draw on top of real glass.
// Each view is addressed by a caller-chosen identifier.
bool isNativeGlassSupported();
bool createNativeGlassView(WId winId, const QString &identifier);
void updateNativeGlassView(
    WId winId,
    const QString &identifier,
    const QRectF &sceneRect,
    qreal cornerRadius,
    const QColor &tint,
    qreal alpha,
    bool clearStyle
);
void destroyNativeGlassView(WId winId, const QString &identifier);
