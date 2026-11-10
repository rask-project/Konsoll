#include "selftest.h"

#include "TerminalManager.h"
#include "term/Terminal.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QProcess>
#include <QThread>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <functional>
#include <iostream>
#include <print>

namespace
{

void check(int &failures, bool condition, const char *what)
{
    konsoll::selfTestCheck(failures, condition, what);
}

// Spin-pump the event loop until `pred` holds or the timeout elapses.
bool waitFor(const char *what, const std::function<bool()> &pred, int timeoutMs)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents();
        if (pred()) {
            return true;
        }
        QThread::msleep(2);
    }
    std::println(std::cout, "timeout waiting for: {}", what);
    return pred();
}

// Count processes whose command line matches `pattern` (pgrep -f).
int pgrepCount(const char *pattern)
{
    QProcess process;
    process.start("pgrep", {"-f", pattern});
    process.waitForFinished(2000);
    const QByteArray output = process.readAllStandardOutput().trimmed();
    if (output.isEmpty()) {
        return 0;
    }
    const auto lines = std::ranges::count(output, '\n');
    return 1 + static_cast<int>(lines);
}

using konsoll::Cell;
using konsoll::Screen;
using konsoll::Terminal;
using konsoll::TerminalBackend;
using konsoll::TerminalManager;

TerminalBackend *activeSession(TerminalManager &manager)
{
    return static_cast<TerminalBackend *>(manager.activeSession());
}

} // namespace

