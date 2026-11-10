#pragma once

#include <span>

namespace konsoll
{

// Headless parser harness: `konsoll --screen-dump ROWS COLS [--attrs]` feeds
// stdin to a Terminal and prints the resulting screen, cursor and the replies
// the terminal queued. Used by tools/vtdrive.py to run vttest, Neovim and
// other programs against the real parser without a window.
int runScreenDump(std::span<char *> args);

} // namespace konsoll
