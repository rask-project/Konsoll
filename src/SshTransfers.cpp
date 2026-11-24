#include "SshTransfers.h"

#include "AgentStatus.h"
#include "NativeDialogs.h"
#include "TextLinks.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QLoggingCategory>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <cstdio>
#include <ranges>
#include <utility>

namespace konsoll
{

namespace
{

// QT_LOGGING_RULES="konsoll.remotefiles.debug=true" shows connections and
// existence checks.
Q_LOGGING_CATEGORY(lcRemoteFiles, "konsoll.remotefiles", QtWarningMsg)

constexpr int kMasterIdleSeconds = 600;
constexpr int kProbeTimeoutMs = 8000;
constexpr qint64 kKindLifetimeMs = 10'000;
constexpr qint64 kQuietRetrySeconds = 60;
constexpr qsizetype kMaxCachedKinds = 1024;
constexpr std::size_t kMaxProbeBatch = 32;
constexpr qsizetype kMaxPrompt = 1000;
constexpr qsizetype kMaxErrorLength = 200;
constexpr qint64 kAskpassTimeoutMs = 10LL * 60 * 1000;
constexpr int kConnectTimeoutMs = 2000;
constexpr qint64 kSshFailure = 255;
// Operation::flags
inline constexpr std::uint64_t kRetried = 1u;

const QString &sshProgram()
{
    static const QString program = QStringLiteral("/usr/bin/ssh");
    return program;
}

const QString &sftpProgram()
{
    static const QString program = QStringLiteral("/usr/bin/sftp");
    return program;
}

// A path for an sftp batch command: unquoted, every character sftp would
// split on, glob or unquote escaped with a backslash (inside quotes sftp
// keeps the backslash before a glob character). sftp resolves relative
// paths in the home directory, so "~/" is dropped. nullopt for a line break,
// which a batch line cannot hold.
std::optional<QString> sftpEscaped(const QString &path)
{
    if (path.contains(u'\n') || path.contains(u'\r')) {
        return std::nullopt;
    }
    QString relative = path;
    if (relative == u"~") {
        relative = QStringLiteral(".");
    } else if (relative.startsWith(QStringLiteral("~/"))) {
        relative.remove(0, 2);
    }
    static const QString kSpecial = QStringLiteral("\\ \t\"'*?[]#");
    QString escaped;
    escaped.reserve(relative.size() * 2);
    std::ranges::for_each(relative, [&escaped](QChar c) {
        if (kSpecial.contains(c)) {
            escaped += u'\\';
        }
        escaped += c;
    });
    return escaped;
}

// The sftp batch line `command -R from to`; empty when a path cannot be
// written in one.
QByteArray sftpCommand(const char *command, const QString &from, const QString &to)
{
    const std::optional<QString> source = sftpEscaped(from);
    const std::optional<QString> target = sftpEscaped(to);
    if (!source || !target) {
        return {};
    }
    return QByteArray(command) + " -R " + source->toUtf8() + ' ' + target->toUtf8() + '\n';
}

// The last line ssh or sftp wrote to stderr: why it failed.
QString errorLine(const QByteArray &output)
{
    const QList<QByteArray> lines = output.trimmed().split('\n');
    return QString::fromUtf8(lines.isEmpty() ? QByteArray{} : lines.back().trimmed()).left(kMaxErrorLength);
}

qint64 exitCodeOf(int exitCode, QProcess::ExitStatus status)
{
    return status == QProcess::NormalExit ? exitCode : -1;
}

} // namespace

std::optional<SshTarget> parseRemoteHost(const QString &remoteHost)
{
    const QStringList fields = remoteHost.split(u';');
    const QString host = fields.value(0);
    if (host.isEmpty() || host == u"localhost") {
        return std::nullopt;
    }
    QString port;
    QByteArray arguments;
    std::ranges::for_each(fields | std::views::drop(1), [&port, &arguments](const QString &field) {
        if (field.startsWith(QStringLiteral("port="))) {
            port = field.mid(5);
        } else if (field.startsWith(QStringLiteral("args="))) {
            arguments = QByteArray::fromBase64(field.mid(5).toLatin1());
        }
    });
    if (!arguments.isEmpty()) {
        QList<QByteArray> words = arguments.split('\0');
        if (!words.isEmpty() && words.back().isEmpty()) {
            words.removeLast();
        }
        if (words.isEmpty()) {
            return std::nullopt;
        }
        SshTarget target{.options = {}, .destination = QString::fromUtf8(words.back())};
        words.removeLast();
        std::ranges::transform(words, std::back_inserter(target.options), [](const QByteArray &word) {
            return QString::fromUtf8(word);
        });
        if (!target.options.isEmpty() && target.options.back() == u"--") {
            target.options.removeLast();
        }
        if (target.destination.isEmpty() || target.destination.startsWith(u'-')) {
            return std::nullopt;
        }
        return target;
    }
    static const QRegularExpression kDestination(QStringLiteral("^(?:[A-Za-z0-9_.-]+@)?[A-Za-z0-9._-]+$"));
    if (!kDestination.match(host).hasMatch()) {
        return std::nullopt;
    }
    SshTarget target{.options = {}, .destination = host};
    if (!port.isEmpty()) {
        bool valid = false;
        const unsigned int number = port.toUInt(&valid);
        if (!valid || number == 0 || number > 65535) {
            return std::nullopt;
        }
        target.options = QStringList{QStringLiteral("-p"), port};
    }
    return target;
}

QString remoteShellPath(const QString &path)
{
    if (path == u"~") {
        return path;
    }
    if (path.startsWith(QStringLiteral("~/"))) {
        return QStringLiteral("~/") + shellQuoted(path.mid(2));
    }
    return shellQuoted(path);
}

SshConnection::SshConnection(SshTarget target)
    : m_target(std::move(target))
    // Short: a socket path has room for 104 bytes.
    , m_controlPath(
          QDir::tempPath() + QStringLiteral("/konsoll-ssh-") + QUuid::createUuid().toString(QUuid::Id128).left(12)
      )
{
    m_clock.start();
}

SshConnection::~SshConnection()
{
    // What waits fails from the event loop, never from here: the callbacks
    // belong to the owner, which may be going too.
    if (m_active.done || !m_operations.empty()) {
        QTimer::singleShot(
            0,
            QCoreApplication::instance(),
            [active = std::move(m_active),
             pending = std::move(m_operations),
             message = tr("The connection to %1 closed.").arg(m_target.destination)] {
                const auto fail = [&message](const Operation &operation) {
                    if (operation.done) {
                        operation.done(std::unexpected(message));
                    }
                };
                fail(active);
                std::ranges::for_each(pending, fail);
            }
        );
    }
    if (!m_askpassToken.isEmpty()) {
        SshAskpass::shared().close(m_askpassToken);
    }
    if (m_masterStarts != 0) {
        static_cast<void>(QProcess::startDetached(
            sshProgram(),
            {QStringLiteral("-S"), m_controlPath, QStringLiteral("-O"), QStringLiteral("exit"), m_target.destination}
        ));
    }
}

// Options for a client of the master: nothing of the user's configuration
// that would open forwardings again or ask for anything.
QStringList SshConnection::multiplexOptions() const
{
    return {
        QStringLiteral("-o"),
        QStringLiteral("ControlPath=") + m_controlPath,
        QStringLiteral("-o"),
        QStringLiteral("ControlMaster=no"),
        QStringLiteral("-o"),
        QStringLiteral("BatchMode=yes"),
        QStringLiteral("-o"),
        QStringLiteral("ClearAllForwardings=yes"),
        QStringLiteral("-o"),
        QStringLiteral("RemoteCommand=none"),
        QStringLiteral("-o"),
        QStringLiteral("PermitLocalCommand=no"),
    };
}

void SshConnection::upload(const QString &localPath, const QString &remotePath, Done done)
{
    const QFileInfo info(localPath);
    if (!info.exists()) {
        done(std::unexpected(tr("%1 no longer exists.").arg(localPath)));
        return;
    }
    QByteArray batch = sftpCommand("put", info.absoluteFilePath(), remotePath);
    if (batch.isEmpty()) {
        done(std::unexpected(tr("sftp cannot send a file whose path has a line break.")));
        return;
    }
    enqueue(Operation{.batch = std::move(batch), .removeOnFailure = {}, .done = std::move(done), .flags = 0});
}

void SshConnection::download(const QString &remotePath, const QString &localPath, Done done)
{
    QByteArray batch = sftpCommand("get", remotePath, localPath);
    if (batch.isEmpty()) {
        done(std::unexpected(tr("sftp cannot fetch a file whose path has a line break.")));
        return;
    }
    enqueue(Operation{.batch = std::move(batch), .removeOnFailure = localPath, .done = std::move(done), .flags = 0});
}

void SshConnection::enqueue(Operation operation)
{
    m_operations.push_back(std::move(operation));
    startNextOperation();
}

void SshConnection::startMaster(bool interactive)
{
    m_masterState = MasterState::Connecting;
    m_interactiveConnect = interactive ? 1 : 0;
    QStringList arguments{
        QStringLiteral("-M"),
        QStringLiteral("-S"),
        m_controlPath,
        QStringLiteral("-o"),
        QStringLiteral("ControlPersist=%1").arg(kMasterIdleSeconds),
        QStringLiteral("-o"),
        QStringLiteral("ClearAllForwardings=yes"),
        QStringLiteral("-o"),
        QStringLiteral("RemoteCommand=none"),
        QStringLiteral("-o"),
        QStringLiteral("PermitLocalCommand=no"),
        QStringLiteral("-o"),
        QStringLiteral("ServerAliveInterval=30"),
        QStringLiteral("-o"),
        QStringLiteral("ConnectTimeout=15"),
        QStringLiteral("-f"),
        QStringLiteral("-N"),
        QStringLiteral("-T"),
    };
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    if (interactive) {
        // ssh asks Konsoll (this executable, see runSshAskpass) instead of
        // a terminal it does not have.
        if (m_askpassToken.isEmpty()) {
            m_askpassToken = SshAskpass::shared().open(m_target.destination);
        }
        environment.insert(QStringLiteral("SSH_ASKPASS"), QCoreApplication::applicationFilePath());
        environment.insert(QStringLiteral("SSH_ASKPASS_REQUIRE"), QStringLiteral("force"));
        environment.insert(QStringLiteral("KONSOLL_ASKPASS_TOKEN"), m_askpassToken);
        // The socket is this process's (its pid is in the name), not one
        // the askpass process would compute, nor one inherited from the
        // Konsoll this one was started in.
        environment.insert(QStringLiteral("KONSOLL_AGENT_SOCKET"), agentStatusSocketPath());
    } else {
        arguments << QStringLiteral("-o") << QStringLiteral("BatchMode=yes");
    }
    // The user's options come after Konsoll's: ssh keeps the first value
    // given for an option.
    arguments << m_target.options << m_target.destination;

    m_master = std::make_unique<QProcess>();
    m_master->setProcessEnvironment(environment);
    m_master->setStandardInputFile(QProcess::nullDevice());
    m_master->setStandardOutputFile(QProcess::nullDevice());
    // Handled from the event loop: the handler may replace the process.
    connect(m_master.get(), &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus status) {
        QTimer::singleShot(0, this, [this, code = exitCodeOf(exitCode, status)] { masterFinished(code); });
    });
    connect(m_master.get(), &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            QTimer::singleShot(0, this, [this] { masterFinished(-1); });
        }
    });
    ++m_masterStarts;
    m_master->start(sshProgram(), arguments);
}

