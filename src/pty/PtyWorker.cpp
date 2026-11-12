#include "PtyWorker.h"

#include <algorithm>

#include <QThread>
#include <QTimer>

namespace konsoll
{

PtyWorker::PtyWorker() : QObject() {}

void PtyWorker::start(
    const QString &shellPath,
    int columns,
    int rows,
    const QString &workingDirectory,
    const QString &remoteHostToken,
    const QString &command
)
{
    if (m_pty) {
        return; // Already started.
    }

    m_pty = std::make_unique<PtyProcess>(this);
    connect(m_pty.get(), &PtyProcess::dataReceived, this, &PtyWorker::onOutput);
    connect(m_pty.get(), &PtyProcess::eofReached, this, &PtyWorker::onEof);
    if (!m_pty->start(shellPath, columns, rows, workingDirectory, remoteHostToken, command)) {
        m_pty.reset();
        emit startFailed(QStringLiteral("The PTY could not be started"));
        return;
    }
    emit started();

    // The PTY foreground process group is queried from the GUI thread for the
    // quit guard, so it cannot read the master descriptor directly. Poll it
    // here (in this thread) and re-emit on change. The timer is created in this
    // thread so its affinity is the worker thread from the start.
    m_foregroundTimer = std::make_unique<QTimer>(this);
    m_foregroundTimer->setTimerType(Qt::CoarseTimer);
    m_foregroundTimer->setInterval(500);
    connect(m_foregroundTimer.get(), &QTimer::timeout, this, &PtyWorker::checkForeground);
    m_foregroundTimer->start();
}

void PtyWorker::write(const QByteArray &data)
{
    if (m_pty) {
        m_pty->write(data);
    }
}

void PtyWorker::resize(int columns, int rows)
{
    if (m_pty) {
        m_pty->resize(columns, rows);
    }
}

void PtyWorker::onOutput(const QByteArray &data)
{
    // Output backpressure: track bytes emitted but not yet consumed by the GUI
    // thread; pause PTY reads above the high-water mark so sustained output
    // cannot grow an unbounded queue of GUI work.
    m_undelivered += static_cast<qint64>(data.size());
    emit outputReceived(data);
    if ((m_undelivered > kHighWater) && m_pty) {
        m_pty->pauseReading();
    }
}

void PtyWorker::outputDelivered(qint64 bytes)
{
    m_undelivered -= bytes;
    m_undelivered = std::max<qint64>(m_undelivered, 0);
    if (m_pty && (m_undelivered <= kLowWater)) {
        m_pty->resumeReading();
    }
}

void PtyWorker::onEof()
{
    // The child's write side is closed; it is exiting. Reap it without
    // blocking: a waitpid here would stall the thread, so a later shutdown
    // could never run if the child took long to go.
    startReaping();
}

void PtyWorker::shutdown()
{
    if (!m_pty) {
        emit shutdownComplete();
        return; // Idempotent: m_pty is null once shutdown has run.
    }
    if (shuttingDown()) {
        return; // Already in progress; shutdownComplete follows.
    }
    if (m_foregroundTimer) {
        m_foregroundTimer->stop();
    }
    // SIGHUP the whole session (the shell and its jobs) while the terminal
    // still has its foreground group, then close the master descriptor.
    m_pty->terminate();
    m_pty->close();
    m_killDeadline = QDeadlineTimer(kGracePeriod);
    m_abandonDeadline = QDeadlineTimer(kGracePeriod + kKillPeriod);
    startReaping();
    pollReap(); // Done at once when everything exited on the SIGHUP.
}

void PtyWorker::startReaping()
{
    if (!m_reapTimer) {
        m_reapTimer = std::make_unique<QTimer>(this);
        m_reapTimer->setInterval(kReapInterval);
        connect(m_reapTimer.get(), &QTimer::timeout, this, &PtyWorker::pollReap);
    }
    if (!m_reapTimer->isActive()) {
        m_reapTimer->start();
    }
}

void PtyWorker::pollReap()
{
    if (!m_pty) {
        m_reapTimer->stop();
        return;
    }
    const std::optional<int> exitCode = m_pty->tryReap();
    if (!shuttingDown()) {
        // After EOF: wait for the exit for as long as it takes.
        if (exitCode) {
            m_reapTimer->stop();
            emit processExited(*exitCode);
        }
        return;
    }
    // Closing the tab ends everything in it: not only the shell, which may
    // exit on the SIGHUP at once, but its jobs too, one of which may ignore
    // SIGHUP and keep running (writing to a terminal that is gone).
    if (exitCode && !m_pty->sessionHasProcesses()) {
        finishShutdown();
        return;
    }
    if (!killed() && m_killDeadline.hasExpired()) {
        m_pty->kill();
        m_killDeadline = QDeadlineTimer(QDeadlineTimer::Forever);
    } else if (killed() && exitCode) {
        finishShutdown(); // SIGKILL sent to what is left of the session.
    } else if (killed() && m_abandonDeadline.hasExpired()) {
        // Stuck in the kernel even after SIGKILL. Release the session anyway
        // so its tab closes; the destructor makes a last non-blocking try.
        qWarning("Konsoll: shell %d was not reaped after SIGKILL; releasing the session", m_pty->pid());
        finishShutdown();
    }
}

void PtyWorker::finishShutdown()
{
    if (m_reapTimer) {
        m_reapTimer->stop();
    }
    m_pty.reset();
    m_undelivered = 0;
    emit shutdownComplete();
}

void PtyWorker::checkForeground()
{
    // Report the current foreground state on every poll; the backend caches
    // it and ignores repeats, so no state is kept here.
    emit foregroundProcessChanged(m_pty ? m_pty->foregroundProcessName() : QString{});
}

} // namespace konsoll
