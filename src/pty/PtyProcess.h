#pragma once

#include <QByteArray>
#include <QObject>
#include <QSocketNotifier>
#include <QString>

#include <sys/types.h>

#include <memory>

namespace konsoll {

// Owns a pseudo-terminal running the user's shell. All descriptor and
// child-process operations run in the owning thread (the session worker
// thread): the master fd is read there via a QSocketNotifier and writes go
// straight to the master fd from that thread.
class PtyProcess : public QObject {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(PtyProcess)

public:
    explicit PtyProcess(QObject *parent = nullptr);
    ~PtyProcess() override;

    // Launch `shellPath` in a fresh PTY sized `cols`x`rows`. A non-empty
    // `command` runs in an interactive login shell (`-i -c command`), which
    // exits with the command's status, instead of an interactive prompt.
    bool start(const QString &shellPath, int cols, int rows, const QString &workingDirectory = {},
               const QString &remoteHostToken = {}, const QString &command = {});

    void write(const QByteArray &data) const;
    void resize(int cols, int rows) const;
    // Close the master descriptor (idempotent); the kernel sends SIGHUP to
    // the slave's foreground process group.
    void close();
    // Send SIGHUP / SIGKILL to the shell's process group.
    void terminate() const;
    void kill() const;
    // Reap the child and return its exit code (-1 when there is no child or
    // it was already reaped).
    int waitForExit();
    // Non-blocking reap; returns true once the child has been reaped.
    bool tryReap();
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
    void closeMaster();
    // The foreground process group when it is not the shell's, else -1.
    pid_t foregroundGroup() const;

    int m_masterFd = -1;
    int m_pid = -1;
    std::unique_ptr<QSocketNotifier> m_notifier;
};

} // namespace konsoll
