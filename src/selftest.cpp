#include "selftest.h"

#include "AgentCommands.h"
#include "AgentIntegrations.h"
#include "AgentStatus.h"
#include "BackgroundJobs.h"
#include "ShellFunctions.h"
#include "TerminalManager.h"
#include "WindowRegistry.h"
#include "term/Terminal.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <functional>
#include <iostream>
#include <optional>
#include <print>

namespace
{

void check(int &failures, bool condition, const char *what)
{
    konsoll::selfTestCheck(failures, condition, what);
}

// Spin-pump the event loop until `pred` holds or the timeout elapses.
bool waitFor(const char *what, const std::function<bool()> &pred, int timeoutMs)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents();
        if (pred()) {
            return true;
        }
        QThread::msleep(2);
    }
    std::println(std::cout, "timeout waiting for: {}", what);
    return pred();
}

// Count processes whose command line matches `pattern` (pgrep -f).
int pgrepCount(const char *pattern)
{
    QProcess process;
    process.start("pgrep", {"-f", pattern});
    process.waitForFinished(2000);
    const QByteArray output = process.readAllStandardOutput().trimmed();
    if (output.isEmpty()) {
        return 0;
    }
    const auto lines = std::ranges::count(output, '\n');
    return 1 + static_cast<int>(lines);
}

using konsoll::BackgroundJob;
using konsoll::BackgroundJobs;
using konsoll::Cell;
using konsoll::Screen;
using konsoll::Terminal;
using konsoll::TerminalBackend;
using konsoll::TerminalManager;

TerminalBackend *activeSession(TerminalManager &manager)
{
    return static_cast<TerminalBackend *>(manager.activeSession());
}

} // namespace

