# Per-Tab Performance Findings

Measured on 2026-11-12 against `build/release/konsoll.app` (Release, arm64,
Qt 6.12) on an Apple Silicon Mac with 8 GB RAM and a ~120 Hz display. Window
size 34 rows × 140 columns. Tools: `top -l N -s 1 -pid`, `footprint -p`,
`sample PID 5`.

## Measured per-tab cost

| Scenario | CPU (one core = 100%) | Memory |
| --- | --- | --- |
| Idle tab (1, 2 or 3 tabs) | 0.0–0.6% | 147–152 MB baseline |
| **Marginal idle tab** | **≈ 0%** | **+2–4 MB** |
| TUI redrawing the full screen at 1 Hz (`top`) | 0.9–1.1% | +a few MB |
| Unknown-kind job, output redirected (`yes > /dev/null`) | 3.3–3.8% | — |
| Unknown-kind job, no output (`sleep 60`) | 6.3–7.6% | — |
| **Continuous output to the PTY (`yes`)** | **~100% and rising (79 → 103%)** | **160 MB → 24 GB in seconds** |

The terminal itself is cheap: an idle tab costs nothing measurable and a
full-screen TUI at 1 Hz costs about 1% of one core. The two non-trivial costs
below are the tab spinner animation and unbounded scrollback growth.

## Finding 1 — Unbounded scrollback is a memory bomb

`Screen::m_maxScrollback` is `0` (unlimited, `src/term/Screen.h`). Every
output line is stored as a full-width `Line` regardless of its content:
140 columns × 20 bytes per `Cell` (`src/term/Cell.h`) = **2800 bytes per
line**. A program that emits short lines at full speed amplifies its output
by roughly `columns × 20 / avg_line_bytes` — for `yes` ("y\n", 2 bytes) that
is ~1400×.

Reproduction: run `yes` in a tab (no redirect). The process grew from ~160 MB
to **24 GB in about 15 seconds** on an 8 GB machine (swap exhausted, system
under heavy memory pressure), while the parser burned one full core. `cat
/dev/urandom`, verbose builds, and any fast chatty program hit the same path.

**Fix direction:** cap the scrollback (in lines and/or bytes, configurable,
dropping oldest lines), and consider storing short lines shorter than the
full width.

## Finding 2 — Clearing the buffer does not release memory

`Screen::clearScrollback()` (`src/term/Screen.cpp`) calls
`m_scrollback.clear()` on a `std::deque<Line>`. `std::deque::clear()`
destroys the elements but **does not return the allocated blocks to the OS**
(verified in isolation: a deque holding 736 MiB kept a 736 MiB
`phys_footprint` after `clear()`).

Consequence: after a `yes`-style burst, ⌃K (clear buffer) empties the
scrollback logically, but the process keeps holding the entire allocation —
in the test, 24 GB stayed allocated until the tab was closed. A user has no
way to reclaim the memory short of closing the session.

**Fix direction:** shrink on clear, e.g. swap in a fresh deque
(`m_scrollback = std::deque<Line>{}`) or `squeeze()`-style reallocation, so
the freed blocks actually go back to the allocator.

## Finding 3 — The tab spinner renders the scene graph every frame

While a foreground job of an *unknown* process kind runs (`ProcessKind::None`
— e.g. `yes`, `sleep`), the tab shows the indeterminate
`TabActivityIndicator` (`controls/TabActivityIndicator.qml`), whose
`RotationAnimator` runs with `loops: Animation.Infinite` (900 ms per turn).
That invalidates the scene graph **every display frame**, driving
`QSGRenderThread` to encode and render a Metal command buffer at the display
refresh rate (~120 Hz) for the whole window — measured at 3.3–7.6% of a core
even with **zero PTY output** (`sleep 60`). `sample` confirmed the render
thread was 100% of the time in `renderSceneGraph` with the PTY idle.

Known kinds (`top` → "monitor", editors, REPLs, …, `src/ProcessKind.cpp`)
show a static icon instead and do not pay this cost — which is why the `top`
TUI measurement came out clean at ~1%.

**Fix direction:** animate the arc in discrete steps (e.g. a 10–15 fps timer
stepping the rotation) instead of a per-frame `RotationAnimator`, or pause
the animation when the window is not the key window.

## Status (2026-11-12)

All three findings are fixed, plus two problems found while fixing them.

**Finding 1 — scrollback storage.** History lines now live in a compacted
`Scrollback` (`src/term/Scrollback.{h,cpp}`): content up to the last
non-blank cell, encoded as style runs with varint code points; the width and
wrap flag are kept exactly. Unlimited stays the default, and Settings ▸
Terminal offers a line limit (`ScrollbackSettings`, `General/scrollbackLines`,
0 = unlimited) that applies live to every session.

**Finding 2 — clearing frees memory.** The real cause was not `std::deque`:
on macOS 27 the system allocator keeps freed memory in the footprint, even
16 MB blocks (`malloc_zone_pressure_relief` changes nothing; `munmap` does).
History blocks are now `mmap`ed 256 KiB regions, unmapped when their last
line goes, so clearing, trimming and closing a tab return the memory at once
(2M lines: 168 MB → 1 MB after a clear).

**Finding 3 — spinner.** `TabActivityIndicator` steps the arc 45° every
125 ms (one turn a second) instead of a per-frame `RotationAnimator`: 8
renders a second instead of the display refresh rate, and none while the
window is not active (the arc stands still as a busy mark).

**New: every reflow copied the whole history three times.** `reflow()` ran
on each resize *and on every exit from the alternate screen* (quitting vim,
less, top) and built three full-width copies of the history. It now streams
the compacted history block by block, and when the width did not change it
only re-wraps the screen. With 300k history lines:

| Operation | Before | After |
| --- | --- | --- |
| Leave the alternate screen | 2183 ms | 0.1 ms |
| Height-only resize | 2072 ms | ~0 ms |
| Width resize | 2041 ms | 131 ms |
| Peak memory | 4292 MB | 33 MB |

The streaming reflow matches the old one exactly on 600 random write /
resize / alternate-screen sequences (differential test against HEAD). The
same-width path keeps history lines as they are, where the old reflow
re-normalized them (it dropped blank continuation rows, the background of
trailing styled spaces and of wide-character wrap padding).

**Memory and parser time** (`konsoll --screen-dump 34 140`, CRLF input — a
bare LF makes a staircase of indented lines and is not what a PTY sends):

| Input | Before | After |
| --- | --- | --- |
| `yes`, 1M lines | 3169 MB, 2.98 s | 16 MB, 1.66 s |
| Colored log, 300k lines (28 MB input held in memory) | 979 MB, 1.15 s | 58 MB, 0.96 s |

A `yes` line now costs 10 bytes of history (2800 before).

**New: two per-chunk costs on the GUI thread** (found with `sample` during a
`yes` burst in the app; both predate this work):

- `TerminalBackend::handleRequests()` built a `QSettings` after every chunk
  of PTY output, even with no request pending — ~40% of the main thread in
  CoreFoundation preference syncing. It now returns early when there is
  nothing to answer.
- `Screen::scrollUpOne` / `scrollDownOne` copy-assigned every row of the
  scroll region on each line feed (34 × 2800 bytes per line, ~35% in
  `memmove`). They now `std::rotate` the rows, which swaps their buffers.

**Re-measured in the app** (Release bundle, one tab, unlimited scrollback;
the shell started with a `ZDOTDIR` whose `.zshrc` runs the scenario; `top -l
-s 1` averages):

| Scenario | Before (previous Release) | After |
| --- | --- | --- |
| Idle tab | 0.5% CPU, 183 MB | 0.5% CPU, 184 MB |
| `sleep` (spinner, no output), active window | 8.4% | 1.0% |
| Same, window not active | 8.4% | 0.0% |
| `yes > /dev/null` (spinner) | 3.3% | 0.9% |
| `top` redrawing at 1 Hz | 1.1% | 1.1% |
| `yes \| head -n 300000` | peak 1402 MB | peak 239 MB |
| `yes \| head -n 5000000` | (≈14 GB, not run) | 14 s, peak 283 MB; 210 MB after a clear |
| `yes` for 15 s | 160 MB → 24 GB | 214 → 483 MB; 192 MB after a clear |

**Extra tabs** (⌘T sent with System Events, 4 s to settle, `footprint`):
each idle tab adds ~4.8 MB (192 → 235 MB from 1 to 10 tabs) and no
measurable CPU, the same in the previous Release. `heap` puts ~3.7 MB of it
in Qt Quick (QML objects, bindings and connections, the QML engine's
notifier pools, tab icons parsed from SVG, scene-graph geometry) and ~0.4 MB
in the session's screen grids; nothing grows while a tab sits idle. The
shell each tab runs is a separate process and not counted here.

Before the two per-chunk fixes the same 5M-line burst had not finished
after 42 s.

**Still open:** a width resize is O(history) (~0.4 µs per line, so ~0.4 s
per step with 1M lines while dragging the window); `yes` still keeps one core
busy parsing, as in any terminal.

## Comparison with iTerm2 (2026-11-12)

Same scenarios, same `.zshrc` driver, same 48×199 window, unlimited
scrollback in both; iTerm2 3.7.7 driven through its AppleScript API (windows
and tabs created with a command), Konsoll from `build/release`. iTerm2 is
measured on its main process (its one `iTermServer` daemon adds ~2 MB).

| Scenario | Konsoll | iTerm2 |
| --- | --- | --- |
| Idle, 1 tab, window active | 0.5% CPU, 192 MB | 9.2% CPU, 242 MB |
| Idle, window not active | ~0% | 1.7% |
| Idle, 10 tabs, window active | ~0%, 235 MB | 7.0%, 270 MB |
| Each extra idle tab | +4.8 MB | +3.1 MB |
| `sleep` (busy tab, no output) | 1.0% | 6.7% |
| `top` redrawing at 1 Hz | 1.1% | 10.8% |
| `yes > /dev/null` | 1.0% | 1.9% |
| `yes \| head -n 5000000`: time, CPU | 14 s, 145% | 11 s, 160% |
| Same: peak, after a clear | 283 MB, 210 MB | 1568 MB, 673 MB |
| `yes` for 15 s: peak, after a clear | 483 MB, 192 MB | 13 GB, 885 MB |

iTerm2 parses a burst ~25% faster and costs less per extra tab; Konsoll
idles near zero, stores history ~20× more compactly (~14 bytes per `yes`
line against ~260) and gives the memory back on a clear.

## Reproduction notes

- Launch the Release bundle, open N tabs, measure with
  `top -l 5 -s 1 -pid $(pgrep -n konsoll) -stats pid,cpu,mem`.
- Continuous-output test: run `yes` in a tab, watch `top` and
  `footprint -p <pid>`, then Ctrl-C. On an 8 GB machine the burst reaches
  swap pressure within seconds — stop it early.
- Spinner test: run `sleep 60` (no output) and compare CPU against an idle
  tab; the tab title shows the spinning arc while the job runs.
- Scrollback-clear test: after a burst, press ⌃K and re-check
  `footprint -p <pid>` — the footprint does not drop.
