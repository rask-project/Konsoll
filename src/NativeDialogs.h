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

    // Presents the native alert before quitting the application, reporting how
    // many sessions across all windows will be terminated.
    Q_INVOKABLE bool confirmQuit(int sessionCount);
    Q_INVOKABLE QVariantMap promptWorkspaceRename(const QString &currentTitle);
    Q_INVOKABLE QVariantMap promptWorkspaceName();
    Q_INVOKABLE void approveQuit();
    Q_INVOKABLE void terminateApplication();
    static bool consumeQuitApproval();
    // Asks whether the program in the terminal titled `title` may read the
    // clipboard (OSC 52 query). True if the user allowed it.
    static bool confirmClipboardRead(const QString &title);
};

} // namespace konsoll