namespace konsoll
{

void selfTestCheck(int &failures, bool condition, const char *what)
{
    if (condition) {
        std::println(std::cout, "PASS  {}", what);
    } else {
        std::println(std::cout, "FAIL  {}", what);
        ++failures;
    }
}

namespace
{

// Tab indicators: the foreground job's kind, and why a background workspace
// wants attention (bell beats a failed command). `other` is a session in
// another workspace, activated to put the new one in the background.
void checkTabIndicators(int &failures, TerminalManager &manager, TerminalBackend *other)
{
    manager.addWorkspace();
    auto *u = activeSession(manager);
    auto *uWorkspace = static_cast<TerminalWorkspaceModel *>(manager.workspaces().last().value<QObject *>());
    check(
        failures,
        waitFor(
            "indicators: shell output",
            [u]() { return !u->bufferText().isEmpty(); },
            15000
        ),
        "indicators: shell produced output"
    );
    u->writeText("less /etc/hosts\n");
    check(
        failures,
        waitFor(
            "indicators: pager",
            [u, uWorkspace]() {
                return u->foregroundProcess() == QStringLiteral("less") &&
                       uWorkspace->processKind() == QStringLiteral("pager") && uWorkspace->busy() &&
                       u->displayTitle() == QStringLiteral("less") && uWorkspace->title() == QStringLiteral("less");
            },
            10000
        ),
        "indicators: foreground job name, kind and title reach the workspace"
    );
    u->writeText("q");
    check(
        failures,
        waitFor(
            "indicators: back at the prompt",
            [u, uWorkspace]() {
                return u->foregroundProcess().isEmpty() && uWorkspace->processKind().isEmpty() &&
                       uWorkspace->title() != QStringLiteral("less");
            },
            10000
        ),
        "indicators: kind cleared at the prompt"
    );
    // An interpreter's script names the program (agents installed with npm
    // run as "node .../bin/codex").
    u->writeText("printf 'sleep 30\\n' > \"${TMPDIR:-/tmp}/codex.pl\"; perl \"${TMPDIR:-/tmp}/codex.pl\"\n");
    check(
        failures,
        waitFor(
            "indicators: agent script",
            [u, uWorkspace]() {
                return u->foregroundProcess() == QStringLiteral("codex") &&
                       uWorkspace->processKind() == QStringLiteral("agent");
            },
            10000
        ),
        "indicators: interpreter script names the program"
    );
    u->writeText("\x03");
    check(
        failures,
        waitFor(
            "indicators: agent ended",
            [u]() { return u->foregroundProcess().isEmpty(); },
            10000
        ),
        "indicators: agent script interrupted"
    );
    // Agent hooks report through Konsoll's socket with the session token:
    // a wrong token is ignored, the state and message reach the workspace,
    // and returning to the prompt clears them.
    u->writeText("KONSOLL_SESSION_TOKEN=bogus \"$KONSOLL_AGENT_HOOK\" codex error </dev/null; "
                 "\"$KONSOLL_AGENT_HOOK\" codex waiting 'Approve the edit?' </dev/null; sleep 30\n");
    check(
        failures,
        waitFor(
            "agent status: waiting",
            [u, uWorkspace]() {
                return u->agentState() == QStringLiteral("waiting") && u->agentName() == QStringLiteral("codex") &&
                       uWorkspace->agentState() == QStringLiteral("waiting") &&
                       uWorkspace->agentMessage() == QStringLiteral("Approve the edit?");
            },
            10000
        ),
        "agent status: hook state and message reach the session and workspace"
    );
    u->writeText("\x03");
    check(
        failures,
        waitFor(
            "agent status: cleared",
            [u, uWorkspace]() { return u->agentState().isEmpty() && uWorkspace->agentState().isEmpty(); },
            10000
        ),
        "agent status: cleared back at the prompt"
    );
    // A permission prompt (approval) answered from Konsoll: Enter reaches the
    // agent's pane, the state moves on and the prompt's task stays.
    u->writeText("\"$KONSOLL_AGENT_HOOK\" claude working 'Fix the build' </dev/null; "
                 "\"$KONSOLL_AGENT_HOOK\" claude working '<task-notification> <task-id>x</task-id>' </dev/null; "
                 "\"$KONSOLL_AGENT_HOOK\" claude approval 'rm -rf build' </dev/null; "
                 "read answer; echo \"ANSWER[$answer]\"; sleep 30\n");
    const bool asked = waitFor(
        "agent status: approval",
        [u, uWorkspace]() {
            return u->agentState() == QStringLiteral("approval") &&
                   uWorkspace->agentState() == QStringLiteral("approval") &&
                   u->agentMessage() == QStringLiteral("rm -rf build") &&
                   u->agentTask() == QStringLiteral("Fix the build");
        },
        10000
    );
    const bool answered = asked && u->answerApproval(true) && !u->answerApproval(true);
    check(
        failures,
        answered && u->agentState() == QStringLiteral("working") && u->agentTask() == QStringLiteral("Fix the build") &&
            waitFor(
                "agent status: answer typed",
                [u]() { return u->bufferText().contains(QStringLiteral("ANSWER[]")); },
                10000
            ),
        "agent status: a permission prompt answered from Konsoll (once), the task kept"
    );
    u->writeText("\x03");
    check(
        failures,
        waitFor(
            "agent status: cleared after approval",
            [u]() { return u->agentState().isEmpty() && u->agentTask().isEmpty(); },
            10000
        ),
        "agent status: the task clears with the agent"
    );
    // A clicked notification names its session by token.
    manager.activateSession(other);
    check(
        failures,
        konsoll::WindowRegistry::instance().activateSessionWithToken(u->sessionToken()) &&
            activeSession(manager) == u && !konsoll::WindowRegistry::instance().activateSessionWithToken({}) &&
            !konsoll::WindowRegistry::instance().activateSessionWithToken(QStringLiteral("unknown")),
        "agent status: a notification's token activates its session"
    );
    // The icon follows the pane worked in last.
    u->writeText("less /etc/hosts\n");
    check(
        failures,
        waitFor(
            "indicators: pager before split",
            [uWorkspace]() { return uWorkspace->processKind() == QStringLiteral("pager"); },
            10000
        ),
        "indicators: pager in the first pane"
    );
    manager.splitActive(static_cast<int>(Qt::Horizontal));
    auto *v = activeSession(manager);
    check(
        failures,
        v != u && waitFor(
                      "indicators: split pane at the prompt",
                      [uWorkspace]() { return uWorkspace->processKind().isEmpty() && !uWorkspace->busy(); },
                      10000
                  ),
        "indicators: the new pane's prompt replaces the pager icon"
    );
    manager.activateSession(u);
    check(
        failures,
        uWorkspace->processKind() == QStringLiteral("pager"),
        "indicators: activating the pager pane brings its icon back"
    );
    u->writeText("q");
    manager.closeSession(v);
    manager.activateSession(other);
    u->writeText("printf '\\e]9;4;1;30\\a'; false\n");
    check(
        failures,
        waitFor(
            "indicators: failure",
            [uWorkspace]() {
                return uWorkspace->attention() == QStringLiteral("failure") && uWorkspace->progressState() == 1 &&
                       uWorkspace->progressValue() == 30;
            },
            10000
        ),
        "indicators: background failure and OSC 9;4 progress"
    );
    u->writeText("printf '\\a'; true\n");
    check(
        failures,
        waitFor(
            "indicators: bell",
            [uWorkspace]() { return uWorkspace->attention() == QStringLiteral("bell"); },
            10000
        ),
        "indicators: bell outranks the failure"
    );
    manager.activateSession(u);
    check(failures, uWorkspace->attention().isEmpty(), "indicators: activation clears attention");
    manager.closeSession(u);
}

// Settings > AI Agents: Konsoll's hooks go into an agent's settings file and
// come out again without touching the user's own settings and hooks.
void checkAgentIntegrations(int &failures)
{
    const QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("agent/settings.json"));
    const auto writeText = [](const QString &target, const QByteArray &contents) {
        QFile out(target);
        return QDir().mkpath(QFileInfo(target).absolutePath()) && out.open(QIODevice::WriteOnly) &&
               out.write(contents) == contents.size();
    };
    const auto readJson = [&path]() {
        QFile json(path);
        return json.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(json.readAll()).object() : QJsonObject{};
    };
    const QByteArray userSettings =
        R"({"model": "x", "hooks": {"Stop": [{"hooks": [{"type": "command", "command": "say done"}]}]}})";
    const bool written = writeText(path, userSettings);
    const auto handlerCount = [](const QJsonObject &settings, const QString &event) {
        const QJsonArray groups = settings.value(QStringLiteral("hooks")).toObject().value(event).toArray();
        return std::ranges::fold_left(groups, qsizetype{0}, [](qsizetype total, const QJsonValue &group) {
            return total + group.toObject().value(QStringLiteral("hooks")).toArray().size();
        });
    };
    const bool installed = written &&
                           konsoll::installAgentHooks(path, "claude", konsoll::claudeHookSpecs()).has_value() &&
                           konsoll::installAgentHooks(path, "claude", konsoll::claudeHookSpecs()).has_value();
    const QJsonObject withHooks = readJson();
    check(
        failures,
        installed && konsoll::hasAgentHooks(path) && withHooks.value(QStringLiteral("model")) == QStringLiteral("x") &&
            handlerCount(withHooks, QStringLiteral("Stop")) == 2 &&
            handlerCount(withHooks, QStringLiteral("UserPromptSubmit")) == 1 &&
            QFile::exists(path + QStringLiteral(".konsoll-backup")),
        "agent integrations: hooks added once, the user's kept, a backup made"
    );
    // Hooks an older Konsoll wrote (here: another state) are outdated.
    QByteArray older = QJsonDocument(withHooks).toJson();
    older.replace("claude approval", "claude waiting");
    const bool currentBefore = konsoll::agentHooksCurrent(path, "claude", konsoll::claudeHookSpecs());
    const bool outdated =
        writeText(path, older) && !konsoll::agentHooksCurrent(path, "claude", konsoll::claudeHookSpecs());
    check(
        failures,
        currentBefore && outdated &&
            konsoll::installAgentHooks(path, "claude", konsoll::claudeHookSpecs()).has_value() &&
            konsoll::agentHooksCurrent(path, "claude", konsoll::claudeHookSpecs()),
        "agent integrations: hooks from an older Konsoll are outdated until reinstalled"
    );
    const bool removed = konsoll::removeAgentHooks(path).has_value();
    check(
        failures,
        removed && !konsoll::hasAgentHooks(path) && readJson() == QJsonDocument::fromJson(userSettings).object(),
        "agent integrations: removal leaves exactly the user's settings"
    );
    const QString commented = directory.filePath(QStringLiteral("commented.json"));
    check(
        failures,
        writeText(commented, "// comment\n{}\n") &&
            !konsoll::installAgentHooks(commented, "qwen", konsoll::qwenHookSpecs()).has_value() &&
            !QFile::exists(commented + QStringLiteral(".konsoll-backup")),
        "agent integrations: a file that is not plain JSON is left alone"
    );
    check(
        failures,
        konsoll::agentHookCommand("codex", konsoll::codexHookSpecs().front()) ==
            QStringLiteral("[ -x \"$KONSOLL_AGENT_HOOK\" ] && \"$KONSOLL_AGENT_HOOK\" codex working '@prompt' || true"),
        "agent integrations: hook command (message quoted)"
    );
}

// The offer to connect an agent seen in a pane: only for an agent Konsoll
// knows whose configuration exists, until it is connected or declined.
void checkAgentConnectOffer(int &failures)
{
    const QTemporaryDir home;
    const QByteArray savedHome = qgetenv("HOME");
    const QString declinedKey = QStringLiteral("AgentIntegrations/declinedOffers");
    const QVariant savedDeclined = QSettings().value(declinedKey);
    QSettings().remove(declinedKey);
    qputenv("HOME", home.path().toLocal8Bit());
    konsoll::AgentIntegrations integrations;
    const bool made =
        QDir(home.path()).mkpath(QStringLiteral(".codex")) && QDir(home.path()).mkpath(QStringLiteral(".qwen"));
    const bool offered = integrations.connectOffer(QStringLiteral("codex")) == QStringLiteral("codex") &&
                         integrations.connectOffer(QStringLiteral("claude")).isEmpty() && // no ~/.claude
                         integrations.connectOffer(QStringLiteral("zsh")).isEmpty();
    const bool connected = integrations.install(QStringLiteral("codex")).isEmpty() &&
                           integrations.connectOffer(QStringLiteral("codex")).isEmpty();
    const bool beforeDecline = integrations.connectOffer(QStringLiteral("qwen")) == QStringLiteral("qwen");
    integrations.declineOffer(QStringLiteral("qwen"));
    const bool declined = integrations.connectOffer(QStringLiteral("qwen")).isEmpty();
    qputenv("HOME", savedHome);
    if (savedDeclined.isValid()) {
        QSettings().setValue(declinedKey, savedDeclined);
    } else {
        QSettings().remove(declinedKey);
    }
    check(
        failures,
        made && offered && connected && beforeDecline && declined,
        "agent integrations: connect offer for a known, configured agent until connected or declined"
    );
}

// New Agent in Worktree: the repository's top, a worktree beside it on a new
// branch (reused the next time), and bad branch names refused.
void checkAgentWorktree(int &failures)
{
    const QTemporaryDir directory;
    const QString repository = directory.filePath(QStringLiteral("repo"));
    const auto run = [](const QString &command) {
        QProcess shell;
        shell.start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), command});
        return shell.waitForFinished(10000) && shell.exitCode() == 0;
    };
    const bool made = QDir().mkpath(repository + QStringLiteral("/sub")) &&
                      run(QStringLiteral("cd '%1' && git init -q && git -c user.email=t@t -c user.name=t commit -q "
                                         "--allow-empty -m init")
                              .arg(repository));
    const konsoll::AgentIntegrations integrations;
    const QString root = integrations.gitRoot(repository + QStringLiteral("/sub"));
    const QVariantMap plan = integrations.worktreePlan(root, QStringLiteral("feat/x"), QStringLiteral("claude"));
    const QString path = plan.value(QStringLiteral("path")).toString();
    QString command = plan.value(QStringLiteral("command")).toString();
    command.replace(QStringLiteral("&& claude"), QStringLiteral("&& pwd"));
    const bool created =
        run(QStringLiteral("cd '%1' && %2").arg(root, command)) && QFileInfo(path + QStringLiteral("/.git")).exists();
    const QString again = integrations.worktreePlan(root, QStringLiteral("feat/x"), QStringLiteral("claude"))
                              .value(QStringLiteral("command"))
                              .toString();
    check(
        failures,
        made && QFileInfo(root).canonicalFilePath() == QFileInfo(repository).canonicalFilePath() &&
            path.endsWith(QStringLiteral("/repo-worktrees/feat-x")) && created &&
            again.startsWith(QStringLiteral("cd ")) && again.endsWith(QStringLiteral("&& claude")) &&
            integrations.worktreePlan(root, QStringLiteral("bad..name"), QStringLiteral("claude"))
                .contains(QStringLiteral("error")) &&
            integrations.gitRoot(directory.path()).isEmpty(),
        "agent worktree: a worktree beside the repository on a new branch, reused, bad names refused"
    );
}

