# AGENTS.md

Guidance for AI agents working in this repository.

## Project

- Konsoll — a native macOS terminal built in QML (Qt Quick).
- Toolchain: C++23, Qt 6.12, CMake.
- Target platform: macOS (Apple Silicon).
- The README's feature list is the current scope: multiple windows, tabs
  (workspaces), split panes, selection and search, mouse reporting, image
  protocols, shell integration, Settings and Background Commands. New
  features build on that; `docs/RELEASE.md` covers signing and notarization.
- Implement only what real programs use: obsolete VT features (page memory,
  DECDLD soft fonts, ReGIS) are deliberately left out.
- Open backlog: color profiles/themes, font ligatures and a configurable line
  height, QML tests (`qmltestrunner`) for `TerminalView`.

## Architecture

- **One `TerminalManager` per window** (`TerminalWindow.qml`) owns that
  window's sessions (`TerminalBackend`) and workspaces (a binary split tree
  of `TerminalLayoutNode`s). `WindowRegistry` tracks the live managers for
  app-wide questions (quit guard, closing a session whose workspace moved).
  `Main.qml` is the app root: the window list, moving workspaces and panes
  between windows, and the app-wide Settings and Background Commands windows.
- **Moving sessions keeps them running.** Workspaces and panes move between
  managers (`moveWorkspaceTo`, `movePane`, `movePaneToWorkspace`) and to the
  background (`releaseSession` / `releaseWorkspace`, back with
  `adoptSession`) by handing over the `std::unique_ptr<TerminalBackend>`;
  the PTY and its thread are untouched.
