#pragma once

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

private:
    WindowRegistry() = default;

    // Non-owning: the QML engine owns each manager; the registry only tracks
    // them for aggregation.
    std::vector<TerminalManager *> m_managers;
};

} // namespace konsoll
