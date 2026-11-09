# Konsoll

A native terminal for macOS, built in QML (Qt Quick) on top of Qt 6.12.

Konsoll is a clean, native macOS terminal window with a real pseudo-terminal
(PTY) running your default shell — no KDE/Plasma dependencies. It renders the
screen with a native Qt Quick scene-graph renderer and offers an iTerm2-style
see-through window.

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

- Real PTY via `forkpty()` running your login shell
- Default shell detection via `getpwuid_r` (fallback `/bin/zsh` → `/bin/sh`)
- In-house ANSI parser (no `libvterm`): SGR colors, cursor, erase,
  alternate screen, scroll regions, `OSC 0/2` window title
- Native scene-graph rendering (`TerminalRenderer`, C++) with ~60 fps
  coalescing; cell size measured from the font
- Unicode width handling: wide characters (CJK, emoji) take two cells,
  zero-width marks take none
- Keyboard input with IME / dead keys (pt-BR accents)
- Window resize reflected into the PTY (`TIOCSWINSZ`)
- Unlimited scrollback with reflow on resize
- **iTerm2-style see-through window** — the whole window (content + title bar)
  is translucent, tinted with the terminal's background color at reduced
  alpha, so the desktop shows through while the terminal stays readable.

### Tabs and split panes

- Multiple tabs, each an independent shell session (`Cmd+T` / `Cmd+W`,
  `Cmd+Tab` / `Cmd+Shift+Tab` to move through activation history)
- Nested horizontal/vertical split panes (`Cmd+D` / `Cmd+Shift+D`), each
  pane an independent session with its own title bar and close control

### Interaction

- Text selection (drag, double-click word, triple-click line, Select All)
- Clipboard copy/paste (text to the PTY; image paste uploads to an SSH host
  and inserts the remote image path; file drops upload to SSH sessions),
  context menu, Clear Screen (`Cmd+L`) and
  Clear Buffer (`Cmd+K`)
- Save the full terminal buffer or current selection to a UTF-8 text file
- Scrollback navigation by wheel and scrollbar
- Scrollback search (literal or regular expression, next/previous match)
- Mouse reporting (DEC 1000/1002/1003, SGR 1006)
- Native macOS menu bar (App / Shell / Edit) and About dialog

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

## Project layout

```
konsoll/
├── CMakeLists.txt
├── main.cpp
├── Main.qml                  # window, menu bar, shortcuts, translucency
├── TerminalWorkspace.qml     # per-tab workspace (split tree) + tab strip
├── TerminalPaneLayout.qml    # binary split layout for a workspace
├── TerminalView.qml          # hosts TerminalRenderer + keyboard capture
├── controls/                 # tab strip, menu, search bar, tool buttons
├── shell-integration/        # OSC 133 snippets (zsh/bash/fish)
├── resources/icons/          # SVG icon theme (add, arrows, close)
├── assets/                   # app icon (icns/png)
├── docs/
│   ├── PLAN.md               # MVP scope, architecture, phases
│   └── RELEASE.md            # signing, notarization, packaging
└── src/
    ├── TerminalBackend.h/.cpp   # QML<->C++ bridge per session
    ├── TerminalManager.h/.cpp   # owns one backend per tab/session
    ├── TerminalRenderer.h/.cpp  # scene-graph renderer for the cell grid
    ├── pty/
    │   └── PtyProcess.h/.cpp    # forkpty, read loop, resize, exit
    ├── term/
    │   ├── Cell.h               # cell struct
    │   ├── CharWidth.h/.cpp     # wide-character table
    │   ├── Terminal.h/.cpp      # ANSI parser + screen model
    │   └── Screen.h/.cpp        # grid, scrollback, cursor
    ├── MacOSTranslucency.h/.mm  # native NSWindow see-through (Obj-C++)
    ├── WindowTranslucency.h/.cpp# QML type: configurable window translucency
    └── AboutDialog.h/.mm       # native About panel (Obj-C++)
```

## Scope

The MVP scope, architecture, and phases are defined in
[`docs/PLAN.md`](docs/PLAN.md). Tabs, split panes, selection/clipboard,
scrollback search, mouse reporting, and OSC 133 shell integration are
implemented on top of the MVP. The remaining post-MVP backlog (bracketed
paste, color profiles/themes, grapheme clustering, Figma design-system
integration, QML tests) is in PLAN §16.
