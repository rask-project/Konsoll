#pragma once

#include <QObject>
#include <QString>
#include <QtQml/qqmlregistration.h>

namespace konsoll
{

class WindowScreen : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QObject *window READ window WRITE setWindow)

public:
    explicit WindowScreen(QObject *parent = nullptr);

    [[nodiscard]] QObject *window() const;
    void setWindow(QObject *window);

    Q_INVOKABLE QString currentScreenName() const;
    Q_INVOKABLE bool restoreScreen(const QString &name) const;

private:
    QObject *m_window = nullptr;
};

} // namespace konsoll
