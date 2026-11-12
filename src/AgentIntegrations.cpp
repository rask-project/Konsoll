#include "AgentIntegrations.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSettings>
#include <QVariantMap>

#include <algorithm>
#include <array>
#include <iterator>
#include <ranges>

namespace konsoll
{

namespace
{

using namespace std::string_view_literals;

// What marks a hook command as Konsoll's (agentHookCommand).
constexpr auto kHookMarker = "KONSOLL_AGENT_HOOK"sv;
constexpr int kHookTimeoutSeconds = 5;
// Codex caps its Interrupt hooks at three seconds.
constexpr int kInterruptTimeoutSeconds = 3;

// The tool a permission prompt is about, as the notification shows it.
constexpr auto kToolSubject = "@tool_input.command|tool_input.file_path|tool_input.url|tool_name"sv;

// Permission prompts report "approval" (the notification can answer them);
// questions and other waits report "waiting". A prompt's text becomes the
// agent's task (TerminalBackend::agentTask).
constexpr auto kClaudeHooks = std::to_array<AgentHookSpec>({
    {.event = "UserPromptSubmit", .matcher = {}, .state = "working", .message = "@prompt"},
    {.event = "PostToolUse", .matcher = {}, .state = "working", .message = {}},
    {.event = "PermissionRequest", .matcher = {}, .state = "approval", .message = kToolSubject},
    {.event = "Notification",
     .matcher = "elicitation_dialog|elicitation_url_dialog|agent_needs_input",
     .state = "waiting",
     .message = "@message"},
    {.event = "Stop", .matcher = {}, .state = "idle", .message = {}},
    {.event = "StopFailure", .matcher = {}, .state = "error", .message = {}},
    {.event = "SessionEnd", .matcher = {}, .state = "ended", .message = {}},
});

// Codex has no notification event; PermissionRequest carries the command.
constexpr auto kCodexHooks = std::to_array<AgentHookSpec>({
    {.event = "UserPromptSubmit", .matcher = {}, .state = "working", .message = "@prompt"},
    {.event = "PostToolUse", .matcher = {}, .state = "working", .message = {}},
    {.event = "PermissionRequest", .matcher = {}, .state = "approval", .message = kToolSubject},
    {.event = "Stop", .matcher = {}, .state = "idle", .message = {}},
    {.event = "Interrupt", .matcher = {}, .state = "idle", .message = {}},
    {.event = "SessionEnd", .matcher = {}, .state = "ended", .message = {}},
});

constexpr auto kQwenHooks = std::to_array<AgentHookSpec>({
    {.event = "UserPromptSubmit", .matcher = {}, .state = "working", .message = "@prompt"},
    {.event = "PostToolUse", .matcher = {}, .state = "working", .message = {}},
    {.event = "PermissionRequest", .matcher = {}, .state = "approval", .message = kToolSubject},
    {.event = "Notification", .matcher = "permission_prompt", .state = "approval", .message = "@message"},
    {.event = "Stop", .matcher = {}, .state = "idle", .message = {}},
    {.event = "StopFailure", .matcher = {}, .state = "error", .message = {}},
    {.event = "SessionEnd", .matcher = {}, .state = "ended", .message = {}},
});

// An agent Konsoll can connect: its configuration directory and the file it
// changes, relative to the home directory. No hook specs: a plugin file.
struct AgentInfo {
    std::string_view id;
    std::string_view name;
    std::string_view configDirectory;
    std::string_view file;
    std::span<const AgentHookSpec> hooks;
};

constexpr auto kAgents = std::to_array<AgentInfo>({
    {.id = "claude",
     .name = "Claude Code",
     .configDirectory = ".claude",
     .file = ".claude/settings.json",
     .hooks = kClaudeHooks},
    {.id = "codex", .name = "Codex", .configDirectory = ".codex", .file = ".codex/hooks.json", .hooks = kCodexHooks},
    {.id = "qwen", .name = "Qwen Code", .configDirectory = ".qwen", .file = ".qwen/settings.json", .hooks = kQwenHooks},
    {.id = "opencode",
     .name = "opencode",
     .configDirectory = ".config/opencode",
     .file = ".config/opencode/plugins/konsoll.js",
     .hooks = {}},
});

QString fromView(std::string_view text)
{
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

QString homeFile(std::string_view relative)
{
    return QDir::homePath() + QLatin1Char('/') + fromView(relative);
}

const AgentInfo *findAgent(const QString &id)
{
    const auto *const found =
        std::ranges::find_if(kAgents, [&id](const AgentInfo &agent) { return fromView(agent.id) == id; });
    return found != kAgents.end() ? found : nullptr;
}

// The settings object in `path`: empty when the file does not exist yet.
std::expected<QJsonObject, QString> readSettings(const QString &path)
{
    QFile file(path);
    if (!file.exists()) {
        return QJsonObject{};
    }
    if (!file.open(QIODevice::ReadOnly)) {
        return std::unexpected(QObject::tr("Could not read %1: %2").arg(path, file.errorString()));
    }
    const QByteArray contents = file.readAll();
    if (contents.trimmed().isEmpty()) {
        return QJsonObject{};
    }
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(contents, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return std::unexpected(
            QObject::tr("%1 is not plain JSON (it may have comments), so Konsoll leaves it alone.").arg(path)
        );
    }
    return document.object();
}

std::expected<void, QString> writeFile(const QString &path, const QByteArray &contents)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        return std::unexpected(QObject::tr("Could not create the folder for %1").arg(path));
    }
    if (QFile::exists(path)) {
        const QString backup = path + QStringLiteral(".konsoll-backup");
        QFile::remove(backup);
        if (!QFile::copy(path, backup)) {
            return std::unexpected(QObject::tr("Could not back up %1").arg(path));
        }
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size() || !file.commit()) {
        return std::unexpected(QObject::tr("Could not write %1: %2").arg(path, file.errorString()));
    }
    return {};
}

bool isKonsollHandler(const QJsonValue &handler)
{
    return handler.toObject().value(QLatin1String("command")).toString().contains(fromView(kHookMarker));
}

bool hasKonsollHandler(const QJsonValue &group)
{
    const QJsonArray handlers = group.toObject().value(QLatin1String("hooks")).toArray();
    return std::ranges::any_of(handlers, isKonsollHandler);
}

// The matcher groups of one event without Konsoll's handlers; a group left
// with no handler goes away.
QJsonArray withoutKonsollHandlers(const QJsonArray &groups)
{
    QJsonArray kept;
    std::ranges::for_each(groups, [&kept](const QJsonValue &groupValue) {
        QJsonObject group = groupValue.toObject();
        QJsonArray handlers;
        std::ranges::copy_if(
            group.value(QLatin1String("hooks")).toArray(),
            std::back_inserter(handlers),
            [](const QJsonValue &handler) { return !isKonsollHandler(handler); }
        );
        if (!handlers.isEmpty()) {
            group.insert(QLatin1String("hooks"), handlers);
            kept.append(group);
        }
    });
    return kept;
}

// `settings` without Konsoll's hooks; an event left empty, and the hooks
// object when nothing is left, go away.
QJsonObject withoutKonsollHooks(QJsonObject settings)
{
    const QJsonObject hooks = settings.value(QLatin1String("hooks")).toObject();
    QJsonObject kept;
    std::ranges::for_each(hooks.keys(), [&hooks, &kept](const QString &event) {
        const QJsonArray groups = withoutKonsollHandlers(hooks.value(event).toArray());
        if (!groups.isEmpty()) {
            kept.insert(event, groups);
        }
    });
    if (kept.isEmpty()) {
        settings.remove(QLatin1String("hooks"));
    } else {
        settings.insert(QLatin1String("hooks"), kept);
    }
    return settings;
}

std::expected<void, QString> writeSettings(const QString &path, const QJsonObject &settings)
{
    return writeFile(path, QJsonDocument(settings).toJson(QJsonDocument::Indented));
}

QByteArray pluginSource()
{
    QFile source(QStringLiteral(":/integrations/opencode/konsoll.js"));
    return source.open(QIODevice::ReadOnly) ? source.readAll() : QByteArray{};
}

bool pluginFileCurrent(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) && file.readAll() == pluginSource();
}

