#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class QProcess;

namespace konsoll
{

// File transfers with the host a pane is connected to over ssh: files
// dropped on the pane go up (into the remote shell's directory, or /tmp for
// an agent), and files named in its output go down by dragging them to the
// Finder (TerminalBackend::dragLinkAt).
//
// Each connection opens an ssh master of its own (ControlMaster, a socket
// in the user's temporary directory) the way the pane's ssh was typed, and
// every transfer runs over it: one authentication per connection. A password,
// a passphrase or a new host key is asked for in a native dialog
// (SSH_ASKPASS is Konsoll itself: runSshAskpass), never kept. The master
// closes with the pane's ssh, and by itself after ten idle minutes.

// The connection a RemoteHost report names: the ssh options as typed (when
// konsoll-ssh.sh reported them) and the destination.
struct SshTarget {
    QStringList options;
    QString destination;
};

// Reads the RemoteHost report (after its token): "target[;port=N]
// [;args=<base64 of the NUL-separated ssh arguments>]". nullopt for a
// local shell or a target that cannot be a destination.
std::optional<SshTarget> parseRemoteHost(const QString &remoteHost);

// A remote path quoted for a shell, "~/..." left to expand to the home.
QString remoteShellPath(const QString &path);

enum class RemoteFileKind : unsigned char {
    Missing,
    File,
    Directory
};

class SshConnection final : public QObject
{
    Q_OBJECT

public:
    using Done = std::function<void(const std::expected<void, QString> &result)>;

    explicit SshConnection(SshTarget target);
    ~SshConnection() override;

    SshConnection(const SshConnection &) = delete;
    SshConnection &operator=(const SshConnection &) = delete;
    SshConnection(SshConnection &&) = delete;
    SshConnection &operator=(SshConnection &&) = delete;

    const SshTarget &target() const { return m_target; }

    // Sends the local file or folder `localPath` to `remotePath`; a folder
    // sent to an existing directory goes inside it (sftp put -R). An
    // existing file is replaced. May ask for the password first.
    void upload(const QString &localPath, const QString &remotePath, Done done);
    // Fetches the remote file or folder into `localPath` (which must not
    // exist), replacing nothing else.
    void download(const QString &remotePath, const QString &localPath, Done done);

    // What `remotePath` named when last checked; nullopt when never.
    // probe() checks it again in the background unless that was a few
    // seconds ago, without asking for a password: kindsChanged() follows.
    // Until then the last answer stands, so a link does not blink out.
    std::optional<RemoteFileKind> cachedKind(const QString &remotePath) const;
    void probe(const QString &remotePath);

    // A connection without a password was refused (and none made since):
    // links to the host's files wait for signIn(), which may ask for one.
    bool needsSignIn() const;
    // Connected, or connecting now.
    bool connectedOrConnecting() const;
    void signIn();

signals:
    // Files were checked, or the connection came up or failed: links may
    // have changed.
    void kindsChanged();
    // signIn() failed (cancelled, wrong password): `message` says why.
    void signInFailed(const QString &message);

private:
    // A process to run once the master is up: sftp with a batch of
    // commands on its stdin.
    struct Operation {
        QByteArray batch;
        QString removeOnFailure; // a partly downloaded file
        Done done;
        std::uint64_t flags{0};
    };
    enum class MasterState : std::uint8_t {
        None,
        Connecting,
        Ready,
        Failed
    };

    QStringList multiplexOptions() const;
    void enqueue(Operation operation);
    void startMaster(bool interactive);
    void masterFinished(qint64 exitCode);
    void startNextOperation();
    void operationFinished(qint64 exitCode);
    void startProbe();
    void probeFinished(qint64 exitCode);
    void failPending(const QString &message);

    std::deque<Operation> m_operations;
    std::vector<QString> m_probeQueue;
    std::vector<QString> m_probing;
    // cachedKind: (time checked << 2) | RemoteFileKind.
    QHash<QString, qint64> m_kinds;
    std::unique_ptr<QProcess> m_master;
    std::unique_ptr<QProcess> m_running;
    std::unique_ptr<QProcess> m_probe;
    Operation m_active;
    SshTarget m_target;
    QString m_controlPath;
    QString m_askpassToken;
    QElapsedTimer m_clock;
    // When a connection without a password last failed (seconds of
    // m_clock; -1: never): probes wait a minute before trying again.
    std::int32_t m_quietFailedAt{-1};
    MasterState m_masterState{MasterState::None};
    // The master connecting may ask for a password.
    std::uint8_t m_interactiveConnect{0};
    // Masters started (any: closed with ssh -O exit at the end).
    std::uint16_t m_masterStarts{0};
};

// The SSH_ASKPASS side of a connection's master: its questions come over
// the agent socket ({"askpass": token, "prompt": text}) and are answered
// from a native dialog ({"ok", "answer"}). Only a connection still waiting
// has a valid token.
class SshAskpass
{
public:
    static SshAskpass &shared();

    QString open(const QString &host);
    void close(const QString &token);

    using Reply = std::function<void(const QJsonObject &reply)>;
    static bool isRequest(const QJsonObject &request);
    void handle(const QJsonObject &request, const Reply &reply) const;

    SshAskpass(const SshAskpass &) = delete;
    SshAskpass &operator=(const SshAskpass &) = delete;
    SshAskpass(SshAskpass &&) = delete;
    SshAskpass &operator=(SshAskpass &&) = delete;
    ~SshAskpass() = default;

private:
    SshAskpass() = default;

    QHash<QString, QString> m_hosts; // token -> host
};

// `konsoll PROMPT` run by ssh as its SSH_ASKPASS (KONSOLL_ASKPASS_TOKEN is
// set): asks the app at KONSOLL_AGENT_SOCKET (the socket's name has the
// app's pid), prints the answer. Exit status 1 when cancelled.
int runSshAskpass(const QString &prompt);

} // namespace konsoll
