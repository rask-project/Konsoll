#pragma once

#include <QByteArrayView>
#include <QObject>
#include <QString>

#include <cstdint>
#include <expected>
#include <memory>

class QLocalServer;
class QLocalSocket;

namespace konsoll
{

// What an AI agent (Claude Code, Codex, opencode, Qwen Code) running in a
// session reports through its hooks: busy with a turn, waiting for the user
// (a permission prompt, a question), done with the turn, failed, or gone.
enum class AgentState : std::uint8_t {
    None, // no agent reported, or it ended
    Working,
    Waiting,
    Idle,
    Error
};

// "working", "waiting", "idle", "error"; empty for None.
QString agentStateName(AgentState state);

// One status line: "<session token> <agent> <state> [<base64 message>]".
struct AgentStatusMessage {
    QString token;
    QString agent;
    QString message;
    // An AgentState, widened so the struct has no padding (-Wpadded).
    std::uint64_t stateValue{0};

    AgentState state() const { return static_cast<AgentState>(stateValue); }
};

enum class AgentStatusError : std::uint8_t {
    Malformed,
    UnknownState,
    BadAgentName
};

std::expected<AgentStatusMessage, AgentStatusError> parseAgentStatus(QByteArrayView line);

// Where the agent hooks reach Konsoll: a Unix socket private to the user,
// exported to every session as KONSOLL_AGENT_SOCKET, and the helper script
// the hooks run (KONSOLL_AGENT_HOOK). Both are fixed for the process and
// safe to read from any thread once AgentStatusServer::instance() ran.
QString agentStatusSocketPath();
QString agentHookPath();

// Listens on agentStatusSocketPath() and announces every valid status line.
// Hooks run without a controlling terminal (Claude Code starts them in a
// session of their own), so they cannot write escape sequences to the tab:
// they send the session's token (KONSOLL_SESSION_TOKEN) here instead, and
// the session with that token takes the state.
class AgentStatusServer final : public QObject
{
    Q_OBJECT

public:
    static AgentStatusServer &instance();
    ~AgentStatusServer() override;

    AgentStatusServer(const AgentStatusServer &) = delete;
    AgentStatusServer &operator=(const AgentStatusServer &) = delete;
    AgentStatusServer(AgentStatusServer &&) = delete;
    AgentStatusServer &operator=(AgentStatusServer &&) = delete;

    bool listening() const;

signals:
    void statusReceived(const QString &token, const QString &agent, AgentState state, const QString &message);

private:
    AgentStatusServer();
    void onNewConnection();
    void readStatus(QLocalSocket *socket);

    std::unique_ptr<QLocalServer> m_server;
};

} // namespace konsoll
