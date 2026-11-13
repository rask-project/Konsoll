# README screenshots and GIFs

The README's "Using Konsoll" guide references the images below in
`docs/images/`. This file lists what each one shows and how to capture it so
they can be retaken whenever the UI changes.

## Prerequisites

- **Permissions** for the terminal that runs the script, in **System
  Settings ▸ Privacy & Security**: *Screen & System Audio Recording*
  (`screencapture`) and *Accessibility* (keys and clicks). Restart the
  terminal after granting them. Don't run the script from Konsoll itself:
  quitting or restarting it would end the session.
- Xcode command-line tools (`swiftc`), `ffmpeg` (`brew install ffmpeg`) and a
  configured build directory of the app.

## The `docshot.sh` script

`tools/screenshots/docshot.sh` does every step: it builds an isolated copy of
Konsoll, launches it with a demo environment, drives it, and captures and
converts the images. Run it without arguments for the full command list.

| Command | What it does |
| --- | --- |
| `setup [BUILD_DIR]` | Builds the copy (default build dir `build/Qt_6_12_0_for_macOS_Debug`), the Swift helpers, a neutral zsh config and a demo git project |
| `launch` / `stop` | Starts the copy (window at 290,120, 1100×680 pt) / quits it with the backdrop and the HUD |
| `wallpaper` | Captures the desktop wallpaper of the built-in display alone (no windows, no desktop icons) to `$KONSOLL_DOCS_DIR/wallpaper.png` |
| `backdrop on [IMAGE\|wallpaper]` / `backdrop off` | A click-through full-screen background behind the app: the neutral gradient, an image, or the captured wallpaper |
| `hud show X Y` / `hud say TEXT` / `hud off` | A caption pill for GIFs (the key being pressed, a step) |
| `windows` / `place WINDOW X Y W H` / `front` | The app's windows (`ID X Y W H LAYER TITLE`), move or resize one (index or title), bring the app forward |
| `click X Y [COUNT]`, `rclick X Y`, `move X Y`, `drag X1 Y1 X2 Y2 [HOLD_MS]` | Mouse input |
| `type [--enter] TEXT`, `key KEY [MODIFIERS]` | Keyboard input, e.g. `key left option`, `key t command`, `key b command,shift` |
| `shot NAME [MARGIN]`, `shot-rect NAME X Y W H` | A PNG of the front window plus a margin of backdrop, or of any rectangle |
| `record NAME SECONDS [MARGIN \| X Y W H]`, `record-wait` | A screen recording in the background while the script acts |
| `gif IN OUT START DURATION [WIDTH]`, `png IN OUT [MAX_WIDTH]` | Optimized GIF (12 fps, 128 colors) and PNG (≤1600 px, 256 colors) |

Coordinates are points with (0,0) at the top-left of the built-in display;
read them off `windows` and a `shot`. Files go to `$KONSOLL_DOCS_DIR`
(default `/tmp/konsoll-docs`), shots and recordings to its `shots/` folder.

### Example: a screenshot and a GIF

```sh
D=tools/screenshots/docshot.sh
$D setup                       # once, or after changing the app
$D wallpaper                   # the desktop picture, without your windows
$D launch
$D backdrop on wallpaper       # the blur and transparency show it

# The tab context menu
$D type --enter 'git --no-pager log --oneline --graph --color'
$D rclick 520 145              # a tab in the strip
sleep 0.8
$D shot tabs                   # -> $KONSOLL_DOCS_DIR/shots/tabs.png
$D png /tmp/konsoll-docs/shots/tabs.png docs/images/tabs.png

# Word editing with the keys shown on screen
$D type 'git commit -m "update the release notes"'
$D hud show 840 780            # below the window's content
$D record word 10
for i in 1 2 3; do $D hud say '⌥ ←   word left'; $D key left option; sleep 0.6; done
$D hud say '⌥ ⌫   delete previous word'; $D key delete option; sleep 1
$D hud say ''
$D record-wait
$D gif /tmp/konsoll-docs/shots/word.mov docs/images/option-word-editing.gif 1 8

$D stop
```

### What it takes care of

