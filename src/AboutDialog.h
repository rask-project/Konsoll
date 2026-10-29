#pragma once

#include <QObject>
#include <QtQml/qqmlregistration.h>

namespace konsoll
{

class AboutDialog : public QObject
{
    Q_OBJECT
    QML_ELEMENT

public:
    using QObject::QObject;

    Q_INVOKABLE void open();
    Q_INVOKABLE void openAboutQt();
};

} // namespace konsoll
