#import <UserNotifications/UserNotifications.h>

#include "Notifications.h"

// Presents banners while the app is frontmost (the default hides them).
@interface KonsollNotificationDelegate : NSObject <UNUserNotificationCenterDelegate>
@end

@implementation KonsollNotificationDelegate
- (void)userNotificationCenter:(UNUserNotificationCenter *)center
       willPresentNotification:(UNNotification *)notification
         withCompletionHandler:(void (^)(UNNotificationPresentationOptions))completionHandler
{
    (void)center;
    (void)notification;
    completionHandler(UNNotificationPresentationOptionBanner | UNNotificationPresentationOptionSound);
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
                              completionHandler:^(BOOL, NSError *) {}];
        return true;
    }();
    (void)prepared;
    return center;
}
} // namespace

void postNotification(const QString &title, const QString &body)
{
    UNUserNotificationCenter *const center = preparedCenter();
    UNMutableNotificationContent *const content = [[UNMutableNotificationContent alloc] init];
    content.title = title.toNSString();
    content.body = body.toNSString();
    content.sound = [UNNotificationSound defaultSound];
    NSString *const identifier = [[NSUUID UUID] UUIDString];
    UNNotificationRequest *const request = [UNNotificationRequest requestWithIdentifier:identifier
                                                                                content:content
                                                                                trigger:nil];
    [center addNotificationRequest:request withCompletionHandler:nil];
    [content release];
}

} // namespace konsoll
