# Konsoll

A native terminal for macOS, built in QML (Qt Quick) on top of Qt 6.12.

Konsoll is a clean, native macOS terminal window with a real pseudo-terminal
(PTY) running your default shell — no KDE/Plasma dependencies. It renders the
screen with GPU-accelerated `Canvas2D` and offers an iTerm2-style see-through
window.

## Requirements

- macOS (Apple Silicon)
- Qt 6.12 (Qt Quick, Qt CanvasPainter)
- CMake 3.16+
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

## Run

```sh
./build/konsoll.app/Contents/MacOS/konsoll
```

On launch Konsoll detects your default shell (from the `passwd` entry) and
starts it in a PTY.

## Features (MVP)

- Real PTY via `forkpty()` running your login shell
- Default shell detection via `getpwuid_r` (fallback `/bin/zsh` → `/bin/sh`)
- In-house ANSI parser (no `libvterm`): SGR colors, cursor, erase,
  alternate screen, scroll regions, `OSC 0/2` window title
- `Canvas2D` (GPU) rendering with ~60 fps coalescing
- Keyboard input with IME / dead keys (pt-BR accents)
- Window resize reflected into the PTY (`TIOCSWINSZ`)
- 1000-line scrollback
- **iTerm2-style see-through window** — the whole window (content + title bar)
  is translucent, tinted with the terminal's background color at reduced
  alpha, so the desktop shows through while the terminal stays readable.

### Window translucency

The see-through look is driven by a configurable QML component:

```qml
WindowTranslucency {
    window: appWindow
    color: Backend.backgroundColor   // tint = terminal bg (uint ARGB)
    alpha: 0.85
}
```

- `color` — the tint color, as a `uint` ARGB value (0xAARRGGBB). Bound to
  `Backend.backgroundColor` so it follows the terminal's background.
- `alpha` — the see-through level (0.85 ≈ the accepted `opacity: 0.85` look).

The native side is Objective-C++ (`src/macs_translucent.mm`): it makes the
`NSWindow` non-opaque and sets its background to the tint color at the given
alpha. The QML `Window` must keep `color: "transparent"` so the backing
surface is transparent and the native tint shows through.

## Project layout

```
konsoll/
├── CMakeLists.txt
├── main.cpp
├── Main.qml                 # window + TerminalView + WindowTranslucency
├── TerminalView.qml         # Canvas2D renderer + keyboard capture
├── docs/
│   └── PLAN.md              # MVP scope, architecture, phases
└── src/
    ├── TerminalBackend.h/.cpp   # QML<->C++ bridge (QObject, signals, props)
    ├── pty/
    │   └── PtyProcess.h/.cpp    # forkpty, read loop, resize, exit
    ├── term/
    │   ├── Cell.h               # cell struct
    │   ├── Terminal.h/.cpp      # ANSI parser + screen model
    │   └── Screen.h/.cpp        # grid, scrollback, cursor
    ├── macs_translucent.h/.mm   # native NSWindow see-through (Obj-C++)
    └── WindowTranslucency.h/.cpp# QML type: configurable window translucency
```

## Scope

The MVP scope, architecture, and phases are defined in
[`docs/PLAN.md`](docs/PLAN.md). Post-MVP features (tabs, splits, search,
selection, mouse reporting) are in the PLAN backlog and intentionally out of
scope for the MVP.
