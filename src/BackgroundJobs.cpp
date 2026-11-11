#include "BackgroundJobs.h"

#include "TerminalBackend.h"
#include "TerminalManager.h"

#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <iterator>
#include <limits>
#include <ranges>
#include <utility>

namespace konsoll
{

namespace
{

const QString kSavedCommandsKey = QStringLiteral("BackgroundCommands/saved");

// The size a command starts at; the Background Commands window resizes it
// to its output pane while it shows it.
constexpr int kInitialColumns = 120;
constexpr int kInitialRows = 32;

BackgroundJobs *&instanceSlot()
{
    // Non-owning: main() owns the instance.
    static BackgroundJobs *s_instance = nullptr;
    return s_instance;
}

QString expandedDirectory(const QString &directory)
{
    const QString trimmed = directory.trimmed();
    if (trimmed.isEmpty()) {
        return QDir::homePath();
    }
    if (trimmed == QStringLiteral("~") || trimmed.startsWith(QStringLiteral("~/"))) {
        return QDir::homePath() + trimmed.sliced(1);
    }
    return trimmed;
}

// A multi-line command is listed by its first line.
QString commandTitle(const QString &command)
{
    const QStringList lines = command.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    const auto first = std::ranges::find_if(lines, [](const QString &line) { return !line.trimmed().isEmpty(); });
    if (first == lines.end()) {
        return command;
    }
    const bool more =
        std::ranges::any_of(std::ranges::subrange(std::next(first), lines.end()), [](const QString &line) {
            return !line.trimmed().isEmpty();
        });
    return more ? first->trimmed() + QStringLiteral(" …") : first->trimmed();
}

QJsonObject savedCommand(const QString &name, const QString &command, const QString &directory)
{
    return {
        {QStringLiteral("name"), name.trimmed()},
        // Kept as typed: a multi-line command keeps its indentation.
        {QStringLiteral("command"), command.trimmed().isEmpty() ? QString{} : command},
        {QStringLiteral("directory"), directory.trimmed()},
    };
}

} // namespace

BackgroundJob::BackgroundJob(
    std::unique_ptr<TerminalBackend> session,
    int number,
    QString title,
    QString command,
    QString directory
)
    : m_session(std::move(session))
    , m_title(std::move(title))
    , m_command(std::move(command))
    , m_directory(std::move(directory))
    , m_startedAt(QDateTime::currentDateTime())
    , m_number(number)
{
    connectSession();
}

BackgroundJob::~BackgroundJob()
{
    if (m_session) {
        disconnect(m_session.get(), nullptr, this, nullptr);
        while (!m_session->close()) {
            QThread::msleep(25);
        }
    }
}

void BackgroundJob::connectSession()
{
    TerminalBackend *session = m_session.get();
    connect(session, &TerminalBackend::displayTitleChanged, this, &BackgroundJob::titleChanged);
    connect(session, &TerminalBackend::currentDirectoryChanged, this, &BackgroundJob::directoryChanged);
    connect(session, &TerminalBackend::processExited, this, [this](int code) {
        const bool stopRequested = (m_flags & StopRequestedFlag) != 0;
        Status status = Status::Finished;
        if (stopRequested) {
            status = Status::Stopped;
        } else if (!m_command.isEmpty() && code != 0) {
            status = Status::Failed;
        }
        finish(status, code);
        if (!stopRequested) {
            emit ended();
        }
        // The program is gone; release its PTY and worker thread. The screen
        // stays readable.
        QTimer::singleShot(0, this, [this]() {
            if (m_session) {
                m_session->close();
            }
        });
    });
    connect(session, &TerminalBackend::startFailed, this, [this](const QString &) {
        finish(Status::Failed, -1);
        emit ended();
    });
    connect(session, &TerminalBackend::workerThreadStopped, this, [this]() { finish(Status::Stopped, -1); });
}

void BackgroundJob::finish(Status status, int exitCode)
{
    if (m_status != Status::Running) {
        return;
    }
    m_status = status;
    m_exitCode = static_cast<std::int16_t>(std::clamp(
        exitCode,
        int{std::numeric_limits<std::int16_t>::min()},
        int{std::numeric_limits<std::int16_t>::max()}
    ));
    m_finishedAt = QDateTime::currentDateTime();
    emit stateChanged();
}

QString BackgroundJob::title() const
{
    if (!m_title.isEmpty()) {
        return m_title;
    }
    if (!m_command.isEmpty()) {
        return commandTitle(m_command);
    }
    return m_session ? m_session->displayTitle() : QString{};
}

QString BackgroundJob::directory() const
{
    if (m_session && !m_session->currentDirectory().isEmpty()) {
        return m_session->currentDirectory();
    }
    return m_directory;
}

QString BackgroundJob::state() const
{
    switch (m_status) {
    case Status::Running:
        return QStringLiteral("running");
    case Status::Finished:
        return QStringLiteral("finished");
    case Status::Failed:
        return QStringLiteral("failed");
    case Status::Stopped:
        return QStringLiteral("stopped");
    }
    return {};
}

QObject *BackgroundJob::session() const
{
    return m_session.get();
}

void BackgroundJob::interrupt() const
{
    if (m_session && running()) {
        m_session->interrupt();
    }
}

bool BackgroundJob::stop()
{
    if (!m_session) {
        return true;
    }
    if (running()) {
        m_flags |= StopRequestedFlag;
    }
    if (!m_session->close()) {
        return false;
    }
    finish(Status::Stopped, -1);
    return true;
}

std::unique_ptr<TerminalBackend> BackgroundJob::takeSession()
{
    if (!m_session) {
        return nullptr;
    }
    disconnect(m_session.get(), nullptr, this, nullptr);
    std::unique_ptr<TerminalBackend> session = std::move(m_session);
    emit sessionChanged();
    return session;
}

BackgroundJobs::BackgroundJobs()
{
    instanceSlot() = this;
    loadSaved();
}

BackgroundJobs::~BackgroundJobs()
{
    if (instanceSlot() == this) {
        instanceSlot() = nullptr;
    }
    std::ranges::for_each(m_jobs, [this](const auto &job) { disconnect(job.get(), nullptr, this, nullptr); });
    m_jobs.clear();
}

BackgroundJobs *BackgroundJobs::instance()
{
    return instanceSlot();
}

BackgroundJobs *BackgroundJobs::create(QQmlEngine *qmlEngine, QJSEngine *jsEngine)
{
    Q_UNUSED(qmlEngine);
    BackgroundJobs *jobs = instanceSlot();
    // main() owns it; the engine must not delete it.
    if (jobs != nullptr) {
        QJSEngine::setObjectOwnership(jobs, QJSEngine::CppOwnership);
    }
    Q_UNUSED(jsEngine);
    return jobs;
}

QVariantList BackgroundJobs::jobs() const
{
    QVariantList list;
    list.reserve(static_cast<qsizetype>(m_jobs.size()));
    std::ranges::transform(m_jobs, std::back_inserter(list), [](const auto &job) {
        return QVariant::fromValue(static_cast<QObject *>(job.get()));
    });
    return list;
}

int BackgroundJobs::runningCount() const
{
    return static_cast<int>(std::ranges::count_if(m_jobs, [](const auto &job) { return job->running(); }));
}

void BackgroundJobs::notifyRunningCount()
{
    const int count = runningCount();
    if (count != m_reportedRunningCount) {
        m_reportedRunningCount = count;
        emit runningCountChanged();
    }
}

BackgroundJob *BackgroundJobs::ownedJob(const QObject *job) const
{
    // By address: QML may hand over a stale pointer.
    const auto it = std::ranges::find_if(m_jobs, [job](const auto &candidate) { return candidate.get() == job; });
    return it == m_jobs.end() ? nullptr : it->get();
}

BackgroundJob *BackgroundJobs::addJob(
    std::unique_ptr<TerminalBackend> session,
    const QString &title,
    const QString &command,
    const QString &directory
)
{
    auto job = std::make_unique<BackgroundJob>(std::move(session), m_nextNumber++, title, command, directory);
    BackgroundJob *jobPtr = job.get();
    connect(jobPtr, &BackgroundJob::stateChanged, this, &BackgroundJobs::notifyRunningCount);
    connect(jobPtr, &BackgroundJob::ended, this, [this, jobPtr]() {
        const QString status = jobPtr->exitCode() >= 0 ? tr("exit status %1").arg(jobPtr->exitCode()) : tr("ended");
        emit jobEnded(jobPtr->title(), status, jobPtr->status() == BackgroundJob::Status::Finished);
    });
    // Newest first.
    m_jobs.insert(m_jobs.begin(), std::move(job));
    emit jobsChanged();
    notifyRunningCount();
    return jobPtr;
}

void BackgroundJobs::eraseJob(BackgroundJob *job)
{
    const auto it = std::ranges::find_if(m_jobs, [job](const auto &candidate) { return candidate.get() == job; });
    if (it == m_jobs.end()) {
        return;
    }
    disconnect(job, nullptr, this, nullptr);
    // QML may still hold the job on its JS stack (the button handler that
    // removed it): destroy it on the next event-loop turn.
    it->release()->deleteLater();
    m_jobs.erase(it);
    emit jobsChanged();
    notifyRunningCount();
}

QObject *BackgroundJobs::run(const QString &command, const QString &directory, const QString &title)
{
    const QString trimmed = command.trimmed();
    if (trimmed.isEmpty()) {
        return nullptr;
    }
    const QString workingDirectory = expandedDirectory(directory);
    auto session = std::make_unique<TerminalBackend>();
    session->resize(kInitialColumns, kInitialRows);
    session->setFocused(false);
    TerminalBackend *sessionPtr = session.get();
    BackgroundJob *job = addJob(std::move(session), title.trimmed(), trimmed, workingDirectory);
    sessionPtr->startCommand(trimmed, workingDirectory);
    return job;
}

QObject *BackgroundJobs::runSaved(int index)
{
    if (index < 0 || index >= m_saved.size()) {
        return nullptr;
    }
    const QJsonObject entry = m_saved.at(index).toObject();
    return run(
        entry.value(QStringLiteral("command")).toString(),
        entry.value(QStringLiteral("directory")).toString(),
        entry.value(QStringLiteral("name")).toString()
    );
}

void BackgroundJobs::interrupt(QObject *job) const
{
    if (const BackgroundJob *owned = ownedJob(job)) {
        owned->interrupt();
    }
}

void BackgroundJobs::stop(QObject *job)
{
    if (BackgroundJob *owned = ownedJob(job)) {
        owned->stop();
    }
}

void BackgroundJobs::remove(QObject *job)
{
    BackgroundJob *owned = ownedJob(job);
    if (owned != nullptr && owned->stop()) {
        eraseJob(owned);
    }
}

void BackgroundJobs::removeEnded()
{
    std::vector<BackgroundJob *> ended;
    std::ranges::for_each(m_jobs, [&ended](const auto &job) {
        if (!job->running()) {
            ended.push_back(job.get());
        }
    });
    std::ranges::for_each(ended, [this](BackgroundJob *job) { remove(job); });
}

bool BackgroundJobs::sendSession(QObject *managerObject, QObject *session)
{
    auto *manager = qobject_cast<TerminalManager *>(managerObject);
    std::unique_ptr<TerminalBackend> released = manager == nullptr ? nullptr : manager->releaseSession(session);
    if (!released) {
        return false;
    }
    released->setFocused(false);
    addJob(std::move(released), QString{}, QString{}, QString{});
    return true;
}

bool BackgroundJobs::sendWorkspace(QObject *managerObject, QObject *workspace)
{
    auto *manager = qobject_cast<TerminalManager *>(managerObject);
    if (manager == nullptr) {
        return false;
    }
    QString title;
    std::vector<std::unique_ptr<TerminalBackend>> sessions = manager->releaseWorkspace(workspace, title);
    // Listed in pane order, the first pane on top.
    std::ranges::for_each(sessions | std::views::reverse, [this, &title](std::unique_ptr<TerminalBackend> &session) {
        session->setFocused(false);
        addJob(std::move(session), title, QString{}, QString{});
    });
    return !sessions.empty();
}

bool BackgroundJobs::openInWindow(QObject *job, QObject *managerObject)
{
    BackgroundJob *owned = ownedJob(job);
    auto *manager = qobject_cast<TerminalManager *>(managerObject);
    if (owned == nullptr || manager == nullptr || !owned->running()) {
        return false;
    }
    // A tab's shell keeps following its own title; a command is named.
    const QString title = owned->command().isEmpty() ? owned->customTitle() : owned->title();
    manager->adoptSession(owned->takeSession(), title);
    eraseJob(owned);
    return true;
}

void BackgroundJobs::saveCommand(const QString &name, const QString &command, const QString &directory)
{
    if (command.trimmed().isEmpty()) {
        return;
    }
    m_saved.append(savedCommand(name, command, directory));
    storeSaved();
}

void BackgroundJobs::updateSavedCommand(
    int index,
    const QString &name,
    const QString &command,
    const QString &directory
)
{
    if (index < 0 || index >= m_saved.size() || command.trimmed().isEmpty()) {
        return;
    }
    m_saved.replace(index, savedCommand(name, command, directory));
    storeSaved();
}

void BackgroundJobs::removeSavedCommand(int index)
{
    if (index < 0 || index >= m_saved.size()) {
        return;
    }
    m_saved.removeAt(index);
    storeSaved();
}

void BackgroundJobs::moveSavedCommand(int from, int to)
{
    const auto count = m_saved.size();
    if (from < 0 || from >= count || to < 0 || to >= count || from == to) {
        return;
    }
    const QJsonValue entry = m_saved.takeAt(from);
    m_saved.insert(to, entry);
    storeSaved();
}

void BackgroundJobs::loadSaved()
{
    const QByteArray json = QSettings().value(kSavedCommandsKey).toByteArray();
    const QJsonDocument document = QJsonDocument::fromJson(json);
    m_saved = document.isArray() ? document.array() : QJsonArray{};
}

void BackgroundJobs::storeSaved()
{
    QSettings().setValue(kSavedCommandsKey, QJsonDocument(m_saved).toJson(QJsonDocument::Compact));
    emit savedCommandsChanged();
}

} // namespace konsoll
