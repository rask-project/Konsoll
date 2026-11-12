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
| `backdrop on\|off` | A neutral, click-through gradient behind the app |
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
$D launch
$D backdrop on

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
- **The see-through window.** Without the backdrop, whatever is behind the
  window (your wallpaper, other apps) shows through. `shot` captures a screen
  rectangle, so menus and popups are included — the tab context menu is a
  separate popup window that a single-window capture would miss.
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

## Shot list

| File | What it shows | How |
| --- | --- | --- |
| `overview.png` | Three tabs (`top` with new output, a finished command ✓, the active one) and the active tab split into `nvim`, a shell with `git log` and a `python3 -m http.server` pane | ⌘T twice, ⌘D, ⇧⌘D, a program in each pane |
| `option-word-editing.gif` | On `git commit -m "update the release notes for version 3"`: ⌥← ×4, ⌥⌦, ⌥⌫, type `changelog`, ⌥→ ×3, ⌃C, then `cd ` + ⌥. | Run `mkdir -p notes/v3` first so ⌥. has an argument; a key HUD names each key |
| `tabs.png` | The overview layout with the tab context menu open | Right-click the active tab |
| `split-panes.png` | The overview layout with a pane title-bar menu open | Right-click a pane's title bar |
| `drag-pane.gif` | The `http.server` pane dragged by its title bar onto the `nvim` pane, then onto the tab strip | Captions in the HUD |
| `tab-to-window.gif` | The split tab pressed, held and dragged out into a new window, then dropped back on the first window's tab strip | Make the window ~780×480 pt so the new window fits beside it |
| `search.png` | ⌘F searching `zip` in `ls -l /usr/bin` output, a few matches in | ↩ to move between matches |
| `command-block.png` | A command and its output outlined after a click on one of the output lines | `git status`, `wc -l src/*.js tests/*.js`, `git log --oneline`; click the `wc` output |
| `save-buffer.png` | Four selected lines of `git log` and the pane context menu | Drag to select, right-click the selection |
| `save-dialog.png` | The save dialog with a file name typed, in Downloads | *Save Buffer or Selection…*, type a name, cancel |
| `background-running.png` | Running: a live `python3 -m http.server 8001` with its output, a finished loop, a failed `ls dist` | Type the commands in the field; `curl` the server for output |
| `background-saved-commands.png` | Two saved commands and the *New Command* form with a two-line command | Add Command… |
| `background-functions.png` | `mkcd` listed and `serve` being written | Add Function… |
| `background-sessions.png` | Every pane of the window with Show / Move to Background | — |
| `move-to-background.gif` | The `http.server` tab, right-click ▸ *Move to background*, the job under Running with new output, *Open in Tab* | Main window and Background Commands side by side; two takes joined |
| `settings.png` | The four Settings sections in a 2×2 grid | One shot per section, stacked with ffmpeg `hstack` / `vstack` |

Before committing, check the shots for anything personal: user and host
names in prompts, paths under your home folder, history, notifications from
other apps, and the menu-bar clock.