// recent_commands, list_panes and read_pane: the agent's own pane's commands
// (with their status), the window's panes, and another pane read only once
// the user allows it.
void checkAgentContextTools(
    int &failures,
    TerminalManager &manager,
    konsoll::AgentCommands &commands,
    TerminalBackend *agent,
    QObject *workspace
)
{
    const auto call = [&commands, agent](const QString &tool, const QJsonObject &arguments) {
        return commands.handle(QJsonObject{
            {QStringLiteral("token"), agent->sessionToken()},
            {QStringLiteral("tool"), tool},
            {QStringLiteral("arguments"), arguments},
        });
    };
    agent->writeText("false; echo CONTEXT_OK\n");
    const bool ran = waitFor(
        "agent tools: commands recorded",
        [&call]() {
            return call(QStringLiteral("recent_commands"), {})
                .value(QStringLiteral("text"))
                .toString()
                .contains(QStringLiteral("$ false; echo CONTEXT_OK\n(exit status 0)\nCONTEXT_OK"));
        },
        10000
    );
    const QString recent = call(QStringLiteral("recent_commands"), {}).value(QStringLiteral("text")).toString();
    if (!ran) {
        std::println(
            std::cout,
            "recent_commands returned:\n{}\npane:\n{}",
            recent.toStdString(),
            agent->bufferText().toStdString()
        );
    }
    check(
        failures,
        ran && recent.contains(QStringLiteral("--mcp")),
        "agent tools: recent_commands lists the pane's commands with status and output"
    );
    const QVariantList sessions = manager.workspaceSessions(workspace);
    const auto other = std::ranges::find_if(sessions, [agent](const QVariant &session) {
        return session.value<QObject *>() != agent;
    });
    auto *const pane = other != sessions.end() ? qobject_cast<TerminalBackend *>(other->value<QObject *>()) : nullptr;
    const QString panes = call(QStringLiteral("list_panes"), {}).value(QStringLiteral("text")).toString();
    check(
        failures,
        pane != nullptr && panes.contains(QStringLiteral("pane %1 (yours)").arg(agent->paneId())) &&
            panes.contains(QStringLiteral("pane %1:").arg(pane->paneId())),
        "agent tools: list_panes names the window's panes"
    );
    if (pane == nullptr) {
        return;
    }
    // read_pane waits for the user: denied, then allowed (from the
    // notification's path, answerApproval).
    std::optional<QJsonObject> denied;
    std::optional<QJsonObject> allowed;
    const auto read = [&commands, agent, pane](std::optional<QJsonObject> &result) {
        commands.handle(
            QJsonObject{
                {QStringLiteral("token"), agent->sessionToken()},
                {QStringLiteral("tool"), QStringLiteral("read_pane")},
                {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("pane"), pane->paneId()}}},
            },
            [&result](const QJsonObject &reply) { result = reply; }
        );
    };
    read(denied);
    const bool asked = !denied && !agent->readRequest().isEmpty() && agent->agentState() == QStringLiteral("approval");
    agent->answerReadRequest(false);
    read(allowed);
    const bool waited = !allowed;
    agent->answerApproval(true);
    check(
        failures,
        asked && denied && denied->value(QStringLiteral("isError")).toBool() && waited && allowed &&
            allowed->value(QStringLiteral("text")).toString().contains(QStringLiteral("PANE_OK")) &&
            agent->readRequest().isEmpty() && agent->agentState() != QStringLiteral("approval"),
        "agent tools: read_pane reads another pane only once the user allows it"
    );
    // Send to Agent: text from another pane is pasted into the agent's pane
    // (same tab first), which becomes active. A plain shell is no agent.
    agent->setAgentStatus(konsoll::AgentState::None, {}, {}); // left working by read_pane's answer
    const bool notYetAgent =
        manager.agentSessionsFor(pane).isEmpty() && !manager.sendToAgent(agent, QStringLiteral("x"));
    agent->setAgentStatus(konsoll::AgentState::Working, QStringLiteral("claude"), {});
    const QVariantList targets = manager.agentSessionsFor(pane);
    const QVariantMap target = targets.value(0).toMap();
    const bool listed = targets.size() == 1 && target.value(QStringLiteral("session")).value<QObject *>() == agent &&
                        target.value(QStringLiteral("label")).toString().startsWith(QStringLiteral("Claude Code — "));
    manager.activateSession(pane);
    const bool sent = manager.sendToAgent(agent, QStringLiteral("echo SENT_\x1bTO_AGENT"));
    agent->writeText("\r");
    check(
        failures,
        notYetAgent && listed && sent && activeSession(manager) == agent &&
            waitFor(
                "agent tools: sent text runs",
                [agent]() { return agent->bufferText().count(QStringLiteral("SENT_TO_AGENT")) >= 2; },
                10000
            ),
        "agent tools: Send to Agent lists the agent panes and pastes into the chosen one (escapes dropped)"
    );
    agent->setAgentStatus(konsoll::AgentState::None, {}, {});
}

