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
  C/POSIX/Qt APIs (e.g. `getpwuid_r`, `ioctl`, `forkpty`).
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

## Verifying QML changes

- After editing any QML, **build and run offscreen** to confirm the QML
  loads before reporting the change done:
  `QT_QPA_PLATFORM=offscreen ./build/<config>/konsoll.app/Contents/MacOS/konsoll`.
  Watch stderr for QML load errors — `Type ... unavailable`,
  `Cannot assign to non-existent property "..."` — which mean a referenced
  property/type does not exist on the C++ side.
- The build already runs QML AOT (`qmlcache`), so a property that does not
  exist on a `QML_ELEMENT` type also fails the build. A clean build plus a
  clean offscreen run together confirm the QML is wired correctly.
- Under offscreen the app **segfaults in `src/macs_translucent.mm`**
  (AppKit translucency with no real `NSWindow`) — this is pre-existing and
  unrelated to QML. A clean QML load (font/rendering messages, no QML error)
  followed by that segfault is the expected, passing result.
