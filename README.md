# Konsoll

A native terminal for macOS, built in QML (Qt Quick) on top of Qt 6.12.

Konsoll is a clean, native macOS terminal with a real pseudo-terminal (PTY)
running your default shell — no KDE/Plasma dependencies. It renders the screen
with a native Qt Quick scene-graph renderer, emulates a VT500-class terminal
with the modern xterm, kitty and iTerm2 extensions, and offers an
iTerm2-style see-through window with native Liquid Glass.

## Requirements

- macOS (Apple Silicon)
- Qt 6.12 (Qt Quick, Qt CanvasPainter, Qt Quick Controls, Qt Quick Dialogs)
- CMake 3.21.1+
- A C++23 compiler (AppleClang / Xcode command-line tools)

## Build

```sh
# Configure (point CMAKE_PREFIX_PATH at your Qt 6.12)
cmake -S . -B build -G Ninja \
    -DCMAKE_PREFIX_PATH=/path/to/Qt/6.12.0/macos-arm64/lib/cmake \
    -DCMAKE_BUILD_TYPE=Debug

# Build
cmake --build build --target konsoll
```

The app is produced at `build/konsoll.app`. (Qt Creator works too — it
generates the build directory itself.)

### Release bundle

The full build, signing, notarization, and packaging process is documented in
[`docs/RELEASE.md`](docs/RELEASE.md). To build the self-contained Release app:

```sh
cmake -S . -B build/release -G Ninja \
    -DCMAKE_PREFIX_PATH=/path/to/Qt/6.12.0/macos-arm64/lib/cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build build/release --config Release --target package-macos
```

The complete, self-contained app bundle is
`build/release/dist/konsoll.app`. The build-tree app at
`build/release/konsoll.app` may still rely on the local Qt installation and
is not the distribution artifact.

## Run

```sh
./build/konsoll.app/Contents/MacOS/konsoll
```

On launch Konsoll detects your default shell (from the `passwd` entry) and
starts it in a PTY.

### Selecting command blocks

Konsoll loads its OSC 133 shell integration automatically for zsh, bash, and
fish in each PTY session; it does not edit shell startup files. Clicking a
command or one of its output lines highlights the full block; Cmd+C or the Copy
menu item copies its text. Other shells continue to run normally and retain
single-line highlighting. This matches iTerm2's automatic integration for
supported shells.

### Saving terminal text

Choose **Save Buffer or Selection…** from the application menu or a terminal's
context menu. With a mouse selection active, Konsoll saves that selected text
(including selected words and lines); otherwise, it saves the complete
scrollback and visible buffer. The file dialog lets you choose the filename
and destination and opens in Downloads by default.

## Features

### Core terminal

- Real PTY via `forkpty()` running your login shell, one worker thread per
  session with output backpressure
- Default shell detection via `getpwuid_r` (fallback `/bin/zsh` → `/bin/sh`)
- Native scene-graph rendering (`TerminalRenderer`, C++) with ~60 fps
  coalescing; cell size measured from the font
- Window resize reflected into the PTY (`TIOCSWINSZ`)
- Unlimited scrollback with reflow on resize (images and double-size lines
  stay with their text)
- Unicode: wide characters (CJK, emoji) take two cells, zero-width marks take
  none, combining marks compose, and grapheme clusters (ZWJ sequences,
  skin tones, flags) are one cell with mode 2027
- Keyboard input with IME / dead keys (pt-BR accents)

### Terminal emulation

An in-house VT500-style state machine (no `libvterm`), validated against
`vttest` and Neovim/LazyVim:

- Every SGR attribute, with colon sub-parameters (curly/colored underlines,
  256 colors and truecolor), and background color erase
- Cursor, tab, erase, insert/delete, scroll regions, left/right margins,
  VT420 rectangular operations, DEC double-width/double-height lines,
  charsets (G0–G3, NRCS, DEC Special Graphics), reverse wraparound,
  132-column mode (DECCOLM), 8-bit C1 controls (S8C1T) and DECUDK
- Device, mode and cursor reports (DA1/2/3, DSR, DECRQM, DECRQSS, XTVERSION,
  XTGETTCAP) answered through an ordered reply queue