- **Settings.** The copy is built with the organization domain
  `com.github.rask-project-docs`, so it reads and writes
  `com.com-github-rask-project-docs.Konsoll` instead of your settings, saved
  commands and functions. `setup` patches `main.cpp`, builds, copies the app
  and restores `main.cpp` (it refuses to run when `main.cpp` has local
  changes), then rebuilds the build directory from the clean sources.
- **Shared shell-integration folder.** That folder
  (`~/Library/Preferences/rask-project/Konsoll/shell-integration`) is keyed by
  the organization *name*, so the copy still rewrites `functions.*` there.
  Your Konsoll writes yours back when it starts: relaunch it afterwards.
- **No personal data.** The copy runs with a fake `HOME` holding the demo
  project, a prompt showing only the directory name, git author `Demo`, and
  an `OSC 7` report of `/Users/demo/…`, which is what pane and tab titles
  show. Still avoid output such as `ls -l` that prints your user name.
- **The see-through window.** The copy uses your opacity and blur
  (`General.windowAlpha`, `General.blurEnabled`), so the shots show the
  window as you use it. Whatever is behind it shows through, so keep the
  backdrop on: `backdrop on wallpaper` puts the desktop picture behind the
  app (captured by `wallpaper` from the wallpaper window alone, so none of
  your windows or desktop icons appear) and `backdrop on` a neutral
  gradient. `shot` captures a screen rectangle, so menus and popups are
  included — the tab context menu is a separate popup window that a
  single-window capture would miss.
- **Dead keys.** `type` sends `"`, `'`, `~`, `^` and `` ` `` followed by a
  space, so ABNT and US International layouts type them literally. `~` may
  still come out wrong; prefer `$HOME` or relative paths.

### Tips

- The first click on an inactive window only activates it: click twice when
  focus matters, and don't click the backdrop to close a menu (click an empty
  part of the tab strip or a shell pane instead; Escape also reaches the
  shell).
- Dragging a tab needs a press-and-hold first: `drag X1 Y1 X2 Y2 1000`.
- Settings and Background Commands open on the last screen used; `place` them
  on the built-in display (`place Running 390 150 900 704`). Their title is
  the selected section.
- Two takes of a GIF join with ffmpeg's concat demuxer (re-encode both to
  `.mp4` first).
- Keep GIFs to 10–18 s and under ~2 MB; check every shot for personal data
  (paths, user names, notifications, the menu-bar clock) before committing.

### Background Commands data

The Saved Commands and Functions shots need saved entries. Write them to the
copy's own domain while it is stopped (they are JSON in a data value):

```sh
python3 - <<'PY'
import json, subprocess
dom = "com.com-github-rask-project-docs.Konsoll"
saved = [{"name": "Static server", "command": "python3 -m http.server 8002", "directory": ""},
         {"name": "Build and test", "command": "npm run build\nnpm test", "directory": ""}]
funcs = [{"name": "mkcd", "description": "Create a folder and enter it", "body": "mkdir -p \"$1\" && cd \"$1\""}]
for key, value in (("BackgroundCommands.saved", saved), ("ShellFunctions.functions", funcs)):
    subprocess.run(["defaults", "write", dom, key, "-data",
                    json.dumps(value, separators=(",", ":")).encode().hex()], check=True)
