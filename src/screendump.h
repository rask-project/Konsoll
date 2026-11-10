#pragma once

#include <QByteArray>

#include <span>

namespace konsoll
{

// Headless parser harness: `konsoll --screen-dump ROWS COLS [--attrs]` feeds
// stdin to a Terminal and prints the resulting screen, cursor and the replies
// the terminal queued. Used by tools/vtdrive.py to run vttest, Neovim and
// other programs against the real parser without a window.
int runScreenDump(std::span<char *> args);

class Terminal;

// The screen, cursor, image placements and queued replies of `term` as text
// (the --screen-dump format; with `attrs` a style line under each row).
// Takes the replies. tests/fixtures/*.expected hold this format.
QByteArray dumpTerminal(Terminal &term, bool attrs);

} // namespace konsoll
