#pragma once

#include <QString>

namespace konsoll
{

// Posts a native macOS notification (UserNotifications) for a terminal
// program's OSC 9 / OSC 777 request. Authorization is asked on first use;
// banners show even while Konsoll is the active app.
void postNotification(const QString &title, const QString &body);

} // namespace konsoll
