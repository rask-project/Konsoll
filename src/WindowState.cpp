#include "WindowState.h"

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
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

static QString stateDir()
{
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/windows");
    QDir().mkpath(dir);
    return dir;
}

static QString statePath(const QString &id)
{
    return stateDir() + QStringLiteral("/") + id + QStringLiteral(".json");
}

static QJsonObject readState(const QString &id)
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

static void writeState(const QString &id, const QJsonObject &state)
{
    QFile file(statePath(id));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return;
    }
    file.write(QJsonDocument(state).toJson());
    file.close();
}

bool WindowState::load()
{
    const QJsonObject state = readState(QStringLiteral("last"));
    if (state.isEmpty()) {
        return false;
    }

    const QJsonObject geometry = state[QStringLiteral("geometry")].toObject();
    setWidth(geometry[QStringLiteral("width")].toInt(1024));
    setHeight(geometry[QStringLiteral("height")].toInt(768));
    setX(geometry[QStringLiteral("x")].toInt());
    setY(geometry[QStringLiteral("y")].toInt());

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

    QJsonObject geometry;
    geometry[QStringLiteral("width")] = m_width;
    geometry[QStringLiteral("height")] = m_height;
    geometry[QStringLiteral("x")] = m_x;
    geometry[QStringLiteral("y")] = m_y;
    if (QWindow *window = qobject_cast<QWindow *>(m_window)) {
        if (QScreen *screen = window->screen()) {
            geometry[QStringLiteral("screen")] = screen->name();
        }
    }
    state[QStringLiteral("geometry")] = geometry;

    writeState(QStringLiteral("last"), state);
}

} // namespace konsoll