- Cursor shapes and blinking (DECSCUSR), focus reporting, synchronized output
- Keys encoded like xterm, with modifyOtherKeys and every kitty keyboard
  protocol enhancement (including the base layout key)
- Mouse reporting (X10/1000/1002/1003, SGR 1006, SGR-pixels 1016),
  horizontal wheel, highlight tracking and alternate scroll
- Bracketed paste
- Option as Meta (a setting, plus `?1036` / `?1039`)

### Images, links and shell integration

- Images: kitty graphics protocol (with animation and Unicode placeholders),
  sixel, and iTerm2 `OSC 1337` inline images
- `OSC 8` hyperlinks (Cmd+click), `OSC 4/10/11/12` color control
- `OSC 52` clipboard (reads ask first by default)
- `OSC 9` / `OSC 777` desktop notifications
- `OSC 7` working directory, `OSC 133` / `OSC 633` command blocks, and the
  iTerm2 `OSC 1337` commands (user variables, current directory, …)
- `OSC 22` mouse pointer shapes

### Windows, tabs and split panes

- Multiple windows (`Cmd+N`), listed in the Window menu; the last active
  window's size and position are restored on the next launch
- Multiple tabs (workspaces) per window, each an independent shell session
  (`Cmd+T` / `Cmd+W`, named with `Cmd+Shift+T`, `Cmd+Tab` /
  `Cmd+Shift+Tab` to move through activation history); press and hold a tab
  to reorder it
- Nested horizontal/vertical split panes (`Cmd+D` / `Cmd+Shift+D`), each
  pane an independent session with its own title bar and close control
- **Move a workspace to another window** — right-click a tab and choose
  *Move to new window* or *Move to window ▸*, or drag the tab out of the
  strip: drop it on another window to join its tabs, or anywhere else to
  tear it off into a new window. The shells keep running.
- **Rearrange split panes by dragging** — drag a pane by its title bar and
  drop it on another pane: the left/right half splits side by side, the
  top/bottom half stacks them (so two panes can swap between a horizontal
  and a vertical split). Drop it on a tab strip to make it a workspace of
  its own — in this window or another one — or anywhere outside the windows
  to tear it off into a new window. The shell keeps running.
- **iTerm2-style see-through window** — the whole window (content + title
  bar) is translucent, tinted with the terminal's background color, with an
  optional blur; the tab strip uses native Liquid Glass on macOS 26

### Interaction

- Text selection (drag, double-click word, triple-click line, Select All)
- Clipboard copy/paste (text to the PTY; image paste uploads to an SSH host
  and inserts the remote image path; file drops upload to SSH sessions),
  context menu, Clear Screen (`Cmd+L`) and
  Clear Buffer (`Cmd+K`)
- Save the full terminal buffer or current selection to a UTF-8 text file
- Scrollback navigation by wheel and scrollbar
- Scrollback search (literal or regular expression, next/previous match)
- Native macOS menu bar (App / Shell / Edit / Window) and About dialog

### Settings

A native-looking Settings window (`Cmd+,`) configures the font (family,
size, weight, italic), window transparency and blur, Option as Meta, and
what programs may do: clipboard reads (`OSC 52`), title reports and window
operations (XTWINOPS), and desktop notifications.

### Window translucency

The see-through look is driven by a configurable QML component:

```qml
WindowTranslucency {
    window: appWindow
    color: SessionManager.activeSession.backgroundColor   // tint = terminal bg
    alpha: 0.85
    blurEnabled: true
}
```

- `color` — the tint color, as a `uint` ARGB value (0xAARRGGBB). Bound to the
  active session's `backgroundColor` so it follows the terminal's background.
- `alpha` — the see-through level (0.85 ≈ the accepted `opacity: 0.85` look).
- `blurEnabled` — enables the native macOS window blur (enabled by default).

The native side is Objective-C++ (`src/MacOSTranslucency.mm`): it makes the
`NSWindow` non-opaque and sets its background to the tint color at the given
alpha. The QML `Window` must keep `color: "transparent"` so the backing
surface is transparent and the native tint shows through.

## Testing

```sh
./build/konsoll.app/Contents/MacOS/konsoll --selftest
```

