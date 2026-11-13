#include "AgentStatus.h"

#include "ShellFunctions.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <optional>
#include <ranges>
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

// The AgentState a state name stands for (kStateNames' index), if any.
std::optional<std::uint64_t> stateIndex(QByteArrayView name)
{
    const auto *const state = std::ranges::find(kStateNames, name);
    if (state == kStateNames.end()) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(std::distance(kStateNames.begin(), state));
}

// The JSON string whose opening quote is at `quote` in `text`, decoded. One
// cut short (the remote hook sends the start of the event) ends where the
// text does, less an escape cut in half.
QString jsonStringAt(const QString &text, qsizetype quote)
{
    bool escaped = false;
    const auto begin = text.cbegin() + quote + 1;
    const auto closing = std::find_if(begin, text.cend(), [&escaped](QChar c) {
        if (escaped) {
            escaped = false;
            return false;
        }
        escaped = c == QLatin1Char('\\');
        return c == QLatin1Char('"');
    });
    const QString literal(begin, closing - begin);
    // "\u00e" needs up to five characters dropped to parse.
    constexpr qsizetype kLongestEscape = 6;
    const auto parsed = std::ranges::find_if(std::views::iota(qsizetype{0}, kLongestEscape), [&literal](qsizetype cut) {
        QJsonParseError error{};
        QJsonDocument::fromJson(
            QStringLiteral("[\"%1\"]").arg(literal.chopped(std::min(cut, literal.size()))).toUtf8(),
            &error
        );
        return error.error == QJsonParseError::NoError;
    });
    if (parsed == std::views::iota(qsizetype{0}, kLongestEscape).end()) {
        return {};
    }
    const QString decodable = literal.chopped(std::min(*parsed, literal.size()));
    return QJsonDocument::fromJson(QStringLiteral("[\"%1\"]").arg(decodable).toUtf8()).array().at(0).toString();
}

// The string at the dotted `path` of `event`: walked when the JSON parsed,
// otherwise the first "<last component>": "..." in the text.
QString eventFieldAt(const QJsonDocument &document, const QString &text, const QString &path)
{
    const QStringList components = path.split(QLatin1Char('.'), Qt::SkipEmptyParts);
    if (components.isEmpty()) {
        return {};
    }
    if (document.isObject()) {
        QJsonValue value = document.object();
        std::ranges::for_each(components, [&value](const QString &component) {
            value = value.toObject().value(component);
        });
        return value.toString();
    }
    const QRegularExpression key(QStringLiteral("\"%1\"\\s*:\\s*\"").arg(QRegularExpression::escape(components.last()))
    );
    const QRegularExpressionMatch match = key.match(text);
    return match.hasMatch() ? jsonStringAt(text, match.capturedEnd() - 1) : QString{};
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
    const std::optional<std::uint64_t> state = stateIndex(fields[2]);
    if (!state) {
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
        .stateValue = *state
    };
}

QString remoteAgentToken(const QString &sessionToken)
{
    constexpr qsizetype kTokenLength = 32;
    return QString::fromLatin1(
        QCryptographicHash::hash((sessionToken + QStringLiteral(":ssh")).toUtf8(), QCryptographicHash::Sha256)
            .toHex()
            .left(kTokenLength)
    );
}

std::expected<AgentStatusMessage, AgentStatusError> parseRemoteAgentReport(QByteArrayView payload, const QString &token)
{
    const QList<QByteArray> fields = payload.toByteArray().split(';');
    if (fields.size() < 3 || token.isEmpty() || QString::fromLatin1(fields[0]) != token) {
        return std::unexpected(AgentStatusError::Malformed);
    }
    if (!validAgentName(fields[1])) {
        return std::unexpected(AgentStatusError::BadAgentName);
    }
    const std::optional<std::uint64_t> state = stateIndex(fields[2]);
    if (!state) {
        return std::unexpected(AgentStatusError::UnknownState);
    }
    QByteArray text;
    QByteArray keys;
    QByteArray event;
    bool decoded = true;
    std::ranges::for_each(fields | std::views::drop(3), [&](const QByteArray &field) {
        const qsizetype separator = field.indexOf('=');
        const QByteArray name = field.left(separator);
        const QByteArray value = separator < 0 ? QByteArray{} : field.mid(separator + 1);
        if (name == "keys") {
            keys = value;
        } else if (name == "text" || name == "event") {
            const auto bytes = QByteArray::fromBase64Encoding(value, QByteArray::AbortOnBase64DecodingErrors);
            decoded = decoded && static_cast<bool>(bytes);
            (name == "text" ? text : event) = bytes.decoded;
        }
    });
    if (!decoded) {
        return std::unexpected(AgentStatusError::Malformed);
    }
    const QString message =
        !text.isEmpty() ? QString::fromUtf8(text) : agentEventField(event, QString::fromLatin1(keys));
    return AgentStatusMessage{
        .token = {},
        .agent = QString::fromLatin1(fields[1]),
        .message = message.simplified().left(kMaxMessageLength),
        .stateValue = *state
    };
}

QString agentEventField(const QByteArray &event, const QString &keys)
{
    if (event.isEmpty()) {
        return {};
    }
    const QJsonDocument document = QJsonDocument::fromJson(event);
    const QString text = QString::fromUtf8(event);
    const QStringList paths = keys.split(QLatin1Char('|'), Qt::SkipEmptyParts);
    auto fields = paths | std::views::transform([&document, &text](const QString &path) {
                      return eventFieldAt(document, text, path);
                  });
    const auto found = std::ranges::find_if(fields, [](const QString &field) { return !field.isEmpty(); });
    return found != fields.end() ? *found : QString{};
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
