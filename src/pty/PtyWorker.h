#pragma once

#include <QByteArray>
#include <QDeadlineTimer>
#include <QObject>
#include <QString>
#include <QTimer>

#include <memory>

#include "PtyProcess.h"

namespace konsoll
{

// Owns a session's PtyProcess and runs in that session's worker thread.
// PTY I/O (start / write / resize / shutdown) is invoked on this object's
// thread via queued calls from the GUI-thread TerminalBackend. Output and
// lifecycle events are re-emitted as signals for the backend to consume on
// the GUI thread. The PTY master descriptor, its QSocketNotifier, and the
// child process are touched only in this thread.
class PtyWorker : public QObject
{
    Q_OBJECT

public:
    PtyWorker();

    // Create and start the PTY in this worker's thread. Emits started on
    // success and startFailed on launch failure. A non-empty `command` runs
    // instead of an interactive shell (see PtyProcess::start).
    Q_INVOKABLE void start(
        const QString &shellPath,
        int columns,
        int rows,
        const QString &workingDirectory,
        const QString &remoteHostToken,
        const QString &command
    );
    // Send a byte block to the PTY master.
    Q_INVOKABLE void write(const QByteArray &data);
    // Apply a new window size to the PTY.
    Q_INVOKABLE void resize(int columns, int rows);
    // Begin bounded shutdown: SIGHUP every process group of the shell's
    // session, close the descriptor, escalate to SIGKILL after the grace
    // period and reap the child. Never blocks the thread: the reap is
    // polled, and past a final deadline the worker gives up on it. Emits
    // shutdownComplete when the PTY resources are released.
    Q_INVOKABLE void shutdown();
    // Acknowledge that the GUI thread consumed `bytes` of emitted output
    // (output backpressure).
    Q_INVOKABLE void outputDelivered(qint64 bytes);

signals:
    // One byte block read from the PTY.
    void outputReceived(const QByteArray &data);
    // The PTY started; the shell is running.
    void started();
    // The shell process exited.
    void processExited(int exitCode);
    // The PTY failed to start.
    void startFailed(const QString &error);
    // The PTY foreground job's executable name (empty while the shell is in
    // the foreground), reported on every poll.
    void foregroundProcessChanged(const QString &name);
    // The worker released its PTY resources.
    void shutdownComplete();

private:
    void checkForeground();
    void onOutput(const QByteArray &data);
    void onEof();
    // Poll for the child's exit every kReapInterval (after EOF, or during
    // shutdown).
    void startReaping();
    void pollReap();
    void finishShutdown();
    // Shutdown state, kept in the deadlines (they are Forever until set):
    // shutting down once the abandon deadline is set, killed once the kill
    // deadline is cleared again after SIGKILL.
    bool shuttingDown() const { return !m_abandonDeadline.isForever(); }
    bool killed() const { return shuttingDown() && m_killDeadline.isForever(); }

    // Output backpressure: pause PTY reads while more than kHighWater bytes
    // are emitted but not yet consumed by the GUI thread; resume below
    // kLowWater.
    static constexpr qint64 kHighWater = static_cast<qint64>(1024) * 1024;
    static constexpr qint64 kLowWater = static_cast<qint64>(256) * 1024;

    static constexpr int kReapInterval = 25;
    // SIGHUP grace before SIGKILL, then how long a killed child may take to
    // be reaped before the worker stops waiting. Together they stay under
    // the GUI's 3 s wait for the thread (TerminalBackend::close).
    static constexpr qint64 kGracePeriod = 1500;
    static constexpr qint64 kKillPeriod = 1000;

    // Forever until shutdown sets them (a default QDeadlineTimer is expired,
    // not Forever).
    QDeadlineTimer m_killDeadline{QDeadlineTimer::Forever};
    QDeadlineTimer m_abandonDeadline{QDeadlineTimer::Forever};
    std::unique_ptr<QTimer> m_foregroundTimer;
    std::unique_ptr<QTimer> m_reapTimer;
    std::unique_ptr<PtyProcess> m_pty;
    qint64 m_undelivered{0};
};

} // namespace konsoll
