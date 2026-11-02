#pragma once

namespace konsoll
{

// Installs an NSApplicationDelegate that intercepts the macOS app-quit flow
// (Cmd+Q, the window close button, or Cmd+W on the last window) and asks for
// confirmation when a task is in progress.
void installQuitGuard();

} // namespace konsoll
