#include "WindowTranslucency.h"
#include "MacOSTranslucency.h"

#include <QColor>
#include <QTimer>
#include <QWindow>

namespace konsoll
{

WindowTranslucency::WindowTranslucency(QObject *parent) : QObject(parent) {}

void WindowTranslucency::setWindow(QObject *window)
{
    if (m_window == window) {
        return;
    }
    m_window = window;
    emit windowChanged();
    // The window handle may not exist yet at binding time; defer to the next
    // event-loop turn, by which point winId() has forced the native window.
    QTimer::singleShot(0, this, [this]() { apply(); });
}

void WindowTranslucency::setColor(uint color)
{
    if (m_color == color) {
        return;
    }
    m_color = color;
    emit colorChanged();
    apply();
}

void WindowTranslucency::setAlpha(qreal alpha)
{
    if (qFuzzyCompare(m_alpha, alpha)) {
        return;
    }
    m_alpha = alpha;
    emit alphaChanged();
    apply();
}

void WindowTranslucency::setWindowMovable(bool movable)
{
    auto const *window = qobject_cast<QWindow *>(m_window);
    if (window == nullptr || !window->winId()) {
        return;
    }
    setNativeWindowMovable(window->winId(), movable);
}

void WindowTranslucency::apply() const
{
    auto const *w = qobject_cast<QWindow *>(m_window);
    if (w == nullptr || !w->winId()) {
        return;
    }

    makeWindowTranslucent(w->winId(), QColor::fromRgba(m_color), m_alpha);
}

} // namespace konsoll
