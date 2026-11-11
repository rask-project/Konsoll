#include "MacOSTranslucency.h"
#include "WindowTranslucency.h"

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
    if (m_window != nullptr) {
        disconnect(m_window, nullptr, this, nullptr);
    }
    m_window = window;
    emit windowChanged();
    // The window handle may not exist yet at binding time; defer to the next
    // event-loop turn, by which point winId() has forced the native window.
    QTimer::singleShot(0, this, [this]() { apply(); });
    // Closing a window drops its NSWindow; showing it again creates a new
    // one without the translucency, hidden title and toolbar. Re-apply then.
    if (auto *const qwindow = qobject_cast<QWindow *>(window)) {
        connect(qwindow, &QWindow::visibleChanged, this, [this](bool visible) {
            if (visible) {
                QTimer::singleShot(0, this, [this]() { apply(); });
            }
        });
    }
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

void WindowTranslucency::setBlurEnabled(bool enabled)
{
    if (blurEnabled() == enabled) {
        return;
    }
    setFlag(BlurEnabled, enabled);
    emit blurEnabledChanged();
    apply();
}

void WindowTranslucency::setFullScreen(bool fullScreen)
{
    if (this->fullScreen() == fullScreen) {
        return;
    }
    setFlag(FullScreen, fullScreen);
    emit fullScreenChanged();
    apply();
}

void WindowTranslucency::setWindowMovable(bool movable)
{
    auto const *window = qobject_cast<QWindow *>(m_window);
    if (window == nullptr || (window->winId() == 0U)) {
        return;
    }
    setNativeWindowMovable(window->winId(), movable);
}

void WindowTranslucency::titleBarDoubleClicked()
{
    auto const *window = qobject_cast<QWindow *>(m_window);
    if (window == nullptr || (window->winId() == 0U)) {
        return;
    }
    performTitleBarDoubleClick(window->winId());
}

void WindowTranslucency::apply() const
{
    auto const *w = qobject_cast<QWindow *>(m_window);
    if (w == nullptr || (w->winId() == 0U)) {
        return;
    }

    makeWindowTranslucent(w->winId(), QColor::fromRgba(m_color), m_alpha);
    setWindowBlurEnabled(w->winId(), blurEnabled(), m_alpha);
    installEmptyWindowToolbar(w->winId(), !fullScreen());
}

} // namespace konsoll
