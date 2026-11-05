# Konsoll — Multi-Window Plan

> Multi-window support for the currently single-window terminal.
> Last updated: 2026-11-05
> Status: planning (not started)

## 1. Goal

Let the user open, manage, and close **multiple independent windows**, each with
its own set of workspaces/sessions, while the app behaves like a native macOS
terminal (iTerm2-style): a shared app menu that acts on the focused window, the
app staying alive after the last window closes, and per-window state restored on
relaunch.

Confirmed scope decisions:

- **Window state:** each window persists its own geometry + session layout and is
  restored on relaunch (iTerm2-style).
- **Last window:** closing the last window does **not** quit the app; the app stays
  alive and `Cmd+N` / dock click reopens a window.

## 2. Current architecture (single window)

- `Main.qml` is the **root scene and the only `Window`**. It holds the `MenuBar`
  (native macOS menu bar), the `Settings` (QSettings), `WindowTranslucency`,
  `WindowScreen`, the `TerminalWorkspace`, and the dialogs.
- `SessionManager` is `TerminalManager`, a **`QML_SINGLETON`** — one global model
  that owns *all* workspaces and sessions (`m_workspaces`, `m_sessions`).
- `TerminalManager::instance()` is a **static pointer** used by
  `src/AppQuitGuard.mm` (the `NSApplicationDelegate`) to decide app quit
  (`anySessionHasActiveProcess()`, `sessionCount()`).
- `WindowTranslucency` and `WindowScreen` are already **per-window** C++
  `QML_ELEMENT`s (they take a `window` property) — they are reusable as-is.
- `main.cpp` sets `app.setQuitOnLastWindowClosed(false)` and loads `Main`.
- The session model is already multi-session capable: each session owns its own
  worker thread (`PtyWorker`), `Terminal`/`Screen`, and PTY. Only the *grouping*
  (one global manager) is single-window.

The two things that make it single-window are: (a) one global `TerminalManager`,
and (b) `Main.qml` being the `Window` itself.

## 3. How iTerm2 does it (reference model)

iTerm2 is an AppKit/Cocoa app. Its structure:

- **`AppController`** — a central per-app controller that owns the list of all open
  windows and mediates window create/close.
- **`WindowController`** (an `NSWindowController` subclass) — one per window, wraps
  an `NSWindow`, and owns that window's `TabController`.
- **`TabController` → `TabGroup` → `Terminal`** — the per-window content: a stack
  of tab groups; each group is a split layout of `Terminal` objects; each
  `Terminal` wraps one PTY session (a real shell process).
- **App-level `NSMenu`** — a single menu bar (`[NSApp setMainMenu:]`). Menu items
  dispatch to the **key window** (the focused window): "New Tab" (⌘T) adds a tab to
  the key window; "New Window" (⌘N) asks `AppController` for a new window.
- **Window state restoration** — each window persists its frame (position/size) and
  its tab/split layout, including each terminal's working directory and profile. On
  relaunch, iTerm2 reopens every window and restarts the shells in the same
  directories.
- **Keep running** — the app stays alive after the last window closes; the dock icon
  or ⌘N reopens a window.

Mapping to Konsoll:

| iTerm2 | Konsoll |
|---|---|
| `AppController` (owns the windows) | `Main.qml` manager root + C++ `AppController`/`WindowRegistry` |
| `WindowController` + `NSWindow` | `TerminalWindow.qml` (a `Window`) |
| `TabController` → `TabGroup` → `Terminal` | `TerminalManager` → workspaces → split tree → `TerminalBackend` (sessions) |
| App-level `NSMenu` → key window | Per-window QML `MenuBar`, shown while that window is active |
| Window state restoration | Per-window state persistence (new, Phase 4) |
| Keep running / dock reopen | `AppQuitGuard` (NO on last close) + `applicationShouldHandleReopen:` |

## 4. Target architecture

- **`Main.qml`** becomes a **manager root**, not a `Window`. It is a plain (invisible)
  `Item` that:
  - owns a `Component` — the **window template**;
  - creates the first window on completion and tracks the live window list;
  - exposes `openWindow()` / `closeWindow()`;
  - holds the shared `Settings` (font, translucency alpha/blur) and the app-global
    dialogs (`AboutDialog`, `SettingsDialog`).
