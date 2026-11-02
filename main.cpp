#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QTimer>

#include "AppQuitGuard.h"

int main(int argc, char *argv[])
{
    QCoreApplication::setOrganizationDomain(QStringLiteral("com.github.rask-project"));
    QCoreApplication::setOrganizationName(QStringLiteral("rask-project"));
    QCoreApplication::setApplicationName(QStringLiteral("Konsoll"));
    QCoreApplication::setApplicationVersion(
        QStringLiteral(KONSOLL_VERSION) + QStringLiteral(" (") + QStringLiteral(KONSOLL_GIT_COMMIT)
        + QStringLiteral(")")
    );

    QGuiApplication app(argc, argv);
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
