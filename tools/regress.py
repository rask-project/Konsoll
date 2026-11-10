#!/usr/bin/env python3
"""Regression fixtures: recorded terminal output and the screen it must give.

tests/fixtures/NAME-ROWSxCOLS.raw holds a program's raw output (recorded
with `tools/vtdrive.py --raw`); NAME-ROWSxCOLS.expected the --screen-dump
--attrs of it. `konsoll --selftest` replays every fixture and fails on any
difference; this script lists, checks or regenerates them.

  tools/regress.py            check every fixture, show diffs
  tools/regress.py --update   rewrite the .expected files (review the diff!)
"""

import argparse
import difflib
import os
import re
import subprocess
import sys

ROOT = os.path.join(os.path.dirname(__file__), "..")
FIXTURES = os.path.join(ROOT, "tests", "fixtures")
DEFAULT_BINARY = os.path.join(ROOT, "build", "Qt_6_12_0_for_macOS_Debug", "konsoll.app", "Contents", "MacOS", "konsoll")


def dump(binary, raw, rows, cols):
    with open(raw, "rb") as data:
        result = subprocess.run([binary, "--screen-dump", rows, cols, "--attrs"], stdin=data,
                                capture_output=True, check=True)
    return result.stdout.decode("utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--update", action="store_true")
    parser.add_argument("--binary", default=DEFAULT_BINARY)
    args = parser.parse_args()

    failures = 0
    for name in sorted(os.listdir(FIXTURES)):
        match = re.fullmatch(r"(.+)-(\d+)x(\d+)\.raw", name)
        if not match:
            continue
        raw = os.path.join(FIXTURES, name)
        expected_path = raw[: -len(".raw")] + ".expected"
        actual = dump(args.binary, raw, match.group(2), match.group(3))
        if args.update:
            with open(expected_path, "w", encoding="utf-8") as out:
                out.write(actual)
            print(f"updated  {name}")
            continue
        expected = open(expected_path, encoding="utf-8").read() if os.path.exists(expected_path) else ""
        if actual == expected:
            print(f"ok       {name}")
        else:
            failures += 1
            print(f"DIFFERS  {name}")
            sys.stdout.writelines(difflib.unified_diff(expected.splitlines(True), actual.splitlines(True),
                                                       "expected", "actual", n=1))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
