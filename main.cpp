#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QTimer>

#include <memory>
#include <span>
#include <string_view>

#include "AppQuitGuard.h"
#include "BackgroundJobs.h"
#include "NativeKeyTracker.h"
#include "Notifications.h"
#include "ShellFunctions.h"
#include "renderdump.h"
#include "screendump.h"
#include "selftest.h"

int main(int argc, char *argv[])
{
    QCoreApplication::setOrganizationDomain(QStringLiteral("com.github.rask-project"));
    QCoreApplication::setOrganizationName(QStringLiteral("rask-project"));
    QCoreApplication::setApplicationName(QStringLiteral("Konsoll"));
    QCoreApplication::setApplicationVersion(
        QStringLiteral(KONSOLL_VERSION) + QStringLiteral(" (") + QStringLiteral(KONSOLL_GIT_COMMIT)
        + QStringLiteral(")")
    );

    // Headless parser harness (tools/vtdrive.py): no Qt application needed.
    if (argc > 1 && std::string_view(argv[1]) == "--screen-dump") {
        return konsoll::runScreenDump(std::span(argv, static_cast<std::size_t>(argc)));
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

    QGuiApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(true);
    if (selfTest) {
        return konsoll::runSelfTest();
    }
    if (renderDump) {
        return konsoll::runRenderDump(std::span(argv, static_cast<std::size_t>(argc)));
    }
    QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/assets/konsoll.png")));

    QIcon::setThemeSearchPaths({QStringLiteral(":/icons")});
    QIcon::setThemeName(QStringLiteral("konsoll"));

    // Native key codes for the kitty protocol's base layout key.
    konsoll::NativeKeyTracker keyTracker;
    QGuiApplication::instance()->installEventFilter(&keyTracker);

    // The functions defined in Konsoll, for the shells about to start.
    if (const auto installed = konsoll::ShellFunctions::installScripts(); !installed) {
        qWarning("Konsoll: %s", qUtf8Printable(installed.error()));
    }

    // Declared before the engine so it outlives every QML view of its jobs.
    const auto backgroundJobs = std::make_unique<konsoll::BackgroundJobs>();
    QObject::connect(backgroundJobs.get(), &konsoll::BackgroundJobs::jobEnded,
                     [](const QString &title, const QString &message, bool succeeded) {
                         konsoll::postNotification(succeeded ? QObject::tr("Background command finished")
                                                             : QObject::tr("Background command failed"),
                                                   QStringLiteral("%1 — %2").arg(title, message));
                     });
    QQmlApplicationEngine engine;

    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection
    );
    engine.loadFromModule("konsoll", "Main");
    QTimer::singleShot(0, [] { konsoll::installQuitGuard(); });

    return QGuiApplication::exec();
}
