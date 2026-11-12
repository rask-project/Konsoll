#!/bin/bash
# Drives an isolated Konsoll to take the README screenshots and GIFs.
# See docs/SCREENSHOTS.md for the workflow and the shot list.
#
# Usage: tools/screenshots/docshot.sh COMMAND [ARGS...]
#
#   setup [BUILD_DIR]        build an isolated copy of the app (own settings
#                            domain), the Swift helpers and a demo project
#   launch                   start the copy with a fake HOME and neutral prompt
#   stop                     quit the copy, the backdrop and the HUD
#
#   backdrop on|off          neutral full-screen background behind the app
#   hud show X Y             caption pill, bottom-center at X,Y
#   hud say TEXT...          set the caption ("" hides it)
#   hud off
#
#   windows                  the app's windows: ID X Y W H LAYER TITLE
#   place WINDOW X Y W H     move/resize WINDOW (index from 1, or its title)
#   front                    bring the app forward
#   click X Y [COUNT] | rclick X Y | move X Y | drag X1 Y1 X2 Y2 [HOLD_MS]
#   type [--enter] TEXT      type TEXT (safe on dead-key layouts)
#   key KEY [MODIFIERS]      KEY: left right up down return escape delete
#                            fwd-delete tab, or one character; MODIFIERS:
#                            comma list of command,shift,option,control
#
#   shot NAME [MARGIN]       capture the front window plus MARGIN pt (28)
#   shot-rect NAME X Y W H   capture a screen rectangle
#   record NAME SECONDS [MARGIN | X Y W H]
#                            record in the background (front window + margin
#                            by default); wait with `record-wait`
#   record-wait
#   gif IN.mov OUT.gif START DURATION [WIDTH]   12 fps palette GIF (900 px)
#   png IN.png OUT.png [MAX_WIDTH]              scale (1600 px) + 256 colors
#
# Coordinates are global points with (0,0) at the top-left of the built-in
# display. Files go to $KONSOLL_DOCS_DIR (default /tmp/konsoll-docs); shots
# and recordings to its shots/ folder unless NAME is a path.
#
# Needs: Screen & System Audio Recording and Accessibility permissions for the
# terminal that runs it, Xcode command-line tools (swiftc), ffmpeg.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TOOLS="$ROOT/tools/screenshots"
DIR=${KONSOLL_DOCS_DIR:-/tmp/konsoll-docs}
BIN="$DIR/bin"
SHOTS="$DIR/shots"
APP="$DIR/konsoll.app/Contents/MacOS/konsoll"
DOMAIN=com.com-github-rask-project-docs.Konsoll
REAL_DOMAIN=com.com-github-rask-project.Konsoll

die() {
    echo "docshot: $*" >&2
    exit 1
}

pid_of() { # NAME -> pid in $DIR/NAME.pid if that process is alive
    local file="$DIR/$1.pid"
    [[ -f $file ]] && kill -0 "$(cat "$file")" 2>/dev/null && cat "$file"
}

app_pid() {
    pid_of app || die "Konsoll is not running; run '$0 launch'"
}

