#pragma once

#include <QObject>
#include <QPointer>
#include <QtQml/qqmlregistration.h>

namespace konsoll
{

// App-level controller that bridges AppKit events into QML. It is a QML
// singleton so the manager root (Main.qml) can listen for windowOpenRequested
// and create a window; AppKit (the dock-click / reopen handler) calls
// requestOpenWindow() to trigger it.
class AppController : public QObject
{
    Q_OBJECT
    QML_NAMED_ELEMENT(AppController)
    QML_SINGLETON

public:
    explicit AppController(QObject *parent = nullptr);

    // Emits windowOpenRequested so the QML manager root creates a window.
    void openWindow();

    // AppKit entry point: asks the running app to open a window (dock click /
    // reopen when there are no visible windows).
    static void requestOpenWindow();

signals:
    void windowOpenRequested();

private:
    static QPointer<AppController> s_instance;
};

} // namespace konsoll
