#pragma once

#include <QDateTime>
#include <QJsonArray>
#include <QObject>
#include <QVariantList>
#include <QtQml>

#include <cstdint>
#include <memory>
#include <vector>

namespace konsoll
{

class TerminalBackend;

// A session running without a tab: a command started from the Background
// Commands window, or a tab's shell sent there. It owns its TerminalBackend
// until the session goes back to a window as a tab.
class BackgroundJob final : public QObject
{
    Q_OBJECT
    QML_NAMED_ELEMENT(BackgroundJob)
    QML_UNCREATABLE("Background jobs are created by BackgroundJobs")

    Q_PROPERTY(QString title READ title NOTIFY titleChanged)
    // The name it was saved under or had as a tab; empty otherwise.
    Q_PROPERTY(QString name READ customTitle CONSTANT)
    // The command line it runs; empty for a shell sent from a tab.
    Q_PROPERTY(QString command READ command CONSTANT)
    Q_PROPERTY(QString directory READ directory NOTIFY directoryChanged)
    // "running", "finished" (exit 0), "failed" (non-zero exit or launch
    // failure) or "stopped" (by the user).
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(bool running READ running NOTIFY stateChanged)
    // Exit status once it ended; -1 when unknown (signal, launch failure).
    Q_PROPERTY(int exitCode READ exitCode NOTIFY stateChanged)
    Q_PROPERTY(int number READ number CONSTANT)
    Q_PROPERTY(QDateTime startedAt READ startedAt CONSTANT)
    Q_PROPERTY(QDateTime finishedAt READ finishedAt NOTIFY stateChanged)
    // The terminal it draws on (TerminalRenderer.session); null once it
    // moved to a tab.
    Q_PROPERTY(QObject *session READ session NOTIFY sessionChanged)

public:
    enum class Status : std::uint8_t {
        Running,
        Finished,
        Failed,
        Stopped
    };

    BackgroundJob(
        std::unique_ptr<TerminalBackend> session,
        int number,
        QString title,
        QString command,
        QString directory
    );
    ~BackgroundJob() override;
    Q_DISABLE_COPY_MOVE(BackgroundJob)

    QString title() const;
    QString command() const { return m_command; }
    QString directory() const;
    QString state() const;
    bool running() const { return m_status == Status::Running; }
    Status status() const { return m_status; }
    int exitCode() const { return m_exitCode; }
    int number() const { return m_number; }
    QDateTime startedAt() const { return m_startedAt; }
    QDateTime finishedAt() const { return m_finishedAt; }
    QObject *session() const;
    TerminalBackend *backend() const { return m_session.get(); }
    // The name it had as a tab (or was saved under); empty otherwise.
    QString customTitle() const { return m_title; }

    void interrupt() const;
    // Ends the session (SIGHUP, then SIGKILL). False while its worker thread
    // has not stopped yet.
    bool stop();
    // Hands the running session over (to a window); the job is done.
    std::unique_ptr<TerminalBackend> takeSession();

signals:
    void titleChanged();
    void directoryChanged();
    void stateChanged();
    void sessionChanged();
    // The session ended on its own (not stopped by the user).
    void ended();

private:
    static constexpr unsigned char StopRequestedFlag = 1u;

    void connectSession();
    void finish(Status status, int exitCode);

    std::unique_ptr<TerminalBackend> m_session;
    QString m_title;
    QString m_command;
    QString m_directory;
    QDateTime m_startedAt;
    QDateTime m_finishedAt;
    int m_number{};
    std::int16_t m_exitCode{-1};
    Status m_status{Status::Running};
    unsigned char m_flags{};
};

// The Background Commands window's model: the jobs and the saved commands
// ({name, command, directory}, kept in QSettings). Owned by main() and
// exposed to QML as a singleton.
class BackgroundJobs final : public QObject
{
    Q_OBJECT
    QML_NAMED_ELEMENT(BackgroundJobs)
    QML_SINGLETON

    Q_PROPERTY(QVariantList jobs READ jobs NOTIFY jobsChanged)
    Q_PROPERTY(int runningCount READ runningCount NOTIFY runningCountChanged)
    Q_PROPERTY(QVariantList savedCommands READ savedCommands NOTIFY savedCommandsChanged)

public:
    BackgroundJobs();
    ~BackgroundJobs() override;
    Q_DISABLE_COPY_MOVE(BackgroundJobs)

    // The live instance (null before main() created it or after it ended).
    static BackgroundJobs *instance();
    static BackgroundJobs *create(QQmlEngine *qmlEngine, QJSEngine *jsEngine);

    QVariantList jobs() const;
    int runningCount() const;
    QVariantList savedCommands() const { return m_saved.toVariantList(); }

    // Starts `command` in `directory` (home when empty; `~` is expanded),
    // listed as `title` (the command itself when empty).
    // `setup` runs before `command` in the same shell without being shown
    // (AgentCommands turns pagers off with it).
    Q_INVOKABLE QObject *
    run(const QString &command, const QString &directory = {}, const QString &title = {}, const QString &setup = {});
    Q_INVOKABLE QObject *runSaved(int index);
    Q_INVOKABLE void interrupt(QObject *job) const;
    Q_INVOKABLE void stop(QObject *job);
    // Stops a running job and drops it from the list.
    Q_INVOKABLE void remove(QObject *job);
    Q_INVOKABLE void removeEnded();
    // Moves a tab's pane (`session` of window `manager`) or a whole tab
    // (`workspace`) here; the shells keep running.
    Q_INVOKABLE bool sendSession(QObject *manager, QObject *session);
    Q_INVOKABLE bool sendWorkspace(QObject *manager, QObject *workspace);
    // Shows a running job as a new tab of window `manager`.
    Q_INVOKABLE bool openInWindow(QObject *job, QObject *manager);

    Q_INVOKABLE void saveCommand(const QString &name, const QString &command, const QString &directory);
    Q_INVOKABLE void
    updateSavedCommand(int index, const QString &name, const QString &command, const QString &directory);
    Q_INVOKABLE void removeSavedCommand(int index);
    Q_INVOKABLE void moveSavedCommand(int from, int to);

    // For the quit confirmation.
    int jobCount() const { return static_cast<int>(m_jobs.size()); }

signals:
    void jobsChanged();
    void runningCountChanged();
    void savedCommandsChanged();
    // A job's program ended on its own (not stopped by the user): `title`
    // with `message` ("exit status 3"); `succeeded` for exit status 0.
    void jobEnded(const QString &title, const QString &message, bool succeeded);

private:
    BackgroundJob *ownedJob(const QObject *job) const;
    BackgroundJob *addJob(
        std::unique_ptr<TerminalBackend> session,
        const QString &title,
        const QString &command,
        const QString &directory
    );
    void eraseJob(BackgroundJob *job);
    void notifyRunningCount();
    void loadSaved();
    void storeSaved();

    std::vector<std::unique_ptr<BackgroundJob>> m_jobs;
    QJsonArray m_saved;
    int m_nextNumber{1};
    int m_reportedRunningCount{};
};

} // namespace konsoll
