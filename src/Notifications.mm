#import <AppKit/AppKit.h>
#import <UserNotifications/UserNotifications.h>

#include "Notifications.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QtLogging>

#include <utility>

namespace
{
std::function<void(const QString &)> &activationHandler()
{
    static std::function<void(const QString &)> handler;
    return handler;
}

std::function<void(const QString &, bool)> &approvalHandler()
{
    static std::function<void(const QString &, bool)> handler;
    return handler;
}

NSString *const kSessionKey = @"konsollSession";
NSString *const kApprovalCategory = @"konsoll.approval";
NSString *const kAllowAction = @"konsoll.allow";
NSString *const kDenyAction = @"konsoll.deny";
} // namespace

// Presents banners while the app is frontmost (the default hides them).
@interface KonsollNotificationDelegate : NSObject <UNUserNotificationCenterDelegate>
@end

@implementation KonsollNotificationDelegate
- (void)userNotificationCenter:(UNUserNotificationCenter *)center
       willPresentNotification:(UNNotification *)notification
         withCompletionHandler:(void (^)(UNNotificationPresentationOptions))completionHandler
{
    (void) center;
    (void) notification;
    completionHandler(UNNotificationPresentationOptionBanner | UNNotificationPresentationOptionSound);
}

// A click on the notification: bring its session forward (on the GUI
// thread; the center may call from another queue).
- (void)userNotificationCenter:(UNUserNotificationCenter *)center
    didReceiveNotificationResponse:(UNNotificationResponse *)response
             withCompletionHandler:(void (^)(void))completionHandler
{
    (void) center;
    NSString *const token = response.notification.request.content.userInfo[kSessionKey];
    NSString *const action = response.actionIdentifier;
    const bool open = [action isEqualToString:UNNotificationDefaultActionIdentifier];
    const bool allow = [action isEqualToString:kAllowAction];
    const bool deny = [action isEqualToString:kDenyAction];
    if (token != nil && (open || allow || deny)) {
        const QString session = QString::fromNSString(token);
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [session, open, allow]() {
                if (open && activationHandler()) {
                    activationHandler()(session);
                } else if (!open && approvalHandler()) {
                    approvalHandler()(session, allow);
                }
            },
            Qt::QueuedConnection
        );
    }
    completionHandler();
}
@end

namespace konsoll
{

namespace
{
UNUserNotificationCenter *preparedCenter()
{
    UNUserNotificationCenter *const center = [UNUserNotificationCenter currentNotificationCenter];
    static const bool prepared = [center]() {
        // The delegate lives for the whole process; the center keeps a weak
        // reference only.
        static KonsollNotificationDelegate *const delegate = [[KonsollNotificationDelegate alloc] init];
        center.delegate = delegate;
        // Allow and Deny answer an agent's permission prompt without bringing
        // Konsoll forward; Deny is marked destructive (red).
        UNNotificationAction *const allowAction =
            [UNNotificationAction actionWithIdentifier:kAllowAction
                                                 title:@"Allow"
                                               options:UNNotificationActionOptionNone];
        UNNotificationAction *const denyAction =
            [UNNotificationAction actionWithIdentifier:kDenyAction
                                                 title:@"Deny"
                                               options:UNNotificationActionOptionDestructive];
        UNNotificationCategory *const approval =
            [UNNotificationCategory categoryWithIdentifier:kApprovalCategory
                                                   actions:@[ allowAction, denyAction ]
                                         intentIdentifiers:@[]
                                                   options:UNNotificationCategoryOptionNone];
        [center setNotificationCategories:[NSSet setWithObject:approval]];
        return true;
    }();
    (void) prepared;
    return center;
}
} // namespace

void setNotificationActivationHandler(std::function<void(const QString &sessionToken)> handler)
{
    activationHandler() = std::move(handler);
}

void setNotificationApprovalHandler(std::function<void(const QString &sessionToken, bool allow)> handler)
{
    approvalHandler() = std::move(handler);
}

void setDockBadge(int count)
{
    NSApp.dockTile.badgeLabel = count > 0 ? [NSString stringWithFormat:@"%d", count] : nil;
}

void postNotification(const QString &title, const QString &body, const QString &sessionToken, NotificationKind kind)
{
    UNUserNotificationCenter *const center = preparedCenter();
    UNMutableNotificationContent *const content = [[UNMutableNotificationContent alloc] init];
    content.title = title.toNSString();
    content.body = body.toNSString();
    content.sound = [UNNotificationSound defaultSound];
    if (!sessionToken.isEmpty()) {
        content.userInfo = @{kSessionKey : sessionToken.toNSString()};
        // Notification Center groups them per session.
        content.threadIdentifier = sessionToken.toNSString();
        if (kind == NotificationKind::Approval) {
            content.categoryIdentifier = kApprovalCategory;
        }
    }
    NSString *const identifier = [[NSUUID UUID] UUIDString];
    UNNotificationRequest *const request = [UNNotificationRequest requestWithIdentifier:identifier
                                                                                content:content
                                                                                trigger:nil];
    // Asking each time is cheap once the user has answered, and the first
    // notification waits for that answer instead of being dropped.
    [request retain];
    [center
        requestAuthorizationWithOptions:(UNAuthorizationOptionAlert | UNAuthorizationOptionSound)
                      completionHandler:^(BOOL granted, NSError *authorizationError) {
                        if (granted) {
                            [center
                                addNotificationRequest:request
                                 withCompletionHandler:^(NSError *addError) {
                                   if (addError != nil) {
                                       qWarning("Notification not shown: %s", addError.localizedDescription.UTF8String);
                                   }
                                 }];
                        } else {
                            // Code 1 without a prompt: the bundle's Info.plist is not bound
                            // to its signature (a linker-signed build).
                            qWarning(
                                "Notifications not allowed: %s",
                                authorizationError != nil ? authorizationError.description.UTF8String
                                                          : "turned off in System Settings > Notifications"
                            );
                        }
                        [request release];
                      }];
    [content release];
}

} // namespace konsoll