runs the parser checks (`src/parserselftest.cpp`), the recorded fixtures in
`tests/fixtures/` (a program's raw output and the screen it must produce)
and the multi-session tests against the real manager/backend/PTY stack.

- `konsoll --screen-dump ROWS COLS [--attrs]` prints the screen that stdin
  draws; `konsoll --render-dump ROWS COLS OUT.png` renders it with the real
  renderer.
- `tools/vtdrive.py` drives a program (vttest, nvim) in a PTY, answering its
  queries with Konsoll's replies; `tools/regress.py` checks or regenerates
  the fixtures.

## Project layout

```
konsoll/
├── CMakeLists.txt
├── main.cpp
├── Main.qml                  # app root: window list, moving workspaces/panes
├── TerminalWindow.qml        # window, menu bar, shortcuts, translucency
├── SettingsWindow.qml        # native-looking Settings window
├── AppSettings.qml           # persisted settings
├── TerminalWorkspace.qml     # per-tab workspace (split tree) + tab strip
├── TerminalPaneLayout.qml    # binary split layout for a workspace
├── TerminalView.qml          # hosts TerminalRenderer + keyboard capture
├── controls/                 # tab strip, menu, search bar, tool buttons
├── shell-integration/        # OSC 133 snippets (zsh/bash/fish)
├── tests/fixtures/           # recorded regression fixtures
├── tools/                    # vtdrive.py, regress.py
├── resources/icons/          # SVG icon theme (add, arrows, close)
├── assets/                   # app icon (icns/png)
├── docs/
│   ├── PLAN.md               # MVP scope, architecture, phases
│   ├── MULTI-WINDOW-PLAN.md  # per-window session managers
│   ├── SESSION-THREADING-*.md# PTY worker threads
│   └── RELEASE.md            # signing, notarization, packaging
└── src/
    ├── TerminalBackend.h/.cpp   # QML<->C++ bridge per session
    ├── TerminalManager.h/.cpp   # a window's workspaces, splits and sessions
    ├── WindowRegistry.h/.cpp    # the open windows' managers
    ├── TerminalRenderer.h/.cpp  # scene-graph renderer for the cell grid
    ├── KeyEncoder.h/.cpp        # xterm / kitty key encoding
    ├── NativeKeyTracker.h/.cpp  # native key codes (kitty base layout key)
    ├── pty/
    │   ├── PtyProcess.h/.cpp    # forkpty, read loop, resize, exit
    │   └── PtyWorker.h/.cpp     # per-session worker thread
    ├── term/
    │   ├── Terminal.h/.cpp      # VT500 state machine + sequence handlers
    │   ├── TerminalReports.cpp  # device, mode and status reports
    │   ├── Screen.h/.cpp        # grid, scrollback, cursor, reflow
    │   ├── ScreenRect.cpp       # rectangular operations
    │   ├── Graphics.h/.cpp      # kitty, sixel and iTerm2 images
    │   ├── Grapheme.h/.cpp      # grapheme clusters, kitty placeholders
    │   ├── Charsets.h/.cpp      # G0–G3 and NRCS charsets
    │   ├── Palette.h/.cpp       # 256-color palette
    │   ├── CharWidth.h/.cpp     # wide-character table
    │   └── Cell.h               # cell struct
    ├── MacOSTranslucency.h/.mm  # native see-through window and glass views
    ├── WindowTranslucency.h/.cpp# QML type: configurable window translucency
    ├── GlassEffect.h/.cpp       # QML type: native Liquid Glass view
    ├── Notifications.h/.mm      # desktop notifications (UserNotifications)
    ├── NativeDialogs.h/.mm      # native alerts and prompts
    └── AboutDialog.h/.mm        # native About panel (Obj-C++)
```

## Scope

The MVP scope, architecture, and phases are defined in
[`docs/PLAN.md`](docs/PLAN.md). Windows, tabs, split panes, selection and
clipboard, scrollback search, the terminal emulation and image protocols
above, and shell integration are implemented on top of the MVP. The
remaining post-MVP backlog (color profiles/themes, ligatures and line
height, Figma design-system integration, QML tests) is in PLAN §16.
Obsolete VT features with no real-world use (VT320+ page memory, DECDLD
soft fonts, ReGIS) are deliberately left out.
