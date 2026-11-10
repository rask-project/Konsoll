#pragma once

namespace konsoll
{

// Headless multi-session validation (docs/SESSION-THREADING-PLAN.md §8.8).
// Runs the real manager/backend/worker stack and returns the process exit code.
int runSelfTest();

// Prints PASS/FAIL for one check and counts failures.
void selfTestCheck(int &failures, bool condition, const char *what);

// Escape-sequence parser checks (src/parserselftest.cpp), run by runSelfTest.
void runParserSelfTest(int &failures);

} // namespace konsoll