// Konsoll's MCP server (konsoll --mcp), started from an agent's pane: its
// tool calls reach Konsoll with the pane's token, open a pane beside the
// agent's without taking the focus, and run background commands only that
// agent can query.
void checkAgentTools(int &failures, TerminalManager &manager)
{
    BackgroundJobs jobs;
    konsoll::AgentCommands commands(&jobs);
    konsoll::AgentStatusServer::instance().setRequestHandler(
        [&commands](const QJsonObject &request, const konsoll::AgentStatusServer::Reply &reply) {
            commands.handle(request, reply);
        }
    );
    manager.addWorkspace();
    auto *agent = activeSession(manager);
    QObject *workspace = manager.workspaces().last().value<QObject *>();
    const QString input = QDir::temp().filePath(QStringLiteral("konsoll-mcp-selftest.in"));
    const QString output = QDir::temp().filePath(QStringLiteral("konsoll-mcp-selftest.out"));
    QFile::remove(output);
    QFile requests(input);
    const bool written =
        requests.open(QIODevice::WriteOnly) &&
        requests.write(
            R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18"}})"
            "\n"
            R"({"jsonrpc":"2.0","method":"notifications/initialized"})"
            "\n"
            R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})"
            "\n"
            R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"open_pane","arguments":{"command":"echo PANE_OK"}}})"
            "\n"
            R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"run_in_background","arguments":{"command":"echo BG_AGENT_OK:$PAGER:$GIT_PAGER"}}})"
            "\n"
        ) > 0;
    requests.close();
    check(
        failures,
        written && waitFor(
                       "agent tools: shell",
                       [agent]() { return !agent->bufferText().isEmpty(); },
                       15000
                   ),
        "agent tools: shell ready"
    );
    agent->writeText(QStringLiteral("'%1' --mcp < '%2' > '%3' 2>/dev/null\n")
                         .arg(QCoreApplication::applicationFilePath(), input, output));
    const auto replies = [&output]() {
        QFile file(output);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
    };
    check(
        failures,
        waitFor(
            "agent tools: replies",
            [&replies, &manager, workspace, &jobs]() {
                return replies().count('\n') == 4 && manager.workspaceSessions(workspace).size() == 2 &&
                       jobs.jobs().size() == 1;
            },
            15000
        ) && replies().contains("\"open_pane\"") &&
            replies().contains("Opened a pane") && activeSession(manager) == agent,
        "agent tools: MCP lists the tools, opens a pane beside the agent's (focus kept), starts a job"
    );
    const auto *job = qobject_cast<const BackgroundJob *>(jobs.jobs().value(0).value<QObject *>());
    const auto status = [&commands, job](const QString &token) {
        return commands.handle(QJsonObject{
            {QStringLiteral("token"), token},
            {QStringLiteral("tool"), QStringLiteral("background_job_status")},
            {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("job"), job != nullptr ? job->number() : -1}}},
        });
    };
    check(
        failures,
        job != nullptr &&
            waitFor(
                "agent tools: job ends",
                [job]() { return !job->running(); },
                15000
            ) &&
            status(agent->sessionToken())
                .value(QStringLiteral("text"))
                .toString()
                .contains(QStringLiteral("BG_AGENT_OK:cat:cat")) &&
            job->command() == QLatin1String("echo BG_AGENT_OK:$PAGER:$GIT_PAGER") &&
            status(QStringLiteral("another-session")).value(QStringLiteral("isError")).toBool(),
        "agent tools: job status with its output (pagers off, setup not shown), for the agent that started it only"
    );
    checkAgentContextTools(failures, manager, commands, agent, workspace);
    konsoll::AgentStatusServer::instance().setRequestHandler({});
    manager.closeWorkspace(workspace);
    QFile::remove(input);
    QFile::remove(output);
}

// Dragging a pane rearranges the splits and moves it between windows.
void checkPaneMoves(int &failures, TerminalManager &manager)
{
    TerminalManager other(nullptr);
    other.start();
    auto *otherFirst = activeSession(other);
    const auto rootOf = [](QObject *workspace) {
        return static_cast<TerminalLayoutNode *>(static_cast<TerminalWorkspaceModel *>(workspace)->rootNode());
    };

    manager.addWorkspace();
    auto *a = activeSession(manager);
    manager.splitActive(static_cast<int>(Qt::Horizontal));
    auto *b = activeSession(manager);
    QObject *split = manager.workspaces().last().value<QObject *>();
    const auto workspaceCount = manager.workspaces().size();

    check(
        failures,
        manager.movePane(a, &manager, b, TerminalManager::PaneEdgeBottom),
        "move pane: below its sibling accepted"
    );
    const TerminalLayoutNode *stacked = rootOf(split);
    check(
        failures,
        !stacked->isLeaf() && stacked->orientation() == static_cast<int>(Qt::Vertical) &&
            static_cast<TerminalLayoutNode *>(stacked->first())->session() == b &&
            static_cast<TerminalLayoutNode *>(stacked->second())->session() == a && manager.activeSession() == a,
        "move pane: side by side becomes a stack, moved pane active"
    );
    check(failures, !manager.movePane(a, &manager, a, TerminalManager::PaneEdgeLeft), "move pane: onto itself refused");

    check(
        failures,
        manager.movePaneToWorkspace(a, &manager, 0) && manager.workspaces().size() == workspaceCount + 1 &&
            rootOf(split)->isLeaf() && rootOf(split)->session() == b && manager.activeWorkspaceIndex() == 0,
        "move pane: into a workspace of its own"
    );

    check(
        failures,
        manager.movePane(a, &other, otherFirst, TerminalManager::PaneEdgeLeft),
        "move pane: to another window accepted"
    );
    const TerminalLayoutNode *joined = rootOf(other.workspaces().first().value<QObject *>());
    check(
        failures,
        !manager.hasSession(a) && other.hasSession(a) && manager.workspaces().size() == workspaceCount &&
            joined->orientation() == static_cast<int>(Qt::Horizontal) &&
            static_cast<TerminalLayoutNode *>(joined->first())->session() == a && other.activeSession() == a,
        "move pane: lone pane leaves its workspace and splits the target"
    );
    a->writeText("echo PANE_MOVED\n");
    check(
        failures,
        waitFor(
            "move pane: alive",
            [a]() { return a->bufferText().contains("PANE_MOVED"); },
            10000
        ),
        "move pane: moved shell still running"
    );

    check(
        failures,
        other.movePaneToWorkspace(a, &manager, 99) && manager.hasSession(a) && other.sessionCount() == 1 &&
            manager.workspaces().size() == workspaceCount + 1 && manager.activeSession() == a &&
            manager.activeWorkspaceIndex() == static_cast<int>(workspaceCount),
        "move pane: back as a new workspace of the first window"
    );
    manager.closeSession(a);
    manager.closeSession(b);
}

