# Konsoll — Multi-Window Plan

> Multi-window support and last-window geometry restoration for Konsoll.
> Last updated: 2026-11-09
> Status: Phases 1–4 implemented; manual acceptance (Phase 5) pending

## 1. Goal

Let the user open, manage, and close **multiple independent windows**, each with
its own set of workspaces/sessions, while the app behaves like a native macOS
terminal (iTerm2-style): a shared app menu that acts on the focused window and
the last active window's geometry restored on relaunch.

Confirmed scope decisions:

- **Window state:** each window persists its own geometry + session layout, but on
  relaunch only the **last active window's geometry** (size + location) is
  restored, into a **single fresh window**. Sessions always start fresh — the saved
  session layout is never restored.
- **Last window:** closing the last window quits the app. Closing one window while
  others remain open leaves those windows and sessions untouched.

## 2. Current implementation

- `Main.qml` is an invisible `Item` manager. It owns shared `AppSettings`, the
  app-level dialogs, the `TerminalWindow` component, and the list of open windows.
- `TerminalWindow.qml` is the per-window `Window`; it owns one `TerminalManager`,
  its `TerminalWorkspace`, translucency/screen helpers, save dialog, menu, and
  window-scoped shortcuts.
- `TerminalManager` is a regular `QML_ELEMENT`, not a singleton. Each window owns
  independent workspaces, sessions, PTY workers, and split layouts.
- `WindowRegistry` tracks the live managers for app-wide quit confirmation.
- `WindowTranslucency` and `WindowScreen` are per-window C++ `QML_ELEMENT`s.
- Geometry + session layout live in a per-window `WindowState` `QML_ELEMENT`
  (no longer in `AppSettings`, which now holds only font + translucency). Each
  `WindowState` persists to its own `windows/<id>.json`; the app-level
  `WindowStateStore` singleton owns the persisted collection of open window IDs
  (`windows/index.json`) and the ID of the last active window
  (`windows/last.txt`). On relaunch only that last active window's geometry is
  restored, into a single fresh window — sessions always start fresh.
- `main.cpp` enables Qt quit on the last window. The AppKit delegate returns `NO`
  from `applicationShouldTerminateAfterLastWindowClosed:` so Qt is the sole
  owner of that shutdown transition.
- When a manager loses its final session, it requests closure of its own window.
  Closing the final app window then exits the app.

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

Mapping to Konsoll:

| iTerm2 | Konsoll |
|---|---|
| `AppController` (owns the windows) | `Main.qml` manager root + C++ `WindowRegistry` |
| `WindowController` + `NSWindow` | `TerminalWindow.qml` (a `Window`) |
| `TabController` → `TabGroup` → `Terminal` | `TerminalManager` → workspaces → split tree → `TerminalBackend` (sessions) |
| App-level `NSMenu` → key window | Per-window QML `MenuBar`, shown while that window is active |
| Window state restoration | Per-window state persistence (Phase 4) |
| Quit after last close | Qt `setQuitOnLastWindowClosed(true)`; AppKit's automatic path is disabled |

## 4. Target architecture

- **`Main.qml`** becomes a **manager root**, not a `Window`. It is a plain (invisible)
  `Item` that:
  - owns a `Component` — the **window template**;
  - creates the first window on completion and tracks the live window list;
  - exposes `openWindow()` / `closeWindow()`;
  - holds the shared `Settings` (font, translucency alpha/blur) and the app-global
    native `AboutDialog`.
- **`TerminalWindow.qml`** (new) is the **window template** — a `Window` containing
  everything that is per-window today: its own `TerminalManager` (the
  `SessionManager` for that window), `TerminalWorkspace`, `WindowTranslucency`,
  `WindowScreen`, the per-window `SaveTextDialog`, the `MenuBar`, and the
  window-scoped shortcuts.
- **`TerminalManager`** becomes **per-window**: a regular `QML_ELEMENT` (no longer
  `QML_SINGLETON`). Each `TerminalWindow` instantiates exactly one.
- **`WindowRegistry`** (new C++ app-level singleton) tracks the live
  `TerminalManager`s so the AppKit quit guard can aggregate across all windows.

Why the manager, not a `Window`: a `Window` is one window. To own *several*
windows, `Main` must be the thing that creates them — the same reason iTerm2 has
an app-level controller above the per-window `WindowController`s. The menu follows from
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
4. **Quit after last close.** Qt's `setQuitOnLastWindowClosed(true)` owns this
   transition. The AppKit delegate returns `NO` from
   `applicationShouldTerminateAfterLastWindowClosed:` to avoid a second,
   competing termination path.
5. **Settings split.** Font + translucency alpha/blur are app-global (shared).
   Window geometry + session layout are per-window (restored, Phase 4).
