#pragma once

#include <QByteArray>
#include <QObject>
#include <QSocketNotifier>
#include <QString>

#include <sys/types.h>

#include <memory>
#include <optional>

namespace konsoll
{

// Owns a pseudo-terminal running the user's shell. All descriptor and
// child-process operations run in the owning thread (the session worker
// thread): the master fd is read there via a QSocketNotifier, and writes go
// to the master fd from that thread, buffered while the PTY input queue is
// full.
class PtyProcess : public QObject
{
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(PtyProcess)

public:
    explicit PtyProcess(QObject *parent = nullptr);
    ~PtyProcess() override;

    // Launch `shellPath` in a fresh PTY sized `cols`x`rows`. A non-empty
    // `command` runs in an interactive login shell (`-i -c command`), which
    // exits with the command's status, instead of an interactive prompt.
    bool start(
        const QString &shellPath,
        int cols,
        int rows,
        const QString &workingDirectory = {},
        const QString &remoteHostToken = {},
        const QString &command = {}
    );

    // Queue `data` for the PTY. Nothing is dropped: what the PTY input
    // queue cannot take now (a large paste, a program not reading) is kept
    // and written as the queue drains, in order.
    void write(const QByteArray &data);
    void resize(int cols, int rows) const;
    // Close the master descriptor (idempotent). Pending input is dropped.
    void close();
    // Send SIGHUP / SIGKILL to every process group of the shell's session:
    // the shell's own group and its jobs (foreground and background), which
    // job control puts in groups of their own. Processes that left the
    // session (setsid daemons) are not signalled.
    void terminate() const;
    void kill() const;
    // Whether any process is left in the shell's session (the shell or a
    // job of it), also after the shell itself has been reaped.
    bool sessionHasProcesses() const;
    // Non-blocking reap. Empty while the child runs; once it is reaped, its
    // exit code (-1 when it did not exit normally, e.g. killed by a signal,
    // or when there is no child).
    std::optional<int> tryReap();
    bool hasExited() const { return m_pid < 0; }
    // Pause / resume notifier-driven reads (output backpressure).
    void pauseReading();
    void resumeReading();
    // The program the foreground job runs (programName(): "nvim", "ssh",
    // "claude", the script node runs), "?" when it cannot be read, or empty
    // while the shell itself is in the foreground.
    QString foregroundProcessName() const;

    int pid() const { return m_pid; }

signals:
    void dataReceived(const QByteArray &data);
    void processExited(int code);
    // The master read side reached EOF (the child's write side is closed).
    void eofReached();

private:
    void onReadable();
    void onWritable();
    // Write as much of m_pendingInput as the PTY takes; arms the write
    // notifier while some is left.
    void flushPendingInput();
    void closeMaster();
    void signalSession(int signal) const;
    // The foreground process group when it is not the shell's, else -1.
    pid_t foregroundGroup() const;

    // Input the PTY has not taken yet; m_pendingOffset bytes of it are
    // already written (dropped from the front only once all of it is, so a
    // large paste is not moved again after every chunk).
    QByteArray m_pendingInput;
    std::unique_ptr<QSocketNotifier> m_notifier;
    std::unique_ptr<QSocketNotifier> m_writeNotifier;
    int m_masterFd = -1;
    int m_pid = -1;
    // The shell's session id (its pid), kept after the reap so the jobs
    // left in the session can still be found and signalled.
    int m_session = -1;
    int m_pendingOffset = 0;
};

} // namespace konsoll
