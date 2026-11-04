#pragma once

#include <QByteArray>
#include <QObject>
#include <QSocketNotifier>

#include <memory>

namespace konsoll {

// Owns a pseudo-terminal running the user's shell. The master fd is read in
// the GUI thread via a QSocketNotifier; writes go straight to the master fd.
class PtyProcess : public QObject {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(PtyProcess)

public:
    explicit PtyProcess(QObject *parent = nullptr);
    ~PtyProcess() override;

    // Launch `shellPath` in a fresh PTY sized `cols`x`rows`.
    bool start(const QString &shellPath, int cols, int rows, const QString &workingDirectory = {},
               const QString &remoteHostToken = {});

    void write(const QByteArray &data) const;
    void resize(int cols, int rows) const;
    void terminate() const;
    bool hasForegroundProcess() const;

    int pid() const { return m_pid; }

signals:
    void dataReceived(const QByteArray &data);
    void processExited(int code);

private:
    void onReadable();
    void closeMaster();

    int m_masterFd = -1;
    int m_pid = -1;
    std::unique_ptr<QSocketNotifier> m_notifier;
};

} // namespace konsoll
