# Clang-Tidy Findings & Remediation Plan

> Generated: 2026-11-04
> Tool: Homebrew LLVM 19.1.6 `clang-tidy`
> Config: `.clang-tidy` (`WarningsAsErrors: "*"`)
> Scope: all 14 C++ / Objective-C++ sources (10 `.cpp`, 4 `.mm`)

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

**42 unique findings** (deduplicated across translation units) across **11
distinct checks**.

| Check | Count | Stage |
|---|---|---|
| `bugprone-easily-swappable-parameters` | 9 | 3 |
| `readability-function-cognitive-complexity` | 7 | 4 |
| `bugprone-narrowing-conversions` | 7 | 2 |
| `misc-no-recursion` | 5 | 4 |
| `cppcoreguidelines-avoid-non-const-global-variables` | 4 | 1 |
| `cppcoreguidelines-avoid-do-while` | 3 | 4 |
| `misc-const-correctness` | 2 | 1 |
| `cppcoreguidelines-explicit-virtual-functions` | 2 | 1 |
| `modernize-use-override` | 2 | 1 |
| `bugprone-implicit-widening-of-multiplication-result` | 1 | 2 |
| `readability-use-std-min-max` | 1 | 1 |
| `bugprone-branch-clone` | 1 | 4 |

> `cppcoreguidelines-explicit-virtual-functions` and `modernize-use-override`
> are the **same two findings** (the two destructors below) reported by two
> checks at once — one fix clears both.

## 3. Staging

Ordered by effort/risk, so each stage is self-contained and shippable:

- **Stage 1 — Trivial / mechanical (8 findings).** Local, one-line, no behavior
  change: `const` qualifiers, `override`, `std::max`, const globals. Lowest
  risk, clears the config noise fast.
- **Stage 2 — Type conversions (8 findings).** Narrowing / implicit-widening.
  Each needs a deliberate `static_cast` (or a type change) with a reason;
  low risk but review each site.
- **Stage 3 — API / parameter design (9 findings).** Adjacent similar
  parameters. Changes function signatures → touches call sites. Decide the
  approach per function (param-struct vs. documented `NOLINT`).
- **Stage 4 — Refactoring (16 findings).** Cognitive complexity, recursion,
  branch-clone, do-while. Highest effort; several are legitimate patterns
  where the right answer is a justified `NOLINT`, not a rewrite.

## 4. Findings by stage

### Stage 1 — Trivial / mechanical (8) — ✅ done 2026-11-04

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

### Stage 2 — Type conversions (8)

**`bugprone-narrowing-conversions` (7)** — each needs an explicit, justified `static_cast` (or a type change to the receiver).
- `src/TerminalBackend.cpp:578` — `const int targetLength = target.size();` (`qsizetype` → `int`).
- `src/TerminalBackend.cpp:805` — `return m_searchMatches.size();` (`qsizetype` → `int`); consider returning `qsizetype`.
- `src/TerminalBackend.cpp:813` — `const int count = m_searchMatches.size();` (`qsizetype` → `int`).
- `src/TerminalBackend.cpp:1564` — `resolveColor(cell.fg, m_defaultFg)` (`unsigned int` → `int`).
- `src/TerminalBackend.cpp:1565` — `resolveColor(cell.bg, m_defaultBg)` (`unsigned int` → `int`).
- `src/term/Terminal.cpp:524` — `qsizetype` → `int`.
- `src/term/Terminal.cpp:559` — `qsizetype` → `int`.

**`bugprone-implicit-widening-of-multiplication-result` (1)**
- `src/TerminalBackend.cpp:1559` — `rowsOut.reserve(s.rows() * s.cols());` — perform the multiplication in the wider type: `rowsOut.reserve(static_cast<qsizetype>(s.rows()) * s.cols());`.

### Stage 3 — API / parameter design (9)

**`bugprone-easily-swappable-parameters` (9)** — adjacent similar/convertible parameters. Two viable approaches per function:
- **(A) Param-struct** — group the related values (e.g. a `CellPos{row,col}`, `MouseReportArgs`, `PtyStartConfig`). The "proper" fix; touches every call site.
- **(B) Documented `NOLINT`** — keep the signature, add `// NOLINT(bugprone-easily-swappable-parameters)` with a one-line reason. Fast; keeps the API.

Findings:
- `src/TerminalBackend.cpp:635` — `sendMouseReport(int button, int row, int col, int event, int modifiers)`.
- `src/TerminalBackend.cpp:869` — `selPress(int row, int col)`.
- `src/TerminalBackend.cpp:883` — `selMove(int row, int col)`.
- `src/TerminalBackend.cpp:941` — `selWord(int row, int col)`.
- `src/TerminalBackend.cpp:1619` — `resolveColor(unsigned int encoded, int defaultColor)` (convertible types).
- `src/pty/PtyProcess.cpp:60` — `start(…, const QString &workingDirectory, const QString &remoteHostToken)`.
- `src/term/Screen.cpp:42` — `ensureRows(std::vector<Line> &grid, int rows, int width)`.
- `src/term/Terminal.cpp:59` — `mouseReport(int button, int row, int col, int event, unsigned int modifiers)` (two overlapping ranges: 3×`int`, then `int`/`unsigned int`).

> Suggested default: `selPress`/`selMove`/`selWord`/`ensureRows` are clear row/col
> pairs → (B) `NOLINT`. `sendMouseReport`/`mouseReport`/`start` carry several
> similar values → (A) param-struct is the cleaner long-term fix.

### Stage 4 — Refactoring (16)

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
