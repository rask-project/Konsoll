#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QtQml/qqmlregistration.h>

namespace konsoll
{

class WindowState : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QObject *window READ window WRITE setWindow)
    Q_PROPERTY(QObject *manager READ manager WRITE setManager)
    Q_PROPERTY(QString windowId READ windowId WRITE setWindowId)
    Q_PROPERTY(int width READ width WRITE setWidth NOTIFY changed)
    Q_PROPERTY(int height READ height WRITE setHeight NOTIFY changed)
    Q_PROPERTY(int x READ x WRITE setX NOTIFY changed)
    Q_PROPERTY(int y READ y WRITE setY NOTIFY changed)
    Q_PROPERTY(QJsonArray layout READ layout NOTIFY changed)
    Q_PROPERTY(bool hasLayout READ hasLayout NOTIFY changed)

public:
    explicit WindowState(QObject *parent = nullptr);

    QObject *window() const;
    void setWindow(QObject *window);
    QObject *manager() const;
    void setManager(QObject *manager);
    QString windowId() const;
    void setWindowId(QString windowId);
    int width() const;
    void setWidth(int width);
    int height() const;
    void setHeight(int height);
    int x() const;
    void setX(int x);
    int y() const;
    void setY(int y);
    QJsonArray layout() const;
    bool hasLayout() const;

    Q_INVOKABLE bool load();
    Q_INVOKABLE void save();
    Q_INVOKABLE void removeState();

signals:
    void changed();

private:
    QObject *m_window{};
    QObject *m_manager{};
    QJsonArray m_layout;
    QString m_windowId;
    int m_width{1024};
    int m_height{768};
    int m_x{0};
    int m_y{0};
};

} // namespace konsoll
