#include "AgentStatus.h"

#include "ShellFunctions.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QSaveFile>

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <utility>

namespace konsoll
{

namespace
{

// A status line or a tool request is short; anything longer is not a hook
// or Konsoll's MCP server talking.
constexpr qsizetype kMaxLineLength = 65536;
constexpr qsizetype kMaxAgentNameLength = 32;
constexpr qsizetype kMaxMessageLength = 200;

// Indexed by AgentState; "ended" names None: the agent's session is over.
constexpr auto kStateNames =
    std::to_array<QByteArrayView>({"ended", "working", "waiting", "idle", "error", "approval"});

bool validAgentName(QByteArrayView name)
{
    return !name.isEmpty() && name.size() <= kMaxAgentNameLength && std::ranges::all_of(name, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}

void installHookScript(const QString &path)
{
    QFile source(QStringLiteral(":/shell-integration/konsoll-agent-hook"));
    if (!source.open(QIODevice::ReadOnly)) {
        return;
    }
    const QByteArray contents = source.readAll();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile target(path);
    if (!target.open(QIODevice::WriteOnly) || target.write(contents) != contents.size() || !target.commit()) {
        qWarning("Konsoll: could not install the agent hook at %s", qUtf8Printable(path));
        return;
    }
    QFile::setPermissions(
        path,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner | QFileDevice::ReadGroup |
            QFileDevice::ExeGroup | QFileDevice::ReadOther | QFileDevice::ExeOther
    );
}

} // namespace

QString agentStateName(AgentState state)
{
    const auto index = static_cast<std::size_t>(state);
    return state == AgentState::None || index >= kStateNames.size() ? QString{}
                                                                    : QString::fromLatin1(kStateNames.at(index));
}

std::expected<AgentStatusMessage, AgentStatusError> parseAgentStatus(QByteArrayView line)
{
    const QList<QByteArray> fields = line.trimmed().toByteArray().split(' ');
    if (fields.size() < 3 || fields.size() > 4 || fields[0].isEmpty()) {
        return std::unexpected(AgentStatusError::Malformed);
    }
    if (!validAgentName(fields[1])) {
        return std::unexpected(AgentStatusError::BadAgentName);
    }
    const auto *const state = std::ranges::find(kStateNames, QByteArrayView(fields[2]));
    if (state == kStateNames.end()) {
        return std::unexpected(AgentStatusError::UnknownState);
    }
    QString message;
    if (fields.size() == 4) {
        const auto decoded = QByteArray::fromBase64Encoding(fields[3], QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded) {
            return std::unexpected(AgentStatusError::Malformed);
        }
        message = QString::fromUtf8(decoded.decoded).simplified().left(kMaxMessageLength);
    }
    return AgentStatusMessage{
        .token = QString::fromLatin1(fields[0]),
        .agent = QString::fromLatin1(fields[1]),
        .message = std::move(message),
        .stateValue = static_cast<std::uint64_t>(std::distance(kStateNames.begin(), state))
    };
}

QString agentStatusSocketPath()
{
    // Per process: two running copies (a development build next to the
    // installed app) each get their own sessions' reports. $TMPDIR is
    // per-user on macOS and keeps the path under the sun_path limit.
    static const QString path = QDir::tempPath() + QStringLiteral("/konsoll-agent-") +
                                QString::number(QCoreApplication::applicationPid()) + QStringLiteral(".sock");
    return path;
}

QString agentHookPath()
{
    static const QString path = shellIntegrationDirectory() + QStringLiteral("/konsoll-agent-hook");
    return path;
}

AgentStatusServer &AgentStatusServer::instance()
{
    static AgentStatusServer s_instance;
    return s_instance;
}

AgentStatusServer::AgentStatusServer() : m_server(std::make_unique<QLocalServer>())
{
    installHookScript(agentHookPath());
    // Only this user may connect: another user's process must not drive the
    // tabs, even with a guessed token.
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    QLocalServer::removeServer(agentStatusSocketPath());
    if (!m_server->listen(agentStatusSocketPath())) {
        qWarning("Konsoll: agent status socket unavailable: %s", qUtf8Printable(m_server->errorString()));
        return;
    }
    connect(m_server.get(), &QLocalServer::newConnection, this, &AgentStatusServer::onNewConnection);
}

AgentStatusServer::~AgentStatusServer()
{
    if (m_server->isListening()) {
        m_server->close();
        QLocalServer::removeServer(agentStatusSocketPath());
    }
}

void AgentStatusServer::setRequestHandler(RequestHandler handler)
{
    m_requestHandler = std::move(handler);
}

bool AgentStatusServer::listening() const
{
    return m_server->isListening();
}

void AgentStatusServer::onNewConnection()
{
    while (QLocalSocket *socket = m_server->nextPendingConnection()) {
        // The server parents pending sockets; each is released with
        // deleteLater() once it disconnects.
        connect(socket, &QLocalSocket::readyRead, this, [this, socket]() { readStatus(socket); });
        connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
    }
}

// One line per connection: the hook writes it and waits for the close.
void AgentStatusServer::readStatus(QLocalSocket *socket)
{
    if (!socket->canReadLine()) {
        if (socket->bytesAvailable() > kMaxLineLength) {
            socket->disconnectFromServer();
        }
        return;
    }
    const QByteArray line = socket->readLine(kMaxLineLength);
    if (line.startsWith('{')) {
        // One request per connection: nothing more is read from it.
        disconnect(socket, &QLocalSocket::readyRead, this, nullptr);
        const QPointer<QLocalSocket> client(socket);
        Reply reply = [client](const QJsonObject &answer) {
            if (client && client->state() == QLocalSocket::ConnectedState) {
                client->write(QJsonDocument(answer).toJson(QJsonDocument::Compact) + '\n');
                client->disconnectFromServer(); // after the reply is written
            }
        };
        const QJsonObject request = QJsonDocument::fromJson(line).object();
        if (m_requestHandler) {
            m_requestHandler(request, std::move(reply));
        } else {
            reply(QJsonObject{{QStringLiteral("error"), tr("Konsoll cannot do that")}});
        }
        return;
    }
    socket->disconnectFromServer();
    if (const auto status = parseAgentStatus(line)) {
        emit statusReceived(status->token, status->agent, status->state(), status->message);
    }
}

} // namespace konsoll
