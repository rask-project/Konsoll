# Clang-Tidy Findings & Remediation Plan

> Generated: 2026-11-04
> Tool: Homebrew LLVM 19.1.6 `clang-tidy`
> Config: `.clang-tidy` (`WarningsAsErrors: "*"`)
> Scope: all 14 C++ / Objective-C++ sources (10 `.cpp`, 4 `.mm`)
> Latest run: 2026-11-04, after the Stage 3 tree-traversal and `waitpid`
> refactors; 8 diagnostics remain (7 cognitive-complexity, 1 branch-clone).

## 1. How to reproduce

```bash
# Per file — uses the build's compile_commands.json for the Qt include paths.
clang-tidy -p build/Qt_6_12_0_for_macOS_Debug --config-file=.clang-tidy <file>
```

> **Tooling caveat — `.mm` crash.** clang-tidy 19 **segfaults** on every
> `.mm` file while running `cppcoreguidelines-avoid-const-or-ref-data-members`
> against the Objective-C `NSObject::isa` ivar (a known AST-matcher bug in
> the tool, not a code issue). To still get coverage of the `.mm` files they
> were re-run with that single check disabled (`--checks` mirroring the config
> minus that one check); all other checks ran normally. Consequently the
> `cppcoreguidelines-avoid-const-or-ref-data-members` result for `.mm` files
> is **not covered** by this run.

## 2. Summary

The initial run found **33 unique findings** (deduplicated across translation
units) across **11 distinct checks**. The latest run leaves **8 diagnostics**
in project sources: 7 cognitive-complexity findings and 1 branch-clone finding.
Stage 1 (9 unique findings) and Stage 2 (8) are complete. In Stage 3, the 5
recursion and 3 do-while findings no longer reproduce; the remaining findings
are listed below.

| Check | Count | Stage |
|---|---|---|
| `readability-function-cognitive-complexity` | 7 | 3 — remaining |
| `bugprone-narrowing-conversions` | 7 | 2 |
| `misc-no-recursion` | 5 | 3 — cleared |
| `cppcoreguidelines-avoid-non-const-global-variables` | 4 | 1 |
| `cppcoreguidelines-avoid-do-while` | 3 | 3 — cleared |
| `misc-const-correctness` | 2 | 1 |
| `cppcoreguidelines-explicit-virtual-functions` | 2 | 1 |
| `modernize-use-override` | 2 | 1 |
| `bugprone-implicit-widening-of-multiplication-result` | 1 | 2 |
| `readability-use-std-min-max` | 1 | 1 |
| `bugprone-branch-clone` | 1 | 3 — remaining |

> `cppcoreguidelines-explicit-virtual-functions` and `modernize-use-override`
> are the **same two findings** (the two destructors below) reported by two
> checks at once — one fix clears both.

## 3. Staging

Ordered by effort/risk, so each stage is self-contained and shippable:

- **Stage 1 — Trivial / mechanical (9 unique findings).** Local, one-line, no behavior
  change: `const` qualifiers, `override`, `std::max`, const globals. Lowest
  risk, clears the config noise fast.
- **Stage 2 — Type conversions (8 findings).** Narrowing / implicit-widening.
  Each needs a deliberate `static_cast` (or a type change) with a reason;
  low risk but review each site.
- **Stage 3 — Refactoring (16 findings).** Cognitive complexity, recursion,
  branch-clone, do-while. The recursion and do-while findings are cleared;
  8 findings remain as listed below.

## 4. Findings by stage

### Stage 1 — Trivial / mechanical (9 unique findings) — ✅ done 2026-11-04

**`misc-const-correctness` (2)** — add `const`.
- `src/TerminalBackend.cpp:1398` — `TransferJob job{…}` → `const TransferJob job{…}`.
- `src/pty/PtyProcess.cpp:190` — `QProcessEnvironment currentEnvironment = …` → `const …`.

**`cppcoreguidelines-explicit-virtual-functions` + `modernize-use-override` (2)** — annotate the two virtual destructors (clears both checks at once).
- `src/TerminalBackend.h:62` — `~TerminalBackend();` → `~TerminalBackend() override;`
- `src/TerminalManager.h:98` — `~TerminalManager();` → `~TerminalManager() override;`

**`readability-use-std-min-max` (1)**
- `src/pty/PtyWorker.cpp:69` — `if (m_undelivered < 0) { m_undelivered = 0; }` → `m_undelivered = std::max(m_undelivered, 0);` (mind the `qint64` type: `std::max<qint64>(m_undelivered, 0)`).

**`cppcoreguidelines-avoid-non-const-global-variables` (4)**
- `src/MacOSTranslucency.mm:12` — `char effectViewAssociationKey;` → `const char …{};` (objc associated-object key; `const` requires a `{}` initializer).
- `src/MacOSTranslucency.mm:13` — `char toolbarAssociationKey;` → `const char …{};`.
- `src/MacOSTranslucency.mm:14` — `char inputForwardingViewAssociationKey;` → `const char …{};`.
- `src/NativeDialogs.mm:12` — `bool s_quitApprovedByQml{};` — **mutable** module flag (set when QML approves quit), so it cannot be `const`. Chose (a): encapsulated in `struct QuitApproval` (const global + `mutable` member).