PY
```

The copy then writes `mkcd` into the shared shell-integration folder (see
above): relaunch your Konsoll afterwards so it writes your functions back.

## Shot list

| File | What it shows | How |
| --- | --- | --- |
| `overview.png` | Three tabs (`top` with new output, a finished command ✓, the active one with the static running icon of `http.server`) and the active tab split into `nvim`, a shell with `git log` and a `python3 -m http.server` pane | ⌘T twice, ⌘D, ⇧⌘D, a program in each pane |
| `option-word-editing.gif` | In a window 314 pt tall (`place 1 290 120 1100 314`, HUD at 840 412), on `git commit -m "update the release notes for version 3"`: ⌥← ×4, ⌥⌦, ⌥⌫, type `changelog`, ⌥→ ×3, ⌃C, then `cd ` + ⌥. | Run `mkdir -p notes/v3` first, then ⌃L (not `clear`, which would become the last argument) so ⌥. inserts `notes/v3`; a key HUD names each key |
| `tabs.png` | The overview layout with the tab context menu open | Right-click the active tab |
| `split-panes.png` | The overview layout with a pane title-bar menu open | Right-click a pane's title bar |
| `drag-pane.gif` | The `http.server` pane dragged by its title bar onto the `nvim` pane, then onto the tab strip | Captions in the HUD |
| `tab-to-window.gif` | The split tab pressed, held and dragged out into a new window, then dropped back on the first window's tab strip | Make the window ~780×480 pt so the new window fits beside it; drop the tab around 950,300 so the new window stays on screen; GIF 1000 px |
| `search.png` | ⌘F searching `zip` in `ls -l /usr/bin` output, a few matches in | ↩ to move between matches |
| `command-block.png` | A command and its output outlined after a click on one of the output lines | `git status`, `wc -l src/*.js tests/*.js`, `git log --oneline`; click the `wc` output |
| `save-buffer.png` | Four selected lines of `git log` and the pane context menu | Drag to select, right-click the selection |
| `save-dialog.png` | The save dialog with a file name typed, in Downloads | *Save Buffer or Selection…*, type a name, cancel |
| `background-running.png` | Running: a live `python3 -m http.server 8001` with its output, a finished loop, a failed `ls dist` | Type the commands in the field; `curl` the server for output |
| `background-saved-commands.png` | Two saved commands and the *New Command* form with a two-line command | Add Command… |
| `background-functions.png` | `mkcd` listed and `serve` being written | Add Function… |
| `background-sessions.png` | Every pane of the window with Show / Move to Background | — |
| `move-to-background.gif` | The `http.server` tab, right-click ▸ *Move to background*, the job under Running with new output, *Open in Tab* | Main window at 50,170 (760×470) and Background Commands at 830,170 (800×560), recorded together (`record NAME 17 20 130 1640 640`, GIF 1000 px). Select the moved job right away so `curl` requests show in its output; with four jobs listed, *Open in Tab* sits lower (y≈480) |
| `settings.png` | The five Settings sections: two rows of two, then Security centered | One shot per section; Security as a `shot-rect` twice as wide (backdrop on both sides), stacked with ffmpeg `hstack` / `vstack` |
| `agent-status.png` | Tabs `webapp` (an agent asking to run `npm test -- login`, a `vite` dev server beside it), `users` (working), `docs` (done), `api` (asking to read a pane) | Demo agents (below) |
| `agent-approval.gif` | The same tab: the permission prompt, allowed from Konsoll, the agent finishing its turn | Frames every 250 ms, 4 fps, 900 px |
| `agents-window.png` | Window ▸ AI Agents with those four agents: Allow / Deny on the two asking, the task and state of the others | — |
| `agent-connect.png` | A tab running a Codex that reports nothing, with the offer to connect it | `~/.codex` exists, not connected |
| `agent-read-request.png` | Tab `api`: a failed `npm test` beside an agent whose `read_pane` waits for Deny / Allow Once | The demo agent calls `konsoll --mcp` itself |
| `send-to-agent.png` | Tab `webapp`: the failed `npm test` beside an agent, two more agents in tabs `auth` and `docs`, and the pane menu's Send Last Command To submenu listing the three | The context menu, then the submenu |
| `settings-agents.png` | Settings ▸ AI Agents: Claude Code and Qwen Code connected, Codex not, opencode not found, the MCP rows | — |

**The AI agent shots** use demo agents in the docshot home: a small binary
copied as `claude`, `codex` and `qwen` (so the pane is named like the agent)
that runs `~/.demo/<name>-$DEMO.zsh`, scripts that clear the screen, print
an agent-like transcript and report states with `"$KONSOLL_AGENT_HOOK"`, plus
an `npm` shell function (a failing `npm test`, `npm run dev` running a
`vite` stand-in). The current set was taken without the screen-recording
permission: the isolated copy, started with a temporary script in
`Main.qml`, set up the tabs and grabbed its own windows (`grabToImage`),
and ffmpeg put each grab over the blurred wallpaper with a dark tint (the
window's native blur), rounded corners, a shadow and the window buttons —
the parts the grab does not include. With the permissions, retake them
with `shot` and `record` like the others.

Before committing, check the shots for anything personal: user and host
names in prompts, paths under your home folder, history, notifications from
other apps, and the menu-bar clock.
