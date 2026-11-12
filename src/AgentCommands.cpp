#include "AgentCommands.h"

#include "BackgroundJobs.h"
#include "TerminalBackend.h"
#include "TerminalManager.h"
#include "WindowRegistry.h"

#include <QJsonArray>
#include <QObject>

#include <algorithm>

namespace konsoll
{

namespace
{

// How much of a background command's output a status reply carries.
constexpr qsizetype kOutputTailLength = 6000;

QJsonObject reply(const QString &text, bool isError = false)
{
    return {{QStringLiteral("text"), text}, {QStringLiteral("isError"), isError}};
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
    return reply(QObject::tr("Unknown tool: %1").arg(tool), true);
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
    const auto *const job =
        qobject_cast<const BackgroundJob *>(m_jobs->run(command, directory.isEmpty() ? cwd : directory, title));
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
