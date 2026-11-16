#pragma once

#include <QObject>
#include <QtQml>

#include <cstdint>

namespace konsoll
{

// Opens Konsoll at a folder from the Finder:
// - the "New Konsoll Tab Here" / "New Konsoll Window Here" Services
//   (NSServices in Info.plist), on a folder's context menu and in
//   Services ▸ of any app that sends a folder or a path;
// - a folder opened with the app (dropped on its Dock icon, `open -a Konsoll
//   DIR`), which Qt delivers as a QFileOpenEvent (CFBundleDocumentTypes
//   declares public.folder).
// Main.qml answers openRequested with a tab (or a window) in that folder.
class FinderIntegration final : public QObject
{
    Q_OBJECT
    QML_NAMED_ELEMENT(FinderIntegration)
    QML_SINGLETON

public:
    static FinderIntegration &shared();
    static FinderIntegration *create(QQmlEngine *qmlEngine, QJSEngine *jsEngine);

    ~FinderIntegration() override = default;
    Q_DISABLE_COPY_MOVE(FinderIntegration)

    // Registers the Services provider and watches for opened folders. Called
    // in main() before the event loop starts: a launch for a folder delivers
    // it as the app finishes launching.
    void install();

    // Opens `path` (a folder, or the folder of a file); `newWindow` asks for a
    // window of its own instead of a tab in the last active window.
    void request(const QString &path, bool newWindow);
    // The app finished launching; `defaultLaunch` is false when it was
    // launched to open a folder or run a Service.
    void finishedLaunching(bool defaultLaunch);

signals:
    // `atLaunch`: the app was launched for this folder, so it replaces the
    // untouched tab of the window opened at launch.
    void openRequested(const QString &directory, bool newWindow, bool atLaunch);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    FinderIntegration() = default;

    std::uint64_t m_flags = 0;
};

} // namespace konsoll
