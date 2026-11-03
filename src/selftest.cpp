#include "selftest.h"

#include "TerminalManager.h"

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
    if (condition) {
        std::println(std::cout, "PASS  {}", what);
    } else {
        std::println(std::cout, "FAIL  {}", what);
        ++failures;
    }
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

using konsoll::TerminalBackend;
using konsoll::TerminalManager;

TerminalBackend *activeSession(TerminalManager &manager)
{
    return static_cast<TerminalBackend *>(manager.activeSession());
}

} // namespace

namespace konsoll
{

int runSelfTest()
{
    // Multi-session validation (docs/SESSION-THREADING-PLAN.md §8.8): the real
    // manager/backend/worker stack, headless on the offscreen platform.
    std::println(std::cout, "Konsoll selftest (session threading §8.8)");
    TerminalManager manager(nullptr);
    manager.start();
    int failures = 0;

    // 1. Concurrent output: three sessions produce output at the same time.
    auto *a = activeSession(manager);
    manager.addWorkspace();
    auto *b = activeSession(manager);
    manager.addWorkspace();
    auto *c = activeSession(manager);
    check(failures, manager.sessionCount() == 3, "three sessions created");

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
