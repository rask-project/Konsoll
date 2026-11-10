#!/usr/bin/env python3
"""Drive a terminal program against Konsoll's parser, headless.

Runs COMMAND in a pseudo-terminal, types the given keys, answers the
program's queries with the replies Konsoll's Terminal queues, and prints the
screen as Konsoll sees it (via `konsoll --screen-dump`).

Steps (in order):
  -k TEXT   type TEXT (Python escapes: \\r, \\x1b, ...)
  -w SECS   wait, answering queries meanwhile
  -d        dump the screen (add --attrs to show styles)

Example:
  tools/vtdrive.py -k '1\\r' -w 1 -d -- vttest
"""

import argparse
import codecs
import fcntl
import os
import pty
import select
import struct
import subprocess
import sys
import termios
import time

DEFAULT_BINARY = os.path.join(
    os.path.dirname(__file__), "..", "build", "Qt_6_12_0_for_macOS_Debug",
    "konsoll.app", "Contents", "MacOS", "konsoll")


class Session:
    def __init__(self, binary, rows, cols, command, attrs):
        self.binary = binary
        self.rows = rows
        self.cols = cols
        self.attrs = attrs
        self.output = b""
        self.replied = b""
        pid, fd = pty.fork()
        if pid == 0:
            os.environ["TERM"] = "xterm-256color"
            os.environ["COLORTERM"] = "truecolor"
            os.environ["TERM_PROGRAM"] = "Konsoll"
            os.execvp(command[0], command)
        self.pid = pid
        self.fd = fd
        fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, cols * 8, rows * 16))

    def dump(self):
        result = subprocess.run(
            [self.binary, "--screen-dump", str(self.rows), str(self.cols)] + (["--attrs"] if self.attrs else []),
            input=self.output, capture_output=True, check=False)
        return result.stdout.decode("utf-8", "replace")

    def answer(self):
        """Send the replies Konsoll queued that the program has not got yet."""
        text = self.dump()
        line = next((l for l in text.splitlines() if l.startswith("--responses ")), "--responses ")
        escaped = line[len("--responses "):].replace("\\e", "\\x1b")
        replies = codecs.decode(escaped, "unicode_escape").encode("latin-1")
        if replies.startswith(self.replied) and len(replies) > len(self.replied):
            os.write(self.fd, replies[len(self.replied):])
            self.replied = replies

    def pump(self, seconds):
        deadline = time.time() + seconds
        while time.time() < deadline:
            ready, _, _ = select.select([self.fd], [], [], 0.05)
            if not ready:
                continue
            try:
                chunk = os.read(self.fd, 65536)
            except OSError:
                return
            if not chunk:
                return
            self.output += chunk
            self.answer()

    def type(self, text):
        os.write(self.fd, codecs.decode(text, "unicode_escape").encode("utf-8"))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--rows", type=int, default=24)
    parser.add_argument("--cols", type=int, default=80)
    parser.add_argument("--attrs", action="store_true")
    parser.add_argument("--binary", default=DEFAULT_BINARY)
    parser.add_argument("--raw", help="also save the program's raw output to this file")
    parser.add_argument("-k", dest="steps", action="append", type=lambda v: ("k", v))
    parser.add_argument("-w", dest="steps", action="append", type=lambda v: ("w", float(v)))
    parser.add_argument("-d", dest="steps", action="append_const", const=("d", None))
    parser.add_argument("command", nargs="+")
    args = parser.parse_args()

    session = Session(args.binary, args.rows, args.cols, args.command, args.attrs)
    session.pump(1.0)
    for kind, value in args.steps or [("d", None)]:
        if kind == "k":
            session.type(value)
            session.pump(0.4)
        elif kind == "w":
            session.pump(value)
        else:
            print(session.dump(), end="")
            print("=" * args.cols)
    if args.raw:
        with open(args.raw, "wb") as raw:
            raw.write(session.output)
    try:
        os.kill(session.pid, 9)
    except OSError:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
