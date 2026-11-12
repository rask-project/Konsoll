#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>

#include <functional>

namespace konsoll
{

class BackgroundJobs;

// Runs the tools Konsoll's MCP server (McpServer.h) offers an agent, for the
// session named by the request's token: open a pane next to the agent's,
// start a background command, report on the background commands that agent
// started (and only those), list the commands run in its own pane, list the
// panes of its window, and read one of them once the user allows it.
// Requests arrive on the agent status socket
// (AgentStatusServer::setRequestHandler); every reply is
// {"text": ..., "isError": bool}.
class AgentCommands final
{
public:
    using Reply = std::function<void(const QJsonObject &reply)>;

    explicit AgentCommands(BackgroundJobs *jobs);

    // Every tool; read_pane replies once the user answers.
    void handle(const QJsonObject &request, const Reply &answer);
    // The tools that answer at once (all but read_pane of another pane).
    QJsonObject handle(const QJsonObject &request);

private:
    QJsonObject recentCommands(const QString &token, const QJsonObject &arguments) const;
    QJsonObject listPanes(const QString &token) const;
    static void readPane(const QString &token, const QJsonObject &arguments, const Reply &answer);
    QJsonObject openPane(const QString &token, const QJsonObject &arguments, const QString &cwd) const;
    QJsonObject runInBackground(const QString &token, const QJsonObject &arguments, const QString &cwd);
    QJsonObject jobStatus(const QString &token, const QJsonObject &arguments) const;

    // The background command numbers each session's agent started.
    QHash<QString, QList<int>> m_jobsByToken;
    // Non-owning: main() owns the jobs and outlives the socket's requests.
    BackgroundJobs *m_jobs;
};

} // namespace konsoll