std::expected<void, QString> installPluginFile(const QString &path)
{
    QFile source(QStringLiteral(":/integrations/opencode/konsoll.js"));
    if (!source.open(QIODevice::ReadOnly)) {
        return std::unexpected(QObject::tr("The opencode plugin is missing from Konsoll"));
    }
    return writeFile(path, source.readAll());
}

// Claude Code's Konsoll plugin (integrations/claude-code) does the same as
// the hooks.
bool claudePluginEnabled(const QJsonObject &settings)
{
    const QJsonObject plugins = settings.value(QLatin1String("enabledPlugins")).toObject();
    const QStringList keys = plugins.keys();
    return std::ranges::any_of(keys, [&plugins](const QString &key) {
        return key.startsWith(QLatin1String("konsoll@")) && plugins.value(key).toBool();
    });
}

// The agents whose connect offer the user turned down.
constexpr QLatin1StringView kDeclinedKey{"AgentIntegrations/declinedOffers"};

QStringList declinedOffers()
{
    return QSettings().value(kDeclinedKey).toStringList();
}

QVariantMap agentEntry(const AgentInfo &agent)
{
    const QString path = homeFile(agent.file);
    const bool plugin = agent.hooks.empty();
    const bool viaPlugin = agent.id == "claude"sv && claudePluginEnabled(readSettings(path).value_or(QJsonObject{}));
    const bool installed = plugin ? QFile::exists(path) : hasAgentHooks(path);
    const bool current =
        plugin ? !installed || pluginFileCurrent(path) : !installed || agentHooksCurrent(path, agent.id, agent.hooks);
    return {
        {QStringLiteral("id"), fromView(agent.id)},
        {QStringLiteral("name"), fromView(agent.name)},
        {QStringLiteral("path"), path},
        {QStringLiteral("available"), QFileInfo(homeFile(agent.configDirectory)).isDir()},
        {QStringLiteral("installed"), installed || viaPlugin},
        {QStringLiteral("viaPlugin"), viaPlugin && !installed},
        {QStringLiteral("outdated"), !current},
        {QStringLiteral("declined"), declinedOffers().contains(fromView(agent.id))},
    };
}