void SshConnection::masterFinished(qint64 exitCode)
{
    if (!m_askpassToken.isEmpty()) {
        SshAskpass::shared().close(std::exchange(m_askpassToken, {}));
    }
    const QString error = m_master ? errorLine(m_master->readAllStandardError()) : QString{};
    qCDebug(lcRemoteFiles) << "master for" << m_target.destination << "exit" << exitCode << error;
    if (exitCode == 0) {
        // -f: ssh went to the background once authenticated.
        m_masterState = MasterState::Ready;
        m_quietFailedAt = -1;
        startNextOperation();
        startProbe();
        emit kindsChanged();
        return;
    }
    const bool quiet = m_interactiveConnect == 0;
    if (quiet && !m_operations.empty()) {
        // A transfer is waiting: worth asking for the password.
        startMaster(true);
        return;
    }
    m_masterState = MasterState::Failed;
    if (quiet) {
        m_quietFailedAt = static_cast<std::int32_t>(m_clock.elapsed() / 1000);
    }
    m_probeQueue.clear();
    const QString message = error.isEmpty() ? tr("Could not connect to %1.").arg(m_target.destination)
                                            : tr("Could not connect to %1: %2").arg(m_target.destination, error);
    // Only a sign-in has nobody else to tell.
    const bool signingIn = !quiet && m_operations.empty() && !m_active.done;
    failPending(message);
    if (signingIn) {
        emit signInFailed(message);
    }
    emit kindsChanged();
}

