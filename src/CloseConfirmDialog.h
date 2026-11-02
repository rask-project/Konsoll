#pragma once

#include <QObject>
#include <QtQml/qqmlregistration.h>

namespace konsoll
{

// Native confirmation shown before closing a session/workspace or quitting
// while a task is in progress. Exposed to QML as a stateless singleton.
class CloseConfirmDialog : public QObject
{
    Q_OBJECT
    QML_NAMED_ELEMENT(CloseConfirmDialog)
    QML_SINGLETON

public:
    using QObject::QObject;

    // Presents the native alert and returns true if the user chose to close
    // anyway, false if they cancelled.
    Q_INVOKABLE bool confirmClose();

    // Presents the native alert before quitting the application.
    bool confirmQuit();
};

} // namespace konsoll
