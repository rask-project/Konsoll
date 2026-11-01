# Konsoll — Action Plan

> Terminal for macOS built in QML (Qt 6.12, Apple Silicon)
> Last updated: 2026-11-01

## 1. Overview

Konsoll is a native terminal for macOS built in QML. Core idea: a clean
macOS window with a real terminal surface (PTY) running the user's default
shell — no KDE/Plasma dependencies.

**Current state:** the end-to-end terminal MVP is implemented. The repository
also contains several post-MVP interaction features (selection, clipboard,
search, mouse reporting, and scroll controls). Manual acceptance criteria
below remain a checklist; implementation presence alone does not record that
each scenario has been run and verified.

## 2. MVP goal

A working end-to-end terminal:

1. **Valid PTY** — the app creates a real pseudo-terminal (master/slave)
   and runs the user's shell inside it.
2. **Default shell detection** — identify the user's login shell
   (`/bin/zsh`, `/bin/bash`, `fish`, etc.) from the `passwd` entry
   (`getpwuid_r`) and launch it in the PTY.
3. **Command sending** — QML keyboard → bytes → PTY → shell; shell output →
   ANSI parser → screen buffer → QML rendering.
4. **Correct feedback** — basic SGR colors, cursor, screen erase, vertical
   scrolling (scrollback), window resize reflected into the PTY
   (`TIOCSWINSZ`).

### MVP acceptance criteria

- [ ] On launch, the user's default shell is detected and started (verify
      with `echo $SHELL` and `dscl` — the opened shell must match the
      user's `pw_shell`).
- [ ] `echo "olá"`, `ls`, `cd`, a `for` loop, and a colored-output command
      (e.g. `ls --color`/`grep --color`) work.
- [ ] `vim` (or `less`) opens, renders, and exits without corrupting the
      screen.
- [ ] Ctrl+C interrupts a running command; Ctrl+D exits the zsh/bash shell.
- [ ] Resizing the window changes `COLUMNS`/`LINES` (verify with
      `tput lines`/`stty size`).
- [ ] Exiting the shell quits the app with code 0.
- [ ] pt-BR accented characters (ç, ã, ò) typed via dead keys render
      correctly.

## 3. Scope

### In (MVP)

- PTY via `forkpty()` (macOS/BSD, `<util.h>`)
- Default shell detection via `getpwuid_r()`
- Minimal ANSI parser (table in §7)
- Screen model (cell grid + scrollback)
- `Canvas2D` rendering (Qt 6.12, GPU) with ~60 fps coalescing
- Keyboard input (key-to-bytes mapping, IME/dead keys)
- Window title via `OSC 0/2`
- Scrollback with reflow on resize and user scrolling (current buffer is
  unlimited; there is no 1000-line cap)

### Out (remaining post-MVP work, §§15–16)

Tabs are implemented; their remaining manual acceptance checks are in §15.6.
Splits, bracketed paste, color profiles/themes, proper multi-cell grapheme
rendering, autocomplete, and Figma design-system integration remain in §16.
Search, selection/clipboard, and mouse reporting are already implemented (see
§14).

## 4. Architecture

```
┌─────────────────────────────────────────────────────────┐
│ QML                                                      │
│  Main.qml ── TerminalView.qml                            │
│    ├─ Canvas2D (renders the cell grid)                   │
│    └─ FocusItem (keyPressed / inputMethod)              │
└──────────────────────┬────────────────────────────────────┘
                 signals / properties
┌──────────────────────▼────────────────────────────────────┐
│ C++  TerminalBackend : QObject                            │
│   • start(shellPath), write(bytes), resize(cols, rows)    │
│   • signals: frameReady(), titleChanged(str),             │
│              processExited(int)                           │
│   • exposes the screen for reading (rows, cells, cursor)  │
├───────────────────────────────────────────────────────────┤
│  Terminal (ANSI parser + screen model)                    │
│   • grid of Cell{char, fg, bg, bold, underline, dim}      │
│   • cursor, scrollback ring buffer, modes (DECOM etc.)   │
├───────────────────────────────────────────────────────────┤
│  PtyProcess                                              │
│   • forkpty() → master fd + child pid                     │
│   • read thread (QSocketNotifier) → bytes                 │
│   • TIOCSWINSZ on resize; read()==0 → process exit        │
└──────────────────────┬────────────────────────────────────┘
                       │ slave pty
              ┌────────▼────────┐
              │  user's shell    │  (zsh/bash/fish/...)
              │  TERM=xterm-256color
              └─────────────────┘
```