bool SshConnection::needsSignIn() const
{
    return m_masterState == MasterState::Failed && m_quietFailedAt >= 0;
}

bool SshConnection::connectedOrConnecting() const
{
    return m_masterState == MasterState::Connecting ||
           (m_masterState == MasterState::Ready && QFileInfo::exists(m_controlPath));
}

void SshConnection::signIn()
{
    if (!connectedOrConnecting()) {
        startMaster(true);
    }
}

void SshConnection::startNextOperation()
{
    if (m_running || m_operations.empty()) {
        return;
    }
    // The master leaves by itself after its idle time: connect again.
    if (m_masterState == MasterState::Ready && !QFileInfo::exists(m_controlPath)) {
        m_masterState = MasterState::None;
    }
    if (m_masterState != MasterState::Ready) {
        if (m_masterState != MasterState::Connecting) {
            startMaster(true);
        }
        return;
    }
    m_active = std::move(m_operations.front());
    m_operations.pop_front();
    QStringList arguments{QStringLiteral("-q"), QStringLiteral("-b"), QStringLiteral("-")};
    arguments << multiplexOptions() << m_target.destination;
    m_running = std::make_unique<QProcess>();
    connect(m_running.get(), &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus status) {
        QTimer::singleShot(0, this, [this, code = exitCodeOf(exitCode, status)] { operationFinished(code); });
    });
    connect(m_running.get(), &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            QTimer::singleShot(0, this, [this] { operationFinished(-1); });
        }
    });
    m_running->start(sftpProgram(), arguments);
    m_running->write(m_active.batch);
    m_running->closeWriteChannel();
}

