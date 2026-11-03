#include "PtyWorker.h"

#include <QEventLoop>
#include <QThread>
#include <QTimer>

namespace konsoll
{

PtyWorker::PtyWorker() : QObject() {}

void PtyWorker::start(const QString &shellPath, int columns, int rows, const QString &workingDirectory)
{
    if (m_pty) {
        return; // Already started.
    }

    m_pty = std::make_unique<PtyProcess>(this);
    connect(m_pty.get(), &PtyProcess::dataReceived, this, &PtyWorker::onOutput);
    connect(m_pty.get(), &PtyProcess::eofReached, this, &PtyWorker::onEof);
    if (!m_pty->start(shellPath, columns, rows, workingDirectory)) {
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
    if (m_undelivered < 0) {
        m_undelivered = 0;
    }
    if (m_pty && (m_undelivered <= kLowWater)) {
        m_pty->resumeReading();
    }
}

void PtyWorker::onEof()
{
    // The child's write side is closed; reap it here (worker thread) so the
    // read path never blocks.
    const int code = m_pty ? m_pty->waitForExit() : -1;
    emit processExited(code);
}

void PtyWorker::shutdown()
{
    if (!m_pty) {
        emit shutdownComplete();
        return; // Idempotent: m_pty is null once shutdown has run.
    }
    if (m_foregroundTimer) {
        m_foregroundTimer->stop();
    }
    // Close the master descriptor (the kernel sends SIGHUP to the slave's
    // foreground process group), then send an explicit SIGHUP for processes
    // that outlive the tty.
    m_pty->close();
    m_pty->terminate();
    if (!m_pty->hasExited()) {
        // Bounded graceful period: let the shell and its process group exit
        // on SIGHUP before escalating to SIGKILL. The master read side is
        // closed, so no read event can signal the exit; poll for the reap
        // instead. The single-shot timer bounds the wait.
        QEventLoop loop;
        QTimer graceTimer;
        graceTimer.setInterval(25);
        connect(&graceTimer, &QTimer::timeout, [this, &loop]() {
            if ((m_pty != nullptr) && m_pty->tryReap()) {
                loop.quit();
            }
        });
        QTimer::singleShot(1500, &loop, &QEventLoop::quit);
        graceTimer.start();
        loop.exec();
        if (!m_pty->hasExited()) {
            m_pty->kill();
        }
    }
    m_pty->waitForExit();
    m_pty.reset();
    m_undelivered = 0;
    emit shutdownComplete();
}

void PtyWorker::checkForeground()
{
    // Report the current foreground state on every poll; the backend caches
    // it and ignores repeats, so no state is kept here.
    const bool hasForeground = m_pty ? m_pty->hasForegroundProcess() : false;
    emit foregroundProcessChanged(hasForeground);
}

} // namespace konsoll