**Decision:** in-house parser (no `libvterm`) — keeps the MVP free of
external dependencies, and the minimal parser is small enough.

## 5. Default shell detection

**Source of truth:** the user's `passwd` entry (`pw_shell`) — the same
value exposed by `dscl . -read /Users/<u> UserShell` and `scutil`.

```cpp
#include <pwd.h>
#include <unistd.h>

QString detectDefaultShell()
{
    struct passwd  pwd{};
    char*          result = nullptr;
    char           buf[4096];

    if (getpwuid_r(getuid(), &pwd, buf, sizeof buf, &result) == 0 && result) {
        const QString shell = QString::fromLocal8Bit(result->pw_shell);
        if (!shell.isEmpty() && QFileInfo(shell).isExecutable())
            return shell;               // e.g. /bin/zsh, /bin/bash, /opt/homebrew/bin/fish
    }
    // Fallback chain:
    if (QFileInfo::exists("/bin/zsh")) return "/bin/zsh";   // macOS 10.15+ default
    return "/bin/sh";
}
```

Rules:

1. Read `pw_shell` via `getpwuid_r(getuid(), ...)` (thread-safe, no extra
   framework link).
2. If empty, missing, or not executable → `/bin/zsh` → `/bin/sh`.
3. Launch the shell as a **login shell**: `argv[0] = "-" + basename`
   (e.g. `-zsh`), like Terminal.app does — so `.zprofile`/`.zshrc`/
   `.bash_profile` get loaded.
4. `TERM=xterm-256color`, initial `COLUMNS`/`LINES`, inherited
   `HOME`/`PATH`.

> Considered alternative: `SCDynamicStoreCopyStringVariable(kSCShellVariable)`
> (SystemConfiguration) — rejected: requires an Apple framework for the
> same data `getpwuid_r` already provides.

## 6. PTY

- **Creation:** `forkpty(&masterFd, &slaveFd, &termios, &winsize)`
  (`<util.h>`). Atomic — avoids the `openpty` + `fork` manual race.
- **Child:** `execvp(shell, {"-zsh", nullptr})` with `TERM`/`COLUMNS`/
  `LINES` in the environment.
- **Read:** dedicated thread with a `QSocketNotifier` (READ) on the master
  fd; read buffer → `Terminal::write(bytes)` → marks the screen dirty.
- **Write:** `write(masterFd, data)` directly (called from QML).
- **Resize:** `ioctl(masterFd, TIOCSWINSZ, &winsize)` + resize the screen
  model. The child gets `SIGWINCH` automatically.
- **Lifecycle:** `read() == 0` → `processExited` → quit the app (MVP:
  Terminal.app behavior).
- **Backpressure:** if the read buffer overflows (e.g. `cat /dev/urandom`),
  drop in chunks and keep the last line coherent.

## 7. ANSI parser (minimal for the MVP)

| Class | Sequences |
|---|---|
| Control | `BEL`, `BS`, `HT`, `LF`, `CR`, `SO`/`SI` (charset shift — ignore) |
| CSI | `CUA` (A/B/C/D), `CPR` (H/f), `ED` (J), `EL` (K), `SGR` (m), `SU/SD` (r/M), `DCH/ICH` (@/P), `XT` (t — only `44;1`/ignore) |
| SGR | reset, bold, dim, underline, strike, 16/256/truecolor (38/48/39/49) |
| ESC | `]` OSC: `0;`/`2;` title, `8` hyperlink (ignore); `M` reverse index; `c` reset; `n` DA (reply `0`); `7`/`8` save/restore cursor |
| DEC | `?25h/l` cursor visibility, `?1049h/l` alternate screen, `?2004h/l` (bracketed paste — accept but unused) |
| Mouse | ignore (MVP) |