- **Background Commands** (`BackgroundJobs`, a QML singleton owned by
  `main()`): `BackgroundJob`s own sessions shown without a tab — commands run
  as `$SHELL -i -c` (status = the command's exit code) or tab shells sent
  there — plus the saved commands (QSettings `BackgroundCommands/saved`).
  `BackgroundWindow.qml` is its UI.
- **Shell functions** (`ShellFunctions`, QML singleton): functions defined in
  Konsoll are kept in QSettings and written as `functions.{zsh,bash,fish}` +
  `functions.serial` in `shellIntegrationDirectory()`. The integration
  scripts (`shell-integration/`) source them at startup and re-source at a
  prompt when the serial changes, so open tabs and `-i -c` background
  commands see them. `main()` rewrites them before the first shell starts.
- **Scrollback** (`src/term/Scrollback`): history lines are stored
  compacted (style runs + varint code points, trailing blanks dropped, width
  kept) in `mmap`ed 256 KiB blocks, so unlimited history stays cheap and
  freed blocks go back to the system (macOS malloc keeps freed memory).
  `Screen::lineAt` returns lines by value; per-frame callers use `copyLine`.
  Reflow streams the history block by block; when the width is unchanged
  (height change, leaving the alternate screen) only the screen is
  re-wrapped; past 50k history lines `TerminalView` applies a width change
  once the window size settles (120 ms), not on every drag step. The limit
  is `ScrollbackSettings` (0 = unlimited).
- **AI agent status** (`AgentStatus`): agents report their state from
  their hooks with `shell-integration/konsoll-agent-hook` (exported as
  `KONSOLL_AGENT_HOOK`). Hooks may run without a controlling terminal
  (Claude Code starts them in a session of their own), so they cannot write
  escape sequences: the script sends `<token> <agent> <state> [base64
  message]` to `AgentStatusServer`'s Unix socket (`KONSOLL_AGENT_SOCKET`,
  user-only), and the `TerminalBackend` whose token
  (`KONSOLL_SESSION_TOKEN`) matches takes it. The state clears when the
  command ends (OSC 133 ; D) or the prompt returns. The workspace shows the
  most urgent pane's state; `TerminalManager` raises tab attention and posts
  a notification (unless the pane is in view) whose click finds the session
  by token (`WindowRegistry::activateSessionWithToken`). While an agent
  reports, its own OSC 9 notifications are dropped (they would repeat ours).
  A permission prompt reports `approval` (questions stay `waiting`): its
  notification has Allow / Deny actions answered by
  `TerminalBackend::answerApproval` (Enter / Escape into the pane). The
  prompt sent with `working` (`@prompt`) is kept as the agent's task; the
  Dock badge counts waiting agents (`WindowRegistry::scheduleAgentBadgeUpdate`).
  Changing the hook specs makes installed hooks outdated (`agentHooksCurrent`):
  Settings offers Update.
  Send to Agent (the pane's context menu) pastes the selection or the last
  command block (`lastCommandText`) into an agent from `TerminalManager::agentSessionsFor`
  (the same tab's first; several become a submenu) with `pasteText`.
  New Agent in Worktree (Shell menu) asks with
  `NativeDialogs::promptAgentWorktree`; `AgentIntegrations::worktreePlan`
  builds the command (`git worktree add` beside the repository, then the
  agent) that `TerminalManager::addWorkspaceRunning` types in a new tab.
  The script is zsh (`zsocket`): `nc` can linger after the write.
- **Agent integrations** (`AgentIntegrations`, QML singleton, Settings ▸ AI
  Agents): merges Konsoll's hooks into Claude Code / Codex / Qwen Code JSON
  settings (all use `{"hooks": {Event: [{matcher, hooks: [command]}]}}`;
  Konsoll's commands are recognized by `KONSOLL_AGENT_HOOK`), never touches a
  file that is not plain JSON, keeps a `.konsoll-backup`, and installs the
  opencode plugin (`integrations/opencode/konsoll.js`, a resource). The
  Claude Code plugin (`integrations/claude-code`, listed in
  `.claude-plugin/marketplace.json`) carries the same hooks as
  `claudeHookSpecs()` — change both together. A pane whose foreground
  process is a known agent that is configured but not connected shows
  `controls/AgentConnectBanner.qml` (`connectOffer`); "Not now" is kept in
  QSettings (`AgentIntegrations/declinedOffers`).
- **MCP server** (`McpServer`, `konsoll --mcp`, no GUI): stdio JSON-RPC
  started by an agent; it inherits the agent's environment (Codex needs
  `env_vars` for that), so `KONSOLL_SESSION_TOKEN` names the agent's pane.
  `tools/call` goes to the app as a JSON line on the agent socket
  (`AgentStatusServer::setRequestHandler`), answered by `AgentCommands`:
  `open_pane` (`TerminalManager::openPaneBeside`, never takes the focus),
  `run_in_background` (`BackgroundJobs::run`) and `background_job_status`
  (only jobs that token started), `recent_commands` (the agent's own pane:
  OSC 133 command blocks with their status and output), `list_panes` (the
  window's panes, no output) and `read_pane`, which waits for the user:
  `TerminalBackend::requestRead` shows `controls/AgentReadRequestBanner.qml`
  and sets the agent's state to `approval`, so the notification's Allow /
  Deny answer it too. The socket's request handler replies asynchronously
  for it. Nothing reads another pane without that. stdout carries only
  protocol messages.
- **Threading:** each session has a `PtyWorker` on its own `QThread` that
  alone touches the PTY descriptor, its notifier and the child process. PTY
  writes, resizes and shutdown go to it as queued calls; output comes back as
  signals with backpressure (reads pause above 1 MiB undelivered). Output is
  batched: at most two blocks are in flight to the GUI thread and what is
  read meanwhile is sent as one block (a PTY read returns a few bytes under
  a burst, and each block costs the GUI a full `onData` pass). Parsing
  and the `Terminal`/`Screen` model stay on the GUI thread — never share
  mutable `Terminal` or `Screen` state across threads.
- **Session lifecycle:** `Created → Starting → Running → Stopping → Stopped`
  (or `Failed`). Close is bounded and never blocks the worker: SIGHUP every
  process group of the shell's session (its jobs run in groups of their
  own), close the master fd, poll the reap; after a 1.5 s grace SIGKILL what
  is left of the session, and 1 s later release it regardless. The GUI waits
  at most 3 s for the thread and otherwise keeps the objects and retries
  when it finishes. Objects that
  queued signals or QML may still reference are destroyed with
  `deleteLater()`, and QML-supplied session pointers are matched by address
  (they may dangle), never `qobject_cast`.
- **Fork/exec:** the child environment is built before `forkpty`; only
  async-signal-safe calls run between fork and exec. The child closes every
  inherited fd above 2 and the master is `FD_CLOEXEC`: a master leaked into
  another shell keeps its terminal from hanging up, so its tab cannot close.
- **PTY input** is never dropped: what the input queue (~1 KiB) cannot take
  is buffered and written as it drains (a lost bracketed-paste end marker
  leaves the program stuck in paste mode).
- **State on relaunch:** only the last active window's geometry is restored,
  into one window with fresh sessions.

## QML

- System Settings-style windows (Settings, Background Commands) share
  `controls/SettingsSidebarItem.qml`, `controls/FormGroup.qml` and
  `controls/FormRow.qml`; reuse them for new panes of that style.
- The tabs are drawn with `controls/GlassSurface.qml`, a glass look emulated
  in QML (the strip behind them is native Liquid Glass where available). Tab
  indicators are static icons (`resources/icons/konsoll/svg/kind-*.svg`, one
  per `ProcessKind`, plus `kind-running`): nothing in the tab strip may
  animate continuously, as every animated frame renders the whole window.

## C++ rules

- **C++23** is required (enforced in `CMakeLists.txt`).
- **Qt 6.12.**
- **Ownership:** always use smart pointers (`std::unique_ptr` /
  `std::shared_ptr`).
- **Raw pointers are forbidden** — no `new`/`delete`, no owning raw
  pointers. Sole exception: non-owning pointers/references required by
  C/POSIX/Qt APIs (e.g. `getpwuid_r`, `ioctl`, `forkpty`). Qt Quick
  scene-graph nodes (`updatePaintNode`) are the one ownership hand-off: create
  them with `std::make_unique` and pass `release()` to the parent node, which
  owns them (see `src/TerminalRenderer.cpp`).
- **Exceptions are forbidden** — no `throw`, `try`, or `catch` anywhere in
  the codebase.
- **Error handling:** fallible operations return `std::expected<T, E>`
  (C++23) with a concrete error type.
- **Algorithms:** always use `std::` algorithms (`<algorithm>`,
  `<ranges>`) instead of hand-rolled loops.
- **Member ordering / alignment:** order class data members from largest
  alignment requirement to smallest (heaviest/most-aligned types first,
  scalars and single-byte fields last) so the compiler inserts the least
  padding. Group by alignment: 16-byte-aligned types (e.g. `std::array` of
  SIMD/`QRgb` runs, `QVector`/`std::vector` of large structs) before
  8-byte (`int`, pointers, `std::unique_ptr`), before 4-byte, before 1-byte
  (`bool`, enums, `unsigned char`). Booleans/flags that fit in a byte should
  be packed, not given a full field each. Debug builds enable `-Wpadded` to
  flag any layout that still wastes padding — keep it clean.

## Formatting

- **Run `tools/format-code.sh` after every change**, before building or
  committing. It formats C++ and Objective-C++ (`.cpp`, `.h`, `.mm`) with
  clang-format (`.clang-format`) and QML with qmlformat (`.qmlformat.ini`).
- `tools/format-code.sh --check` changes nothing and exits 1 listing the files
  that need formatting; `--changed` limits it to files changed against HEAD.
- QML follows qmlformat's defaults (the QML Coding Conventions, as Qt
  Creator formats it): 4-space indent, no line wrapping, attribute order kept.
  Don't hand-wrap long bindings — qmlformat joins them back.

## clang-tidy

- **Follow the clang-tidy rules proactively.** Before writing or changing C++,
  read `.clang-tidy` and write code that already conforms to its checks, so you
  don't need to refactor afterward. The config is the source of truth for the
  rules — `WarningsAsErrors: "*"` makes every enabled check an error.
- Key enforced rules (see `.clang-tidy`):
  - `readability-function-cognitive-complexity` — threshold 25; keep functions
    under it (extract helpers, table-drive, early-return).
  - `misc-no-recursion` — no recursion; rewrite to iterative form with an
    explicit stack.
  - `cppcoreguidelines-avoid-do-while` — no `do { … } while (…)`; use a `while`
    loop.
  - No `NOLINT` to suppress findings — refactor the code to fix the finding.
  - Flag/mask types: `inline constexpr unsigned int` constants in a namespace,
    unsigned literals with the `u` suffix (keeps `bugprone-signed-bitwise` quiet).
- Verify per file (uses the build's `compile_commands.json`):
  `clang-tidy -p build/Qt_6_12_0_for_macOS_Debug --config-file=.clang-tidy <file>`.
- **`.mm` caveat:** clang-tidy 19 segfaults on every `.mm` file while running
  `cppcoreguidelines-avoid-const-or-ref-data-members` (known AST-matcher bug, not
  a code issue). Run `.mm` files with that single check disabled; all other checks
  run normally.

## Native macOS interop

- Some window behavior is native AppKit, done in **Objective-C++**
  (`src/*.mm`; `CMakeLists.txt` sets `LANGUAGES CXX OBJCXX`). On macOS
  `QWindow::winId()` is the window's `contentView` (`NSView*`), not the
  `NSWindow` — get the window via `[nsView window]`.
- **Configurable native behavior is exposed to QML as a C++ QML type**
  (`QML_ELEMENT` in `namespace konsoll`), and QML binds its properties —
  e.g. `WindowTranslucency` (`window`/`color`/`alpha`) drives the
  see-through window. Don't reach into native code from QML directly; add a
  property to such a type instead.
- AppKit colors expect **0.0–1.0** components: use `QColor::redF()`/
  `greenF()`/`blueF()`, not `red()`/`green()`/`blue()` (which return 0–255
  ints and clamp to white).

## Testing the terminal

- `konsoll --selftest` runs the parser checks (`src/parserselftest.cpp`), the
  recorded fixtures and the session tests. Run it after any change to
  `src/term/`, the key encoder or the renderer.
- `tests/fixtures/NAME-ROWSxCOLS.raw` is a program's raw output and
  `.expected` the screen it must produce. Record new ones with
  `tools/vtdrive.py --raw FILE ... -- PROGRAM`, regenerate with
  `tools/regress.py --update` and review the diff before committing.
- `konsoll --screen-dump ROWS COLS [--attrs]` prints what stdin draws;
  `konsoll --render-dump ROWS COLS OUT.png` renders it with the real
  `TerminalRenderer` — use it to check rendering changes visually.
- `tools/vtdrive.py` drives a program (vttest, nvim) in a pty, answers its
  queries with Konsoll's replies and dumps the screen.
