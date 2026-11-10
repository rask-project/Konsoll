#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QTimer>

#include <span>
#include <string_view>

#include "AppQuitGuard.h"
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

    // Headless multi-session validation (docs/SESSION-THREADING-PLAN.md §8.8):
    // select the offscreen platform before QGuiApplication picks one.
    bool selfTest = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--selftest") {
            selfTest = true;
            break;
        }
    }
    if (selfTest) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }

    QGuiApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(true);
    if (selfTest) {
        return konsoll::runSelfTest();
    }
    QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/assets/konsoll.png")));

    QIcon::setThemeSearchPaths({QStringLiteral(":/icons")});
    QIcon::setThemeName(QStringLiteral("konsoll"));

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
