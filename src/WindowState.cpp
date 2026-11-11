#include "WindowState.h"

#include "TerminalManager.h"

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QRect>
#include <QScreen>
#include <QStandardPaths>
#include <QWindow>
#include <algorithm>

namespace konsoll
{

WindowState::WindowState(QObject *parent) : QObject(parent) {}

QObject *WindowState::window() const
{
    return m_window;
}

void WindowState::setWindow(QObject *window)
{
    m_window = window;
}

QObject *WindowState::manager() const
{
    return m_manager;
}

void WindowState::setManager(QObject *manager)
{
    m_manager = manager;
}

QString WindowState::windowId() const
{
    return m_windowId;
}

void WindowState::setWindowId(QString windowId)
{
    m_windowId = std::move(windowId);
}

QJsonArray WindowState::layout() const
{
    return m_layout;
}

bool WindowState::hasLayout() const
{
    return !m_layout.isEmpty();
}

int WindowState::width() const
{
    return m_width;
}

void WindowState::setWidth(int width)
{
    if (m_width == width) {
        return;
    }
    m_width = width;
    emit changed();
}

int WindowState::height() const
{
    return m_height;
}

void WindowState::setHeight(int height)
{
    if (m_height == height) {
        return;
    }
    m_height = height;
    emit changed();
}

int WindowState::x() const
{
    return m_x;
}

void WindowState::setX(int x)
{
    if (m_x == x) {
        return;
    }
    m_x = x;
    emit changed();
}

int WindowState::y() const
{
    return m_y;
}

void WindowState::setY(int y)
{
    if (m_y == y) {
        return;
    }
    m_y = y;
    emit changed();
}

namespace
{

QString stateDir()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/windows");
    QDir().mkpath(dir);
    return dir;
}

QString statePath(const QString &id)
{
    return stateDir() + QStringLiteral("/") + id + QStringLiteral(".json");
}

QJsonObject readState(const QString &id)
{
    const QString path = statePath(id);
    if (!QFile::exists(path)) {
        return QJsonObject();
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QJsonObject();
    }
    const QByteArray data = file.readAll();
    file.close();
    const QJsonDocument doc = QJsonDocument::fromJson(data);
    return doc.isObject() ? doc.object() : QJsonObject();
}

void writeState(const QString &id, const QJsonObject &state)
{
    QFile file(statePath(id));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return;
    }
    file.write(QJsonDocument(state).toJson());
    file.close();
}

} // namespace

bool WindowState::load()
{
    const QJsonObject state = readState(m_windowId);
    if (state.isEmpty()) {
        return false;
    }

    const QJsonObject geometry = state[QStringLiteral("geometry")].toObject();
    setWidth(geometry[QStringLiteral("width")].toInt(1024));
    setHeight(geometry[QStringLiteral("height")].toInt(768));
    setX(geometry[QStringLiteral("x")].toInt());
    setY(geometry[QStringLiteral("y")].toInt());
    m_state = geometry[QStringLiteral("state")].toString(QStringLiteral("windowed"));
    m_layout = state[QStringLiteral("layout")].toArray();
    emit changed();

    if (QWindow *window = qobject_cast<QWindow *>(m_window)) {
        const QString screenName = geometry[QStringLiteral("screen")].toString();
        if (!screenName.isEmpty()) {
            const auto screens = QGuiApplication::screens();
            const auto screen = std::ranges::find_if(screens, [&screenName](QScreen *candidate) {
                return candidate->name() == screenName;
            });
            if (screen != screens.end()) {
                window->setScreen(*screen);
            }
        }
    }

    return true;
}

void WindowState::save()
{
    QJsonObject state;
    state[QStringLiteral("version")] = 1;

    QWindow *window = qobject_cast<QWindow *>(m_window);
    // A windowed window's live geometry is authoritative: the tracked values
    // miss the initial placement (assigned while the window is still hidden)
    // when the window is never moved. Fullscreen/maximized geometry belongs to
    // the monitor, so keep the last normal geometry tracked from QML then.
    if (window != nullptr && window->visibility() == QWindow::Windowed) {
        const QRect live = window->geometry();
        m_x = live.x();
        m_y = live.y();
        m_width = live.width();
        m_height = live.height();
    }
    if (window != nullptr) {
        switch (window->visibility()) {
        case QWindow::Maximized:
            m_state = QStringLiteral("maximized");
            break;
        case QWindow::FullScreen:
            m_state = QStringLiteral("fullscreen");
            break;
        case QWindow::Windowed:
            m_state = QStringLiteral("windowed");
            break;
        default: // Hidden/Minimized/AutomaticVisibility: keep the last shown state.
            break;
        }
    }

    QJsonObject geometry;
    geometry[QStringLiteral("width")] = m_width;
    geometry[QStringLiteral("height")] = m_height;
    geometry[QStringLiteral("x")] = m_x;
    geometry[QStringLiteral("y")] = m_y;
    geometry[QStringLiteral("state")] = m_state;
    if (window != nullptr) {
        if (QScreen *screen = window->screen()) {
            geometry[QStringLiteral("screen")] = screen->name();
        }
    }
    state[QStringLiteral("geometry")] = geometry;

    if (TerminalManager *manager = qobject_cast<TerminalManager *>(m_manager)) {
        state[QStringLiteral("layout")] = manager->serializeState();
    }

    writeState(m_windowId, state);
}

void WindowState::removeState()
{
    QFile::remove(statePath(m_windowId));
}

} // namespace konsoll
