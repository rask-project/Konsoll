#include "WindowRegistry.h"

#include "TerminalManager.h"

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