6. **Dialogs split.** `AboutDialog` (native AppKit panel) is app-global, owned by
   the manager. `SettingsDialog` is a Qt Quick `Popup`, which only opens inside a
   window, so each window owns one bound to the shared `Settings` (a single
   instance in the windowless manager `Item` silently never opened).
   `SaveTextDialog` is per-window (it acts on that window's session).

## 6. Implementation phases

Each phase is independently buildable; later phases build on earlier ones.

### Phase 1 — Per-window session model (C++) — implemented

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

### Phase 2 — QML window template + manager root — implemented

Files: `Main.qml`, new `TerminalWindow.qml`, `TerminalWorkspace.qml`,
`TerminalPaneLayout.qml`, `TerminalView.qml`, `controls/TerminalTabButton.qml`,
`controls/TerminalTabStrip.qml`, `CMakeLists.txt`.

- Extract the body of `Main.qml` into `TerminalWindow.qml` (a `Window`): its
  `TerminalManager { id: sessionManager }`, `TerminalWorkspace`,
  `WindowTranslucency`, `WindowScreen`, `SaveTextDialog`, `MenuBar`, and the
  window-scoped shortcuts.
- `Main.qml` becomes an invisible `Item` manager: a `Component` for the window
  template, first-window creation on completion, the live window list,
  `openWindow()`/`closeWindow()`, the shared `Settings` and the app-global
  `AboutDialog`.
- Thread the per-window `sessionManager` through
  `TerminalWorkspace` → `TerminalPaneLayout` → `TerminalView` →
  `TerminalTabButton`, replacing the global `SessionManager` references with the
  passed-in manager.
- Pass each window a back-reference to the manager (`windowManager`) for the
  "New Window"/"Close Window" menu items.
- `CMakeLists.txt`: add `TerminalWindow.qml` to `QML_FILES`.

### Phase 3 — App-level window management + lifecycle — implemented; needs manual acceptance pass

Files: `src/AppQuitGuard.mm`, `main.cpp`.

- `AppQuitGuard`:
  - `applicationShouldTerminateAfterLastWindowClosed:` → `NO`; Qt owns last-window shutdown.
  - `applicationShouldTerminate:` aggregates across all windows (Phase 1 registry).
- `main.cpp`: set `setQuitOnLastWindowClosed(true)`.
- Add the window menu items / shortcuts: ⌘N (new window), ⌘⌥W (close window).
- `⌘N` creates another window while at least one window is open. Closing the last
  window exits the app; opening it again from the Dock starts a fresh app instance.
- The latest shutdown adjustment makes Qt the only last-window termination path;
  verify manually that closing the last window exits cleanly without a delayed
  crash report.

Implementation notes: `Main.qml` owns the window template and live list;
`TerminalWindow.qml` owns its `TerminalManager`. New windows are shown on
creation, the close button removes only that window, and app termination is
delegated to the AppKit guard so its confirmation aggregates all managers.
When a manager loses its final session, it requests closure of its own window;
that event closes that window only; the app exits naturally when it was the last
window.

Confirmation rules (refined):
- **Close a window** (`onClosing`) — confirm only when the window holds more than
  one session/tab **or** its single session is blocked by a running command; a
  lone idle tab closes without prompting. Only that (active) window's own
  `sessionManager` is consulted, so the other windows are left untouched.
- **Quit** (⌘Q / menu) — **always** confirms, aggregating the total session count
  across **all** windows (`WindowRegistry::totalSessionCount()`), even when each
  window holds a single idle tab.

### Phase 4 — Per-window state restoration (iTerm2-style) — done

Files: `src/WindowState.h/.cpp` (new), `src/WindowStateStore.h/.cpp` (new,
app-level singleton), `TerminalWindow.qml`, `Main.qml`, `TerminalManager`
(layout serialization), `AppQuitGuard.mm`. `AppSettings.qml` no longer holds
geometry keys (they moved into `WindowState`).

Done:
- **Per-window `WindowState`** — per-window `QML_ELEMENT`
  (`window`/`manager`/`windowId`/`width`/`height`/`x`/`y`/`layout`/`hasLayout` +
  `load()`/`save()`/`removeState()`), registered in `CMakeLists.txt` and
  instantiated per `TerminalWindow`. Persists width/height/x/y + `QScreen` name +
  serialized layout to its own `windows/<id>.json`. `TerminalWindow` records
  geometry on change (only while `Windowed`, so maximized/fullscreen sizes don't
  clobber the restorable dimensions) and restores it in `Component.onCompleted`.
- **App-level `WindowStateStore`** — `QML_SINGLETON` owning the persisted
  collection of open window IDs (`windows/index.json`): `listWindowIds()`,
  `generateWindowId()`, `saveCollection(ids)`, and a C++-owned `terminating()`
  flag (set only on a confirmed quit by `AppQuitGuard`) so `windowClosed()` can
  distinguish a mid-session close (removes the ID + deletes the file) from a
  quit-time close (skips disk ops).
- **Stable per-window IDs** — `Main.qml::openWindow(windowId)` generates a UUID
  when none is passed; each window is keyed on its own ID.
- **C++ layout serialization** — `TerminalManager::serializeState()` /
  `restoreState(const QJsonArray &state)` + `serializeLayout()` /
  `buildLayoutNode()` capture workspaces, custom titles, split trees
  (orientation + children), and each session's working directory;
  `restoreState()` recreates sessions via `session->start(dir)`.
- **Save/restore wiring** — only the **last active window's** state is kept on
  disk, and it is written whenever it changes so every way the app ends (last
  window closed, Quit menu, Dock quit, logout) leaves it current:
  `Main.qml::noteActiveWindow()` saves the newly active window (state file +
  `last.txt`) and removes the previously active window's file; `TerminalWindow`
  saves its geometry 500 ms after a move/resize while it is the last active
  window and windowed (fullscreen/maximized geometry is ignored);
  `windowClosed(w)` saves `w` when it is the last window (closing it ends the
  app), otherwise removes its file and re-points the last active window at a
  remaining one; during a confirmed quit (`terminating()`) it does nothing,
  since `requestQuit()` → `saveLastActiveWindow()` already ran.
  `Component.onCompleted` reopens a single window keyed on the last active
  window's ID (or a fresh ID when none).
  `TerminalWindow` calls `windowState.load()` and restores only the geometry
  (width/height/x/y) in `Component.onCompleted` — it never calls
  `sessionManager.restoreState()`, so sessions always start fresh.
- **Robustness** — a missing/corrupt `windows/<id>.json`, a missing/corrupt
  `index.json`, and a missing/unknown `last.txt` are all handled gracefully (a
  single fresh window opens).

### Phase 5 — Acceptance + polish — pending

- [ ] Manual pass over §8; fix focus, translucency, and menu-dispatch regressions.
- [ ] Verify last-window close exits without the delayed crash report mentioned
  during implementation; this has not yet been reproduced after the shutdown fix.
- [ ] Verify shutdown terminates and reaps every shell across all windows.

## 7. Risks and open details

- **App-global event filter — resolved.** The manager commits activation history
  only when its own `QWindow` is focused.
- **Window menu lifecycle — resolved.** The app exits when no windows remain, so
  there is no windowless state that needs an in-process reopen action.
- **`Settings` (QSettings) location.** It lives in `Main.qml`'s manager root and is
  shared by all windows; keep the `category`/keys unchanged.
- **Dialog parenting.** `SettingsDialog` is per-window (verified: Cmd+, and
  Settings… open it over that window); `AboutDialog` is the native app-global
  panel; `SaveTextDialog` is per-window and should restore focus to that
  window's terminal.
- **C++ rules.** Keep `WindowRegistry` conformant: smart pointers
  only, no exceptions, `std::` algorithms, members ordered heaviest-alignment first
  (Debug enables `-Wpadded`), and clang-tidy clean (cognitive complexity, no
  do-while, no `NOLINT`).
- **Restoration scope.** Per-window layout serialization (split tree + working
  directories) is implemented in C++ (`serializeState`/`restoreState`) but is no
  longer used on relaunch: only the last active window's geometry is restored,
  into a single fresh window. Keep that geometry-only restore a focused, testable
  unit; the layout serialization remains available should a full restore be wanted
  again.

## 8. Acceptance criteria

- ⌘N opens a new window with its own fresh session; windows are independent
  (commands/output do not leak across windows).
- Each window's menu bar is the app menu bar while that window is focused; menu
  actions act on that window's sessions.
- Closing a window (red button / ⌘⌥W) closes only that window's sessions; other
  windows keep running.
- Closing the **last** window quits the app. Closing any other window leaves the
  remaining windows and sessions intact.
- Closing the last workspace/session in one window closes only that window; other
  windows remain intact. Closing the final window exits the app cleanly.
- Closing a window with more than one session/tab, or with a single session blocked
  by a running command, prompts for confirmation; a lone idle tab closes without
  prompting. Only the active window's sessions are considered.
- App quit (⌘Q / menu) **always** confirms, aggregating the total session count
  across **all** windows — even when each window holds a single idle tab.
- Translucency tint follows each window's own active session background; alpha/blur
  settings are shared.
- After quit + relaunch, a single fresh window opens with the last active window's
  geometry (size + location); sessions always start fresh (the saved layout is not
  restored).
- Shutdown terminates/reaps every shell in every window (no orphaned processes).
