#!/usr/bin/env python3
# Copyright (C) 2026 T. Steinmann (Rodent IV libification fork)
# SPDX-License-Identifier: GPL-3.0-or-later
"""Normalise nondeterministic fields in Rodent IV UCI output before diffing.

Reads stdin (raw engine output from uci_driver.py), writes normalised output to
stdout. Only wall-clock-dependent numbers are replaced; everything that reflects
engine *behaviour* (depth, nodes, score, pv, bestmove, option list, ...) is left
untouched so a behavioural change still shows up as a diff.

Replacements:
  * "info ... time <n> ..."      -> time <T>       (search wall time, ms)
  * "info ... nps <n> ..."       -> nps <N>        (nodes per second)
  * bench summary line
      "<nodes> nodes searched in <ms>, speed <nps> nps (Score: <f>)"
    -> "<nodes> nodes searched in <T>, speed <N> nps (Score: <S>)"
    (nodes are kept -- they are the deterministic part; ms/nps/score-vs-nps
     ratio are wall-clock dependent)
"""

import re
import sys

RE_TIME = re.compile(r"(\btime )\d+")
RE_NPS = re.compile(r"(\bnps )\d+")
RE_BENCH = re.compile(
    r"^(\d+ nodes searched) in \d+, speed \d+ nps \(Score: [0-9.]+\)\s*$")


def normalise(line):
    m = RE_BENCH.match(line)
    if m:
        return m.group(1) + " in <T>, speed <N> nps (Score: <S>)"
    line = RE_TIME.sub(r"\1<T>", line)
    line = RE_NPS.sub(r"\1<N>", line)
    return line


def main():
    out = []
    for line in sys.stdin:
        out.append(normalise(line.rstrip("\n")))
    sys.stdout.write("\n".join(out))
    if out:
        sys.stdout.write("\n")


if __name__ == "__main__":
    main()
