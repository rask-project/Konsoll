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
  Over ssh (`SshIntegration`): the `ssh` function of the zsh/bash
  integration (`shell-integration/konsoll-ssh.sh`) sends, for an
  interactive login only, `ssh-remote-command` (a base64 POSIX bootstrap
  Konsoll writes into `shellIntegrationDirectory()`) plus
  `KONSOLL_REMOTE_AGENT_TOKEN` (`remoteAgentToken`, derived from the
  session's: the host never gets the socket's token). The bootstrap unpacks
  the integration into a private temporary directory (pid-owned, removed at
  exit) and execs the login shell with `KONSOLL_REMOTE`; there
  `konsoll-remote.sh` wraps claude/codex/qwen/opencode to pass Konsoll's
  hooks for that run (Claude `--settings`, Codex `-c hooks=`
  `codexHooksToml()`, Qwen `QWEN_CODE_SYSTEM_DEFAULTS_PATH`) and the remote
  `konsoll-agent-hook` (`konsoll-remote-hook`, POSIX sh) writes
  `OSC 1337 ; KonsollAgent=<token>;<agent>;<state>[;text=|keys=;event=]` to
  the agent's tty (`KONSOLL_TTY`) in one write; `parseRemoteAgentReport`
  checks the token. Nothing is installed on the host.
  New Agent in Worktree (Shell menu) asks with
  `NativeDialogs::promptAgentWorktree`; `AgentIntegrations::worktreePlan`
  builds the command (`git worktree add` beside the repository, then the
  agent) that `TerminalManager::addWorkspaceRunning` types in a new tab.
  The item is inserted in the menu only while the active pane is in a local
  repository (`isInGitRepository`, stats only; not a `remote` pane): native
  menus ignore `MenuItem.visible`.
  The script is zsh (`zsocket`): `nc` can linger after the write.
