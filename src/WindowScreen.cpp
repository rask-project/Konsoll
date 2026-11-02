#include "WindowScreen.h"

#include <algorithm>
#include <QGuiApplication>
#include <QScreen>
#include <QWindow>

namespace konsoll {

WindowScreen::WindowScreen(QObject *parent) : QObject(parent) {}

QObject *WindowScreen::window() const
{
    return m_window;
}

void WindowScreen::setWindow(QObject *window)
{
    m_window = window;
}

QString WindowScreen::currentScreenName() const
{
    auto const *window = qobject_cast<QWindow *>(m_window);
    auto const *screen = window != nullptr ? window->screen() : nullptr;
    return screen != nullptr ? screen->name() : QString {};
}

bool WindowScreen::restoreScreen(const QString &name) const
{
    auto *window = qobject_cast<QWindow *>(m_window);
    if (window == nullptr || name.isEmpty()) {
        return false;
    }

    auto const screens = QGuiApplication::screens();
    auto const screen = std::ranges::find_if(screens, [&name](QScreen *candidate) {
        return candidate->name() == name;
    });
    if (screen == screens.end()) {
        return false;
    }

    window->setScreen(*screen);
    return true;
}

} // namespace konsoll
