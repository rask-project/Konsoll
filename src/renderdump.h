#pragma once

#include <span>

namespace konsoll
{

// Headless rendering check: `konsoll --render-dump ROWS COLS OUT.png` feeds
// stdin to a session without a PTY, draws it with the real TerminalRenderer
// in an offscreen window and saves the frame. Lets the rendering of styles,
// the cursor and images be inspected without a screen.
int runRenderDump(std::span<char *> args);

} // namespace konsoll
