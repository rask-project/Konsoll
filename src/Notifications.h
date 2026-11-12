#pragma once

#include <QString>

#include <cstdint>
#include <functional>

namespace konsoll
{

// Posts a native macOS notification (UserNotifications) for a terminal
// program's OSC 9 / OSC 777 request. Authorization is asked on first use;
// banners show even while Konsoll is the active app.
// `sessionToken` names the session the notification is about: clicking it
// calls the activation handler with it.
// An Approval notification also offers Allow and Deny (an agent's permission
// prompt), answered through the action handler.
enum class NotificationKind : std::uint8_t {
    Plain,
    Approval
};
void postNotification(
    const QString &title,
    const QString &body,
    const QString &sessionToken = {},
    NotificationKind kind = NotificationKind::Plain
);

// Called on the GUI thread when the user clicks a notification that names a
// session (main() brings that session's tab forward).
void setNotificationActivationHandler(std::function<void(const QString &sessionToken)> handler);
// Called on the GUI thread when the user picks Allow or Deny on an Approval
// notification (Konsoll stays in the background).
void setNotificationApprovalHandler(std::function<void(const QString &sessionToken, bool allow)> handler);

// The count on Konsoll's Dock icon (agents waiting for the user); 0 hides it.
void setDockBadge(int count);

} // namespace konsoll
