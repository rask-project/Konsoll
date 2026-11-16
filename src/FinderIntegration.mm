#import <AppKit/AppKit.h>

#include "FinderIntegration.h"

#include <QDir>
#include <QFileInfo>
#include <QFileOpenEvent>
#include <QGuiApplication>
#include <QTimer>
#include <QUrl>

#include <chrono>
#include <optional>

namespace
{

namespace LaunchFlag
{
// The app has finished launching (NSApplicationDidFinishLaunchingNotification).
inline constexpr std::uint64_t Launched = 1u << 0u;
// It was launched to open a folder or run a Service, not by the user alone.
inline constexpr std::uint64_t LaunchedToOpen = 1u << 1u;
// The launch's folder has been opened: later ones get tabs of their own.
inline constexpr std::uint64_t Claimed = 1u << 2u;
} // namespace LaunchFlag

// How long after launching a folder still counts as the one the app was
// launched for (a Service request comes once the app is up).
constexpr std::chrono::seconds launchRequestWindow{10};

// The folder to open for `path`: itself, or the folder holding a file. A path
// sent as text may start with ~ or be a file:// URL.
std::optional<QString> folderFor(const QString &path)
{
    QString candidate = path.trimmed();
    if (candidate.startsWith(QStringLiteral("file://"))) {
        candidate = QUrl(candidate).toLocalFile();
    } else if (candidate == QStringLiteral("~") || candidate.startsWith(QStringLiteral("~/"))) {
        candidate = QDir::homePath() + candidate.mid(1);
    }
    if (candidate.isEmpty()) {
        return std::nullopt;
    }
    const QFileInfo info(candidate);
    if (info.isDir()) {
        return info.absoluteFilePath();
    }
    if (info.exists()) {
        return info.absolutePath();
    }
    return std::nullopt;
}

} // namespace

// The Services provider: NSMessage "openFolder" in Info.plist, NSUserData
// "window" for the new-window item.
@interface KonsollServicesProvider : NSObject
- (void)openFolder:(NSPasteboard *)pasteboard userData:(NSString *)userData error:(NSString **)error;
@end

@implementation KonsollServicesProvider

- (void)openFolder:(NSPasteboard *)pasteboard userData:(NSString *)userData error:(NSString **)error
{
    Q_UNUSED(error);
    const bool newWindow = [userData isEqualToString:@"window"];
    NSArray<NSURL *> *const urls = [pasteboard readObjectsForClasses:@[ [NSURL class] ]
                                                             options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
    if (urls.count > 0) {
        [urls enumerateObjectsUsingBlock:^(NSURL *url, NSUInteger index, BOOL *stop) {
          Q_UNUSED(index);
          Q_UNUSED(stop);
          konsoll::FinderIntegration::shared().request(QString::fromNSString(url.path), newWindow);
        }];
        return;
    }
    // A path selected as text (NSRequiredContext FilePath).
    if (NSString *const text = [pasteboard stringForType:NSPasteboardTypeString]) {
        konsoll::FinderIntegration::shared().request(QString::fromNSString(text), newWindow);
    }
}

@end

namespace konsoll
{

FinderIntegration &FinderIntegration::shared()
{
    static FinderIntegration instance;
    return instance;
}

FinderIntegration *FinderIntegration::create(QQmlEngine *qmlEngine, QJSEngine *jsEngine)
{
    Q_UNUSED(qmlEngine);
    Q_UNUSED(jsEngine);
    FinderIntegration &integration = shared();
    // A function-local static: the engine must not delete it.
    QJSEngine::setObjectOwnership(&integration, QJSEngine::CppOwnership);
    return &integration;
}

void FinderIntegration::install()
{
    // NSApp keeps the provider (a strong property).
    KonsollServicesProvider *const provider = [[KonsollServicesProvider alloc] init];
    [NSApp setServicesProvider:provider];
    [provider release];
    NSUpdateDynamicServices();

    // The notification center keeps the observer for the app's lifetime.
    [[NSNotificationCenter defaultCenter]
        addObserverForName:NSApplicationDidFinishLaunchingNotification
                    object:NSApp
                     queue:nil
                usingBlock:^(NSNotification *notification) {
                  NSNumber *const isDefault = notification.userInfo[NSApplicationLaunchIsDefaultLaunchKey];
                  FinderIntegration::shared().finishedLaunching(isDefault == nil || isDefault.boolValue);
                }];

    QGuiApplication::instance()->installEventFilter(this);
}

void FinderIntegration::finishedLaunching(bool defaultLaunch)
{
    m_flags |= LaunchFlag::Launched;
    if (defaultLaunch) {
        return;
    }
    m_flags |= LaunchFlag::LaunchedToOpen;
    QTimer::singleShot(launchRequestWindow, this, [this] { m_flags |= LaunchFlag::Claimed; });
}

void FinderIntegration::request(const QString &path, bool newWindow)
{
    const std::optional<QString> folder = folderFor(path);
    if (!folder) {
        return;
    }
    // Before launching finishes (a folder opened with the app) or right after
    // a launch for a folder (a Service), the first folder is the launch's.
    const bool launching = (m_flags & LaunchFlag::Launched) == 0u;
    const bool launchedToOpen = (m_flags & LaunchFlag::LaunchedToOpen) != 0u;
    const bool atLaunch = (launching || launchedToOpen) && (m_flags & LaunchFlag::Claimed) == 0u;
    if (atLaunch) {
        m_flags |= LaunchFlag::Claimed;
    }
    // A Service runs without making Konsoll the active app.
    [NSApp activate];
    emit openRequested(*folder, newWindow, atLaunch);
}

bool FinderIntegration::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() != QEvent::FileOpen) {
        return QObject::eventFilter(watched, event);
    }
    const QUrl url = static_cast<const QFileOpenEvent *>(event)->url();
    if (!url.isLocalFile() || !QFileInfo(url.toLocalFile()).isDir()) {
        return QObject::eventFilter(watched, event);
    }
    request(url.toLocalFile(), false);
    return true;
}

} // namespace konsoll
