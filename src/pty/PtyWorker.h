#pragma once

#include <QByteArray>
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
    // success and startFailed on launch failure.
    Q_INVOKABLE void start(const QString &shellPath, int columns, int rows, const QString &workingDirectory,
                           const QString &remoteHostToken);
    // Send a byte block to the PTY master.
    Q_INVOKABLE void write(const QByteArray &data);
    // Apply a new window size to the PTY.
    Q_INVOKABLE void resize(int columns, int rows);
    // Begin bounded shutdown: close the descriptor, SIGHUP the process group,
    // escalate to SIGKILL after the grace period, and reap the child. Emits
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
    // The PTY foreground process group is reported on every poll.
    void foregroundProcessChanged(bool hasForeground);
    // The worker released its PTY resources.
    void shutdownComplete();

private:
    void checkForeground();
    void onOutput(const QByteArray &data);
    void onEof();

    // Output backpressure: pause PTY reads while more than kHighWater bytes
    // are emitted but not yet consumed by the GUI thread; resume below
    // kLowWater.
    static constexpr qint64 kHighWater = static_cast<qint64>(1024) * 1024;
    static constexpr qint64 kLowWater = static_cast<qint64>(256) * 1024;

    std::unique_ptr<QTimer> m_foregroundTimer;
    std::unique_ptr<PtyProcess> m_pty;
    qint64 m_undelivered{0};
};

} // namespace konsoll
