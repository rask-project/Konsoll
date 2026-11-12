#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>

namespace konsoll
{

class BackgroundJobs;

// Runs the tools Konsoll's MCP server (McpServer.h) offers an agent, for the
// session named by the request's token: open a pane next to the agent's,
// start a background command, and report on the background commands that
// agent started (and only those). Requests arrive on the agent status
// socket (AgentStatusServer::setRequestHandler); every reply is
// {"text": ..., "isError": bool}.
class AgentCommands final
{
public:
    explicit AgentCommands(BackgroundJobs *jobs);

    QJsonObject handle(const QJsonObject &request);

private:
    QJsonObject openPane(const QString &token, const QJsonObject &arguments, const QString &cwd) const;
    QJsonObject runInBackground(const QString &token, const QJsonObject &arguments, const QString &cwd);
    QJsonObject jobStatus(const QString &token, const QJsonObject &arguments) const;

    // The background command numbers each session's agent started.
    QHash<QString, QList<int>> m_jobsByToken;
    // Non-owning: main() owns the jobs and outlives the socket's requests.
    BackgroundJobs *m_jobs;
};

} // namespace konsoll
