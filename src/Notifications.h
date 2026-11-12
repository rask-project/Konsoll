#pragma once

#include <QString>

#include <functional>

namespace konsoll
{

// Posts a native macOS notification (UserNotifications) for a terminal
// program's OSC 9 / OSC 777 request. Authorization is asked on first use;
// banners show even while Konsoll is the active app.
// `sessionToken` names the session the notification is about: clicking it
// calls the activation handler with it.
void postNotification(const QString &title, const QString &body, const QString &sessionToken = {});

// Called on the GUI thread when the user clicks a notification that names a
// session (main() brings that session's tab forward).
void setNotificationActivationHandler(std::function<void(const QString &sessionToken)> handler);

} // namespace konsoll