- **SSH file transfers** (`SshTransfers`): `_konsoll_ssh` reports the ssh
  arguments as typed (`RemoteHost=<token>;<target>;args=<base64, NUL-
  separated>`, `parseRemoteHost`), and the shell set up over ssh reports
  its directory (`OSC 1337 ; KonsollRemoteDir=<remoteAgentToken>;<path>`,
  kept in the directory marks as `remoteDirectory`). Each `TerminalBackend`
  over ssh has one `SshConnection`: its own master (`ssh -M -f -N`,
  ControlPersist 10 min, a socket in `$TMPDIR`, closed with `-O exit`) opened
  with the user's options; transfers are `sftp -b -` over it (paths escaped,
  never quoted: sftp keeps a backslash inside quotes), existence checks one
  `ssh … sh -s` per batch. A master that needs a password is only opened
  for a transfer: `SSH_ASKPASS` is Konsoll itself (`KONSOLL_ASKPASS_TOKEN`
  set: `runSshAskpass`), asking the app over the agent socket
  (`SshAskpass`, a token per connecting master) and
  `NativeDialogs::answerSshPrompt` (the answer is passed on, never kept).
  Pointer moves probe quietly (BatchMode) and never prompt; when the host
  refuses that, Cmd on its output hints at Cmd+click, which signs in
  (`signInHintAt`, `signInAt`). A probe is killed after 8 s (one stuck
  check would hold the rest), and an expired answer stands while it is
  checked again. `QT_LOGGING_RULES="konsoll.remotefiles.debug=true"` logs
  connections and checks. Drops go into `remoteDirectory()`, or to
  `/tmp` with the path typed for an agent or an unknown directory.
  Cmd+click opens on release so that Cmd+drag can drag a link
  (`dragLinkAt`, `FileDrag.mm`): a local file as its URL, a remote one
  (`konsoll-remote:` links, shown once the host said it exists) as an
  `NSFilePromiseProvider` fetched with `sftp get -R` where it is dropped.
  `--selftest` runs real transfers, links in a real remote zsh/bash and
  the sign-in when `KONSOLL_SELFTEST_SSH_HOST` (and
  `KONSOLL_SELFTEST_SSH_CONFIG`) name a key-only host on this machine (a
  user-run `sshd -f` on a high port will do).
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
- **Text links** (`TextLinks`, Cmd+hover / Cmd+click): besides OSC 8,
  `TerminalBackend::linkAt` finds URLs and existing file paths in the
  soft-wrapped line under the pointer, on demand only (nothing is scanned as
  output arrives). `textLinkCandidates` is pure (word, `path:line:col`,
  git's `a/` `b/`, quotes, words joined across single spaces);
  `FileLinkResolver` checks them with cached `lstat`s and never touches a
  slow volume (`getfsstat(MNT_NOWAIT)` mount table: network, autofs, FUSE),
  symlink targets included. Relative paths resolve with
  `Terminal::directoryAtLine`: directory marks anchored on
  `Screen::lineAnchor` (remapped on reflow) record the directory each line
  was printed in; an empty mark is remote output (RemoteHost, a foreign
  OSC 7 host, a Remote foreground job), where only URLs link.
  `fileOpenAction` never runs anything (executables, scripts, installers
  are opened as text or revealed); `LinkSettings` holds the editor command
  (Settings > Terminal), a terminal editor runs in a new tab
  (`editorTabRequested`). Option+click on a folder types `cd '<folder>'`
  (`changeDirectoryToLinkAt`) only at an empty prompt (`atEmptyPrompt`: no
  foreground job, cursor still at OSC 133 ; B); Option+hover shows only
  those folders (`folderLinkAt`). The zsh `chpwd` hook and fish's
  `--on-variable PWD` send OSC 7 at once (to a terminal stdout only), so
  `cd src && ls` resolves in `src`; bash reports at the prompt.
  Cmd resting 350 ms on an image, PDF or video link shows a preview
  (`ImagePreview.mm`): `previewSourceFor` filters by extension and gives an
  `image://konsoll-preview/` source (path + mtime) that
  `ImagePreviewProvider` answers with a QuickLook thumbnail
  (`QLThumbnailGenerator`, 2 s limit) on the QML loader thread. Nothing
  without Cmd, by design: no previews on plain hover.
- **Updates** (`Updater`, QML singleton, Sparkle 2 pinned in
  `cmake/Sparkle.cmake` and embedded in `Contents/Frameworks`): Sparkle's
  standard UI reads the appcast (`SUFeedURL`, the `appcast` branch, written
  by `release.yml` with `tools/update-appcast.py`) and checks the EdDSA
  signature (`SUPublicEDKey`; the bundle is only ad-hoc signed). Tags with a
  suffix go in the `beta` channel, offered when Settings ▸ Updates allows it
  (`allowedChannelsForUpdater:`). Checking and downloading are on by
  default (`SUEnableAutomaticChecks`, `SUAutomaticallyUpdate`): updates
  install on quit. `CFBundleVersion` is derived from the tag
  (`-beta.N` → `X.Y.Z.(100+N)`, stable → `X.Y.Z.1000`) so Sparkle orders
  pre-releases first. "Check for Updates…" is inserted into the native app
  menu by `Updater::start()`: a QML item in the app-named `Menu` that Qt does
  not recognize (About/Settings/Quit) would open a second app menu. No feed
  URL or public key (Debug builds) leaves the updater off.
- **Open at a folder from the Finder** (`FinderIntegration`, QML
  singleton): the `NSServices` "New Konsoll Tab/Window Here" (Info.plist,
  message `openFolder`) and folders opened with the app
  (`CFBundleDocumentTypes` `public.folder`, rank Alternate → QFileOpenEvent)
  emit `openRequested`, which `Main.qml` answers with a tab or a window
  (`addWorkspaceInDirectory`). A launch for a folder replaces the launch
  window's untouched tab. `AppQuitGuard`'s delegate replaces Qt's, so it
  forwards `application:openFiles:` to Qt.
- **Neovim (Konsoll)** (`EditorMode`, QML singleton): a helper bundle in
  `Contents/Helpers` (`cmake/EditorInfo.plist.in`: bundle id `….nvim`, its
  own name and document types) runs a copy of the executable, which detects
  the bundle id (or `--editor [PATH…]`) and loads `EditorMain.qml`: one
  `EditorWindow.qml` per file or folder, a `TerminalManager` with one
  session started with `addCommandWorkspace` (`$SHELL -i -c 'exec nvim'`),
  no tabs, splits, Finder Services or updater. Settings are shared (QSettings
  is keyed by the application name). Closing a window or quitting types
  `:confirm qa` (`quitKeys`) and waits for nvim to exit; the quit guard
  cancels the first terminate. Konsoll registers the helper with Launch
  Services (`registerHelperApplication`). Its document types
  (`cmake/EditorDocumentTypes.plist.in`: the text types macOS declares, less
  those another app edits better, plus `UTImportedTypeDeclarations` for
  extensions macOS has no type for) and Konsoll's Services and folder type
  (`cmake/FinderKeys.plist.in`) enter the plists only with
  `KONSOLL_FINDER_INTEGRATION` (off in Debug: Launch Services lists every
  copy it has seen, so a development build would duplicate them).
  `makeDefaultEditor` uses `LSSetDefaultRoleHandlerForContentType`: NSWorkspace's
  replacement asks the user to confirm each type. The build tree's helper finds
  Sparkle through the extra `@executable_path/../../../../Frameworks` rpath;
  at install `cmake/EditorHelper.cmake` copies the deployed executable,
  repoints its `@loader_path/../Frameworks` references to the outer bundle
  and writes a `qt.conf` (plugins, QML imports) before PruneBundle signs.
- **App icons** (`cmake/AppIcons.cmake`): `assets/konsoll.icon` and
  `assets/konsoll-nvim.icon` (Icon Composer documents: `icon.json` + SVG
  layers) are compiled by `actool` into `Assets.car` (`CFBundleIconName`,
  what macOS 26+ draws) plus a `.icns` for older systems. A plain `.icns`
  whose shape is not the system's rounded square is shrunk onto a gray tile
  there. Without Xcode the legacy `assets/*.icns` are used.
- **Dock menu** (`DockMenu`, QML singleton): "New Tab" / "New Window"
  above AppKit's window list. Qt's `applicationDockMenu:` is lost with its
  delegate, so `AppQuitGuard`'s answers it with `DockMenu::menu()`;
  `Main.qml` answers the signals (a tab in the last active window).
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
- **Ownership:** every owned object has exactly one owner, a
  `std::unique_ptr` (or a plain value member). Ownership moves with
  `std::move`; code that only uses an object gets a non-owning
  `const T *` (or a reference) and never keeps it past the owner's lifetime.
- **`std::shared_ptr` is forbidden** (and with it `std::make_shared`,
  `std::weak_ptr`, `QSharedPointer`). Shared ownership is never the answer:
  if the logic seems to need it, rethink it so one object owns the thing
  and the others borrow it (look it up by id/serial when they need it, hand
  the owner the callback, keep the state in the owner or in the one lambda
  that uses it).
- **Raw pointers are forbidden** — no `new`/`delete`, no owning raw
  pointers. Exceptions: the non-owning `const T *` above, and non-owning
  pointers/references required by C/POSIX/Qt APIs (e.g. `getpwuid_r`,
  `ioctl`, `forkpty`). Qt Quick
  scene-graph nodes (`updatePaintNode`) are the one ownership hand-off: create
  them with `std::make_unique` and pass `release()` to the parent node, which
  owns them (see `src/TerminalRenderer.cpp`). QML image providers are the
  other: `QQmlEngine::addImageProvider` takes a `release()`d `unique_ptr`
  and owns it (`main.cpp`).
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