Screen model:

- Grid of `Cell { QChar ch; uint fg, bg; flags bold|underline|dim|strike }`.
- Cursor with position + insert/overstrike modes.
- Scroll regions (`DECSTBM`) honored.
- **Character width:** each Unicode code point occupies one cell. This keeps
  supplementary-plane Nerd Font icons aligned, but wide glyphs and grapheme
  clusters (for example emoji and ZWJ sequences) are not measured/rendered as
  multi-cell characters yet.
- Alternate screen: separate buffer, restore the main one on exit
  (vim/less).

## 8. QML rendering

- **`Canvas2D`** (Qt 6.12, GPU-accelerated) as the renderer: one `fillRect`
  per cell for the background + `fillText` per cell for the text
  (grouping consecutive runs with the same style to cut call count).
- System monospace font: `SF Mono` (fallback `Menlo`), configurable size;
  `letterSpacing` 0.
- **Coalescing:** `TerminalBackend` emits `frameReady()` at most every
  16 ms (QTimer in the read thread's event loop) — QML only repaints when
  the screen is dirty.
- Cursor: rect/caret drawn on the canvas; blink via `PropertyAnimation`
  optional (MVP: solid cursor).
- Fallback: if `Canvas2D` misbehaves on macOS 27, migrate to `Canvas`
  (CPU) — the API is identical.

## 9. Keyboard / input

- `FocusItem` with `focus: true` capturing `keyPressed`/`keyReleased` and
  `inputMethod` (IME) — required for pt-BR dead keys (ç, ã, ê).
- Mapping (key → bytes):

| Key | Bytes |
|---|---|
| Enter | `0x0D` |
| Backspace | `0x7F` |
| Tab | `0x09` |
| Esc | `0x1B` |
| ↑ ↓ → ← | `0x1B [ A` `B` `C` `D` |
| Home / End | `0x1B [ H` / `F` |
| Delete | `0x1B [ 3 ~` |
| PgUp / PgDn | `0x1B [ 5 ~` / `6 ~` |
| Ctrl+letter | `(code - 'a' + 1) & 0x1F` |
| Ctrl+Shift+letter | `UPPER & 0x1F` |
| Printable text | UTF-8 via the event `text()` / IME commit |

- Disable `QQuickWindow::inputMethodQuery` so the native text caret does
  not appear (the terminal owns its own cursor).

## 10. File layout

```
konsoll/
├── CMakeLists.txt            # + qt_add_executable with the new .cpp files
├── main.cpp                  # (unchanged for the MVP)
├── Main.qml                  # window + TerminalView
├── src/
│   ├── TerminalBackend.h/.cpp   # QML bridge (QObject, signals, properties)
│   ├── pty/
│   │   └── PtyProcess.h/.cpp   # forkpty, read loop, resize, exit
│   └── term/
│       ├── Cell.h              # cell struct
│       ├── Terminal.h/.cpp     # ANSI parser + screen model
│       └── Screen.h/.cpp      # grid, scrollback, cursor
├── qml/
│   └── components/
│       └── TerminalView.qml    # Canvas2D + FocusItem
└── tests/
    ├── tst_terminal.cpp        # unit: ANSI parser (Qt Test)
    └── tst_pty.cpp             # smoke: forkpty → echo → read
```

## 11. Execution phases

| # | Phase | Deliverable | Implementation status |
|---|---|---|---|
| 0 | Foundation | CMake/QML module, registered backend and native window behavior | Implemented |
| 1 | Shell detection | `getpwuid_r()` detection with executable fallback | Implemented |
| 2 | PTY | `forkpty`, nonblocking `QSocketNotifier` reads, writes, resize and exit handling | Implemented |
| 3 | ANSI parser | `Terminal` + `Screen`, including SGR, cursor, erase, scroll regions, alternate screen, OSC title, DA/DSR and color queries | Implemented; see §7 scope and limitations |
| 4 | QML rendering | `TerminalView` with Canvas2D, snapshots and 16 ms frame coalescing | Implemented |
| 5 | Keyboard | Special/control key mappings, composed text and IME/dead-key input | Implemented |
| 6 | Resize + scroll | `TIOCSWINSZ`, screen reflow, unlimited scrollback, wheel and scrollbar navigation | Implemented |
| 7 | Final acceptance | §2 manual scenarios and polish | Not recorded as verified; run the checklist on macOS |
| 8 | Session model | Independent terminal session object/model per tab | Implemented; manual acceptance pending |
| 9 | Multi-session backend | Route input, output, resize, title, exit and view state by session | Implemented; manual acceptance pending |
| 10 | Title-bar tabs | Adaptive multi-row QML tab strip with add/close controls | Implemented; macOS visual acceptance pending |
| 11 | Tab actions | Create, activate, close and keyboard shortcuts | Implemented (`Ctrl+T`, `Ctrl+W`, MRU navigation); manual acceptance pending |
| 12 | Tabs acceptance | Verify session isolation, resizing, lifecycle and visual behavior | Build passes; manual checks pending |

**Original order:** 0 → 1 → 2 → 3 → 4 → 5 → 6 → 7. Phases 0–6 are
implemented; phase 7 is the remaining MVP manual acceptance pass. Tab
implementation phases 8–11 are complete; phase 12 is the remaining tab
acceptance pass. Phases 3 and 4 were independently implementable.

## 12. Testing

- **Unit (Qt Test):** planned; no test target or `tests/` sources are currently
  present in the repository. ANSI parser coverage should include each sequence
  class (CSI, SGR, OSC, DEC).
- **Smoke (manual, scriptable):** `printf 'ls\n'` into the pty → expect
  output.
- **UI (manual):** §2 checklist run on a real zsh and a real bash.
- **Performance:** `yes | head -c 1000000` — measure fps and memory with
  `qmlprofiler` (target: no drop below 30 fps).

## 13. Known risks and limitations

| Risk | Mitigation |
|---|---|
| `forkpty` is a BSD extension (not POSIX) | Fine for macOS; isolate in `PtyProcess` so the implementation can be swapped if needed |
| Emoji width (`wcwidth` counts 1) | Known MVP limitation; document it and handle in the backlog |
| pt-BR IME/dead keys corrupting bytes | Test explicitly in phase 5 (acceptance criterion) |
| `Canvas2D` is new in Qt 6.12 (stability) | Fallback to CPU `Canvas` already planned (§8) |
| Backpressure on massive output | Nonblocking PTY writes drop the remainder when the master buffer is full; sustained output behavior should be measured |
| Bad alternate-screen implementation corrupting scroll | Cover with a dedicated unit test |

## 14. Implemented beyond the original MVP

The following items were originally listed as post-MVP work and are now
implemented in the repository:

- Text selection by drag, double-click word selection, triple-click line
  selection, and Select All
- Clipboard copy/paste, context menu, and Clear Buffer
- Scrollback navigation by wheel and scrollbar
- Scrollback search with literal or regular-expression queries and
  next/previous match navigation
- Mouse reporting for DEC modes 1000/1002/1003 and SGR mode 1006
- OSC 133 shell integration for command block highlighting and block copy in
  the bundled zsh, bash, and fish startup integrations
- Native macOS translucent window tint, About dialogs, and application icon

These feature implementations are present; the §2 manual acceptance checklist
is still the place to record end-to-end verification.

## 15. Implemented feature: QML tabs with independent terminal sessions

Tabs are implemented as QML controls backed by a manager that owns independent
terminal backend instances. They are drawn in the expanded, transparent
title-bar area and do not use native `NSWindow` tabbing. Manual macOS
acceptance remains outstanding.

### 15.1 Visual and interaction design

- Use a custom QML tab strip with `TabButton` delegates; manage selection and
  placement from the session model.
- Keep the title-bar surface transparent. Derive the tab surfaces from the
  system palette and accent color for light and dark appearance. Keep
  inactive tabs quieter and avoid macOS glass, blur, or material effects.
- Prefer an active tab width of 250 logical pixels and inactive width of 150;
  shrink tabs to a minimum of 88 pixels, then wrap them into additional rows.
  The first row reserves the leading inset for the traffic-light controls and
  trailing space for the add button. Following rows start near the left window
  edge and use the space below the add button.
- Place each tab's close button on its left side, following macOS convention.
  Keep the add icon button at the trailing corner of the title-bar area.
- Increase the title-bar safe area as rows are added and keep the terminal
  viewport below the complete tab strip.
- Use the system accent for selection and active-tab emphasis, with enough
  text contrast in both light and dark appearance.

### 15.2 Session model

Each tab represents one independent terminal session. A session owns its own
`PtyProcess`, `Terminal`/`Screen`, shell process, title/current directory,
scrollback, selection, search state, command highlights, and mouse modes.
Current-directory titles are received through OSC 7 shell integration.
Switching tabs changes the session shown by the renderer without stopping or
resetting the other shells.

`TerminalManager` owns one `TerminalBackend` instance per session and exposes
the dynamic session list and active session to QML. Signals and invokable
operations are session-scoped so output, input, resize, search, clipboard and
process-exit handling cannot leak between tabs.

All sessions share the window's terminal viewport dimensions. On window
resize, update each session's PTY and screen size so switching tabs does not
restore stale `COLUMNS`/`LINES` values.

### 15.3 Backend implementation

- Each `TerminalBackend` owns one terminal and PTY. `TerminalManager` creates
  sessions, selects the active session, and closes sessions safely.
- Each `TerminalView` and its search controls bind to one session; a
  `StackLayout` displays the active session while retaining the others.
- Closing a tab terminates and reaps its shell. An exited shell closes its
  tab while other sessions remain; exiting the final shell quits the app.

### 15.4 QML title-bar UI

1. Place the tab strip in the expanded client/title-bar area and keep the
   terminal view inset below its dynamic height.
2. Use session-backed `TabButton` delegates with explicit, responsive
   placement and active state.
3. Style labels, close controls, borders, and backgrounds for both system
   appearances; use the system accent for active emphasis.
4. Keep the add button at the trailing edge and clear of the safe-area inset.
5. Closing a tab selects a surviving session; `Ctrl+T` creates a tab and
   `Ctrl+W` closes the active tab. `Ctrl+Tab` and `Ctrl+Shift+Tab` navigate
   through activation history.

### 15.5 Tab actions and lifecycle decisions

- Add a tab with the plus button or `Ctrl+T`.
- Close the active tab with its close button or `Ctrl+W`.
- Navigate through most-recently activated tabs with `Ctrl+Tab` and
  `Ctrl+Shift+Tab`, wrapping through the history while Ctrl is held. Releasing
  Ctrl commits the selected tab as the newest activation.
- Preserve the shell and all terminal state when switching tabs.
- Closing the final tab or exiting the final shell quits the app, preserving
  the existing single-session lifecycle. Exiting one shell while other tabs
  remain leaves the other sessions running.
- Ensure application shutdown terminates/reaps every remaining shell.

### 15.6 Acceptance criteria

- Opening two tabs starts two independent shells; commands and output remain
  isolated per tab.
- Switching tabs restores each session's screen, cursor, scrollback and
  current-directory title without restarting its shell.
- Closing one tab leaves other shells running and selects a surviving tab.
- Resizing updates `stty size` in each tab, including inactive tabs.
- Search, selection, command highlights, clipboard operations and mouse
  reporting apply to the intended session.
- The tab strip stays clear of window controls; the terminal starts below it.
- Tabs shrink to their minimum, wrap into additional rows, and leave the add
  button clear while using the available width in following rows.
- Tab widths, active border/background, add and close controls follow the
  specified design in dark and light appearances.
- Window shutdown closes and reaps all child shell processes.

## 16. Remaining post-MVP backlog (unordered)

- Splits (one PTY per pane)
- Bracketed paste (`?2004h` mode is accepted but paste is not wrapped)
- Color profiles/themes + configurable font
- Ligatures + proper `grapheme` clustering (emoji, ZWJ)
- Figma design-system integration (tokens + components)
- QML tests (`qmltestrunner`) for `TerminalView`
