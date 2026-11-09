#pragma once

#include <QString>
#include <QVariant>
#include <QtQml/qqmlregistration.h>

namespace konsoll
{

// App-level store for the collection of open windows. Each window has a stable
// string ID; the collection (which windows are open) is persisted so the app can
// reopen every window on relaunch. The "terminating" flag is set by the AppKit
// quit guard when a quit is confirmed, so a per-window close handler can tell a
// mid-session close (drop the window's state) from a quit-time close (keep it).
class WindowStateStore : public QObject
{
    Q_OBJECT
    QML_NAMED_ELEMENT(WindowStateStore)
    QML_SINGLETON

public:
    using QObject::QObject;

    // The IDs of the windows to reopen on the next launch (the persisted collection).
    Q_INVOKABLE QStringList listWindowIds() const;

    // A fresh, unique ID for a newly opened window.
    Q_INVOKABLE QString generateWindowId() const;

    // Persist the collection of open window IDs.
    Q_INVOKABLE void saveCollection(const QVariant &ids) const;

    // The ID of the most recently active window, whose geometry is restored as
    // the single window on the next launch.
    Q_INVOKABLE QString lastWindowId() const;

    // Persist the most recently active window's ID.
    Q_INVOKABLE void saveLastWindowId(const QString &id) const;

    // True while the app is terminating (a confirmed quit), so close handlers can
    // skip dropping per-window state that the quit already saved.
    Q_INVOKABLE bool terminating() const;
    static void setTerminating(bool terminating);

private:
    static bool s_terminating;
};

} // namespace konsoll