// Background commands: a command's status and exit code, Ctrl+C and Stop,
// and sessions moving between a window and the background.
void checkBackgroundJobs(int &failures, TerminalManager &manager)
{
    BackgroundJobs jobs;
    const auto *ok = static_cast<BackgroundJob *>(jobs.run(QStringLiteral("echo BG_OK; exit 0")));
    const auto *failed =
        static_cast<BackgroundJob *>(jobs.run(QStringLiteral("echo BG_FAIL; exit 3"), QStringLiteral("~")));
    check(failures, ok != nullptr && failed != nullptr && jobs.jobCount() == 2, "background: commands started");
    check(
        failures,
        waitFor(
            "background: ok ends",
            [ok]() { return !ok->running(); },
            20000
        ) && ok->state() == QStringLiteral("finished") &&
            ok->exitCode() == 0 && ok->backend()->bufferText().contains("BG_OK"),
        "background: exit 0 finishes, output kept"
    );
    check(
        failures,
        waitFor(
            "background: fail ends",
            [failed]() { return !failed->running(); },
            20000
        ) && failed->state() == QStringLiteral("failed") &&
            failed->exitCode() == 3,
        "background: non-zero exit fails with its status"
    );

    auto *sleeper = static_cast<BackgroundJob *>(jobs.run(QStringLiteral("sleep 31338")));
    check(
        failures,
        waitFor(
            "background: sleep spawned",
            []() { return pgrepCount("sleep 31338") > 0; },
            15000
        ) && jobs.runningCount() == 1,
        "background: long command running"
    );
    jobs.interrupt(sleeper);
    check(
        failures,
        waitFor(
            "background: interrupted",
            [sleeper]() { return !sleeper->running(); },
            10000
        ) && pgrepCount("sleep 31338") == 0,
        "background: Ctrl+C ends it"
    );
    auto *stopped = static_cast<BackgroundJob *>(jobs.run(QStringLiteral("sleep 31339")));
    waitFor("background: second sleep", []() { return pgrepCount("sleep 31339") > 0; }, 15000);
    jobs.stop(stopped);
    check(
        failures,
        stopped->state() == QStringLiteral("stopped") && waitFor(
                                                             "background: stopped",
                                                             []() { return pgrepCount("sleep 31339") == 0; },
                                                             5000
                                                         ),
        "background: Stop ends it, no orphan"
    );
    jobs.removeEnded();
    check(failures, jobs.jobCount() == 0 && jobs.runningCount() == 0, "background: ended jobs removed");

    manager.addWorkspace();
    auto *shell = activeSession(manager);
    waitFor("background: tab shell", [shell]() { return !shell->bufferText().isEmpty(); }, 15000);
    const int sessionsBefore = manager.sessionCount();
    check(
        failures,
        jobs.sendSession(&manager, shell) && !manager.hasSession(shell) &&
            manager.sessionCount() == sessionsBefore - 1 && jobs.runningCount() == 1,
        "background: tab session moves there"
    );
    shell->writeText("echo BG_ALIVE\n");
    check(
        failures,
        waitFor(
            "background: alive",
            [shell]() { return shell->bufferText().contains("BG_ALIVE"); },
            10000
        ),
        "background: moved shell still running"
    );
    QObject *job = jobs.jobs().first().value<QObject *>();
    check(
        failures,
        jobs.openInWindow(job, &manager) && manager.hasSession(shell) && manager.activeSession() == shell &&
            jobs.jobCount() == 0,
        "background: opens again as the active tab"
    );
    manager.closeSession(shell);

    // A window's only workspace leaves a fresh one behind.
    TerminalManager lone(nullptr);
    lone.start();
    auto *loneShell = activeSession(lone);
    QObject *loneWorkspace = lone.workspaces().first().value<QObject *>();
    check(
        failures,
        jobs.sendWorkspace(&lone, loneWorkspace) && lone.sessionCount() == 1 && !lone.hasSession(loneShell) &&
            lone.activeSession() != nullptr,
        "background: last tab out leaves a new one"
    );
}

// Functions defined in Konsoll reach background commands at start and open
// tabs at their next prompt; multi-line commands run whole. The user's own
// functions are restored afterwards.
void checkShellFunctions(int &failures, TerminalManager &manager)
{
    const QString key = QStringLiteral("ShellFunctions/functions");
    const QVariant saved = QSettings().value(key);
    {
        manager.addWorkspace();
        auto *shell = activeSession(manager);
        waitFor("functions: tab shell", [shell]() { return !shell->bufferText().isEmpty(); }, 15000);

        konsoll::ShellFunctions functions;
        check(
            failures,
            !functions.validate(-1, QStringLiteral("9bad")).isEmpty() &&
                !functions.validate(-1, QStringLiteral("_konsoll_x")).isEmpty(),
            "functions: invalid names refused"
        );
        const QString error = functions.saveFunction(
            -1,
            QStringLiteral("konsoll_selftest_fn"),
            QString{},
            QStringLiteral("echo \"FN_OUT:$1\"\necho FN_SECOND_LINE")
        );
        check(
            failures,
            error.isEmpty() && !functions.validate(-1, QStringLiteral("konsoll_selftest_fn")).isEmpty(),
            "functions: saved, name now taken"
        );

        BackgroundJobs jobs;
        const auto *job = static_cast<BackgroundJob *>(jobs.run(QStringLiteral("konsoll_selftest_fn arg1")));
        check(
            failures,
            waitFor(
                "functions: job ends",
                [job]() { return !job->running(); },
                20000
            ) && job->exitCode() == 0 &&
                job->backend()->bufferText().contains("FN_OUT:arg1") &&
                job->backend()->bufferText().contains("FN_SECOND_LINE"),
            "functions: callable from a background command"
        );

        shell->writeText("konsoll_selftest_fn tab\n");
        check(
            failures,
            waitFor(
                "functions: tab",
                [shell]() { return shell->bufferText().contains("FN_OUT:tab"); },
                10000
            ),
            "functions: an open tab picks them up at its next prompt"
        );

        const auto *multi = static_cast<BackgroundJob *>(
            jobs.run(QStringLiteral("echo MULTI_ONE\nfor i in 1 2; do\n  echo MULTI_$i\ndone\nexit 4"))
        );
        check(
            failures,
            waitFor(
                "multi-line: ends",
                [multi]() { return !multi->running(); },
                20000
            ) && multi->exitCode() == 4 &&
                multi->backend()->bufferText().contains("MULTI_ONE") &&
                multi->backend()->bufferText().contains("MULTI_2") &&
                multi->title() == QStringLiteral("echo MULTI_ONE …"),
            "multi-line: the whole script runs, listed by its first line"
        );

        functions.removeFunction(0);
        shell->writeText("konsoll_selftest_fn gone || echo FN_REMOVED\n");
        check(
            failures,
            waitFor(
                "functions: removed",
                [shell]() { return shell->bufferText().contains("FN_REMOVED"); },
                10000
            ),
            "functions: a removed function goes away"
        );
        manager.closeSession(shell);
    }
    if (saved.isValid()) {
        QSettings().setValue(key, saved);
    } else {
        QSettings().remove(key);
    }
    check(failures, konsoll::ShellFunctions::installScripts().has_value(), "functions: user's functions restored");
}

