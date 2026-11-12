#include "McpServer.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>

#include <algorithm>
#include <array>
#include <cstdio>
#include <iostream>
#include <string>
#include <string_view>

namespace konsoll
{

namespace
{

using namespace std::string_view_literals;

constexpr int kSocketTimeoutMs = 5000;
// A reply may wait for the user (read_pane asks first).
constexpr int kReplyTimeoutMs = 10 * 60 * 1000;
// The newest MCP revision this server follows; an older one a client asks
// for is accepted as is (the methods used here did not change).
constexpr auto kProtocolVersions = std::to_array({"2025-11-25"sv, "2025-06-18"sv, "2025-03-26"sv, "2024-11-05"sv});

QJsonObject stringProperty(const QString &description)
{
    return {{QStringLiteral("type"), QStringLiteral("string")}, {QStringLiteral("description"), description}};
}

QJsonObject
tool(const QString &name, const QString &description, const QJsonObject &properties, const QJsonArray &required)
{
    return {
        {QStringLiteral("name"), name},
        {QStringLiteral("description"), description},
        {QStringLiteral("inputSchema"),
         QJsonObject{
             {QStringLiteral("type"), QStringLiteral("object")},
             {QStringLiteral("properties"), properties},
             {QStringLiteral("required"), required},
         }},
    };
}

QJsonArray tools()
{
    QJsonObject direction = stringProperty(QStringLiteral("Where the pane opens: right (default) or below."));
    direction.insert(QLatin1String("enum"), QJsonArray{QStringLiteral("right"), QStringLiteral("below")});
    return {
        tool(
            QStringLiteral("open_pane"),
            QStringLiteral("Open a new Konsoll pane next to yours, visible to the user, and run a shell command in it "
                           "(a dev server, a watcher, logs to keep an eye on). The pane stays open after the command "
                           "ends. You cannot read its output: use run_in_background for that."),
            {{QStringLiteral("command"), stringProperty(QStringLiteral("The shell command to run in the new pane."))},
             {QStringLiteral("direction"), direction}},
            {QStringLiteral("command")}
        ),
        tool(
            QStringLiteral("run_in_background"),
            QStringLiteral("Start a shell command as a Konsoll Background Command: it keeps running without a tab, the "
                           "user can watch or stop it in Window > Background Commands, and you can check its state "
                           "and output with background_job_status."),
            {{QStringLiteral("command"), stringProperty(QStringLiteral("The shell command to run."))},
             {QStringLiteral("title"), stringProperty(QStringLiteral("A short name for the user (optional)."))},
             {QStringLiteral("directory"),
              stringProperty(QStringLiteral("Working directory (optional; defaults to yours)."))}},
            {QStringLiteral("command")}
        ),
        tool(
            QStringLiteral("recent_commands"),
            QStringLiteral("The last commands run at a prompt in your own Konsoll pane before you started (the user's "
                           "build, tests, a failing command), each with its exit status and the end of its output. "
                           "Use it when the user refers to something that just happened in their terminal."),
            {{QStringLiteral("count"),
              QJsonObject{
                  {QStringLiteral("type"), QStringLiteral("integer")},
                  {QStringLiteral("description"),
                   QStringLiteral("How many commands, newest last (default 5, at most 20).")},
              }}},
            {}
        ),
        tool(
            QStringLiteral("list_panes"),
            QStringLiteral("The panes of your Konsoll window: their number, tab, the program running and the working "
                           "directory. No output: read_pane reads one with the user's permission."),
            {},
            {}
        ),
        tool(
            QStringLiteral("read_pane"),
            QStringLiteral("Read another pane of your window (a dev server's log, the user's test run): its recent "
                           "commands and the end of its screen. The user is asked first and may refuse; the call waits "
                           "for the answer."),
            {{QStringLiteral("pane"),
              QJsonObject{
                  {QStringLiteral("type"), QStringLiteral("integer")},
                  {QStringLiteral("description"), QStringLiteral("The pane number from list_panes.")},
              }}},
            {QStringLiteral("pane")}
        ),
        tool(
            QStringLiteral("background_job_status"),
            QStringLiteral("The state (running, finished, failed, stopped), exit code and last part of the output of a "
                           "background command you started with run_in_background."),
            {{QStringLiteral("job"),
              QJsonObject{
                  {QStringLiteral("type"), QStringLiteral("integer")},
                  {QStringLiteral("description"), QStringLiteral("The job number run_in_background returned.")},
              }}},
            {QStringLiteral("job")}
        ),
    };
}

QJsonObject result(const QJsonValue &id, const QJsonObject &value)
{
    return {
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), id},
        {QStringLiteral("result"), value}
    };
}

QJsonObject error(const QJsonValue &id, int code, const QString &message)
{
    return {
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), id},
        {QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), code}, {QStringLiteral("message"), message}}},
    };
}