out_path() { # NAME EXT -> path
    if [[ $1 == */* ]]; then echo "$1"; else echo "$SHOTS/$1.$2"; fi
}

# The frontmost normal window of the app: "X Y W H".
front_bounds() {
    "$BIN/ctl" windows "$(app_pid)" | awk '$6 == 0 { print $2, $3, $4, $5; exit }'
}

osa() { # AppleScript lines for System Events, run with the app in front
    local pid
    pid=$(app_pid)
    osascript -e 'tell application "System Events"' \
        -e "set frontmost of (first process whose unix id is $pid) to true" \
        -e 'delay 0.2' -e "$1" -e 'end tell'
}

applescript_string() {
    local s=${1//\\/\\\\}
    printf '"%s"' "${s//\"/\\\"}"
}

# Dead keys (ABNT, US International) swallow the next character: send each
# of them followed by a space, which produces the bare character.
type_script() {
    local text=$1 chunk="" lines="" ch i
    for ((i = 0; i < ${#text}; i++)); do
        ch=${text:i:1}
        case $ch in
        \" | \' | '~' | '^' | '`')
            [[ -n $chunk ]] && lines+="keystroke $(applescript_string "$chunk")"$'\n'
            chunk=""
            lines+="keystroke $(applescript_string "$ch")"$'\n'"keystroke \" \""$'\n'
            ;;
        *) chunk+=$ch ;;
        esac
    done
    [[ -n $chunk ]] && lines+="keystroke $(applescript_string "$chunk")"$'\n'
    printf '%s' "$lines"
}

modifiers_clause() { # "command,shift" -> " using {command down, shift down}"
    [[ -z ${1:-} ]] && return
    local list="" mod
    IFS=, read -ra mods <<<"$1"
    for mod in "${mods[@]}"; do
        case $mod in
        command | shift | option | control) list+="${list:+, }$mod down" ;;
        *) die "unknown modifier: $mod" ;;
        esac
    done
    echo " using {$list}"
}

key_script() {
    local code
    case $1 in
    left) code=123 ;;
    right) code=124 ;;
    up) code=126 ;;
    down) code=125 ;;
    return) code=36 ;;
    escape) code=53 ;;
    delete) code=51 ;;
    fwd-delete) code=117 ;;
    tab) code=48 ;;
    ?) echo "keystroke $(applescript_string "$1")$(modifiers_clause "${2:-}")"; return ;;
    *) die "unknown key: $1" ;;
    esac
    echo "key code $code$(modifiers_clause "${2:-}")"
}

# --- setup ---------------------------------------------------------------

build_isolated_app() {
    local build_dir=$1
    [[ -f $build_dir/build.ninja || -f $build_dir/Makefile ]] || die "no configured build in $build_dir"
    git -C "$ROOT" diff --quiet -- main.cpp || die "main.cpp has local changes; commit or stash them first"
    # Settings live under the organization domain: point the copy at its own.
    trap 'git -C "$ROOT" checkout -- main.cpp' EXIT
    sed -i '' 's/"com.github.rask-project")/"com.github.rask-project-docs")/' "$ROOT/main.cpp"
    grep -q rask-project-docs "$ROOT/main.cpp" || die "could not patch the settings domain in main.cpp"
    cmake --build "$build_dir" --target konsoll
    rm -rf "$DIR/konsoll.app"
    cp -R "$build_dir/konsoll.app" "$DIR/konsoll.app"
    git -C "$ROOT" checkout -- main.cpp
    trap - EXIT
    echo "Rebuilding $build_dir from the unpatched sources…"
    cmake --build "$build_dir" --target konsoll >/dev/null
}

write_demo_shell() {
    mkdir -p "$DIR/zdot"
    cat >"$DIR/zdot/.zshrc" <<'EOF'
# Neutral prompt for screenshots: the directory name only, no user or host.
PROMPT='%F{39}%1~%f %F{213}❯%f '
HISTFILE=$HOME/.zsh_history
HISTSIZE=1000
SAVEHIST=1000
export CLICOLOR=1 LSCOLORS=ExGxFxdaCxDaDahbadacec
export GIT_AUTHOR_NAME=Demo GIT_COMMITTER_NAME=Demo
export GIT_AUTHOR_EMAIL=demo@example.com GIT_COMMITTER_EMAIL=demo@example.com
cd "$HOME/webapp"
# Pane and tab titles come from OSC 7: report /Users/demo/... instead of
# the real path of the fake HOME. Emitted from the prompt, after the
# integration's own precmd report.
setopt prompt_subst
_demo_osc7() { print -rn -- $'\e]7;file://localhost'"${PWD/#$HOME//Users/demo}"$'\a' }
PROMPT='%{$(_demo_osc7)%}'"$PROMPT"
EOF
}

write_demo_project() {
    local repo="$DIR/home/webapp"
    [[ -d $repo/.git ]] && return
    mkdir -p "$repo/src" "$repo/tests"
    (
        cd "$repo"
        export GIT_AUTHOR_NAME=Demo GIT_COMMITTER_NAME=Demo
        export GIT_AUTHOR_EMAIL=demo@example.com GIT_COMMITTER_EMAIL=demo@example.com
        git init -q -b main
        printf '# webapp\n\nA small demo project.\n' >README.md
        git add README.md && git commit -qm "Initial commit"
        printf 'export const add = (a, b) => a + b;\n' >src/math.js
        printf 'console.log("hello");\n' >src/index.js
        git add src && git commit -qm "Add the math module"
        printf 'test("add", () => {});\n' >tests/math.test.js
        git add tests && git commit -qm "Test the math module"
        printf '{\n  "name": "webapp",\n  "scripts": { "dev": "vite", "test": "vitest" }\n}\n' >package.json
        printf '<!doctype html>\n<title>webapp</title>\n<h1>Hello from webapp</h1>\n' >index.html
        git add . && git commit -qm "Add the landing page and package.json"
        # An uncommitted change, so git status has something to show.
        cat >src/math.js <<'EOF'
// Small arithmetic helpers used by the landing page.

export const add = (a, b) => a + b;
export const sub = (a, b) => a - b;

export function clamp(value, min, max) {
  return Math.min(Math.max(value, min), max);
}

export function average(values) {
  if (values.length === 0) {
    return 0;
  }
  return values.reduce(add, 0) / values.length;
}
EOF
    )
}

write_demo_settings() {
    local font
    font=$(defaults read "$REAL_DOMAIN" General.fontFamily 2>/dev/null || true)
    [[ -n $font ]] && defaults write "$DOMAIN" General.fontFamily "$font"
    defaults write "$DOMAIN" General.fontPointSize -int 14
    defaults write "$DOMAIN" General.windowAlpha -float 1.0
    defaults write "$DOMAIN" General.windowState normal
}

cmd_setup() {
    local build_dir=${1:-$ROOT/build/Qt_6_12_0_for_macOS_Debug}
    mkdir -p "$DIR" "$BIN" "$SHOTS"
    local tool
    for tool in ctl backdrop hud; do
        swiftc -O "$TOOLS/$tool.swift" -o "$BIN/$tool"
    done
    build_isolated_app "$build_dir"
    write_demo_shell
    write_demo_project
    write_demo_settings
    echo "Ready in $DIR. Next: $0 launch"
}

# --- running -------------------------------------------------------------

cmd_launch() {
    [[ -x $APP ]] || die "no isolated app; run '$0 setup' first"
    [[ -n $(pid_of app || true) ]] && die "already running (pid $(pid_of app))"
    (
        cd "$DIR/home" || exit 1
        HOME="$DIR/home" ZDOTDIR="$DIR/zdot" nohup "$APP" >"$DIR/app.log" 2>&1 &
        echo $! >"$DIR/app.pid"
    )
    local i
    for i in {1..100}; do
        [[ -n $(front_bounds 2>/dev/null || true) ]] && break
        sleep 0.1
    done
    [[ -n $(front_bounds 2>/dev/null || true) ]] || die "no window after 10 s; see $DIR/app.log"
    cmd_place 1 290 120 1100 680
    echo "Konsoll running (pid $(cat "$DIR/app.pid"))"
}

stop_pid() {
    local pid
    pid=$(pid_of "$1" || true)
    [[ -n $pid ]] && kill "$pid" 2>/dev/null || true
    rm -f "$DIR/$1.pid"
}

cmd_stop() {
    stop_pid hud
    stop_pid backdrop
    stop_pid app
    cat <<'EOF'
Stopped. The copy shares ~/Library/Preferences/rask-project/Konsoll/shell-integration
with your Konsoll and rewrote functions.* there with its own (demo) list; your
Konsoll writes yours back when it starts.
EOF
}

cmd_backdrop() {
    case ${1:-} in
    on)
        stop_pid backdrop
        nohup "$BIN/backdrop" >/dev/null 2>&1 &
        echo $! >"$DIR/backdrop.pid"
        sleep 0.5
        [[ -n $(pid_of app || true) ]] && cmd_front
        ;;
    off) stop_pid backdrop ;;
    *) die "usage: backdrop on|off" ;;
    esac
}

cmd_hud() {
    case ${1:-} in
    show)
        [[ $# -eq 3 ]] || die "usage: hud show X Y"
        stop_pid hud
        : >"$DIR/hud.txt"
        nohup "$BIN/hud" "$DIR/hud.txt" "$2" "$3" >/dev/null 2>&1 &
        echo $! >"$DIR/hud.pid"
        ;;
    say)
        shift
        printf '%s\n' "$*" >"$DIR/hud.txt"
        ;;
    off) stop_pid hud ;;
    *) die "usage: hud show X Y | hud say TEXT | hud off" ;;
    esac
}

cmd_place() {
    [[ $# -eq 5 ]] || die "usage: place WINDOW X Y W H"
    local window
    if [[ $1 =~ ^[0-9]+$ ]]; then window="window $1"; else window="(first window whose name is $(applescript_string "$1"))"; fi
    osascript -e "tell application \"System Events\" to tell (first process whose unix id is $(app_pid)) to tell $window to set {position, size} to {{$2, $3}, {$4, $5}}" >/dev/null
    osascript -e "tell application \"System Events\" to tell (first process whose unix id is $(app_pid)) to perform action \"AXRaise\" of $window" >/dev/null
}

cmd_front() {
    osascript -e "tell application \"System Events\" to set frontmost of (first process whose unix id is $(app_pid)) to true"
}

cmd_type() {
    local enter=false
    [[ ${1:-} == --enter ]] && enter=true && shift
    [[ $# -eq 1 ]] || die "usage: type [--enter] TEXT"
    local script
    script=$(type_script "$1")
    $enter && script+=$'\nkey code 36'
    osa "$script"
}

# --- capturing -----------------------------------------------------------

cmd_shot() {
    [[ $# -ge 1 ]] || die "usage: shot NAME [MARGIN]"
    local margin=${2:-28} x y w h
    read -r x y w h <<<"$(front_bounds)"
    [[ -n ${h:-} ]] || die "no window to capture"
    cmd_shot_rect "$1" $((x - margin)) $((y - margin)) $((w + 2 * margin)) $((h + 2 * margin))
}

cmd_shot_rect() {
    [[ $# -eq 5 ]] || die "usage: shot-rect NAME X Y W H"
    local out
    out=$(out_path "$1" png)
    mkdir -p "$(dirname "$out")"
    screencapture -x -R "$2,$3,$4,$5" "$out"
    echo "$out"
}

cmd_record() {
    [[ $# -ge 2 ]] || die "usage: record NAME SECONDS [MARGIN | X Y W H]"
    local out rect
    out=$(out_path "$1" mov)
    if [[ $# -eq 6 ]]; then
        rect="$3,$4,$5,$6"
    else
        local margin=${3:-28} x y w h
        read -r x y w h <<<"$(front_bounds)"
        rect="$((x - margin)),$((y - margin)),$((w + 2 * margin)),$((h + 2 * margin))"
    fi
    mkdir -p "$(dirname "$out")"
    rm -f "$out"
    nohup screencapture -x -v -V "$2" -R "$rect" "$out" >/dev/null 2>&1 &
    echo $! >"$DIR/record.pid"
    sleep 1.5 # screencapture takes a moment to start
    echo "$out"
}

cmd_record_wait() {
    local pid
    pid=$(pid_of record || true)
    [[ -z $pid ]] && return
    while kill -0 "$pid" 2>/dev/null; do sleep 0.2; done
    rm -f "$DIR/record.pid"
}

cmd_gif() {
    [[ $# -ge 4 ]] || die "usage: gif IN OUT START DURATION [WIDTH]"
    local width=${5:-900}
    ffmpeg -loglevel error -y -ss "$3" -t "$4" -i "$1" -vf \
        "fps=12,scale=$width:-1:flags=lanczos,split[a][b];[a]palettegen=stats_mode=diff:max_colors=128[p];[b][p]paletteuse=dither=bayer:bayer_scale=5:diff_mode=rectangle" \
        -loop 0 "$2"
    ls -lh "$2"
}

cmd_png() {
    [[ $# -ge 2 ]] || die "usage: png IN OUT [MAX_WIDTH]"
    local max=${3:-1600}
    ffmpeg -loglevel error -y -i "$1" -vf \
        "scale='min($max,iw)':-1:flags=lanczos,split[a][b];[a]palettegen=max_colors=256:stats_mode=full[p];[b][p]paletteuse=dither=sierra2_4a" \
        "$2"
    ls -lh "$2"
}

# --- dispatch ------------------------------------------------------------

command=${1:-}
[[ -n $command ]] || {
    sed -n '2,/^set -euo/p' "$0" | sed '$d; s/^# \{0,1\}//'
    exit 2
}
shift
case $command in
setup) cmd_setup "$@" ;;
launch) cmd_launch ;;
stop) cmd_stop ;;
backdrop) cmd_backdrop "$@" ;;
hud) cmd_hud "$@" ;;
windows) "$BIN/ctl" windows "$(app_pid)" ;;
place) cmd_place "$@" ;;
front) cmd_front ;;
click | rclick | move | drag) "$BIN/ctl" "$command" "$@" ;;
type) cmd_type "$@" ;;
key) osa "$(key_script "$@")" ;;
shot) cmd_shot "$@" ;;
shot-rect) cmd_shot_rect "$@" ;;
record) cmd_record "$@" ;;
record-wait) cmd_record_wait ;;
gif) cmd_gif "$@" ;;
png) cmd_png "$@" ;;
*) die "unknown command: $command (run without arguments for help)" ;;
esac
