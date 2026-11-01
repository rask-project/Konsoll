#include "TerminalManager.h"

#include <QTimer>

#include <algorithm>
#include <iterator>
#include <ranges>
#include <utility>

namespace konsoll
{

TerminalManager::TerminalManager(QObject *parent) : QObject(parent) {}

QVariantList TerminalManager::sessions() const
{
    QVariantList list;
    list.reserve(static_cast<qsizetype>(m_sessions.size()));
    std::ranges::transform(m_sessions, std::back_inserter(list), [](const auto &session) {
        return QVariant::fromValue(static_cast<QObject *>(session.get()));
    });
    return list;
}

QObject *TerminalManager::activeSession() const
{
    if (m_activeIndex < 0 || m_activeIndex >= static_cast<int>(m_sessions.size())) {
        return nullptr;
    }
    return m_sessions.at(static_cast<std::size_t>(m_activeIndex)).get();
}

void TerminalManager::start()
{
    if (m_sessions.empty()) {
        createSession();
    }
}

void TerminalManager::addSession()
{
    createSession();
}

TerminalBackend *TerminalManager::createSession()
{
    auto session = std::make_unique<TerminalBackend>();
    auto *sessionPtr = session.get();
    connect(sessionPtr, &TerminalBackend::processExited, this, [this, sessionPtr](int code) {
        if (m_sessions.size() > 1) {
            emit sessionExited(sessionPtr, code);
        }
        // Defer removal until the PTY's readable callback has returned. The
        // exited session is destroyed by closeSession, or the app quits if it
        // was the final tab.
        QTimer::singleShot(0, this, [this, sessionPtr]() { closeSession(sessionPtr); });
    });

    m_sessions.push_back(std::move(session));
    emit sessionsChanged();
    setActiveIndexInternal(static_cast<int>(m_sessions.size()) - 1);
    sessionPtr->start();
    return sessionPtr;
}

void TerminalManager::closeSession(QObject *session)
{
    const auto it = std::ranges::find_if(m_sessions, [session](const auto &candidate) {
        return candidate.get() == session;
    });
    if (it == m_sessions.end()) {
        return;
    }
    if (m_sessions.size() == 1) {
        emit quitRequested();
        return;
    }

    const int removedIndex = static_cast<int>(std::distance(m_sessions.begin(), it));
    const bool wasActive = removedIndex == m_activeIndex;
    m_sessions.erase(it);

    if (wasActive) {
        m_activeIndex = std::min(removedIndex, static_cast<int>(m_sessions.size()) - 1);
        emit activeSessionChanged();
    } else if (removedIndex < m_activeIndex) {
        --m_activeIndex;
        emit activeSessionChanged();
    }
    emit sessionsChanged();
}

void TerminalManager::setActiveIndex(int index)
{
    if (index < 0 || index >= static_cast<int>(m_sessions.size())) {
        return;
    }
    setActiveIndexInternal(index);
}

void TerminalManager::setActiveIndexInternal(int index)
{
    if (m_activeIndex == index) {
        return;
    }
    m_activeIndex = index;
    emit activeSessionChanged();
}

} // namespace konsoll