### Stage 2 — Type conversions (8) — ✅ done 2026-11-04

**`bugprone-narrowing-conversions` (7)** — each needs an explicit, justified `static_cast` (or a type change to the receiver).
- `src/TerminalBackend.cpp:578` — `const int targetLength = target.size();` (`qsizetype` → `int`). → `const auto targetLength = target.size();`
- `src/TerminalBackend.cpp:805` — `return m_searchMatches.size();` (`qsizetype` → `int`). → `return static_cast<int>(m_searchMatches.size());` (kept `int` — QML `Q_INVOKABLE int search`).
- `src/TerminalBackend.cpp:813` — `const int count = m_searchMatches.size();` (`qsizetype` → `int`). → `const int count = static_cast<int>(m_searchMatches.size());`
- `src/TerminalBackend.cpp:1564` — `resolveColor(cell.fg, m_defaultFg)` (`unsigned int` → `int`). → changed `resolveColor`'s 2nd param `int` → `unsigned int` (declaration + definition); drops the narrowing, no `static_cast` needed.
- `src/TerminalBackend.cpp:1565` — `resolveColor(cell.bg, m_defaultBg)` (`unsigned int` → `int`). → same 2nd-param change as 1564.
- `src/term/Terminal.cpp:524` — `qsizetype` → `int`. → `const int separator = static_cast<int>(body.indexOf(';', 3));`
- `src/term/Terminal.cpp:559` — `qsizetype` → `int`. → `const int idx = static_cast<int>(body.indexOf(';'));`

**`bugprone-implicit-widening-of-multiplication-result` (1)**
- `src/TerminalBackend.cpp:1559` — `rowsOut.reserve(s.rows() * s.cols());` — perform the multiplication in the wider type: `rowsOut.reserve(static_cast<qsizetype>(s.rows()) * s.cols());`. → applied verbatim.

### Stage 3 — Refactoring (16) — partial

**Cleared in the latest run:** all 5 `misc-no-recursion` findings and all 3
`cppcoreguidelines-avoid-do-while` findings. The explicit-stack traversals
are in `src/TerminalManager.cpp`; the EINTR retries in `src/pty/PtyProcess.cpp`
now use ordinary `while` loops.

**Still reported in the latest run (8 diagnostics):**

| File | Finding | Count |
|---|---|---:|
| `src/Terminal.cpp` | `readability-function-cognitive-complexity` (`processByte`, `handleOsc`) | 2 |
| `src/Screen.cpp` | `readability-function-cognitive-complexity` (`reflow`) | 1 |
| `src/pty/PtyProcess.cpp` | `readability-function-cognitive-complexity` (`start`) | 1 |
| `src/TerminalBackend.cpp` | `readability-function-cognitive-complexity` (`keyPress`, `linkAt`) | 2 |
| `src/TerminalManager.cpp` | `readability-function-cognitive-complexity` (`closeSession`) | 1 |
| `src/Terminal.cpp` | `bugprone-branch-clone` | 1 |

The line numbers and complexity scores below are from the initial run and are
kept as the baseline. Use the latest diagnostics above to locate current lines.

**`readability-function-cognitive-complexity` (7)** — threshold 25; extract helpers / table-drive / early-return to bring each under.
- `src/term/Terminal.cpp:101` — `processByte` (51).
- `src/pty/PtyProcess.cpp:60` — `start` (46).
- `src/term/Screen.cpp:570` — `reflow` (43).
- `src/term/Terminal.cpp:504` — `handleOsc` (37).
- `src/TerminalBackend.cpp:450` — `keyPress` (28).
- `src/TerminalBackend.cpp:547` — `linkAt` (27).
- `src/TerminalManager.cpp:357` — `closeSession` (26).

**`misc-no-recursion` (5)** — tree traversals over the layout tree. Decide per function: **iterative rewrite** (explicit stack) vs. **justified `NOLINT`** (recursion is natural here; depth is bounded by the layout tree, which is small).
- `src/TerminalManager.cpp:28` — `notifyTreeChanged`.
- `src/TerminalManager.cpp:473` — `findLeaf`.
- `src/TerminalManager.cpp:487` — `firstSession`.
- `src/TerminalManager.cpp:510` — `findNodeSlot`.
- `src/TerminalManager.cpp:566` — recursive `removeNode` lambda (`operator()`).

**`bugprone-branch-clone` (1)**
- `src/term/Terminal.cpp:132` — repeated branch body in a conditional chain; merge the duplicated branches or extract the shared body.

**`cppcoreguidelines-avoid-do-while` (3)** — all are the `do { waitpid } while (result < 0 && errno == EINTR)` EINTR-retry idiom. The `do-while` is the natural form here; recommended fix is a justified `NOLINT` (rewriting to `while(true){…break;}` only adds noise).
- `src/pty/PtyProcess.cpp:44`
- `src/pty/PtyProcess.cpp:52`
- `src/pty/PtyProcess.cpp:341`
