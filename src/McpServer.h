#pragma once

#include <QJsonObject>

#include <functional>

namespace konsoll
{

// `konsoll --mcp`: a Model Context Protocol server on stdin/stdout that an
// agent starts as a local (stdio) MCP server. It inherits the agent's
// environment, so KONSOLL_SESSION_TOKEN names the pane the agent runs in;
// each tool call goes to the running Konsoll over KONSOLL_AGENT_SOCKET
// (AgentCommands answers it). Outside Konsoll it offers no tools.
int runMcpServer();

// One JSON-RPC message in, the reply out (empty for a notification).
// `forward` sends a tool call to Konsoll and returns its reply.
QJsonObject handleMcpMessage(
    const QJsonObject &message,
    bool inKonsoll,
    const std::function<QJsonObject(const QJsonObject &)> &forward
);

} // namespace konsoll
