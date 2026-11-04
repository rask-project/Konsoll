#pragma once

#include <QObject>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

namespace konsoll
{

// Native AppKit dialogs exposed to QML as a stateless singleton.
class NativeDialogs : public QObject
{
    Q_OBJECT
    QML_NAMED_ELEMENT(NativeDialogs)
    QML_SINGLETON

public:
    using QObject::QObject;

    // Presents the native alert and returns true if the user chose to close
    // anyway, false if they cancelled.
    Q_INVOKABLE bool confirmClose();

    // Presents the native alert before quitting the application.
    Q_INVOKABLE bool confirmQuit();
    Q_INVOKABLE QVariantMap promptWorkspaceRename(const QString &currentTitle);
    Q_INVOKABLE void approveQuit();
    Q_INVOKABLE void terminateApplication();
    static bool consumeQuitApproval();
};

} // namespace konsoll
