#include "TerminalManager.h"

#include <QCoreApplication>
#include <QKeyEvent>
#include <QTimer>

#include <algorithm>
#include <iterator>
#include <ranges>
#include <utility>

namespace konsoll
{

TerminalManager::TerminalManager(QObject *parent) : QObject(parent)
{
    if (QCoreApplication *application = QCoreApplication::instance()) {
        application->installEventFilter(this);
    }
}

bool TerminalManager::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::KeyRelease) {
        const auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->key() == Qt::Key_Control || keyEvent->key() == Qt::Key_Meta) {
            commitActivationHistory();
        }
    }
    return QObject::eventFilter(watched, event);
}

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
    recordActivation(static_cast<int>(m_sessions.size()) - 1);
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
    std::erase(m_activationHistory, removedIndex);
    std::ranges::for_each(m_activationHistory, [removedIndex](int &historyIndex) {
        if (historyIndex > removedIndex) {
            --historyIndex;
        }
    });

    if (wasActive) {
        m_activeIndex = std::min(removedIndex, static_cast<int>(m_sessions.size()) - 1);
        recordActivation(static_cast<int>(m_activeIndex));
        emit activeSessionChanged();
    } else if (removedIndex < m_activeIndex) {
        --m_activeIndex;
        emit activeSessionChanged();
    }
    if (!wasActive) {
        const auto activeHistory = std::ranges::find(m_activationHistory, static_cast<int>(m_activeIndex));
        if (activeHistory == m_activationHistory.end()) {
            recordActivation(static_cast<int>(m_activeIndex));
        } else {
            m_activationHistoryCursor = static_cast<qsizetype>(std::distance(m_activationHistory.begin(), activeHistory));
        }
    }
    emit sessionsChanged();
}

void TerminalManager::setActiveIndex(int index)
{
    if (index < 0 || index >= static_cast<int>(m_sessions.size())) {
        return;
    }
    setActiveIndexInternal(index);
    recordActivation(index);
}

void TerminalManager::navigateActivationHistory(int direction)
{
    const qsizetype historySize = static_cast<qsizetype>(m_activationHistory.size());
    if (historySize < 2) {
        return;
    }

    const qsizetype step = direction < 0 ? -1 : 1;
    m_activationHistoryCursor = (m_activationHistoryCursor + step + historySize) % historySize;
    setActiveIndexInternal(m_activationHistory.at(static_cast<std::size_t>(m_activationHistoryCursor)));
}

void TerminalManager::recordActivation(int index)
{
    std::erase(m_activationHistory, index);
    m_activationHistory.insert(m_activationHistory.begin(), index);
    m_activationHistoryCursor = 0;
}

void TerminalManager::commitActivationHistory()
{
    if (m_activationHistoryCursor > 0 &&
        m_activationHistoryCursor < static_cast<qsizetype>(m_activationHistory.size())) {
        recordActivation(m_activationHistory.at(static_cast<std::size_t>(m_activationHistoryCursor)));
    }
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
