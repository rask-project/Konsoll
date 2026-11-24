#import <AppKit/AppKit.h>
#import <CoreServices/CoreServices.h>

#include "EditorMode.h"

#include <QDir>
#include <QFileInfo>
#include <QFileOpenEvent>
#include <QGuiApplication>
#include <QUrl>

#include <algorithm>
#include <ranges>
#include <string_view>

namespace
{

namespace EditorFlag
{
// Running as the editor (the helper bundle, or --editor).
inline constexpr std::uint64_t Active = 1u << 0u;
} // namespace EditorFlag

// A second quit within this long of the first, with nvim still running,
// stops waiting for it.
constexpr std::chrono::seconds quitRequestTimeout{3};

// `text` as one word for sh, zsh, bash and fish: single quotes, with each
// quote closed, escaped and reopened ('\'' reads the same in all four).
QString shellQuoted(const QString &text)
{
    QString quoted = text;
    quoted.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + quoted + QLatin1Char('\'');
}

// Opens files of type `identifier` with the app `bundleIdentifier` (every
// role, so a double-click opens them there); false when Launch Services
// refuses (a type the app does not declare). NSWorkspace's
// setDefaultApplicationAtURL:toOpenContentType: would ask the user to
// confirm each type; this call does not, and takes effect a few seconds later.
bool setDefaultApplication(NSString *bundleIdentifier, NSString *identifier)
{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    const OSStatus status = LSSetDefaultRoleHandlerForContentType(
        reinterpret_cast<CFStringRef>(identifier),
        kLSRolesAll,
        reinterpret_cast<CFStringRef>(bundleIdentifier)
    );
#pragma clang diagnostic pop
    if (status != noErr) {
        qWarning("Konsoll: could not open %s with the editor (%d)", identifier.UTF8String, static_cast<int>(status));
    }
    return status == noErr;
}

} // namespace

namespace konsoll
{

EditorMode &EditorMode::shared()
{
    static EditorMode instance;
    return instance;
}

EditorMode *EditorMode::create(QQmlEngine *qmlEngine, QJSEngine *jsEngine)
{
    Q_UNUSED(qmlEngine);
    Q_UNUSED(jsEngine);
    EditorMode &mode = shared();
    // A function-local static: the engine must not delete it.
    QJSEngine::setObjectOwnership(&mode, QJSEngine::CppOwnership);
    return &mode;
}

void EditorMode::detect(std::span<char *> arguments)
{
    EditorMode &mode = shared();
    NSString *const identifier = [[NSBundle mainBundle] bundleIdentifier];
    if (identifier != nil && [identifier hasSuffix:@".nvim"]) {
        mode.m_flags |= EditorFlag::Active;
    }
    if (arguments.size() < 2) {
        return;
    }
    const auto options = arguments.subspan(1);
    const auto flag = std::ranges::find_if(options, [](const char *argument) {
        return argument != nullptr && std::string_view(argument) == "--editor";
    });
    if (flag == options.end()) {
        return;
    }
    mode.m_flags |= EditorFlag::Active;
    std::ranges::for_each(std::ranges::subrange(std::next(flag), options.end()), [&mode](const char *argument) {
        if (argument != nullptr) {
            mode.m_launchPaths.append(QFileInfo(QString::fromLocal8Bit(argument)).absoluteFilePath());
        }
    });
}

bool EditorMode::isActive()
{
    return (shared().m_flags & EditorFlag::Active) != 0u;
}

void EditorMode::install()
{
    // The notification center keeps the observer for the app's lifetime.
    [[NSNotificationCenter defaultCenter]
        addObserverForName:NSApplicationDidFinishLaunchingNotification
                    object:NSApp
                     queue:nil
                usingBlock:^(NSNotification *notification) {
                  NSNumber *const isDefault = notification.userInfo[NSApplicationLaunchIsDefaultLaunchKey];
                  EditorMode::shared().finishedLaunching(isDefault == nil || isDefault.boolValue);
                }];
    QGuiApplication::instance()->installEventFilter(this);
}

void EditorMode::finishedLaunching(bool defaultLaunch)
{
    emit launched(defaultLaunch && m_launchPaths.isEmpty());
    const QStringList paths = std::exchange(m_launchPaths, {});
    std::ranges::for_each(paths, [this](const QString &path) { emit openRequested(path); });
}

bool EditorMode::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() != QEvent::FileOpen) {
        return QObject::eventFilter(watched, event);
    }
    const QUrl url = static_cast<const QFileOpenEvent *>(event)->url();
    if (!url.isLocalFile()) {
        return QObject::eventFilter(watched, event);
    }
    emit openRequested(url.toLocalFile());
    return true;
}

