#pragma once

#include <QColor>
#include <QWindow>

// Makes the window see-through like iTerm2: the NSWindow becomes non-opaque and
// its background is the given color at the given alpha, so the desktop shows
// through while the terminal stays readable. The whole window (content + title
// bar) is tinted see-through — that is the iTerm2 look. macOS-specific.
void makeWindowTranslucent(WId winId, const QColor &color, qreal alpha);
void setWindowBlurEnabled(WId winId, bool enabled, qreal alpha);
void setNativeWindowMovable(WId winId, bool movable);