void SshConnection::operationFinished(qint64 exitCode)
{
    const std::unique_ptr<QProcess> process = std::move(m_running);
    Operation operation = std::exchange(m_active, Operation{});
    if (!operation.done) {
        startNextOperation(); // failed meanwhile
        return;
    }
    if (exitCode == kSshFailure && (operation.flags & kRetried) == 0u) {
        // The master went away: once more over a new one.
        operation.flags |= kRetried;
        m_masterState = MasterState::None;
        m_operations.push_front(std::move(operation));
        startNextOperation();
        return;
    }
    if (exitCode != 0) {
        if (!operation.removeOnFailure.isEmpty() && QFileInfo(operation.removeOnFailure).isFile()) {
            QFile::remove(operation.removeOnFailure);
        }
        const QString error = process ? errorLine(process->readAllStandardError()) : QString{};
        operation.done(
            std::unexpected(error.isEmpty() ? tr("The transfer with %1 failed.").arg(m_target.destination) : error)
        );
    } else {
        operation.done({});
    }
    startNextOperation();
}

std::optional<RemoteFileKind> SshConnection::cachedKind(const QString &remotePath) const
{
    const auto found = m_kinds.constFind(remotePath);
    if (found == m_kinds.constEnd()) {
        return std::nullopt;
    }
    return static_cast<RemoteFileKind>(*found & 3);
}

