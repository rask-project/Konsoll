#pragma once

#include <QString>

#include <vector>

class QObject;

namespace konsoll
{

class TerminalManager;

// App-level registry of the live per-window TerminalManagers. Each manager
// registers itself in its constructor and deregisters in its destructor, so
// the registry always reflects the windows that are actually open. It gives
// the AppKit quit guard an aggregate view across all windows without
// reintroducing a single global session model.
class WindowRegistry final
{
public:
    static WindowRegistry &instance();

    void add(TerminalManager *manager);
    void remove(TerminalManager *manager);

    int totalSessionCount() const;
    bool anySessionHasActiveProcess() const;
    bool anySessionHasRunningCommand() const;
    // Closes `session` in whichever window owns it now: a workspace may move
    // to another window between an exit and its queued close. `session` may
    // dangle; it is only compared by address.
    void closeSession(QObject *session) const;
    // Brings forward the session with that token (KONSOLL_SESSION_TOKEN), in
    // whichever window holds it. False when no window does.
    bool activateSessionWithToken(const QString &token) const;
    // The window manager holding the session with that token, or null.
    TerminalManager *managerWithToken(const QString &token) const;
    // Updates the Dock badge (agents waiting for the user, in every window)
    // on the next event-loop turn, once for any number of calls.
    void scheduleAgentBadgeUpdate();

private:
    WindowRegistry() = default;

    // Non-owning: the QML engine owns each manager; the registry only tracks
    // them for aggregation.
    std::vector<TerminalManager *> m_managers;
    bool m_badgeUpdatePending{false};
};

} // namespace konsoll