QString EditorMode::commandFor(const QString &path)
{
    const QString target = path.isEmpty() ? QString{} : QStringLiteral(" -- ") + shellQuoted(path);
    // The login shell's rc files put nvim on the PATH (an app opened from the
    // Finder only gets the system's). Without nvim, the window says so and
    // closes on Return instead of vanishing.
    return QStringLiteral("command -v nvim >/dev/null 2>&1 && exec nvim") + target +
           QStringLiteral("; printf '%s\\n' 'Neovim (nvim) was not found in your PATH.' "
                          "'Install it (brew install neovim) and open the file again.' '' 'Press Return to close.'; "
                          "read reply");
}

QString EditorMode::directoryFor(const QString &path)
{
    if (path.isEmpty()) {
        return QDir::homePath();
    }
    const QFileInfo info(path);
    if (info.isDir()) {
        return info.absoluteFilePath();
    }
    // A file, or one about to be created in an existing folder.
    const QString folder = info.absolutePath();
    return QFileInfo(folder).isDir() ? folder : QDir::homePath();
}

QString EditorMode::titleFor(const QString &path)
{
    // A folder from the Finder ends with a slash.
    const QString name = QFileInfo(QDir::cleanPath(path)).fileName();
    return name.isEmpty() ? QStringLiteral("Neovim") : name;
}

QString EditorMode::quitKeys()
{
    // Ctrl+\ Ctrl+N reaches Normal mode from any mode (insert, command line,
    // a :terminal buffer).
    return QStringLiteral("\x1c\x0e:confirm qa\r");
}

QStringList EditorMode::choosePaths()
{
    NSOpenPanel *const panel = [NSOpenPanel openPanel];
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = YES;
    panel.allowsMultipleSelection = YES;
    panel.prompt = @"Open";
    if ([panel runModal] != NSModalResponseOK) {
        return {};
    }
    __block QStringList paths;
    [panel.URLs enumerateObjectsUsingBlock:^(NSURL *url, NSUInteger index, BOOL *stop) {
      Q_UNUSED(index);
      Q_UNUSED(stop);
      paths.append(QString::fromNSString(url.path));
    }];
    return paths;
}

bool EditorMode::makeDefaultEditor()
{
    // The types of the bundle's "Text" document type (EditorInfo.plist.in):
    // public.text (HTML, RTF…) and folders keep their apps.
    NSString *const bundleIdentifier = [[NSBundle mainBundle] bundleIdentifier];
    NSArray *const documentTypes = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleDocumentTypes"];
    __block bool requested = false;
    [documentTypes enumerateObjectsUsingBlock:^(NSDictionary *documentType, NSUInteger index, BOOL *stop) {
      Q_UNUSED(index);
      Q_UNUSED(stop);
      if (![documentType[@"CFBundleTypeName"] isEqualToString:@"Text"]) {
          return;
      }
      NSArray<NSString *> *const identifiers = documentType[@"LSItemContentTypes"];
      [identifiers enumerateObjectsUsingBlock:^(NSString *identifier, NSUInteger typeIndex, BOOL *typeStop) {
        Q_UNUSED(typeIndex);
        Q_UNUSED(typeStop);
        if (setDefaultApplication(bundleIdentifier, identifier)) {
            requested = true;
        }
      }];
    }];
    return requested;
}

bool EditorMode::requestQuit()
{
    const auto now = std::chrono::steady_clock::now();
    if (now - m_quitRequestedAt < quitRequestTimeout) {
        return true;
    }
    m_quitRequestedAt = now;
    emit quitRequested();
    return false;
}

void EditorMode::registerHelperApplication()
{
    // A build that declares nothing for the Finder offers nothing in Open With.
    if constexpr (KONSOLL_FINDER_INTEGRATION == 0) {
        return;
    }
    NSURL *const helperURL = [[[NSBundle mainBundle] bundleURL]
        URLByAppendingPathComponent:@"Contents/Helpers/" KONSOLL_EDITOR_BUNDLE_NAME ".app"
                        isDirectory:YES];
    if (![[NSFileManager defaultManager] fileExistsAtPath:helperURL.path]) {
        return;
    }
    // Launch Services answers over IPC; the block keeps the URL.
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
      // NSURL and CFURL are toll-free bridged.
      const OSStatus status = LSRegisterURL(reinterpret_cast<CFURLRef>(helperURL), static_cast<Boolean>(false));
      if (status != noErr) {
          qWarning("Konsoll: could not register the editor app (%d)", static_cast<int>(status));
      }
    });
}

} // namespace konsoll