QJsonObject initializeResult(const QJsonObject &params, bool inKonsoll)
{
    const QString requested = params.value(QLatin1String("protocolVersion")).toString();
    const bool known = std::ranges::any_of(kProtocolVersions, [&requested](std::string_view version) {
        return requested == QLatin1StringView(version.data(), static_cast<qsizetype>(version.size()));
    });
    const std::string_view newest = kProtocolVersions.front();
    return {
        {QStringLiteral("protocolVersion"),
         known ? requested : QString::fromLatin1(newest.data(), static_cast<qsizetype>(newest.size()))},
        {QStringLiteral("capabilities"), QJsonObject{{QStringLiteral("tools"), QJsonObject{}}}},
        {QStringLiteral("serverInfo"),
         QJsonObject{
             {QStringLiteral("name"), QStringLiteral("konsoll")},
             {QStringLiteral("version"), QCoreApplication::applicationVersion()},
         }},
        {QStringLiteral("instructions"),
         inKonsoll ? QStringLiteral("You run in a Konsoll terminal pane. Use open_pane for long-running commands the "
                                    "user should see (dev servers, watchers) instead of blocking your own shell. When "
                                    "the user mentions something that happened in their terminal (a failed build, an "
                                    "error in another pane), recent_commands and read_pane show it.")
                   : QStringLiteral("Not running inside Konsoll: no tools are available.")},
    };
}

QJsonObject
callTool(const QJsonObject &params, bool inKonsoll, const std::function<QJsonObject(const QJsonObject &)> &forward)
{
    const QJsonObject reply = inKonsoll ? forward(QJsonObject{
                                              {QStringLiteral("tool"), params.value(QLatin1String("name"))},
                                              {QStringLiteral("arguments"), params.value(QLatin1String("arguments"))},
                                          })
                                        : QJsonObject{
                                              {QStringLiteral("text"), QStringLiteral("Not running inside Konsoll.")},
                                              {QStringLiteral("isError"), true},
                                          };
    return {
        {QStringLiteral("content"),
         QJsonArray{QJsonObject{
             {QStringLiteral("type"), QStringLiteral("text")},
             {QStringLiteral("text"), reply.value(QLatin1String("text")).toString()},
         }}},
        {QStringLiteral("isError"), reply.value(QLatin1String("isError")).toBool()},
    };
}

// Sends one request line to Konsoll and waits for its reply line.
QJsonObject forwardToKonsoll(const QString &socketPath, const QJsonObject &request)
{
    const QJsonObject unavailable{
        {QStringLiteral("text"), QStringLiteral("Konsoll did not answer (was it closed?).")},
        {QStringLiteral("isError"), true},
    };
    QLocalSocket socket;
    socket.connectToServer(socketPath);
    if (!socket.waitForConnected(kSocketTimeoutMs)) {
        return unavailable;
    }
    socket.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
    if (!socket.waitForBytesWritten(kSocketTimeoutMs)) {
        return unavailable;
    }
    while (!socket.canReadLine() && socket.waitForReadyRead(kReplyTimeoutMs)) {
    }
    const QJsonObject reply = QJsonDocument::fromJson(socket.readLine()).object();
    return reply.isEmpty() ? unavailable : reply;
}

} // namespace

QJsonObject handleMcpMessage(
    const QJsonObject &message,
    bool inKonsoll,
    const std::function<QJsonObject(const QJsonObject &)> &forward
)
{
    const QString method = message.value(QLatin1String("method")).toString();
    const QJsonObject params = message.value(QLatin1String("params")).toObject();
    if (!message.contains(QLatin1String("id"))) {
        return {}; // a notification (initialized, cancelled, ...): no reply
    }
    const QJsonValue id = message.value(QLatin1String("id"));
    if (method == QLatin1String("initialize")) {
        return result(id, initializeResult(params, inKonsoll));
    }
    if (method == QLatin1String("ping")) {
        return result(id, QJsonObject{});
    }
    if (method == QLatin1String("tools/list")) {
        return result(id, QJsonObject{{QStringLiteral("tools"), inKonsoll ? tools() : QJsonArray{}}});
    }
    if (method == QLatin1String("tools/call")) {
        return result(id, callTool(params, inKonsoll, forward));
    }
    return error(id, -32601, QStringLiteral("Method not found: %1").arg(method));
}

int runMcpServer()
{
    const QString token = qEnvironmentVariable("KONSOLL_SESSION_TOKEN");
    const QString socketPath = qEnvironmentVariable("KONSOLL_AGENT_SOCKET");
    const bool inKonsoll = !token.isEmpty() && !socketPath.isEmpty();
    const QString cwd = QDir::currentPath();
    const auto forward = [&token, &socketPath, &cwd](const QJsonObject &call) {
        QJsonObject request = call;
        request.insert(QLatin1String("token"), token);
        request.insert(QLatin1String("cwd"), cwd);
        return forwardToKonsoll(socketPath, request);
    };
    std::string line;
    while (std::getline(std::cin, line)) {
        QJsonParseError parseError{};
        const QJsonDocument document = QJsonDocument::fromJson(QByteArray::fromStdString(line), &parseError);
        if (line.find_first_not_of(" \t\r") == std::string::npos) {
            continue;
        }
        const QJsonObject reply = parseError.error == QJsonParseError::NoError && document.isObject()
                                      ? handleMcpMessage(document.object(), inKonsoll, forward)
                                      : error(QJsonValue::Null, -32700, QStringLiteral("Parse error"));
        if (!reply.isEmpty()) {
            std::cout << QJsonDocument(reply).toJson(QJsonDocument::Compact).toStdString() << '\n' << std::flush;
        }
    }
    return 0;
}

} // namespace konsoll
