#pragma once

#include <QDateTime>
#include <QObject>
#include <QtQml>

#include <memory>

namespace konsoll
{

// Updates through Sparkle: "Check for Updates…" in the app menu and
// Settings ▸ Updates. Sparkle reads the appcast (SUFeedURL in Info.plist),
// shows the release notes, downloads the archive, checks its EdDSA signature
// (SUPublicEDKey) and replaces the app, then relaunches it; installing quits
// through the quit guard like any other quit.
//
// Releases tagged with a suffix (v3.7.0-beta.1) are posted to the "beta"
// channel of the appcast; only users who chose beta updates see them.
class Updater final : public QObject
{
    Q_OBJECT
    QML_NAMED_ELEMENT(Updater)
    QML_SINGLETON

    // False when the build has no feed URL or public key (Debug builds): the
    // menu item and the settings are disabled.
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(bool automaticChecks READ automaticChecks WRITE setAutomaticChecks NOTIFY automaticChecksChanged)
    Q_PROPERTY(bool automaticDownloads READ automaticDownloads WRITE setAutomaticDownloads NOTIFY
                   automaticDownloadsChanged)
    // Also offer pre-releases (the appcast's "beta" channel).
    Q_PROPERTY(bool betaUpdates READ betaUpdates WRITE setBetaUpdates NOTIFY betaUpdatesChanged)
    // When Sparkle last looked for an update; invalid if it never did.
    Q_PROPERTY(QDateTime lastCheck READ lastCheck NOTIFY lastCheckChanged)

public:
    static Updater &shared();
    static Updater *create(QQmlEngine *qmlEngine, QJSEngine *jsEngine);

    ~Updater() override;
    Q_DISABLE_COPY_MOVE(Updater)

    // Starts Sparkle's scheduled checks; called once the app's UI is up.
    void start();

    bool available() const;
    bool automaticChecks() const;
    void setAutomaticChecks(bool enabled);
    bool automaticDownloads() const;
    void setAutomaticDownloads(bool enabled);
    static bool betaUpdates();
    void setBetaUpdates(bool enabled);
    QDateTime lastCheck() const;

    // Checks now and shows the result, even when no update is found.
    Q_INVOKABLE void checkForUpdates();

signals:
    void automaticChecksChanged();
    void automaticDownloadsChanged();
    void betaUpdatesChanged();
    void lastCheckChanged();

private:
    Updater();

    struct Native;
    std::unique_ptr<Native> m_native;
};

} // namespace konsoll
