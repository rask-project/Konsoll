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
- Native Qt Quick scene-graph rendering (`TerminalRenderer`, C++) with
  ~60 fps coalescing
- Keyboard input (key-to-bytes mapping, IME/dead keys)
- Window title via `OSC 0/2`
- Scrollback with reflow on resize and user scrolling (current buffer is
  unlimited; there is no 1000-line cap)

### Out (remaining post-MVP work, §§15–16)

Tabs and nested horizontal/vertical splits are implemented; their remaining
manual acceptance checks are in §§15.6 and 17. Splits give each pane an
independent shell session. Bracketed paste, color profiles/themes, grapheme
clustering (ZWJ sequences, skin tones), autocomplete, and Figma design-system
integration remain in §16.
Search, selection/clipboard, and mouse reporting are already implemented (see
§14).

## 4. Architecture

```
┌─────────────────────────────────────────────────────────┐
│ QML                                                      │
│  Main.qml ── TerminalView.qml                            │
│    ├─ TerminalRenderer (C++ scene graph, cell grid)      │
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
│   • worker thread (QSocketNotifier) → bytes               │
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
- **Read:** per-session worker thread with a `QSocketNotifier` (READ) on
  the master fd; read buffer → `Terminal::write(bytes)` → marks the
  screen dirty.
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
| CSI | `CUA` (A/B/C/D), `CPR` (H/f), `ED` (J), `EL` (K), `ECH` (X), `SGR` (m), `SU/SD` (r/M), `DCH/ICH` (@/P), `XT` (t — only `44;1`/ignore) |
| SGR | reset, bold, dim, underline, strike, 16/256/truecolor (38/48/39/49) |
| ESC | `]` OSC: `0;`/`2;` title, `8` hyperlink (ignore); `M` reverse index; `c` reset; `n` DA (reply `0`); `7`/`8` save/restore cursor |
| DEC | `?25h/l` cursor visibility, `?1049h/l` alternate screen, `?2004h/l` (bracketed paste — accept but unused) |
| Mouse | ignore (MVP) |

Screen model:

- Grid of `Cell { uint fg, bg, ch; uchar flags }`: `ch` is a full code point;
  flags are bold|dim|underline|strike|reverse|invisible plus `Wide` /
  `WideSpacer` for two-cell characters.
- Cursor with position + insert/overstrike modes.
- Scroll regions (`DECSTBM`) honored.
- **Background Color Erase (BCE):** erased cells (`ED`, `EL`, `ECH`), lines
  scrolled in (`LF` at the bottom margin, `SU/SD`, `IL/DL`) and columns
  inserted/deleted (`ICH/DCH`) take the current SGR background, as in
  xterm/iTerm2 — Neovim paints its theme background this way. The `Terminal`
  pushes its background to `Screen::setEraseBackground()` after every SGR and
  on `RIS`; grid creation, resize and full reset use the default background.
- **Character width** (wcwidth semantics, so rows match what TUIs such as
  OpenTUI/Ink/ncurses lay out):
  - Zero-width code points (`Mn`/`Me`/`Cf` except soft hyphen, Hangul medial
    jamo) take no cell. A combining mark folds into the previous character
    when NFC has a precomposed form (`c` + U+0327 → `ç`, as in macOS NFD file
    names); otherwise it is dropped. VS15 (U+FE0E) is dropped.
  - Wide characters (East Asian Wide/Fullwidth, `Emoji_Presentation`; table in
    `src/term/CharWidth.cpp`) take two cells: a `Wide` lead holding the code
    point and a `WideSpacer` cell. VS16 (U+FE0F) widens the previous narrow
    character.
  - A wide character that does not fit in the last column wraps whole; the
    left-over column becomes wrap padding (a `WideSpacer` with no lead),
    dropped when reflow re-joins lines. Reflow never splits a pair, and
    overwriting/erasing half of a pair blanks both halves.
  - Copy/save skip spacer cells. Nerd Font icons (supplementary plane) stay
    one cell.
  - Not handled: grapheme clusters (ZWJ sequences, skin-tone modifiers) are
    split into separate cells, so they can be wider than the application
    expects.
- **CSI private prefixes:** sequences whose parameters start with `<`, `=` or
  `>` (other than kitty keyboard `CSI > u` / `CSI < u`) are ignored rather than
  dispatched — e.g. modifyOtherKeys `CSI > 4 ; 2 m` must not be read as SGR.
- Alternate screen: separate buffer, restore the main one on exit
  (vim/less).

## 8. Rendering

The grid is drawn by **`TerminalRenderer`** (`src/TerminalRenderer.h/.cpp`), a
C++ `QQuickItem` (`QML_ELEMENT`) that builds Qt Quick scene-graph nodes in
`updatePaintNode()`. `TerminalView.qml` only places it and handles input,
selection gestures and the scrollbar.

- **Data flow:** on `TerminalBackend::frameReady()` the renderer pulls the
  visible rows on the GUI thread (`visibleRowCells()`, colors already resolved:
  palette, reverse video, contrast), bakes the selection text color and the
  cursor glyph color into the cells, and diffs each row against the previous
  frame. `updatePaintNode()` (render thread, GUI blocked) only consumes that
  state. No per-cell data crosses into JavaScript.
- **Node tree:**
  - background — one `QSGGeometryNode` with `QSGVertexColorMaterial`
    (premultiplied): cell backgrounds (default-background cells are left empty
    so the window translucency shows through), search match, selection,
    OSC 133 command highlight (fill + 1 px frame, square corners) and the
    block cursor (two cells wide on a wide character);
  - text — one `QSGTextNode` per visible row (`NativeRendering`), rebuilt only
    when that row's cells changed. Each style run (same color, weight,
    visibility) is one `QTextLayout` placed at its column. Wide characters and
    glyphs the primary font lacks (fallback fonts have other advances) get
    their own layout pinned to their cell;
  - overlay — one `QSGGeometryNode`: underline, strike-through, link hover.
- **Font and cell metrics:** the QML font is used as-is (point size
  included), with `JetBrainsMono Nerd Font Mono` as fallback family, kerning
  off and ligature/contextual features (`liga`, `clig`, `dlig`, `hlig`,
  `calt`) disabled so each glyph stays in its cell. `cellWidth` = advance of
  `M`, `cellHeight` = ⌈ascent + descent⌉; both are read-only renderer
  properties the view uses for `cols`/`rows`, mouse mapping and scrolling.
  Bold is pulled back onto the grid with a small letter-spacing correction.
- **Coalescing:** `frameReady()` is the only repaint trigger. The backend
  emits it from a trailing-edge 16 ms timer and holds it during DEC 2026
  synchronized updates; selection/highlight changes schedule the same frame.
  Repainting on any other signal (e.g. `scrollOffsetChanged`, emitted per PTY
  chunk) paints torn mid-update frames.
- **Ownership:** scene-graph nodes are created with `std::make_unique` and
  handed to Qt with `release()` (the parent node owns them) — the Qt-API
  exception to the raw-pointer rule.
- Cursor: solid block; the glyph under it is drawn in the background color.

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
│   ├── TerminalRenderer.h/.cpp  # scene-graph renderer (QQuickItem)
│   ├── pty/
│   │   └── PtyProcess.h/.cpp   # forkpty, read loop, resize, exit
│   └── term/
│       ├── Cell.h              # cell struct
│       ├── CharWidth.h/.cpp    # wide-character table
│       ├── Terminal.h/.cpp     # ANSI parser + screen model
│       └── Screen.h/.cpp      # grid, scrollback, cursor
├── qml/
│   └── components/
│       └── TerminalView.qml    # TerminalRenderer + input
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
| 4 | Rendering | `TerminalRenderer` scene-graph item (per-row text nodes, font-measured cells) with 16 ms frame coalescing; replaced the original Canvas2D view | Implemented |
| 5 | Keyboard | Special/control key mappings, composed text and IME/dead-key input | Implemented |
| 6 | Resize + scroll | `TIOCSWINSZ`, screen reflow, unlimited scrollback, wheel and scrollbar navigation | Implemented |
| 7 | Final acceptance | §2 manual scenarios and polish | Not recorded as verified; run the checklist on macOS |
| 8 | Session model | Independent terminal session object/model per tab | Implemented; manual acceptance pending |
| 9 | Multi-session backend | Route input, output, resize, title, exit and view state by session | Implemented; manual acceptance pending |
| 10 | Title-bar tabs | Adaptive multi-row QML tab strip with add/close controls | Implemented; macOS visual acceptance pending |
| 11 | Tab actions | Create, activate, close and keyboard shortcuts | Implemented (`Ctrl+T`, `Ctrl+W`, MRU navigation); manual acceptance pending |
| 12 | Tabs acceptance | Verify session isolation, resizing, lifecycle and visual behavior | Build passes; manual checks pending |
| 13 | Split panes | Nested horizontal/vertical splits, per-pane title bars and session history navigation | Implemented; manual checks pending |

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
| Character width vs. the application's layout | Zero-width and wide characters follow wcwidth (§7); grapheme clusters (ZWJ, skin tones) are still split |
| pt-BR IME/dead keys corrupting bytes | Test explicitly in phase 5 (acceptance criterion) |
| Repainting mid-update shows torn frames | Repaint only on `frameReady` (coalesced, DEC 2026-aware), §8 |
| Backpressure on massive output | Nonblocking PTY writes drop the remainder when the master buffer is full; sustained output behavior should be measured |
| Bad alternate-screen implementation corrupting scroll | Cover with a dedicated unit test |

## 14. Implemented beyond the original MVP

The following items were originally listed as post-MVP work and are now
implemented in the repository:

- Text selection by drag, double-click word selection, triple-click line
  selection, and Select All
- Clipboard copy/paste (text is sent to the PTY; local image paste is forwarded
  to image-aware CLI apps, while SSH image paste and dropped files are uploaded
  with `scp` and the remote path is inserted),
  context menu, and Clear Buffer
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

- Bracketed paste (`?2004h` mode is accepted but paste is not wrapped)
- Color profiles/themes + configurable font
- Grapheme clustering (ZWJ sequences, skin-tone modifiers) — cells hold one
  code point today
- Optional ligatures and a configurable line height
- Figma design-system integration (tokens + components)
- QML tests (`qmltestrunner`) for `TerminalView`

## 17. Implemented feature: nested split panes

Each top-level tab is a workspace with a binary split tree. A leaf owns one
independent terminal session and PTY. Splits can be nested horizontally
(side-by-side panes) or vertically (stacked panes). Each leaf has its own
title bar with a close control on the left. Splits are created from the
terminal context menu, keeping pane controls out of the title bar and
preserving terminal key combinations such as Ctrl+D. New splits start at
equal proportions, and their dividers remain adjustable. Closing a leaf
collapses its parent split and expands the remaining subtree into the
available space. A workspace tab shows the title or current directory of its
most recently active session and restores that session when reselected.

The active session is tracked independently from the active workspace. The
activation history stores session identities, so `Ctrl+Tab`/`Meta+Tab` and
their Shift variants navigate across panes and workspaces without depending
on mutable list indices. Selecting a history entry activates its workspace
and focuses its terminal. `Ctrl+T` creates a workspace; `Ctrl+W` closes the
current workspace. On macOS, `Cmd+D` and `Cmd+Shift+D` create side-by-side
and stacked splits; physical `Ctrl+D` is sent to the active shell as EOF.

### 17.1 Acceptance criteria

- Split a pane horizontally and vertically; each new pane starts an
  independent shell.
- Nest both split orientations and resize the dividers; each PTY reports its
  own current `stty size`.
- Each pane title reflects its current directory or shell title, and its
  close control removes only that session.
- Closing a pane collapses empty branches without disturbing surviving
  shells or their terminal state.
- `Ctrl+Tab`/`Meta+Tab` and Shift variants navigate sessions in activation
  history across both panes and workspaces; releasing the modifier commits
  the selected session.
- Closing a workspace closes all of its sessions and leaves other workspaces
  running. When the only workspace contains multiple panes, its tab close
  control closes the active pane and keeps the workspace open. Closing the
  final remaining session quits the app.
- Exiting one shell removes only its pane; exiting the final shell quits the
  app.
