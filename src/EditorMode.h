#pragma once

#include <QObject>
#include <QStringList>
#include <QtQml>

#include <chrono>
#include <cstdint>
#include <span>

namespace konsoll
{

// Konsoll as a Neovim app: the "Neovim (Konsoll)" helper bundle
// (Contents/Helpers, its own bundle id, name, icon and document types) runs
// the same executable, which then opens a window per file or folder running
// nvim, with no tabs, splits or shell (EditorMain.qml, EditorWindow.qml).
// Settings are Konsoll's: QSettings is keyed by the application name, not the
// bundle id. Konsoll itself registers the helper with Launch Services so that
// it is offered in Open With.
class EditorMode final : public QObject
{
    Q_OBJECT
    QML_NAMED_ELEMENT(EditorMode)
    QML_SINGLETON

    Q_PROPERTY(bool active READ active CONSTANT)

public:
    static EditorMode &shared();
    static EditorMode *create(QQmlEngine *qmlEngine, QJSEngine *jsEngine);

    ~EditorMode() override = default;
    Q_DISABLE_COPY_MOVE(EditorMode)

    // Decided once in main(), before anything reads it: the helper's bundle
    // id (….nvim), or `--editor [PATH…]` on the command line, whose paths are
    // opened once the app has launched.
    static void detect(std::span<char *> arguments);
    static bool isActive();
    bool active() const { return isActive(); }

    // Watches for files and folders opened with the app and for the end of
    // the launch. Called in main() before the event loop starts: a launch for
    // a file delivers it as the app finishes launching.
    void install();

    // The command an editor window runs for `path` (a file, a folder, or
    // empty for an empty buffer): nvim in the user's login shell (its PATH),
    // or a message when nvim is not installed.
    Q_INVOKABLE static QString commandFor(const QString &path);
    // The directory nvim starts in for `path`: the folder itself, or the
    // folder holding a file (the home folder for an empty buffer).
    Q_INVOKABLE static QString directoryFor(const QString &path);
    // What a window shows before nvim sets a title: the file or folder name.
    Q_INVOKABLE static QString titleFor(const QString &path);
    // Keys asking nvim to quit (`:confirm qa` from Normal mode): nvim asks
    // about unsaved buffers, and the window closes when it exits.
    Q_INVOKABLE static QString quitKeys();
    // File ▸ Open…: files and folders chosen in the native panel.
    Q_INVOKABLE static QStringList choosePaths();
    // Makes the editor the default app for the text types its bundle declares,
    // all at once and without a confirmation per type. False when none could
    // be set (a build without document types: KONSOLL_FINDER_INTEGRATION).
    Q_INVOKABLE static bool makeDefaultEditor();

    // The app quit (Cmd+Q, the Dock): true when an earlier request is still
    // pending (nvim did not quit), so the quit goes ahead after a
    // confirmation; otherwise asks every window to quit its nvim.
    bool requestQuit();

    // Konsoll (not the editor) registers its helper bundle with Launch
    // Services, off the main thread (not in builds without
    // KONSOLL_FINDER_INTEGRATION, which declare no document types).
    static void registerHelperApplication();

signals:
    // A file or folder opened with the app (Finder, `open -a`, the Dock).
    void openRequested(const QString &path);
    // The app finished launching; `openWindow` is false when it was launched
    // to open files, which bring their own windows.
    void launched(bool openWindow);
    // The app is quitting: every window asks its nvim to quit.
    void quitRequested();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    EditorMode() = default;
    void finishedLaunching(bool defaultLaunch);

    QStringList m_launchPaths;
    std::chrono::steady_clock::time_point m_quitRequestedAt;
    std::uint64_t m_flags = 0;
};

} // namespace konsoll
