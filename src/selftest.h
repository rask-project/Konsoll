#pragma once

namespace konsoll
{

// Headless multi-session validation (docs/SESSION-THREADING-PLAN.md §8.8).
// Runs the real manager/backend/worker stack on the offscreen platform and
// returns the process exit code.
int runSelfTest();

} // namespace konsoll
