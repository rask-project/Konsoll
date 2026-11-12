#include "WindowRegistry.h"

#include "Notifications.h"
#include "TerminalManager.h"

#include <QCoreApplication>
#include <QTimer>

#include <algorithm>
#include <ranges>

namespace konsoll
{

WindowRegistry &WindowRegistry::instance()
{
    static WindowRegistry s_instance;
    return s_instance;
}

void WindowRegistry::add(TerminalManager *manager)
{
    if (std::ranges::find(m_managers, manager) == m_managers.end()) {
        m_managers.push_back(manager);
    }
}

void WindowRegistry::remove(TerminalManager *manager)
{
    std::erase(m_managers, manager);
    scheduleAgentBadgeUpdate();
}

void WindowRegistry::scheduleAgentBadgeUpdate()
{
    if (m_badgeUpdatePending || QCoreApplication::instance() == nullptr) {
        return;
    }
    m_badgeUpdatePending = true;
    QTimer::singleShot(0, QCoreApplication::instance(), [this]() {
        m_badgeUpdatePending = false;
        setDockBadge(std::ranges::fold_left(m_managers, 0, [](int total, const TerminalManager *manager) {
            return total + manager->agentsWaitingCount();
        }));
    });
}

int WindowRegistry::totalSessionCount() const
{
    int count = 0;
    for (TerminalManager *manager : m_managers) {
        count += manager->sessionCount();
    }
    return count;
}

bool WindowRegistry::anySessionHasActiveProcess() const
{
    return std::ranges::any_of(m_managers, [](TerminalManager *manager) {
        return manager->anySessionHasActiveProcess();
    });
}

bool WindowRegistry::anySessionHasRunningCommand() const
{
    return std::ranges::any_of(m_managers, [](TerminalManager *manager) {
        return manager->anySessionHasRunningCommand();
    });
}

bool WindowRegistry::activateSessionWithToken(const QString &token) const
{
    return std::ranges::any_of(m_managers, [&token](TerminalManager *manager) {
        return manager->activateSessionWithToken(token);
    });
}

TerminalManager *WindowRegistry::managerWithToken(const QString &token) const
{
    const auto found = std::ranges::find_if(m_managers, [&token](const TerminalManager *manager) {
        return manager->sessionWithToken(token) != nullptr;
    });
    return found != m_managers.end() ? *found : nullptr;
}

void WindowRegistry::closeSession(QObject *session) const
{
    const auto owner = std::ranges::find_if(m_managers, [session](const TerminalManager *manager) {
        return manager->hasSession(session);
    });
    if (owner != m_managers.end()) {
        (*owner)->closeSession(session);
    }
}

} // namespace konsoll
