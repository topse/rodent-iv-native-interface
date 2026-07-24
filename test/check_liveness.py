#!/usr/bin/env python3
# Copyright (C) 2026 T. Steinmann (Rodent IV libification fork)
# SPDX-License-Identifier: GPL-3.0-or-later
"""Behavioural liveness checks that can't be expressed as byte-identical baselines.

These guard exactly the paths Phase 1 ("no more exit()") will touch: a search
must be interruptible with `stop`, the engine must stay alive afterwards, and
`quit` must terminate the process cleanly (exit code 0) rather than via a raw
exit()/abort in mid-search.

Scenarios (all must pass; exits nonzero on the first failure):
  1. stop-liveness : go infinite -> stop -> a bestmove appears -> engine still
                     answers isready -> quit exits cleanly (returncode 0).
  2. quit-in-search: start a long search, send quit while it runs -> process
                     exits cleanly (0) within a timeout (no hang, no crash).
  3. book-liveness : with the book on, startpos produces some bestmove (the book
                     path runs and returns a move; the move itself is random).

Usage: check_liveness.py <engine> [--home DIR]
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
            [exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, bufsize=1, env=env)
        self.lines = []
        self._consumed = 0  # high-water mark: lines already waited past
        self._cv = threading.Condition()
        self._t = threading.Thread(target=self._drain, daemon=True)
        self._t.start()

    def _drain(self):
        for line in self.proc.stdout:
            with self._cv:
                self.lines.append(line.rstrip("\n"))
                self._cv.notify_all()
        with self._cv:
            self._cv.notify_all()

    def send(self, cmd):
        self.proc.stdin.write(cmd + "\n")
        self.proc.stdin.flush()

    def wait_for(self, pred, timeout):
        end = time.monotonic() + timeout
        with self._cv:
            while True:
                while self._consumed < len(self.lines):
                    line = self.lines[self._consumed]
                    self._consumed += 1
                    if pred(line):
                        return True
                if self.proc.poll() is not None and self._consumed >= len(self.lines):
                    return False
                rem = end - time.monotonic()
                if rem <= 0:
                    return False
                self._cv.wait(rem)


def fail(msg):
    print(f"  FAIL: {msg}")
    return False


def scenario_stop(exe, env):
    e = Engine(exe, env)
    e.send("uci")
    if not e.wait_for(lambda l: l.strip() == "uciok", 10):
        return fail("no uciok")
    e.send("setoption name UseBook value false")  # force a real search, not a book move
    e.send("isready")
    if not e.wait_for(lambda l: l.strip() == "readyok", 10):
        return fail("no readyok before search")
    e.send("position startpos")
    e.send("go infinite")
    time.sleep(1.0)
    e.send("stop")
    if not e.wait_for(lambda l: l.startswith("bestmove"), 10):
        return fail("no bestmove after stop")
    e.send("isready")
    if not e.wait_for(lambda l: l.strip() == "readyok", 10):
        return fail("engine unresponsive after stop")
    e.send("quit")
    try:
        rc = e.proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        e.proc.kill()
        return fail("did not exit after quit")
    if rc != 0:
        return fail(f"nonzero exit code after quit: {rc}")
    return True


def scenario_quit_in_search(exe, env):
    e = Engine(exe, env)
    e.send("uci")
    if not e.wait_for(lambda l: l.strip() == "uciok", 10):
        return fail("no uciok")
    e.send("setoption name UseBook value false")
    e.send("position startpos")
    e.send("go infinite")
    time.sleep(0.5)
    e.send("quit")           # quit while the search is running
    try:
        rc = e.proc.wait(timeout=8)
    except subprocess.TimeoutExpired:
        e.proc.kill()
        return fail("hung when quit sent during search")
    if rc != 0:
        return fail(f"nonzero exit code on quit-in-search: {rc}")
    return True


def scenario_book(exe, env):
    e = Engine(exe, env)
    e.send("uci")
    if not e.wait_for(lambda l: l.strip() == "uciok", 10):
        return fail("no uciok")
    e.send("setoption name UseBook value true")
    e.send("position startpos")
    e.send("go depth 8")
    if not e.wait_for(lambda l: l.startswith("bestmove") and len(l.split()) >= 2, 20):
        return fail("no bestmove with book on")
    e.send("quit")
    try:
        e.proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        e.proc.kill()
        return fail("did not exit after quit")
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("engine")
    ap.add_argument("--home")
    args = ap.parse_args()

    env = dict(os.environ)
    if args.home:
        env["RODENT4HOME"] = args.home

    scenarios = [
        ("stop-liveness", scenario_stop),
        ("quit-in-search", scenario_quit_in_search),
        ("book-liveness", scenario_book),
    ]
    ok = True
    for name, fn in scenarios:
        print(f"[liveness] {name}")
        try:
            passed = fn(args.engine, env)
        except Exception as ex:  # noqa: BLE001
            passed = fail(f"exception: {ex}")
        if passed:
            print("  PASS")
        else:
            ok = False
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