// Text of soft-wrapped lines (command blocks, buffer saves): a space in the
// last column before the wrap is kept, on screen and in the history.
void checkWrappedLineText(int &failures)
{
    TerminalBackend onScreen;
    onScreen.startReplay(10, 4, QByteArrayLiteral("false;    echo OK\r\n"));
    TerminalBackend inHistory;
    inHistory.startReplay(10, 2, QByteArrayLiteral("false;    echo OK\r\nb\r\nc\r\n"));
    check(
        failures,
        onScreen.bufferText().startsWith(QStringLiteral("false;    echo OK\n")) &&
            inHistory.bufferText().startsWith(QStringLiteral("false;    echo OK\nb\n")),
        "wrapped text: a space in the last column before the wrap is kept"
    );
}

} // namespace

int runSelfTest()
{
    // Multi-session validation: the real manager/backend/worker stack.
    std::println(std::cout, "Konsoll selftest");
    TerminalManager manager(nullptr);
    manager.start();
    int failures = 0;

    // 0. Parser: zero-width code points take no cell. OpenTUI apps (Qwen Code)
    // suffix icons with VS15; a cell per selector pushed their last column
    // onto the next row.
    {
        Terminal term(4, 10, QStringLiteral("selftest"));
        term.write(QByteArray("\xe2\x97\x86\xef\xb8\x8e"
                              "ab")); // U+25C6 U+FE0E a b
        const Screen &screen = term.screen();
        check(
            failures,
            screen.cellAt(0, 0).ch == 0x25c6u && screen.cellAt(0, 1).ch == 'a' && screen.cellAt(0, 2).ch == 'b' &&
                screen.cursorCol() == 3,
            "zero-width: variation selector takes no cell"
        );
        term.write(QByteArray("\r\nac\xcc\xa7"
                              "a\xcc\x83"
                              "o")); // NFD "ação"
        check(
            failures,
            screen.cellAt(1, 1).ch == 0xe7u && screen.cellAt(1, 2).ch == 0xe3u && screen.cellAt(1, 3).ch == 'o' &&
                screen.cursorCol() == 4,
            "zero-width: combining marks compose (NFD -> NFC)"
        );
    }

    // 0a. Prefixed CSI sequences are not SGR: Claude Code's modifyOtherKeys
    // request (CSI > 4 ; 2 m) must not underline/dim the following text.
    {
        Terminal term(4, 10, QStringLiteral("selftest"));
        term.write(QByteArray("\x1b[>4;2mabc\x1b[>4m\x1b[>0q"));
        const unsigned char flags = term.screen().cellAt(0, 0).flags;
        check(
            failures,
            (flags & (konsoll::CellFlag::Underline | konsoll::CellFlag::Dim)) == 0u &&
                term.screen().cellAt(0, 2).ch == 'c' && term.screen().cursorCol() == 3,
            "csi: CSI > 4;2 m (modifyOtherKeys) is not SGR"
        );
    }

    // 0d. Control characters never land in a cell: a stray C0 byte or C1 code
    // point rendered as a missing-glyph box over the text. (C1 controls with a
    // meaning, such as U+009B CSI, act as ESC Fe; U+0080 has none.)
    {
        Terminal term(4, 10, QStringLiteral("selftest"));
        term.write(QByteArray("a\x0e\x0f\x01\x7f"
                              "b\xc2\x80"
                              "c"));
        const Screen &screen = term.screen();
        check(
            failures,
            screen.cellAt(0, 0).ch == 'a' && screen.cellAt(0, 1).ch == 'b' && screen.cellAt(0, 2).ch == 'c' &&
                screen.cursorCol() == 3,
            "controls: C0 and C1 controls take no cell"
        );
    }

    // 0c. Background Color Erase + ECH: Neovim paints its theme background with
    // erase sequences after setting the SGR background.
    {
        Terminal term(4, 10, QStringLiteral("selftest"));
        const Screen &screen = term.screen();
        term.write(QByteArray("abcdef\x1b[48;2;1;2;3m\r\x1b[2X\x1b[4G\x1b[K"));
        constexpr unsigned int kBg = 0x80010203u;
        check(
            failures,
            screen.cellAt(0, 0).ch == ' ' && screen.cellAt(0, 0).bg == kBg && screen.cellAt(0, 1).bg == kBg &&
                screen.cellAt(0, 2).ch == 'c' && screen.cellAt(0, 2).bg == 0u,
            "erase: ECH blanks N cells with the current background"
        );
        check(
            failures,
            screen.cellAt(0, 3).ch == ' ' && screen.cellAt(0, 9).bg == kBg,
            "erase: EL fills with the current background (BCE)"
        );
        term.write(QByteArray("\x1b[0m\x1b[2J"));
        check(failures, screen.cellAt(1, 5).bg == 0u, "erase: SGR reset restores default-background erase");
    }

    // 0b. Wide characters take two cells (lead + spacer), as TUIs assume.
    {
        const auto isWide = [](const Cell &cell) { return (cell.flags & konsoll::CellFlag::Wide) != 0u; };
        const auto isSpacer = [](const Cell &cell) { return (cell.flags & konsoll::CellFlag::WideSpacer) != 0u; };
        Terminal term(4, 10, QStringLiteral("selftest"));
        const Screen &screen = term.screen();
        term.write(QByteArray("a\xe2\x9c\x85"
                              "b")); // a U+2705 b
        check(
            failures,
            isWide(screen.cellAt(0, 1)) && screen.cellAt(0, 1).ch == 0x2705u && isSpacer(screen.cellAt(0, 2)) &&
                screen.cellAt(0, 3).ch == 'b' && screen.cursorCol() == 4,
            "wide: emoji occupies two cells"
        );
        term.write(QByteArray("\r\xe4\xb8\xad")); // overwrite a, then U+4E2D over the emoji's lead
        term.write(QByteArray("\rz"));            // z over the CJK lead breaks the pair
        check(
            failures,
            screen.cellAt(0, 0).ch == 'z' && !isSpacer(screen.cellAt(0, 1)) && screen.cellAt(0, 1).ch == ' ',
            "wide: overwriting a lead clears its spacer"
        );
        term.write(QByteArray("\r\n\xe2\x9d\xa4\xef\xb8\x8f"
                              "c")); // U+2764 U+FE0F c
        check(
            failures,
            isWide(screen.cellAt(1, 0)) && isSpacer(screen.cellAt(1, 1)) && screen.cellAt(1, 2).ch == 'c',
            "wide: VS16 widens the previous character"
        );
        term.write(QByteArray("\r\nxxxxxxxxx\xe4\xb8\xad")); // 9 cells + wide: wraps whole
        check(
            failures,
            isSpacer(screen.cellAt(2, 9)) && isWide(screen.cellAt(3, 0)) && screen.cellAt(3, 0).ch == 0x4e2du,
            "wide: no room in the last column wraps the whole character"
        );
    }
    {
        const auto isWide = [](const Cell &cell) { return (cell.flags & konsoll::CellFlag::Wide) != 0u; };
        Terminal term(4, 10, QStringLiteral("selftest"));
        const Screen &screen = term.screen();
        term.write(QByteArray("abcdefgh\xe4\xb8\xad")); // fills exactly 10 cells
        term.resize(4, 9);
        check(
            failures,
            screen.cellAt(0, 7).ch == 'h' && isWide(screen.cellAt(1, 0)),
            "wide: reflow never splits a wide pair"
        );
        term.resize(4, 10);
        check(
            failures,
            isWide(screen.cellAt(0, 8)) && screen.cellAt(0, 8).ch == 0x4e2du,
            "wide: reflow re-joins without the wrap padding"
        );
    }

    runParserSelfTest(failures);
    checkAgentIntegrations(failures);
    checkAgentConnectOffer(failures);
    checkAgentWorktree(failures);
    checkWrappedLineText(failures);

    // 1. Concurrent output: three sessions produce output at the same time.
    auto *a = activeSession(manager);
    manager.addWorkspace();
    auto *b = activeSession(manager);
    manager.addWorkspace();
    auto *c = activeSession(manager);
    check(failures, manager.sessionCount() == 3, "three sessions created");

    // Named workspace (Ctrl+Shift+T / Cmd+Shift+T): the custom title wins
    // over the directory, and an empty title stays unnamed.
    manager.addNamedWorkspace(QStringLiteral("Named WS"));
    {
        auto *named = static_cast<TerminalWorkspaceModel *>(manager.workspaces().last().value<QObject *>());
        check(
            failures,
            named != nullptr && named->title() == QStringLiteral("Named WS"),
            "named workspace: custom title shown"
        );
    }
    manager.addNamedWorkspace(QString{});
    {
        auto *unnamed = static_cast<TerminalWorkspaceModel *>(manager.workspaces().last().value<QObject *>());
        check(
            failures,
            unnamed != nullptr && unnamed->customTitle().isEmpty(),
            "named workspace: empty title stays unnamed"
        );
    }

    a->writeText("seq 1 5000\n");
    b->writeText("seq 1 5000\n");
    c->writeText("seq 1 5000\n");
    check(
        failures,
        waitFor(
            "a: seq output",
            [a]() { return a->bufferText().contains("\n5000\n"); },
            20000
        ),
        "concurrent output: session A"
    );
    check(
        failures,
        waitFor(
            "b: seq output",
            [b]() { return b->bufferText().contains("\n5000\n"); },
            20000
        ),
        "concurrent output: session B"
    );
    check(
        failures,
        waitFor(
            "c: seq output",
            [c]() { return c->bufferText().contains("\n5000\n"); },
            20000
        ),
        "concurrent output: session C"
    );

    // 2. Independent input: each session receives only its own input.
    a->writeText("echo SELFTEST_A\n");
    b->writeText("echo SELFTEST_B\n");
    check(
        failures,
        waitFor(
            "a: marker",
            [a]() { return a->bufferText().contains("SELFTEST_A"); },
            10000
        ),
        "independent input: A echo"
    );
    check(
        failures,
        waitFor(
            "b: marker",
            [b]() { return b->bufferText().contains("SELFTEST_B"); },
            10000
        ),
        "independent input: B echo"
    );
    check(failures, !a->bufferText().contains("SELFTEST_B"), "independent input: A has no B marker");
    check(failures, !b->bufferText().contains("SELFTEST_A"), "independent input: B has no A marker");

    // 3. Resize isolation.
    const int bCols = b->cols();
    const int bRows = b->rows();
    a->resize(120, 40);
    check(failures, a->cols() == 120 && a->rows() == 40, "resize: A resized");
    check(failures, b->cols() == bCols && b->rows() == bRows, "resize: B unchanged");

    // 4. Process exit: natural exit auto-closes only that session.
    const int countBefore = manager.sessionCount();
    c->writeText("exit\n");
    check(
        failures,
        waitFor(
            "c: auto close",
            [&manager, &countBefore]() { return manager.sessionCount() == countBefore - 1; },
            15000
        ),
        "process exit: C auto-closed"
    );
    a->writeText("echo STILL_ALIVE_A\n");
    check(
        failures,
        waitFor(
            "a: still alive",
            [a]() { return a->bufferText().contains("STILL_ALIVE_A"); },
            10000
        ),
        "process exit: A still running"
    );

    // 5. Fast close/reopen, bounded close time.
    const int baseline = manager.sessionCount();
    for (int i = 0; i < 5; ++i) {
        manager.addWorkspace();
        auto *s = activeSession(manager);
        std::array<char, 48> label{};
        std::snprintf(label.data(), label.size(), "reopen %d: shell output", i);
        check(
            failures,
            waitFor(
                label.data(),
                [s]() { return !s->bufferText().isEmpty(); },
                15000
            ),
            "fast close/reopen: shell output"
        );
        QElapsedTimer timer;
        timer.start();
        manager.closeSession(s);
        const qint64 elapsedMs = timer.elapsed();
        check(failures, manager.sessionCount() == baseline, "fast close/reopen: session removed");
        check(failures, elapsedMs < 3000, "fast close/reopen: close bounded");
    }

    // 5a. Input larger than the PTY input queue (~1 KiB) arrives whole: the
    // rest used to be dropped, and with it a bracketed paste's closing
    // ESC[201~, which left the program (Codex) stuck in paste mode.
    {
        manager.addWorkspace();
        auto *s = activeSession(manager);
        // The digits of the count map to letters (20000 -> caaaa) so the
        // echoed command line cannot match the result.
        s->writeText("stty -icanon -echo; echo PASTE_RE''ADY; head -c 20000 | wc -c | tr 0-9 a-j\n");
        check(
            failures,
            waitFor(
                "paste: reader ready",
                [s]() { return s->bufferText().contains("PASTE_READY"); },
                15000
            ),
            "large input: reader started"
        );
        s->writeText(QString(20000, QLatin1Char('x')));
        check(
            failures,
            waitFor(
                "paste: all bytes read",
                [s]() { return s->bufferText().contains("caaaa"); },
                15000
            ),
            "large input: every byte reaches the program"
        );
        manager.closeSession(s);
    }

    // 5c. Closing a tab whose job ignores SIGHUP and keeps writing, while a
    // tab opened after it is still running. That later shell used to
    // inherit the first tab's PTY master, so closing it hung nothing up: the
    // job ran on, the killed shell blocked in exit and the tab never closed.
    {
        constexpr const char *kMarker = "KONSOLL_SELFTEST_IGNORES_HUP";
        manager.addWorkspace();
        auto *stubborn = activeSession(manager);
        manager.addWorkspace();
        auto *later = activeSession(manager);
        const int sessionsBefore = manager.sessionCount();
        stubborn->writeText(
            QStringLiteral("perl -e '$SIG{HUP}=q(IGNORE); $|=1; while (1) { print qq(x) x 80, qq(\\n) }' %1\n")
                .arg(QLatin1StringView(kMarker))
        );
        check(
            failures,
            waitFor(
                "hup: job running",
                [kMarker]() { return pgrepCount(kMarker) > 0; },
                15000
            ),
            "close session: SIGHUP-ignoring job started"
        );
        QElapsedTimer timer;
        timer.start();
        manager.closeSession(stubborn);
        check(
            failures,
            waitFor(
                "hup: session removed",
                [&manager, &sessionsBefore]() { return manager.sessionCount() == sessionsBefore - 1; },
                5000
            ),
            "close session: removed despite a job ignoring SIGHUP"
        );
        check(failures, timer.elapsed() < 5000, "close session: bounded with a job ignoring SIGHUP");
        check(
            failures,
            waitFor(
                "hup: job killed",
                [kMarker]() { return pgrepCount(kMarker) == 0; },
                5000
            ),
            "close session: the tab's jobs are killed"
        );
        manager.closeSession(later);
    }

    // 5b. Moving a workspace to another window keeps its shells running.
    {
        TerminalManager other(nullptr);
        other.start();
        bool sourceEmptied = false;
        bool otherEmptied = false;
        QObject::connect(&other, &TerminalManager::windowShouldClose, [&otherEmptied]() { otherEmptied = true; });
        QObject::connect(&manager, &TerminalManager::windowShouldClose, [&sourceEmptied]() { sourceEmptied = true; });

        manager.addWorkspace();
        auto *left = activeSession(manager);
        manager.splitActive(static_cast<int>(Qt::Horizontal));
        auto *right = activeSession(manager);
        QObject *moving = manager.workspaces().last().value<QObject *>();
        const int sourceBefore = manager.sessionCount();
        const auto workspacesBefore = manager.workspaces().size();
        check(
            failures,
            waitFor(
                "move: shell output",
                [right]() { return !right->bufferText().isEmpty(); },
                15000
            ),
            "move workspace: split shell started"
        );

        check(failures, manager.moveWorkspaceTo(moving, &other, 0), "move workspace: accepted");
        check(
            failures,
            manager.sessionCount() == sourceBefore - 2 && manager.workspaces().size() == workspacesBefore - 1 &&
                !manager.hasSession(left) && !manager.hasSession(right),
            "move workspace: source gives up the workspace and both panes"
        );
        check(
            failures,
            other.sessionCount() == 3 && other.workspaces().size() == 2 &&
                other.workspaces().first().value<QObject *>() == moving && other.activeSession() == right &&
                other.activeWorkspaceIndex() == 0,
            "move workspace: target adopts it at the index, active pane kept"
        );
        check(
            failures,
            manager.activeSession() != nullptr && manager.activeSession() != left && manager.activeSession() != right &&
                !sourceEmptied,
            "move workspace: source activates another session"
        );

        right->writeText("echo MOVED_ALIVE\n");
        check(
            failures,
            waitFor(
                "move: alive",
                [right]() { return right->bufferText().contains("MOVED_ALIVE"); },
                10000
            ),
            "move workspace: moved shell still running"
        );
        left->writeText("exit\n");
        check(
            failures,
            waitFor(
                "move: exit closes in target",
                [&other]() { return other.sessionCount() == 2; },
                15000
            ),
            "move workspace: exit closes the pane in the new window"
        );

        QObject *original = other.workspaces().last().value<QObject *>();
        check(
            failures,
            other.moveWorkspaceTo(moving, &other, 1) && other.workspaces().last().value<QObject *>() == moving,
            "move workspace: same window reorders"
        );
        const int sourceCount = manager.sessionCount();
        other.moveWorkspaceTo(original, &manager, 99);
        other.moveWorkspaceTo(moving, &manager, 99);
        check(
            failures,
            otherEmptied && other.sessionCount() == 0 && manager.sessionCount() == sourceCount + 2,
            "move workspace: last workspace out closes the window"
        );
        manager.closeSession(right);
        manager.closeWorkspace(original);
    }

    // 5c. Dragging a pane rearranges the splits and moves it between windows.
    checkPaneMoves(failures, manager);

    // 5d. Commands in the background.
    checkBackgroundJobs(failures, manager);
    checkShellFunctions(failures, manager);

    // 6. App shutdown while a shell is running: no orphaned process group.
    manager.addWorkspace();
    auto *s = activeSession(manager);
    check(
        failures,
        waitFor(
            "shutdown: shell output",
            [s]() { return !s->bufferText().isEmpty(); },
            15000
        ),
        "shutdown: shell produced output"
    );
    s->writeText("sleep 31337\n");
    check(
        failures,
        waitFor(
            "shutdown: sleep spawned",
            []() { return pgrepCount("sleep 31337") > 0; },
            10000
        ),
        "shutdown: sleep started"
    );
    manager.closeSession(s);
    check(
        failures,
        waitFor(
            "shutdown: sleep gone",
            []() { return pgrepCount("sleep 31337") == 0; },
            5000
        ),
        "shutdown: no orphaned sleep"
    );

    // 6b. Tab indicators.
    checkTabIndicators(failures, manager, a);
    checkAgentTools(failures, manager);

    // 7. Output backpressure: sustained output above the 1 MiB high-water mark.
    manager.addWorkspace();
    auto *t = activeSession(manager);
    check(
        failures,
        waitFor(
            "backpressure: shell output",
            [t]() { return !t->bufferText().isEmpty(); },
            15000
        ),
        "backpressure: shell produced output"
    );
    t->writeText("seq 1 250000\n");
    check(
        failures,
        waitFor(
            "backpressure: seq done",
            [t]() { return t->maxScrollOffset() >= 249900; },
            60000
        ),
        "backpressure: 250k lines delivered"
    );
    check(failures, t->bufferText().contains("\n250000\n"), "backpressure: last line intact");

    // Remaining sessions (A and B) are closed by the manager destructor.
    if (failures == 0) {
        std::println(std::cout, "selftest: all checks passed");
    } else {
        std::println(std::cout, "selftest: {} check(s) failed", failures);
    }
    return failures == 0 ? 0 : 1;
}

} // namespace konsoll
