#import <UserNotifications/UserNotifications.h>

#include "Notifications.h"

#include <QCoreApplication>
#include <QMetaObject>

#include <utility>

namespace
{
std::function<void(const QString &)> &activationHandler()
{
    static std::function<void(const QString &)> handler;
    return handler;
}

NSString *const kSessionKey = @"konsollSession";
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
    if ([response.actionIdentifier isEqualToString:UNNotificationDefaultActionIdentifier] && token != nil) {
        const QString session = QString::fromNSString(token);
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [session]() {
                if (activationHandler()) {
                    activationHandler()(session);
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
        [center requestAuthorizationWithOptions:(UNAuthorizationOptionAlert | UNAuthorizationOptionSound)
                              completionHandler:^(BOOL, NSError *) {
                              }];
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

void postNotification(const QString &title, const QString &body, const QString &sessionToken)
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
    }
    NSString *const identifier = [[NSUUID UUID] UUIDString];
    UNNotificationRequest *const request = [UNNotificationRequest requestWithIdentifier:identifier
                                                                                content:content
                                                                                trigger:nil];
    [center addNotificationRequest:request withCompletionHandler:nil];
    [content release];
}

} // namespace konsoll
