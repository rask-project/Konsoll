#pragma once

#include <QObject>
#include <QtQml>

#ifdef __OBJC__
@class NSMenu;
#endif

namespace konsoll
{

// The Dock icon's menu: "New Tab" and "New Window" above the window list
// AppKit adds. Konsoll's app delegate (AppQuitGuard) replaces Qt's, so it
// answers applicationDockMenu: with menu(). Main.qml answers the signals.
class DockMenu final : public QObject
{
    Q_OBJECT
    QML_NAMED_ELEMENT(DockMenu)
    QML_SINGLETON

public:
    static DockMenu &shared();
    static DockMenu *create(QQmlEngine *qmlEngine, QJSEngine *jsEngine);

    ~DockMenu() override = default;
    Q_DISABLE_COPY_MOVE(DockMenu)

#ifdef __OBJC__
    // Built once and kept for the app's lifetime (AppKit asks for it each
    // time the Dock menu opens).
    static NSMenu *menu();
#endif

signals:
    void newTabRequested();
    void newWindowRequested();

private:
    DockMenu() = default;
};

} // namespace konsoll
