# AGENTS.md

Guidance for AI agents working in this repository.

## Project

- Konsoll — a native macOS terminal built in QML (Qt Quick).
- Toolchain: C++23, Qt 6.12, CMake.
- Target platform: macOS (Apple Silicon).
- MVP scope and phases: `docs/PLAN.md`. Do not add post-MVP features
  (tabs, splits, search, mouse reporting, selection) to the MVP — see its
  §14 backlog.

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