// `settings` with Konsoll's hooks for `agent` in place of any it had.
QJsonObject withAgentHooks(const QJsonObject &read, std::string_view agent, std::span<const AgentHookSpec> specs)
{
    QJsonObject settings = withoutKonsollHooks(read);
    QJsonObject hooks = settings.value(QLatin1String("hooks")).toObject();
    std::ranges::for_each(specs, [&hooks, agent](const AgentHookSpec &spec) {
        const QJsonObject handler{
            {QStringLiteral("type"), QStringLiteral("command")},
            {QStringLiteral("command"), agentHookCommand(agent, spec)},
            {QStringLiteral("timeout"), spec.event == "Interrupt"sv ? kInterruptTimeoutSeconds : kHookTimeoutSeconds},
        };
        QJsonObject group{{QStringLiteral("hooks"), QJsonArray{handler}}};
        if (!spec.matcher.empty()) {
            group.insert(QLatin1String("matcher"), fromView(spec.matcher));
        }
        QJsonArray groups = hooks.value(fromView(spec.event)).toArray();
        groups.append(group);
        hooks.insert(fromView(spec.event), groups);
    });
    settings.insert(QLatin1String("hooks"), hooks);
    return settings;
}

// Konsoll's hook groups in `settings` ("event matcher command" each, sorted):
// equal for two files when they have the same Konsoll hooks, in any order.
QStringList konsollHandlers(const QJsonObject &settings)
{
    const QJsonObject hooks = settings.value(QLatin1String("hooks")).toObject();
    QStringList handlers;
    std::ranges::for_each(hooks.keys(), [&hooks, &handlers](const QString &event) {
        std::ranges::for_each(hooks.value(event).toArray(), [&event, &handlers](const QJsonValue &value) {
            const QJsonObject group = value.toObject();
            std::ranges::for_each(group.value(QLatin1String("hooks")).toArray(), [&](const QJsonValue &handler) {
                const QString command = handler.toObject().value(QLatin1String("command")).toString();
                if (command.contains(fromView(kHookMarker))) {
                    handlers.append(
                        event + QLatin1Char(' ') + group.value(QLatin1String("matcher")).toString() + QLatin1Char(' ') +
                        command
                    );
                }
            });
        });
    });
    handlers.sort();
    return handlers;
}

} // namespace

std::span<const AgentHookSpec> claudeHookSpecs()
{
    return kClaudeHooks;
}

std::span<const AgentHookSpec> codexHookSpecs()
{
    return kCodexHooks;
}

std::span<const AgentHookSpec> qwenHookSpecs()
{
    return kQwenHooks;
}

QString agentHookCommand(std::string_view agent, const AgentHookSpec &spec)
{
    QString arguments = fromView(agent) + QLatin1Char(' ') + fromView(spec.state);
    if (!spec.message.empty()) {
        // Quoted: @a|b would otherwise be a pipe. The specs hold no quote.
        arguments += QStringLiteral(" '") + fromView(spec.message) + QLatin1Char('\'');
    }
    return QStringLiteral("[ -x \"$%1\" ] && \"$%1\" %2 || true").arg(fromView(kHookMarker), arguments);
}

std::expected<void, QString>
installAgentHooks(const QString &path, std::string_view agent, std::span<const AgentHookSpec> specs)
{
    const auto read = readSettings(path);
    if (!read) {
        return std::unexpected(read.error());
    }
    return writeSettings(path, withAgentHooks(*read, agent, specs));
}

bool agentHooksCurrent(const QString &path, std::string_view agent, std::span<const AgentHookSpec> specs)
{
    const QJsonObject settings = readSettings(path).value_or(QJsonObject{});
    return konsollHandlers(settings) == konsollHandlers(withAgentHooks(settings, agent, specs));
}

std::expected<void, QString> removeAgentHooks(const QString &path)
{
    if (!QFile::exists(path)) {
        return {};
    }
    const auto read = readSettings(path);
    if (!read) {
        return std::unexpected(read.error());
    }
    if (!hasAgentHooks(path)) {
        return {};
    }
    return writeSettings(path, withoutKonsollHooks(*read));
}

