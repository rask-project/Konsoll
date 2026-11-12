#include "AgentCommands.h"

#include "AgentIntegrations.h"
#include "BackgroundJobs.h"
#include "TerminalBackend.h"
#include "TerminalManager.h"
#include "WindowRegistry.h"

#include <QJsonArray>
#include <QObject>
#include <QPointer>

#include <algorithm>
#include <iterator>
#include <memory>
#include <utility>

namespace konsoll
{

namespace
{

// How much of a background command's output a status reply carries.
constexpr qsizetype kOutputTailLength = 6000;
// recent_commands / read_pane: how many commands, how much of each output,
// and how much of the screen read_pane adds.
constexpr int kDefaultCommandCount = 5;
constexpr int kMaxCommandCount = 20;
constexpr qsizetype kCommandOutputLength = 3000;
constexpr qsizetype kScreenTailLength = 4000;
// No one reads an agent's background command as it runs, so a pager (git
// log, man) would wait for a key forever. `export` works in zsh, bash, fish.
constexpr auto kNoPagerSetup = "export PAGER=cat GIT_PAGER=cat; ";

QJsonObject reply(const QString &text, bool isError = false)
{
    return {{QStringLiteral("text"), text}, {QStringLiteral("isError"), isError}};
}

// TerminalBackend::recentCommands as text an agent reads.
QString commandsText(const QJsonArray &commands)
{
    if (commands.isEmpty()) {
        return QObject::tr("No command was run at a prompt there (or its shell has no Konsoll integration).");
    }
    QStringList parts;
    std::ranges::transform(commands, std::back_inserter(parts), [](const QJsonValue &value) {
        const QJsonObject command = value.toObject();
        const int code = command.value(QLatin1String("exitCode")).toInt(-1);
        const QString output = command.value(QLatin1String("output")).toString();
        return QObject::tr("$ %1\n(exit status %2)\n%3")
            .arg(
                command.value(QLatin1String("command")).toString(),
                code < 0 ? QObject::tr("unknown") : QString::number(code),
                output.isEmpty() ? QObject::tr("(no output)") : output
            );
    });
    return parts.join(QStringLiteral("\n\n"));
}

} // namespace

AgentCommands::AgentCommands(BackgroundJobs *jobs) : m_jobs(jobs) {}

QJsonObject AgentCommands::handle(const QJsonObject &request)
{
    const QString token = request.value(QLatin1String("token")).toString();
    const QString tool = request.value(QLatin1String("tool")).toString();
    const QJsonObject arguments = request.value(QLatin1String("arguments")).toObject();
    const QString cwd = request.value(QLatin1String("cwd")).toString();
    if (token.isEmpty()) {
        return reply(QObject::tr("This agent is not running in Konsoll."), true);
    }
    if (tool == QLatin1String("open_pane")) {
        return openPane(token, arguments, cwd);
    }
    if (tool == QLatin1String("run_in_background")) {
        return runInBackground(token, arguments, cwd);
    }
    if (tool == QLatin1String("background_job_status")) {
        return jobStatus(token, arguments);
    }
    if (tool == QLatin1String("recent_commands")) {
        return recentCommands(token, arguments);
    }
    if (tool == QLatin1String("list_panes")) {
        return listPanes(token);
    }
    return reply(QObject::tr("Unknown tool: %1").arg(tool), true);
}

void AgentCommands::handle(const QJsonObject &request, const Reply &answer)
{
    const QString token = request.value(QLatin1String("token")).toString();
    if (!token.isEmpty() && request.value(QLatin1String("tool")).toString() == QLatin1String("read_pane")) {
        readPane(token, request.value(QLatin1String("arguments")).toObject(), answer);
        return;
    }
    answer(handle(request));
}

QJsonObject AgentCommands::recentCommands(const QString &token, const QJsonObject &arguments) const
{
    const TerminalManager *const manager = WindowRegistry::instance().managerWithToken(token);
    const TerminalBackend *const session = manager != nullptr ? manager->sessionWithToken(token) : nullptr;
    if (session == nullptr) {
        return reply(QObject::tr("Your session is not in a Konsoll tab (it may run in the background)."), true);
    }
    const int count =
        std::clamp(arguments.value(QLatin1String("count")).toInt(kDefaultCommandCount), 1, kMaxCommandCount);
    return reply(commandsText(session->recentCommands(count, kCommandOutputLength)));
}

QJsonObject AgentCommands::listPanes(const QString &token) const
{
    TerminalManager *const manager = WindowRegistry::instance().managerWithToken(token);
    if (manager == nullptr) {
        return reply(QObject::tr("Your session is not in a Konsoll tab (it may run in the background)."), true);
    }
    const TerminalBackend *const own = manager->sessionWithToken(token);
    QStringList lines;
    std::ranges::for_each(manager->workspaces(), [manager, own, &lines](const QVariant &workspaceValue) {
        auto *const workspace = qobject_cast<TerminalWorkspaceModel *>(workspaceValue.value<QObject *>());
        if (workspace == nullptr) {
            return;
        }
        std::ranges::for_each(manager->workspaceSessions(workspace), [&](const QVariant &sessionValue) {
            const auto *const session = qobject_cast<const TerminalBackend *>(sessionValue.value<QObject *>());
            if (session == nullptr) {
                return;
            }
            lines.append(QObject::tr("pane %1%2: tab \"%3\", %4, in %5")
                             .arg(session->paneId())
                             .arg(session == own ? QObject::tr(" (yours)") : QString{})
                             .arg(workspace->title())
                             .arg(
                                 session->foregroundProcess().isEmpty() ? QObject::tr("at the shell prompt")
                                                                        : session->foregroundProcess()
                             )
                             .arg(session->currentDirectory()));
        });
    });
    return reply(
        lines.join(QLatin1Char('\n')) + QObject::tr("\n\nread_pane reads one of them once the user allows it.")
    );
}

void AgentCommands::readPane(const QString &token, const QJsonObject &arguments, const Reply &answer)
{
    TerminalManager *const manager = WindowRegistry::instance().managerWithToken(token);
    TerminalBackend *const agent = manager != nullptr ? manager->sessionWithToken(token) : nullptr;
    const int paneId = arguments.value(QLatin1String("pane")).toInt(-1);
    TerminalBackend *const target = manager != nullptr ? manager->sessionWithPaneId(paneId) : nullptr;
    if (agent == nullptr || target == nullptr) {
        answer(reply(QObject::tr("No pane %1 in your window (list_panes lists them).").arg(paneId), true));
        return;
    }
    const QPointer<TerminalBackend> pane(target);
    const auto contents = [pane]() {
        if (!pane) {
            return reply(QObject::tr("That pane was closed."), true);
        }
        const QString commands = commandsText(pane->recentCommands(kDefaultCommandCount, kCommandOutputLength));
        return reply(QObject::tr("%1\n\nThe end of its screen:\n%2")
                         .arg(commands, pane->bufferText().trimmed().right(kScreenTailLength)));
    };
    if (target == agent) {
        answer(contents());
        return;
    }
    const QString name =
        AgentIntegrations().agentName(agent->agentName().isEmpty() ? agent->foregroundProcess() : agent->agentName());
    // The pane's title, and its directory when the title is something else.
    const QString title = target->displayTitle();
    const QString directory = target->currentDirectory();
    const QString place = directory.isEmpty() || title == directory ? title : title + QStringLiteral(" — ") + directory;
    // Answered once: by the user, or refused if the agent's pane closes first.
    auto pending = std::make_shared<Reply>(answer);
    const bool asked = agent->requestRead(
        QObject::tr("%1 wants to read the pane “%2”: its recent commands and output.").arg(name, place),
        [pending, contents](bool allow) {
            if (const Reply once = std::exchange(*pending, {})) {
                once(allow ? contents() : reply(QObject::tr("The user did not allow reading that pane."), true));
            }
        }
    );
    if (!asked) {
        answer(reply(QObject::tr("Another request to read a pane is waiting for the user."), true));
        return;
    }
    QObject::connect(agent, &QObject::destroyed, [pending]() {
        if (const Reply once = std::exchange(*pending, {})) {
            once(reply(QObject::tr("Your pane was closed."), true));
        }
    });
}

QJsonObject AgentCommands::openPane(const QString &token, const QJsonObject &arguments, const QString &cwd) const
{
    const QString command = arguments.value(QLatin1String("command")).toString().trimmed();
    if (command.isEmpty()) {
        return reply(QObject::tr("command is required"), true);
    }
    TerminalManager *const manager = WindowRegistry::instance().managerWithToken(token);
    TerminalBackend *const session = manager != nullptr ? manager->sessionWithToken(token) : nullptr;
    if (session == nullptr) {
        return reply(QObject::tr("Your session is not in a Konsoll tab (it may run in the background)."), true);
    }
    const bool below = arguments.value(QLatin1String("direction")).toString() == QLatin1String("below");
    const int orientation = static_cast<int>(below ? Qt::Vertical : Qt::Horizontal);
    if (manager->openPaneBeside(session, orientation, command, cwd) == nullptr) {
        return reply(QObject::tr("Konsoll could not open a pane there."), true);
    }
    return reply(QObject::tr("Opened a pane %1 yours running: %2. The user sees it; you cannot read its output.")
                     .arg(below ? QObject::tr("below") : QObject::tr("to the right of"), command));
}

QJsonObject AgentCommands::runInBackground(const QString &token, const QJsonObject &arguments, const QString &cwd)
{
    const QString command = arguments.value(QLatin1String("command")).toString().trimmed();
    if (command.isEmpty()) {
        return reply(QObject::tr("command is required"), true);
    }
    const QString directory = arguments.value(QLatin1String("directory")).toString();
    const QString title = arguments.value(QLatin1String("title")).toString();
    const auto *const job = qobject_cast<const BackgroundJob *>(
        m_jobs->run(command, directory.isEmpty() ? cwd : directory, title, QString::fromLatin1(kNoPagerSetup))
    );
    if (job == nullptr) {
        return reply(QObject::tr("Konsoll could not start the command."), true);
    }
    m_jobsByToken[token].append(job->number());
    return reply(QObject::tr("Started background command %1. Check it with background_job_status (job %1); the user "
                             "sees it in Window ▸ Background Commands.")
                     .arg(job->number()));
}

QJsonObject AgentCommands::jobStatus(const QString &token, const QJsonObject &arguments) const
{
    const int number = arguments.value(QLatin1String("job")).toInt(-1);
    if (!m_jobsByToken.value(token).contains(number)) {
        return reply(QObject::tr("No background command %1 started by you.").arg(number), true);
    }
    const QVariantList jobs = m_jobs->jobs();
    const auto found = std::ranges::find_if(jobs, [number](const QVariant &value) {
        const auto *const job = qobject_cast<const BackgroundJob *>(value.value<QObject *>());
        return job != nullptr && job->number() == number;
    });
    const auto *const job =
        found != jobs.end() ? qobject_cast<const BackgroundJob *>(found->value<QObject *>()) : nullptr;
    if (job == nullptr) {
        return reply(QObject::tr("Background command %1 was removed.").arg(number), true);
    }
    const auto *const session = qobject_cast<const TerminalBackend *>(job->session());
    const QString output = session != nullptr ? session->bufferText().trimmed().right(kOutputTailLength) : QString{};
    QString status = QObject::tr("Command: %1\nState: %2").arg(job->command(), job->state());
    if (!job->running()) {
        status += QObject::tr("\nExit code: %1").arg(job->exitCode());
    }
    return reply(status + QObject::tr("\nOutput (last part):\n%1").arg(output));
}

} // namespace konsoll