namespace konsoll
{

void selfTestCheck(int &failures, bool condition, const char *what)
{
    if (condition) {
        std::println(std::cout, "PASS  {}", what);
    } else {
        std::println(std::cout, "FAIL  {}", what);
        ++failures;
    }
}

int runSelfTest()
{
    // Multi-session validation (docs/SESSION-THREADING-PLAN.md §8.8): the real
    // manager/backend/worker stack, headless on the offscreen platform.
    std::println(std::cout, "Konsoll selftest (session threading §8.8)");
    TerminalManager manager(nullptr);
    manager.start();
    int failures = 0;

    // 0. Parser: zero-width code points take no cell. OpenTUI apps (Qwen Code)
    // suffix icons with VS15; a cell per selector pushed their last column
    // onto the next row.
    {
        Terminal term(4, 10, QStringLiteral("selftest"));
        term.write(QByteArray("\xe2\x97\x86\xef\xb8\x8e" "ab")); // U+25C6 U+FE0E a b
        const Screen &screen = term.screen();
        check(failures,
              screen.cellAt(0, 0).ch == 0x25c6u && screen.cellAt(0, 1).ch == 'a' && screen.cellAt(0, 2).ch == 'b' &&
                  screen.cursorCol() == 3,
              "zero-width: variation selector takes no cell");
        term.write(QByteArray("\r\nac\xcc\xa7" "a\xcc\x83" "o")); // NFD "ação"
        check(failures,
              screen.cellAt(1, 1).ch == 0xe7u && screen.cellAt(1, 2).ch == 0xe3u && screen.cellAt(1, 3).ch == 'o' &&
                  screen.cursorCol() == 4,
              "zero-width: combining marks compose (NFD -> NFC)");
    }

    // 0a. Prefixed CSI sequences are not SGR: Claude Code's modifyOtherKeys
    // request (CSI > 4 ; 2 m) must not underline/dim the following text.
    {
        Terminal term(4, 10, QStringLiteral("selftest"));
        term.write(QByteArray("\x1b[>4;2mabc\x1b[>4m\x1b[>0q"));
        const unsigned char flags = term.screen().cellAt(0, 0).flags;
        check(failures,
              (flags & (konsoll::CellFlag::Underline | konsoll::CellFlag::Dim)) == 0u &&
                  term.screen().cellAt(0, 2).ch == 'c' && term.screen().cursorCol() == 3,
              "csi: CSI > 4;2 m (modifyOtherKeys) is not SGR");
    }

    // 0d. Control characters never land in a cell: a stray C0 byte or C1 code
    // point rendered as a missing-glyph box over the text.
    {
        Terminal term(4, 10, QStringLiteral("selftest"));
        term.write(QByteArray("a\x0e\x0f\x01\x7f" "b\xc2\x9b" "c"));
        const Screen &screen = term.screen();
        check(failures,
              screen.cellAt(0, 0).ch == 'a' && screen.cellAt(0, 1).ch == 'b' && screen.cellAt(0, 2).ch == 'c' &&
                  screen.cursorCol() == 3,
              "controls: C0 and C1 controls take no cell");
    }

    // 0c. Background Color Erase + ECH: Neovim paints its theme background with
    // erase sequences after setting the SGR background.
    {
        Terminal term(4, 10, QStringLiteral("selftest"));
        const Screen &screen = term.screen();
        term.write(QByteArray("abcdef\x1b[48;2;1;2;3m\r\x1b[2X\x1b[4G\x1b[K"));
        constexpr unsigned int kBg = 0x80010203u;
        check(failures,
              screen.cellAt(0, 0).ch == ' ' && screen.cellAt(0, 0).bg == kBg && screen.cellAt(0, 1).bg == kBg &&
                  screen.cellAt(0, 2).ch == 'c' && screen.cellAt(0, 2).bg == 0u,
              "erase: ECH blanks N cells with the current background");
        check(failures, screen.cellAt(0, 3).ch == ' ' && screen.cellAt(0, 9).bg == kBg,
              "erase: EL fills with the current background (BCE)");
        term.write(QByteArray("\x1b[0m\x1b[2J"));
        check(failures, screen.cellAt(1, 5).bg == 0u, "erase: SGR reset restores default-background erase");
    }

    // 0b. Wide characters take two cells (lead + spacer), as TUIs assume.
    {
        const auto isWide = [](const Cell &cell) { return (cell.flags & konsoll::CellFlag::Wide) != 0u; };
        const auto isSpacer = [](const Cell &cell) { return (cell.flags & konsoll::CellFlag::WideSpacer) != 0u; };
        Terminal term(4, 10, QStringLiteral("selftest"));
        const Screen &screen = term.screen();
        term.write(QByteArray("a\xe2\x9c\x85" "b")); // a U+2705 b
        check(failures,
              isWide(screen.cellAt(0, 1)) && screen.cellAt(0, 1).ch == 0x2705u && isSpacer(screen.cellAt(0, 2)) &&
                  screen.cellAt(0, 3).ch == 'b' && screen.cursorCol() == 4,
              "wide: emoji occupies two cells");
        term.write(QByteArray("\r\xe4\xb8\xad")); // overwrite a, then U+4E2D over the emoji's lead
        term.write(QByteArray("\rz"));           // z over the CJK lead breaks the pair
        check(failures, screen.cellAt(0, 0).ch == 'z' && !isSpacer(screen.cellAt(0, 1)) && screen.cellAt(0, 1).ch == ' ',
              "wide: overwriting a lead clears its spacer");
        term.write(QByteArray("\r\n\xe2\x9d\xa4\xef\xb8\x8f" "c")); // U+2764 U+FE0F c
        check(failures, isWide(screen.cellAt(1, 0)) && isSpacer(screen.cellAt(1, 1)) && screen.cellAt(1, 2).ch == 'c',
              "wide: VS16 widens the previous character");
        term.write(QByteArray("\r\nxxxxxxxxx\xe4\xb8\xad")); // 9 cells + wide: wraps whole
        check(failures,
              isSpacer(screen.cellAt(2, 9)) && isWide(screen.cellAt(3, 0)) && screen.cellAt(3, 0).ch == 0x4e2du,
              "wide: no room in the last column wraps the whole character");
    }
    {
        const auto isWide = [](const Cell &cell) { return (cell.flags & konsoll::CellFlag::Wide) != 0u; };
        Terminal term(4, 10, QStringLiteral("selftest"));
        const Screen &screen = term.screen();
        term.write(QByteArray("abcdefgh\xe4\xb8\xad")); // fills exactly 10 cells
        term.resize(4, 9);
        check(failures, screen.cellAt(0, 7).ch == 'h' && isWide(screen.cellAt(1, 0)),
              "wide: reflow never splits a wide pair");
        term.resize(4, 10);
        check(failures, isWide(screen.cellAt(0, 8)) && screen.cellAt(0, 8).ch == 0x4e2du,
              "wide: reflow re-joins without the wrap padding");
    }

    runParserSelfTest(failures);

    // 1. Concurrent output: three sessions produce output at the same time.
    auto *a = activeSession(manager);
    manager.addWorkspace();
    auto *b = activeSession(manager);
    manager.addWorkspace();
    auto *c = activeSession(manager);
    check(failures, manager.sessionCount() == 3, "three sessions created");

    // Named workspace (Ctrl+Shift+T / Cmd+Shift+T): the custom title wins
    // over the directory, and an empty title stays unnamed.
    manager.addNamedWorkspace(QStringLiteral("Named WS"));
    {
        auto *named = static_cast<TerminalWorkspaceModel *>(manager.workspaces().last().value<QObject *>());
        check(failures, named != nullptr && named->title() == QStringLiteral("Named WS"),
              "named workspace: custom title shown");
    }
    manager.addNamedWorkspace(QString{});
    {
        auto *unnamed = static_cast<TerminalWorkspaceModel *>(manager.workspaces().last().value<QObject *>());
        check(failures, unnamed != nullptr && unnamed->customTitle().isEmpty(),
              "named workspace: empty title stays unnamed");
    }

    a->writeText("seq 1 5000\n");
    b->writeText("seq 1 5000\n");
    c->writeText("seq 1 5000\n");
    check(failures, waitFor("a: seq output", [a]() { return a->bufferText().contains("\n5000\n"); }, 20000), "concurrent output: session A");
    check(failures, waitFor("b: seq output", [b]() { return b->bufferText().contains("\n5000\n"); }, 20000), "concurrent output: session B");
    check(failures, waitFor("c: seq output", [c]() { return c->bufferText().contains("\n5000\n"); }, 20000), "concurrent output: session C");

    // 2. Independent input: each session receives only its own input.
    a->writeText("echo SELFTEST_A\n");
    b->writeText("echo SELFTEST_B\n");
    check(failures, waitFor("a: marker", [a]() { return a->bufferText().contains("SELFTEST_A"); }, 10000), "independent input: A echo");
    check(failures, waitFor("b: marker", [b]() { return b->bufferText().contains("SELFTEST_B"); }, 10000), "independent input: B echo");
    check(failures, !a->bufferText().contains("SELFTEST_B"), "independent input: A has no B marker");
    check(failures, !b->bufferText().contains("SELFTEST_A"), "independent input: B has no A marker");

    // 3. Resize isolation.
    const int bCols = b->cols();
    const int bRows = b->rows();
    a->resize(120, 40);
    check(failures, a->cols() == 120 && a->rows() == 40, "resize: A resized");
    check(failures, b->cols() == bCols && b->rows() == bRows, "resize: B unchanged");

    // 4. Process exit: natural exit auto-closes only that session.
    const int countBefore = manager.sessionCount();
    c->writeText("exit\n");
    check(failures, waitFor("c: auto close", [&manager, &countBefore]() { return manager.sessionCount() == countBefore - 1; }, 15000), "process exit: C auto-closed");
    a->writeText("echo STILL_ALIVE_A\n");
    check(failures, waitFor("a: still alive", [a]() { return a->bufferText().contains("STILL_ALIVE_A"); }, 10000), "process exit: A still running");

    // 5. Fast close/reopen, bounded close time.
    const int baseline = manager.sessionCount();
    for (int i = 0; i < 5; ++i) {
        manager.addWorkspace();
        auto *s = activeSession(manager);
        std::array<char, 48> label{};
        std::snprintf(label.data(), label.size(), "reopen %d: shell output", i);
        check(failures, waitFor(label.data(), [s]() { return !s->bufferText().isEmpty(); }, 15000), "fast close/reopen: shell output");
        QElapsedTimer timer;
        timer.start();
        manager.closeSession(s);
        const qint64 elapsedMs = timer.elapsed();
        check(failures, manager.sessionCount() == baseline, "fast close/reopen: session removed");
        check(failures, elapsedMs < 3000, "fast close/reopen: close bounded");
    }

    // 6. App shutdown while a shell is running: no orphaned process group.
    manager.addWorkspace();
    auto *s = activeSession(manager);
    check(failures, waitFor("shutdown: shell output", [s]() { return !s->bufferText().isEmpty(); }, 15000), "shutdown: shell produced output");
    s->writeText("sleep 31337\n");
    check(failures, waitFor("shutdown: sleep spawned", []() { return pgrepCount("sleep 31337") > 0; }, 10000), "shutdown: sleep started");
    manager.closeSession(s);
    check(failures, waitFor("shutdown: sleep gone", []() { return pgrepCount("sleep 31337") == 0; }, 5000), "shutdown: no orphaned sleep");

    // 7. Output backpressure: sustained output above the 1 MiB high-water mark.
    manager.addWorkspace();
    auto *t = activeSession(manager);
    check(failures, waitFor("backpressure: shell output", [t]() { return !t->bufferText().isEmpty(); }, 15000), "backpressure: shell produced output");
    t->writeText("seq 1 250000\n");
    check(failures, waitFor("backpressure: seq done", [t]() { return t->maxScrollOffset() >= 249900; }, 60000), "backpressure: 250k lines delivered");
    check(failures, t->bufferText().contains("\n250000\n"), "backpressure: last line intact");

    // Remaining sessions (A and B) are closed by the manager destructor.
    if (failures == 0) {
        std::println(std::cout, "selftest: all checks passed");
    } else {
        std::println(std::cout, "selftest: {} check(s) failed", failures);
    }
    return failures == 0 ? 0 : 1;
}

} // namespace konsoll
