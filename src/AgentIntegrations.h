#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QtQml>

#include <expected>
#include <span>
#include <string_view>

namespace konsoll
{

// One hook an agent runs: on `event` (filtered by `matcher`, empty for
// none) it reports `state` with `message` (text, or @key of the event's
// JSON) through konsoll-agent-hook.
struct AgentHookSpec {
    std::string_view event;
    std::string_view matcher;
    std::string_view state;
    std::string_view message;
};

// The hooks Konsoll adds for each agent that uses Claude Code's hook
// format (a "hooks" object of event -> [{matcher, hooks: [command]}]).
std::span<const AgentHookSpec> claudeHookSpecs();
std::span<const AgentHookSpec> codexHookSpecs();
std::span<const AgentHookSpec> qwenHookSpecs();

// The command a hook runs: konsoll-agent-hook through KONSOLL_AGENT_HOOK,
// a no-op outside Konsoll.
QString agentHookCommand(std::string_view agent, const AgentHookSpec &spec);

// Adds the hooks for `agent` to the JSON settings file at `path` (created
// when missing), replacing ones Konsoll added before and keeping
// everything else. The previous file is kept as `path`.konsoll-backup.
std::expected<void, QString>
installAgentHooks(const QString &path, std::string_view agent, std::span<const AgentHookSpec> specs);
// Removes the hooks Konsoll added from `path`, keeping everything else.
std::expected<void, QString> removeAgentHooks(const QString &path);
// Whether `path` has hooks Konsoll added.
bool hasAgentHooks(const QString &path);
// Whether the hooks Konsoll added to `path` are exactly the current `specs`
// (false after an update changed them: Settings offers to update).
bool agentHooksCurrent(const QString &path, std::string_view agent, std::span<const AgentHookSpec> specs);

// Settings > AI Agents: connects the agents Konsoll knows to the tab
// status (AgentStatus.h). Claude Code, Codex and Qwen Code get hooks in
// their settings files; opencode gets a plugin file.
class AgentIntegrations final : public QObject
{
    Q_OBJECT
    QML_NAMED_ELEMENT(AgentIntegrations)
    QML_SINGLETON

    // One map per agent: id, name, path (the file Konsoll changes),
    // available (the agent's configuration directory exists), installed,
    // viaPlugin (Claude Code's Konsoll plugin is enabled instead), outdated
    // (connected with hooks an older Konsoll wrote) and declined (its
    // connect offer was turned down).
    Q_PROPERTY(QVariantList agents READ agents NOTIFY agentsChanged)

public:
    explicit AgentIntegrations(QObject *parent = nullptr);

    QVariantList agents() const;

    // Return an error message, or an empty string on success.
    Q_INVOKABLE QString install(const QString &id);
    Q_INVOKABLE QString uninstall(const QString &id);
    // Re-reads the files (they may change outside Konsoll).
    Q_INVOKABLE void refresh();
    // The id of the agent a pane's foreground process `processName` is, when
    // Konsoll should offer to connect it: one it knows, whose configuration
    // exists, not connected yet (or connected with outdated hooks) and not
    // declined. Empty otherwise.
    Q_INVOKABLE QString connectOffer(const QString &processName) const;
    Q_INVOKABLE QString agentName(const QString &id) const;
    // Connected, but with hooks an older Konsoll wrote.
    Q_INVOKABLE bool outdated(const QString &id) const;
    // "Not now" on the offer: not asked again (Settings ▸ AI Agents still
    // connects it).
    Q_INVOKABLE void declineOffer(const QString &id);
    // How to register Konsoll's MCP server (konsoll --mcp, McpServer.h) with
    // the agent: a shell command, or the configuration to add where the
    // agent has no command that forwards Konsoll's environment.
    Q_INVOKABLE QString mcpSetup(const QString &id) const;
    Q_INVOKABLE void copyToClipboard(const QString &text) const;

    // The top of the git repository `directory` is in, or empty.
    Q_INVOKABLE QString gitRoot(const QString &directory) const;
    // New Agent in Worktree: the shell command a new tab in `root` runs to
    // create (or reuse) a worktree for `branch` beside the repository
    // (<repo>-worktrees/<branch>) and start agent `id` in it.
    // {command, path} or {error}.
    Q_INVOKABLE QVariantMap worktreePlan(const QString &root, const QString &branch, const QString &id) const;

signals:
    void agentsChanged();
};

} // namespace konsoll