- **`TerminalWindow.qml`** (new) is the **window template** — a `Window` containing
  everything that is per-window today: its own `TerminalManager` (the
  `SessionManager` for that window), `TerminalWorkspace`, `WindowTranslucency`,
  `WindowScreen`, the per-window `SaveTextDialog`, the `MenuBar`, and the
  window-scoped shortcuts.
- **`TerminalManager`** becomes **per-window**: a regular `QML_ELEMENT` (no longer
  `QML_SINGLETON`). Each `TerminalWindow` instantiates exactly one.
- **`WindowRegistry`** (new C++ app-level singleton) tracks the live
  `TerminalManager`s so the AppKit quit guard can aggregate across all windows.
- **`AppController`** (C++ singleton) bridges AppKit → QML for "open a window"
  (dock click / ⌘N when there are zero windows).

Why the manager, not a `Window`: a `Window` is one window. To own *several*
windows, `Main` must be the thing that creates them — the same reason iTerm2 has
an `AppController` above the per-window `WindowController`s. The menu follows from
this: each `TerminalWindow` carries its own `MenuBar`, and macOS shows the
**active** window's menu bar — which is exactly iTerm2's "one app menu acting on
the key window."

## 5. Design decisions

1. **Per-window session model.** Each window gets its own `TerminalManager`
   (independent workspaces/sessions). This is the core change; it is what lets
   `Main` stop being the `Window`.
2. **Menu bar per window.** Each `TerminalWindow` has its own `MenuBar`; the
   focused window's bar is the app menu bar. "New Window"/"Close Window" items call
   the manager (a back-reference passed into the window), everything else calls
   that window's `sessionManager`.
3. **App-level aggregation in C++.** A `WindowRegistry` gives the AppKit quit guard
   a view across all windows (total session count, any active process) without
   reintroducing a single global model.
4. **Keep running + reopen.** `applicationShouldTerminateAfterLastWindowClosed:`
   returns `NO`; `applicationShouldHandleReopen:` opens a window. `main.cpp`
   already sets `setQuitOnLastWindowClosed(false)`.
5. **Settings split.** Font + translucency alpha/blur are app-global (shared).
   Window geometry + session layout are per-window (restored, Phase 4).