void SshConnection::probe(const QString &remotePath)
{
    const auto found = m_kinds.constFind(remotePath);
    const bool fresh = found != m_kinds.constEnd() && m_clock.elapsed() - (*found >> 2) <= kKindLifetimeMs;
    if (fresh || std::ranges::contains(m_probeQueue, remotePath) || std::ranges::contains(m_probing, remotePath)) {
        return;
    }
    // Never a password for a pointer move: a connection that needs one
    // waits for a transfer to make it.
    const bool quietFailedRecently =
        m_quietFailedAt >= 0 && m_clock.elapsed() / 1000 - m_quietFailedAt < kQuietRetrySeconds;
    if (m_masterState == MasterState::Failed && quietFailedRecently) {
        return;
    }
    m_probeQueue.push_back(remotePath);
    if (m_masterState == MasterState::Ready && !QFileInfo::exists(m_controlPath)) {
        m_masterState = MasterState::None;
    }
    if (m_masterState == MasterState::None || m_masterState == MasterState::Failed) {
        startMaster(false);
        return;
    }
    startProbe();
}

// One ssh for the paths waiting: a POSIX sh script on stdin answers a line
// per path ("d" directory, "f" other file, "-" missing), each marked so that
// whatever a login script prints cannot be taken for an answer.
void SshConnection::startProbe()
{
    if (m_probe || m_probeQueue.empty() || m_masterState != MasterState::Ready) {
        return;
    }
    const auto count = std::min(m_probeQueue.size(), kMaxProbeBatch);
    m_probing.assign(m_probeQueue.begin(), m_probeQueue.begin() + static_cast<std::ptrdiff_t>(count));
    m_probeQueue.erase(m_probeQueue.begin(), m_probeQueue.begin() + static_cast<std::ptrdiff_t>(count));
    QByteArray script;
    std::ranges::for_each(m_probing, [&script](const QString &path) {
        const QByteArray quoted = remoteShellPath(path).toUtf8();
        script += "if [ -d " + quoted + " ]; then echo KONSOLL:d; elif [ -e " + quoted +
                  " ]; then echo KONSOLL:f; else echo KONSOLL:-; fi\n";
    });
    QStringList arguments{QStringLiteral("-T")};
    arguments << multiplexOptions() << m_target.destination << QStringLiteral("sh -s");
    m_probe = std::make_unique<QProcess>();
    connect(m_probe.get(), &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus status) {
        QTimer::singleShot(0, this, [this, code = exitCodeOf(exitCode, status)] { probeFinished(code); });
    });
    connect(m_probe.get(), &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            QTimer::singleShot(0, this, [this] { probeFinished(-1); });
        }
    });
    qCDebug(lcRemoteFiles) << "probing" << m_probing.size() << "paths on" << m_target.destination;
    m_probe->start(sshProgram(), arguments);
    m_probe->write(script);
    m_probe->closeWriteChannel();
    // A check that hangs (a connection gone quiet) would hold every later
    // one: it is given up instead.
    QTimer::singleShot(kProbeTimeoutMs, m_probe.get(), [probe = m_probe.get()] {
        qCDebug(lcRemoteFiles) << "probe timed out";
        probe->kill();
    });
}

