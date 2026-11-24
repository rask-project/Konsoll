#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QTimer>

#include <memory>
#include <span>
#include <string_view>

#include "AgentCommands.h"
#include "AgentStatus.h"
#include "AppQuitGuard.h"
#include "BackgroundJobs.h"
#include "EditorMode.h"
#include "FileDrag.h"
#include "FinderIntegration.h"
#include "ImagePreview.h"
#include "McpServer.h"
#include "NativeKeyTracker.h"
#include "Notifications.h"
#include "ShellFunctions.h"
#include "SshIntegration.h"
#include "SshTransfers.h"
#include "TerminalBackend.h"
#include "TerminalManager.h"
#include "Updater.h"
#include "WindowRegistry.h"
#include "renderdump.h"
#include "screendump.h"
#include "selftest.h"

namespace
{

// Before the event loop, which delivers what the app was launched to open:
// the editor watches for files and folders opened with it; Konsoll sets its
// icon, installs its Finder Services and offers the editor in Open With.
void installOpenHandlers(bool editor)
{
    if (editor) {
        // The editor keeps its bundle's icon in the Dock (setWindowIcon
        // replaces it).
        QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/assets/konsoll-nvim.png")));
        konsoll::EditorMode::shared().install();
        return;
    }
    QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/assets/konsoll.png")));
    konsoll::FinderIntegration::shared().install();
    konsoll::EditorMode::registerHelperApplication();
}

// Once the windows are up: the quit guard and, for Konsoll, scheduled update
// checks (Sparkle). The editor is updated with Konsoll, whose bundle holds it.
void finishLaunching(bool editor)
{
    konsoll::installQuitGuard();
    if (!editor) {
        konsoll::Updater::shared().start();
    }
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication::setOrganizationDomain(QStringLiteral("com.github.rask-project"));
    QCoreApplication::setOrganizationName(QStringLiteral("rask-project"));
    QCoreApplication::setApplicationName(QStringLiteral("Konsoll"));
    QCoreApplication::setApplicationVersion(
        QStringLiteral(KONSOLL_VERSION) + QStringLiteral(" (") + QStringLiteral(KONSOLL_GIT_COMMIT) +
        QStringLiteral(")")
    );

    // Headless parser harness (tools/vtdrive.py): no Qt application needed.
    if (argc > 1 && std::string_view(argv[1]) == "--screen-dump") {
        return konsoll::runScreenDump(std::span(argv, static_cast<std::size_t>(argc)));
    }

    // ssh's SSH_ASKPASS for a file transfer's connection (SshTransfers.h):
    // the question is answered in the running app.
    if (argc == 2 && qEnvironmentVariableIsSet("KONSOLL_ASKPASS_TOKEN")) {
        const QCoreApplication askpass(argc, argv);
        return konsoll::runSshAskpass(QString::fromLocal8Bit(argv[1]));
    }

    // MCP server an agent starts (konsoll --mcp): stdio, no GUI.
    if (argc > 1 && std::string_view(argv[1]) == "--mcp") {
        const QCoreApplication mcp(argc, argv);
        return konsoll::runMcpServer();
    }

    // Headless multi-session validation (konsoll --selftest).
    bool selfTest = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--selftest") {
            selfTest = true;
            break;
        }
    }
    const bool renderDump = argc > 1 && std::string_view(argv[1]) == "--render-dump";

    // Neovim (Konsoll): the helper bundle, or --editor.
    konsoll::EditorMode::detect(std::span(argv, static_cast<std::size_t>(argc)));
    const bool editor = konsoll::EditorMode::isActive();

    QGuiApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(true);
    if (selfTest) {
        return konsoll::runSelfTest();
    }
    if (renderDump) {
        return konsoll::runRenderDump(std::span(argv, static_cast<std::size_t>(argc)));
    }

    QIcon::setThemeSearchPaths({QStringLiteral(":/icons")});
    QIcon::setThemeName(QStringLiteral("konsoll"));

    // Native key codes for the kitty protocol's base layout key.
    konsoll::NativeKeyTracker keyTracker;
    QGuiApplication::instance()->installEventFilter(&keyTracker);
    // Cmd+drag of a file link starts a native drag from the mouse event.
    konsoll::trackMouseForFileDrags();

    // The functions defined in Konsoll, for the shells about to start.
    if (const auto installed = konsoll::ShellFunctions::installScripts(); !installed) {
        qWarning("Konsoll: %s", qUtf8Printable(installed.error()));
    }
    // What ssh brings to a host (konsoll-ssh.sh), for this version.
    if (const auto written = konsoll::writeSshIntegration(); !written) {
        qWarning("Konsoll: %s", qUtf8Printable(written.error()));
    }

    // A clicked notification about a session (an agent waiting, a program's
    // OSC 9) shows that session's tab.
    konsoll::setNotificationActivationHandler([](const QString &token) {
        konsoll::WindowRegistry::instance().activateSessionWithToken(token);
    });
    // Allow / Deny on a permission notification answer the agent's prompt in
    // its pane without bringing Konsoll forward.
    konsoll::setNotificationApprovalHandler([](const QString &token, bool allow) {
        if (konsoll::TerminalManager *manager = konsoll::WindowRegistry::instance().managerWithToken(token)) {
            if (konsoll::TerminalBackend *session = manager->sessionWithToken(token)) {
                session->answerApproval(allow);
            }
        }
    });

    // Declared before the engine so it outlives every QML view of its jobs.
    const auto backgroundJobs = std::make_unique<konsoll::BackgroundJobs>();
    QObject::connect(
        backgroundJobs.get(),
        &konsoll::BackgroundJobs::jobEnded,
        [](const QString &title, const QString &message, bool succeeded) {
            konsoll::postNotification(
                succeeded ? QObject::tr("Background command finished") : QObject::tr("Background command failed"),
                QStringLiteral("%1 — %2").arg(title, message)
            );
        }
    );
    // Tool calls from agents' MCP servers (konsoll --mcp).
    konsoll::AgentCommands agentCommands(backgroundJobs.get());
    konsoll::AgentStatusServer::instance().setRequestHandler(
        [&agentCommands](const QJsonObject &request, const konsoll::AgentStatusServer::Reply &reply) {
            if (konsoll::SshAskpass::isRequest(request)) {
                konsoll::SshAskpass::shared().handle(request, reply);
                return;
            }
            agentCommands.handle(request, reply);
        }
    );
    installOpenHandlers(editor);
    QQmlApplicationEngine engine;
    // The engine owns its image providers.
    engine.addImageProvider(
        QString::fromLatin1(konsoll::kImagePreviewProvider),
        std::make_unique<konsoll::ImagePreviewProvider>().release()
    );

    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection
    );
    engine.loadFromModule("konsoll", editor ? "EditorMain" : "Main");
    QTimer::singleShot(0, [editor] { finishLaunching(editor); });

    const int exitCode = QGuiApplication::exec();
    // The socket server outlives main()'s locals.
    konsoll::AgentStatusServer::instance().setRequestHandler({});
    return exitCode;
}
