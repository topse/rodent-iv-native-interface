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
  4. personality   : loading a personality changes the search.

Usage: check_liveness.py <engine> [--home DIR]
"""

import argparse
import os
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from filter import normalise  # noqa: E402  (same directory, same harness)


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


def search_result(exe, env, setup, fen, depth=12):
    """Run one fixed-depth search after `setup` and return its last info line.

    Normalised through the harness's own filter, so the comparison is over what
    the search *did* (depth, nodes, score, pv) and not over the wall clock.
    """
    e = Engine(exe, env)
    e.send("uci")
    if not e.wait_for(lambda l: l.strip() == "uciok", 10):
        return None
    e.send("setoption name Threads value 1")   # deterministic: no Lazy SMP
    e.send("setoption name UseBook value false")
    for cmd in setup:
        e.send(cmd)
    e.send("isready")
    if not e.wait_for(lambda l: l.strip() == "readyok", 10):
        return None
    e.send(f"position fen {fen}")
    e.send(f"go depth {depth}")
    if not e.wait_for(lambda l: l.startswith("bestmove"), 60):
        return None
    e.send("quit")
    try:
        e.proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        e.proc.kill()
    infos = [normalise(l) for l in e.lines
             if l.startswith(f"info depth {depth} ") and " score " in l]
    return infos[-1] if infos else None


def scenario_personality(exe, env):
    """A loaded personality must actually reach the evaluation.

    This is not a liveness property but a correctness one, and it lives here
    rather than only in 05_personality_load's baseline because the failure mode
    is silent in *both* directions: ParseSetoption ignores a command it does not
    recognise, and a stale baseline can be re-captured by someone who assumes the
    change was intended. A no-op personality load looks exactly like a working
    one until you compare two of them.

    An asymmetric middlegame (the transcripts' position) so evaluation weights
    have something to disagree about; Threads 1 so the comparison is not down to
    SMP scheduling.
    """
    fen = "r3r1k1/2p2ppp/p1p1bn2/8/1q2P3/2NPQN2/PPP3PP/R4RK1 b - - 2 15"

    base = search_result(exe, env, [], fen)
    if base is None:
        return fail("no search result for the default personality")

    # Both load forms, because they are separate code paths in ParseSetoption.
    by_file = search_result(exe, env, ["setoption name PersonalityFile value cloe.txt"], fen)
    if by_file is None:
        return fail("no search result after PersonalityFile")
    if by_file == base:
        return fail("PersonalityFile did not change the search -- the load was a "
                    "no-op (check the `value` keyword and that basic.ini is found)")

    by_alias = search_result(exe, env, ["setoption name Personality value Tal"], fen)
    if by_alias is None:
        return fail("no search result after Personality")
    if by_alias == base:
        return fail("Personality did not change the search -- the alias did not "
                    "match (strcmp is case-sensitive; basic.ini spells it `Tal`)")

    # A misspelling must stay a no-op, so a future "helpful" fuzzy match does not
    # slip in unnoticed: the engine is characterised, not improved, here.
    mis_cased = search_result(exe, env, ["setoption name Personality value tal"], fen)
    if mis_cased != base:
        return fail("a mis-cased alias changed the search; alias matching is "
                    "expected to stay exact (strcmp)")

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
        ("personality-affects-search", scenario_personality),
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