void SshConnection::probeFinished(qint64 exitCode)
{
    const std::unique_ptr<QProcess> process = std::move(m_probe);
    const std::vector<QString> paths = std::exchange(m_probing, {});
    if (exitCode == kSshFailure) {
        m_masterState = MasterState::None;
    }
    QList<QByteArray> answers;
    if (exitCode == 0 && process) {
        const QList<QByteArray> lines = process->readAllStandardOutput().split('\n');
        std::ranges::copy_if(lines, std::back_inserter(answers), [](const QByteArray &line) {
            return line.startsWith("KONSOLL:");
        });
    }
    qCDebug(lcRemoteFiles) << "probe exit" << exitCode << "answers" << answers.size() << "of" << paths.size();
    if (answers.size() == static_cast<qsizetype>(paths.size())) {
        if (m_kinds.size() > kMaxCachedKinds) {
            m_kinds.clear();
        }
        const qint64 now = m_clock.elapsed();
        std::ranges::for_each(std::views::iota(std::size_t{0}, paths.size()), [&](std::size_t index) {
            const char answer = answers.at(static_cast<qsizetype>(index)).at(8);
            const RemoteFileKind kind = answer == 'd'   ? RemoteFileKind::Directory
                                        : answer == 'f' ? RemoteFileKind::File
                                                        : RemoteFileKind::Missing;
            m_kinds.insert(paths[index], (now << 2) | static_cast<qint64>(kind));
        });
        emit kindsChanged();
    }
    startProbe();
}

void SshConnection::failPending(const QString &message)
{
    std::deque<Operation> operations = std::exchange(m_operations, {});
    if (m_active.done) {
        operations.push_front(std::exchange(m_active, Operation{}));
    }
    std::ranges::for_each(operations, [&message](const Operation &operation) {
        if (operation.done) {
            operation.done(std::unexpected(message));
        }
    });
}

SshAskpass &SshAskpass::shared()
{
    static SshAskpass askpass;
    return askpass;
}

QString SshAskpass::open(const QString &host)
{
    const QString token = QUuid::createUuid().toString(QUuid::Id128);
    m_hosts.insert(token, host);
    return token;
}

void SshAskpass::close(const QString &token)
{
    m_hosts.remove(token);
}

bool SshAskpass::isRequest(const QJsonObject &request)
{
    return request.contains(QLatin1String("askpass"));
}

void SshAskpass::handle(const QJsonObject &request, const Reply &reply) const
{
    const QString token = request.value(QLatin1String("askpass")).toString();
    const QString host = token.isEmpty() ? QString{} : m_hosts.value(token);
    if (host.isEmpty()) {
        reply(QJsonObject{{QStringLiteral("ok"), false}});
        return;
    }
    const QString prompt = request.value(QLatin1String("prompt")).toString().left(kMaxPrompt);
    // Not from inside the socket's read handler: the dialog runs a modal loop.
    QTimer::singleShot(0, QCoreApplication::instance(), [host, prompt, reply] {
        const std::optional<QString> answer = NativeDialogs::answerSshPrompt(host, prompt);
        reply(
            answer ? QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("answer"), *answer}}
                   : QJsonObject{{QStringLiteral("ok"), false}}
        );
    });
}

int runSshAskpass(const QString &prompt)
{
    const QString socketPath = qEnvironmentVariable("KONSOLL_AGENT_SOCKET");
    if (socketPath.isEmpty()) {
        return 1;
    }
    QLocalSocket socket;
    socket.connectToServer(socketPath);
    if (!socket.waitForConnected(kConnectTimeoutMs)) {
        return 1;
    }
    const QJsonObject request{
        {QStringLiteral("askpass"), qEnvironmentVariable("KONSOLL_ASKPASS_TOKEN")},
        {QStringLiteral("prompt"), prompt},
    };
    socket.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
    if (!socket.waitForBytesWritten(kConnectTimeoutMs)) {
        return 1;
    }
    // The user may take a while to type it.
    QElapsedTimer waited;
    waited.start();
    while (!socket.canReadLine() && socket.state() == QLocalSocket::ConnectedState &&
           waited.elapsed() < kAskpassTimeoutMs) {
        socket.waitForReadyRead(kConnectTimeoutMs);
    }
    const QJsonObject reply = QJsonDocument::fromJson(socket.readLine()).object();
    if (!reply.value(QLatin1String("ok")).toBool()) {
        return 1;
    }
    const QByteArray answer = reply.value(QLatin1String("answer")).toString().toUtf8() + '\n';
    std::fwrite(answer.constData(), 1, static_cast<std::size_t>(answer.size()), stdout);
    std::fflush(stdout);
    return 0;
}

} // namespace konsoll
