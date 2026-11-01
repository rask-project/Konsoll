#pragma once

#include "TerminalBackend.h"

#include <QObject>
#include <QVariantList>
#include <QtQml>

#include <memory>
#include <vector>

namespace konsoll
{

class TerminalManager final : public QObject
{
    Q_OBJECT

    QML_NAMED_ELEMENT(SessionManager)
    QML_SINGLETON

    Q_PROPERTY(QVariantList sessions READ sessions NOTIFY sessionsChanged)
    Q_PROPERTY(QObject *activeSession READ activeSession NOTIFY activeSessionChanged)
    Q_PROPERTY(int activeIndex READ activeIndex NOTIFY activeSessionChanged)

public:
    explicit TerminalManager(QObject *parent = nullptr);

    QVariantList sessions() const;
    QObject *activeSession() const;
    int activeIndex() const { return static_cast<int>(m_activeIndex); }

    Q_INVOKABLE void start();
    Q_INVOKABLE void addSession();
    Q_INVOKABLE void closeSession(QObject *session);
    Q_INVOKABLE void setActiveIndex(int index);

signals:
    void sessionsChanged();
    void activeSessionChanged();
    void quitRequested();
    void sessionExited(QObject *session, int code);

private:
    TerminalBackend *createSession();
    void setActiveIndexInternal(int index);

    std::vector<std::unique_ptr<TerminalBackend>> m_sessions;
    qsizetype m_activeIndex{-1};
};

} // namespace konsoll