6. **Dialogs split.** `AboutDialog`/`SettingsDialog` are app-global (owned by the
   manager, open over the active window). `SaveTextDialog` is per-window (it acts on
   that window's session).

## 6. Implementation phases

Each phase is independently buildable; later phases build on earlier ones.

### Phase 1 — Per-window session model (C++)

Files: `src/TerminalManager.h/.cpp`, `src/AppQuitGuard.mm`, new
`src/WindowRegistry.h/.cpp`, `src/selftest.cpp`.

- Remove `QML_SINGLETON` + `QML_NAMED_ELEMENT(SessionManager)`; make
  `TerminalManager` a plain `QML_ELEMENT`.
- Remove the static `s_instance` / `instance()`.
- Add `WindowRegistry` (C++ app-level singleton): `TerminalManager` registers itself
  in its constructor and deregisters in its destructor; the registry exposes
  aggregates — `totalSessionCount()`, `anySessionHasActiveProcess()`,
  `anySessionHasRunningCommand()`.
- `AppQuitGuard.mm`: replace `TerminalManager::instance()` with registry queries
  (aggregate across all windows).
- Re-scope the app-global event filter (the Ctrl/Meta-release activation-history
  commit) so one window's key release does not commit another window's history — see
  §7.
- `selftest.cpp`: it already constructs a local `TerminalManager`; it must keep
  passing (a manager is now an ordinary object).

### Phase 2 — QML window template + manager root

Files: `Main.qml`, new `TerminalWindow.qml`, `TerminalWorkspace.qml`,
`TerminalPaneLayout.qml`, `TerminalView.qml`, `controls/TerminalTabButton.qml`,
`CMakeLists.txt`.

- Extract the body of `Main.qml` into `TerminalWindow.qml` (a `Window`): its
  `TerminalManager { id: sessionManager }`, `TerminalWorkspace`,
  `WindowTranslucency`, `WindowScreen`, `SaveTextDialog`, `MenuBar`, and the
  window-scoped shortcuts.
- `Main.qml` becomes an invisible `Item` manager: a `Component` for the window
  template, first-window creation on completion, the live window list,
  `openWindow()`/`closeWindow()`, and the shared `Settings` + app-global dialogs.
- Thread the per-window `sessionManager` through
  `TerminalWorkspace` → `TerminalPaneLayout` → `TerminalView` →
  `TerminalTabButton`, replacing the global `SessionManager` references with the
  passed-in manager.
- Pass each window a back-reference to the manager (`windowManager`) for the
  "New Window"/"Close Window" menu items.
- `CMakeLists.txt`: add `TerminalWindow.qml` to `QML_FILES`.

### Phase 3 — App-level window management + lifecycle

Files: new `src/AppController.h/.mm` (or fold into `WindowRegistry`),
`src/AppQuitGuard.mm`, `main.cpp`.

- `AppController` (C++ singleton): `openWindow()` queued to the GUI thread → tells
  `Main` to create a window.
- `AppQuitGuard`:
  - `applicationShouldTerminateAfterLastWindowClosed:` → `NO` (keep running).
  - `applicationShouldHandleReopen:hasVisibleWindows:` → `openWindow()` (dock click).
  - `applicationShouldTerminate:` aggregates across all windows (Phase 1 registry).
- `main.cpp`: keep `setQuitOnLastWindowClosed(false)`.
- Add the window menu items / shortcuts: ⌘N (new window), ⌘⌥W (close window).
- Handle the **zero-windows** state: no active window means no QML `MenuBar`, so the
  menu cannot create a window — the dock click (AppKit) is the reopen path.

### Phase 4 — Per-window state restoration (iTerm2-style)

Files: new `src/WindowState.h/.cpp` (or `Settings`-backed QML), `TerminalWindow.qml`,
`TerminalManager` (layout serialization).

- Persist **per window**: geometry (frame, maximized/fullscreen) and the session
  layout (workspaces, split tree, each session's working directory).
- Save on app quit and on window close; load on launch.
- On relaunch, restore each window: recreate its `TerminalManager` with the saved
  layout and start each session in the saved working directory.

### Phase 5 — Acceptance + polish

- Manual pass over §8; fix focus, translucency, and menu-dispatch regressions.

## 7. Risks and open details

- **App-global event filter.** `TerminalManager` installs an app-level event filter
  that commits the activation history on Ctrl/Meta release. With several managers,
  each would see the same key release and commit its *own* history. Scope the commit
  to the focused window (e.g. only the manager whose window is active commits), or
  move the filter onto the window.
- **Menu bar with zero windows.** No active window ⇒ no QML `MenuBar` ⇒ no ⌘N from
  the menu. The dock click is the only reopen path in that state (iTerm2 behaves the
  same). Document this in the UI.
- **`Settings` (QSettings) location.** It currently lives inside `Main.qml`'s
  `Window`. It must move to the manager root so all windows share it; confirm the
  `category`/keys are unchanged.
- **Dialog parenting.** `SettingsDialog`/`AboutDialog` are app-global and must open
  over the *active* window; `SaveTextDialog` is per-window and restores focus to
  that window's terminal.
- **C++ rules.** Keep `WindowRegistry`/`AppController` conformant: smart pointers
  only, no exceptions, `std::` algorithms, members ordered heaviest-alignment first
  (Debug enables `-Wpadded`), and clang-tidy clean (cognitive complexity, no
  do-while, no `NOLINT`).
- **Restoration size.** Per-window layout serialization (split tree + working
  directories) is the largest new piece of code; keep it a focused, testable unit.

## 8. Acceptance criteria

- ⌘N opens a new window with its own fresh session; windows are independent
  (commands/output do not leak across windows).
- Each window's menu bar is the app menu bar while that window is focused; menu
  actions act on that window's sessions.
- Closing a window (red button / ⌘⌥W) closes only that window's sessions; other
  windows keep running.
- Closing the **last** window does not quit the app; the dock icon (or a new-window
  path) reopens a window.
- App quit (⌘Q / menu) still honors the running-command confirm dialog, aggregating
  across **all** windows.
- Translucency tint follows each window's own active session background; alpha/blur
  settings are shared.
- After quit + relaunch, every previously open window is restored with its geometry
  and its sessions restarted in the same working directories.
- Shutdown terminates/reaps every shell in every window (no orphaned processes).
