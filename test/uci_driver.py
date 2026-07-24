#!/usr/bin/env python3
# Copyright (C) 2026 T. Steinmann (Rodent IV libification fork)
# SPDX-License-Identifier: GPL-3.0-or-later
"""Drive a Rodent IV UCI session from a transcript file and print the engine's stdout.

Why this exists instead of a plain `cat transcript | engine` pipe:
Rodent polls stdin *during* a search, so a `quit` (or the next command) that is
already sitting in the pipe aborts an in-progress `go` mid-iteration -- the output
then depends on how far the search happened to get, which is not reproducible.
This driver behaves like a well-mannered UCI GUI: after a command that produces a
terminating response it waits for that response before sending the next command.

Synchronisation points (everything else is fire-and-forget):
    uci      -> wait for a line == "uciok"
    isready  -> wait for a line == "readyok"
    go ...   -> wait for a line starting with "bestmove"
    bench .. -> wait for a line containing "nodes searched"
    quit     -> send, then drain until the process exits

Transcript format: one UCI command per line. Blank lines and lines beginning
with '#' are ignored (comments). If the file has no explicit `quit`, one is sent.

The engine's stdout is reproduced verbatim (no command echo, no added prefixes),
i.e. exactly what a GUI would see. Nondeterministic fields (time/nps) are left
intact here; normalise them with filter.py before diffing.
"""

import argparse
import os
import subprocess
import sys
import threading
import time


class Engine:
    def __init__(self, exe, env):
        self.proc = subprocess.Popen(
            [exe],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,  # line buffered
            env=env,
        )
        self.lines = []            # every stdout line received, in order
        self._consumed = 0         # high-water mark: lines already waited past
        self._lock = threading.Lock()
        self._cv = threading.Condition(self._lock)
        self._reader = threading.Thread(target=self._drain, daemon=True)
        self._reader.start()

    def _drain(self):
        for line in self.proc.stdout:
            line = line.rstrip("\n")
            with self._cv:
                self.lines.append(line)
                self._cv.notify_all()
        with self._cv:
            self._cv.notify_all()

    def send(self, cmd):
        if self.proc.stdin:
            self.proc.stdin.write(cmd + "\n")
            self.proc.stdin.flush()

    def wait_for(self, predicate, timeout):
        """Block until a line *after* the last consumed one satisfies predicate.

        Uses a persistent high-water mark so a later wait cannot match output
        left over from an earlier command (e.g. a previous search's bestmove).
        """
        deadline = time.monotonic() + timeout
        with self._cv:
            while True:
                while self._consumed < len(self.lines):
                    line = self.lines[self._consumed]
                    self._consumed += 1
                    if predicate(line):
                        return True
                if self.proc.poll() is not None and self._consumed >= len(self.lines):
                    return False  # process exited without the expected line
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return False
                self._cv.wait(remaining)

    def drain_until_exit(self, timeout):
        try:
            self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        self._reader.join(timeout=2)


def sync_point(cmd):
    """Return (predicate, description) for a command that expects a response, else None."""
    head = cmd.split(None, 1)[0] if cmd.split() else ""
    if head == "uci":
        return (lambda l: l.strip() == "uciok", "uciok")
    if head == "isready":
        return (lambda l: l.strip() == "readyok", "readyok")
    if head == "go":
        return (lambda l: l.startswith("bestmove"), "bestmove")
    if head == "bench":
        return (lambda l: "nodes searched" in l, "nodes searched")
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("engine", help="path to the rodent executable")
    ap.add_argument("transcript", help="path to a .uci transcript file")
    ap.add_argument("--home", help="value for RODENT4HOME (default: unset)")
    ap.add_argument("--timeout", type=float, default=120.0,
                    help="per-sync-point timeout in seconds (default 120)")
    args = ap.parse_args()

    env = dict(os.environ)
    if args.home:
        env["RODENT4HOME"] = args.home

    with open(args.transcript) as f:
        commands = []
        for raw in f:
            s = raw.strip()
            if not s or s.startswith("#"):
                continue
            commands.append(s)

    eng = Engine(args.engine, env)
    saw_quit = False
    for cmd in commands:
        eng.send(cmd)
        if cmd.split()[0] == "quit":
            saw_quit = True
            break
        sp = sync_point(cmd)
        if sp:
            ok = eng.wait_for(sp[0], args.timeout)
            if not ok:
                sys.stderr.write(
                    "uci_driver: timed out or engine exited waiting for "
                    f"'{sp[1]}' after command: {cmd}\n")
                eng.proc.kill()
                break

    if not saw_quit:
        eng.send("quit")
    eng.drain_until_exit(timeout=10)

    sys.stdout.write("\n".join(eng.lines))
    if eng.lines:
        sys.stdout.write("\n")


if __name__ == "__main__":
    main()
