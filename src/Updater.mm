#import <AppKit/AppKit.h>
#import <Sparkle/Sparkle.h>

#include "Updater.h"

#include <QSettings>

namespace
{

const QString kBetaKey = QStringLiteral("Updates/betaUpdates");

// "Check for Updates…" goes in the app menu after the About items. Qt only
// moves the items it recognizes (About, Settings, Quit) there; any other item
// of a QML Menu titled after the app would show up in a second app-named
// menu. The app menu is Qt's and stays across windows, so it is added once.
void addCheckForUpdatesItem(SPUStandardUpdaterController *controller)
{
    NSMenu *const appMenu = [[[NSApp mainMenu] itemAtIndex:0] submenu];
    if (appMenu == nil) {
        return;
    }
    const NSUInteger lastAbout =
        [appMenu.itemArray indexOfObjectWithOptions:NSEnumerationReverse
                                        passingTest:^BOOL(NSMenuItem *candidate, NSUInteger, BOOL *) {
                                          return [candidate.title hasPrefix:@"About"];
                                        }];
    const NSInteger index = lastAbout == NSNotFound ? 0 : static_cast<NSInteger>(lastAbout) + 1;
    NSMenuItem *const item = [[NSMenuItem alloc] initWithTitle:@"Check for Updates…"
                                                        action:@selector(checkForUpdates:)
                                                 keyEquivalent:@""];
    // The controller enables it only while a check can start.
    item.target = controller;
    [appMenu insertItem:item atIndex:index];
    [item release];
}

} // namespace

// Sparkle asks it which channels to look in, and reports the end of each
// check so Settings shows when it last ran.
@interface KonsollUpdaterDelegate : NSObject <SPUUpdaterDelegate>
@end

@implementation KonsollUpdaterDelegate

- (NSSet<NSString *> *)allowedChannelsForUpdater:(SPUUpdater *)updater
{
    Q_UNUSED(updater);
    // The default channel (stable releases) is always included.
    return konsoll::Updater::betaUpdates() ? [NSSet setWithObject:@"beta"] : [NSSet set];
}

- (void)updater:(SPUUpdater *)updater
    didFinishUpdateCycleForUpdateCheck:(SPUUpdateCheck)updateCheck
                                 error:(nullable NSError *)error
{
    Q_UNUSED(updater);
    Q_UNUSED(updateCheck);
    Q_UNUSED(error);
    emit konsoll::Updater::shared().lastCheckChanged();
}

@end

namespace konsoll
{

// Both live as long as the process (Sparkle keeps the delegate weak): they
// are never released, as AppKit may still call them while the app quits.
struct Updater::Native {
    SPUStandardUpdaterController *controller = nil;
    KonsollUpdaterDelegate *delegate = nil;
};

Updater::Updater() : m_native(std::make_unique<Native>()) {}

Updater::~Updater() = default;

Updater &Updater::shared()
{
    static Updater instance;
    return instance;
}

Updater *Updater::create(QQmlEngine *qmlEngine, QJSEngine *jsEngine)
{
    Q_UNUSED(qmlEngine);
    Q_UNUSED(jsEngine);
    Updater &updater = shared();
    // A function-local static: the engine must not delete it.
    QJSEngine::setObjectOwnership(&updater, QJSEngine::CppOwnership);
    return &updater;
}

void Updater::start()
{
    if (!available() || m_native->controller != nil) {
        return;
    }
    m_native->delegate = [[KonsollUpdaterDelegate alloc] init];
    m_native->controller = [[SPUStandardUpdaterController alloc] initWithStartingUpdater:YES
                                                                         updaterDelegate:m_native->delegate
                                                                      userDriverDelegate:nil];
    addCheckForUpdatesItem(m_native->controller);
    emit automaticChecksChanged();
    emit automaticDownloadsChanged();
    emit lastCheckChanged();
}

bool Updater::available() const
{
    NSDictionary *const info = [[NSBundle mainBundle] infoDictionary];
    const id feed = info[@"SUFeedURL"];
    const id key = info[@"SUPublicEDKey"];
    return [feed isKindOfClass:[NSString class]] && [feed length] > 0 && [key isKindOfClass:[NSString class]] &&
           [key length] > 0;
}

bool Updater::automaticChecks() const
{
    return m_native->controller != nil && m_native->controller.updater.automaticallyChecksForUpdates;
}

void Updater::setAutomaticChecks(bool enabled)
{
    if (m_native->controller == nil || enabled == automaticChecks()) {
        return;
    }
    m_native->controller.updater.automaticallyChecksForUpdates = enabled;
    emit automaticChecksChanged();
}

bool Updater::automaticDownloads() const
{
    return m_native->controller != nil && m_native->controller.updater.automaticallyDownloadsUpdates;
}

void Updater::setAutomaticDownloads(bool enabled)
{
    if (m_native->controller == nil || enabled == automaticDownloads()) {
        return;
    }
    m_native->controller.updater.automaticallyDownloadsUpdates = enabled;
    emit automaticDownloadsChanged();
}

bool Updater::betaUpdates()
{
    return QSettings().value(kBetaKey, false).toBool();
}

void Updater::setBetaUpdates(bool enabled)
{
    if (enabled == betaUpdates()) {
        return;
    }
    QSettings().setValue(kBetaKey, enabled);
    // An update found for the old channel must not be offered.
    if (m_native->controller != nil) {
        [m_native->controller.updater resetUpdateCycleAfterShortDelay];
    }
    emit betaUpdatesChanged();
}

QDateTime Updater::lastCheck() const
{
    if (m_native->controller == nil) {
        return {};
    }
    NSDate *const date = m_native->controller.updater.lastUpdateCheckDate;
    return date != nil ? QDateTime::fromNSDate(date) : QDateTime();
}

void Updater::checkForUpdates()
{
    if (m_native->controller != nil) {
        [m_native->controller checkForUpdates:nil];
    }
}

} // namespace konsoll
