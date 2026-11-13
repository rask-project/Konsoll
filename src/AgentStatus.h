#pragma once

#include <QByteArrayView>
#include <QJsonObject>
#include <QObject>
#include <QString>

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>

class QLocalServer;
class QLocalSocket;

namespace konsoll
{

// What an AI agent (Claude Code, Codex, opencode, Qwen Code) running in a
// session reports through its hooks: busy with a turn, waiting for the user
// (a question; Approval for a permission prompt), done with the turn,
// failed, or gone.
enum class AgentState : std::uint8_t {
    None, // no agent reported, or it ended
    Working,
    Waiting,
    Idle,
    Error,
    Approval // waiting for the user to allow or deny a tool (a permission prompt)
};

// "working", "waiting", "idle", "error", "approval"; empty for None.
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

// The token an agent in a shell Konsoll set up over ssh (konsoll-ssh.sh)
// reports with: derived from the session's, so the host never learns the
// one that drives this Mac's socket. KONSOLL_REMOTE_AGENT_TOKEN.
QString remoteAgentToken(const QString &sessionToken);

// An agent's report from over ssh, written to its terminal as
// OSC 1337 ; KonsollAgent=<token>;<agent>;<state>[;text=<base64>]
//                       [;keys=<key>|<key>...;event=<base64 JSON>]
// (`payload` is what follows "KonsollAgent="). The message is the text, or
// the first of the keys present in the hook's event (dotted paths, as
// konsoll-agent-hook's @key), which may be cut short. Malformed unless the
// token is `token`. The returned token is empty.
std::expected<AgentStatusMessage, AgentStatusError>
parseRemoteAgentReport(QByteArrayView payload, const QString &token);

// The string at the first of `keys` ("a.b|c") present in the JSON `event`;
// `event` may be cut short (the remote hook sends its start), and then a
// key is looked up by its last component in what arrived.
QString agentEventField(const QByteArray &event, const QString &keys);

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

    // Answers a JSON request line ({"token", "tool", "arguments"}, sent by
    // `konsoll --mcp` for an agent's tool call) with a JSON reply line,
    // through `reply`: at once, or later (a request waiting for the user).
    // A reply after the client went away is dropped.
    using Reply = std::function<void(const QJsonObject &reply)>;
    using RequestHandler = std::function<void(const QJsonObject &request, Reply reply)>;
    void setRequestHandler(RequestHandler handler);

signals:
    void statusReceived(const QString &token, const QString &agent, AgentState state, const QString &message);

private:
    AgentStatusServer();
    void onNewConnection();
    void readStatus(QLocalSocket *socket);

    std::unique_ptr<QLocalServer> m_server;
    RequestHandler m_requestHandler;
};

} // namespace konsoll