bool hasAgentHooks(const QString &path)
{
    const QJsonObject hooks = readSettings(path).value_or(QJsonObject{}).value(QLatin1String("hooks")).toObject();
    const QStringList events = hooks.keys();
    return std::ranges::any_of(events, [&hooks](const QString &event) {
        return std::ranges::any_of(hooks.value(event).toArray(), hasKonsollHandler);
    });
}

AgentIntegrations::AgentIntegrations(QObject *parent) : QObject(parent) {}

QVariantList AgentIntegrations::agents() const
{
    QVariantList list;
    std::ranges::transform(kAgents, std::back_inserter(list), [](const AgentInfo &agent) {
        return QVariant(agentEntry(agent));
    });
    return list;
}

QString AgentIntegrations::install(const QString &id)
{
    const AgentInfo *const agent = findAgent(id);
    if (agent == nullptr) {
        return tr("Unknown agent: %1").arg(id);
    }
    const QString path = homeFile(agent->file);
    const auto installed =
        agent->hooks.empty() ? installPluginFile(path) : installAgentHooks(path, agent->id, agent->hooks);
    emit agentsChanged();
    return installed ? QString{} : installed.error();
}

QString AgentIntegrations::uninstall(const QString &id)
{
    const AgentInfo *const agent = findAgent(id);
    if (agent == nullptr) {
        return tr("Unknown agent: %1").arg(id);
    }
    const QString path = homeFile(agent->file);
    std::expected<void, QString> removed;
    if (agent->hooks.empty()) {
        if (QFile::exists(path) && !QFile::remove(path)) {
            removed = std::unexpected(tr("Could not remove %1").arg(path));
        }
    } else {
        removed = removeAgentHooks(path);
    }
    emit agentsChanged();
    return removed ? QString{} : removed.error();
}

void AgentIntegrations::refresh()
{
    emit agentsChanged();
}

QString AgentIntegrations::connectOffer(const QString &processName) const
{
    // The foreground process is named like the agent's id (ProcessKind.cpp
    // names node scripts by the script: "qwen", "codex").
    const AgentInfo *const agent = findAgent(processName);
    if (agent == nullptr) {
        return {};
    }
    const QVariantMap entry = agentEntry(*agent);
    const bool connected =
        entry.value(QStringLiteral("installed")).toBool() && !entry.value(QStringLiteral("outdated")).toBool();
    const bool offer = entry.value(QStringLiteral("available")).toBool() && !connected &&
                       !entry.value(QStringLiteral("declined")).toBool();
    return offer ? processName : QString{};
}

bool AgentIntegrations::outdated(const QString &id) const
{
    const AgentInfo *const agent = findAgent(id);
    return agent != nullptr && agentEntry(*agent).value(QStringLiteral("outdated")).toBool();
}

QString AgentIntegrations::agentName(const QString &id) const
{
    const AgentInfo *const agent = findAgent(id);
    return agent != nullptr ? fromView(agent->name) : id;
}

void AgentIntegrations::declineOffer(const QString &id)
{
    QStringList declined = declinedOffers();
    if (findAgent(id) == nullptr || declined.contains(id)) {
        return;
    }
    declined.append(id);
    QSettings().setValue(kDeclinedKey, declined);
    emit agentsChanged();
}

QString AgentIntegrations::mcpSetup(const QString &id) const
{
    const QString binary = QCoreApplication::applicationFilePath();
    if (id == QLatin1String("claude")) {
        return QStringLiteral("claude mcp add --scope user konsoll -- \"%1\" --mcp").arg(binary);
    }
    if (id == QLatin1String("qwen")) {
        return QStringLiteral("qwen mcp add --scope user konsoll \"%1\" --mcp").arg(binary);
    }
    // Codex passes MCP servers only the environment variables it is told to.
    if (id == QLatin1String("codex")) {
        return QStringLiteral("# ~/.codex/config.toml\n"
                              "[mcp_servers.konsoll]\n"
                              "command = \"%1\"\n"
                              "args = [\"--mcp\"]\n"
                              "env_vars = [\"KONSOLL_SESSION_TOKEN\", \"KONSOLL_AGENT_SOCKET\"]")
            .arg(binary);
    }
    if (id == QLatin1String("opencode")) {
        return QStringLiteral("// ~/.config/opencode/opencode.json(c)\n"
                              "\"mcp\": {\n"
                              "  \"konsoll\": { \"type\": \"local\", \"command\": [\"%1\", \"--mcp\"] }\n"
                              "}")
            .arg(binary);
    }
    return {};
}

void AgentIntegrations::copyToClipboard(const QString &text) const
{
    QGuiApplication::clipboard()->setText(text);
}

} // namespace konsoll
