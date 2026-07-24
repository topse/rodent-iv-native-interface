#!/usr/bin/env bash
# Copyright (C) 2026 T. Steinmann (Rodent IV libification fork)
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Verification harness for the Rodent IV libification refactor.
#
# Runs every transcript in test/transcripts/ through the UCI driver and the
# nondeterminism filter, and diffs the result against the committed baseline in
# test/baselines/. Then runs the behavioural liveness checks. Any diff or failed
# check is a behavioural change and fails the harness.
#
# Determinism relies on:
#   * RODENT4HOME pointed at the repo root (finds personalities/ books/ basic.ini);
#   * UseBook off in every search transcript (book moves are time-seeded random);
#   * a line-driven driver that waits for `bestmove` (a plain pipe would let a
#     queued `quit` abort the search mid-iteration).
#
# Usage:
#   test/run_harness.sh [ENGINE]        # verify against committed baselines
#   test/run_harness.sh --update [ENGINE]
#                                       # (re)capture baselines from ENGINE
#
# ENGINE defaults to build/rodentIV; if it is missing, the script configures and
# builds it with CMake first (so: clean clone -> test/run_harness.sh -> green).

set -u

UPDATE=0
if [[ "${1:-}" == "--update" ]]; then
    UPDATE=1
    shift
fi

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEST_DIR="$REPO_ROOT/test"
ENGINE="${1:-$REPO_ROOT/build/rodentIV}"
PY="${PYTHON:-python3}"

export RODENT4HOME="$REPO_ROOT/"

if [[ ! -x "$ENGINE" ]]; then
    echo "engine not found at $ENGINE -- configuring and building with CMake"
    cmake -S "$REPO_ROOT" -B "$REPO_ROOT/build" -DCMAKE_BUILD_TYPE=Release >/dev/null || exit 2
    cmake --build "$REPO_ROOT/build" -j >/dev/null || exit 2
fi

mkdir -p "$TEST_DIR/baselines"

run_one() {  # transcript path -> normalised output on stdout
    "$PY" "$TEST_DIR/uci_driver.py" "$ENGINE" "$1" --home "$RODENT4HOME" \
        | "$PY" "$TEST_DIR/filter.py"
}

pass=0; fail=0; updated=0
for tr in "$TEST_DIR"/transcripts/*.uci; do
    name="$(basename "$tr" .uci)"
    base="$TEST_DIR/baselines/$name.txt"
    if [[ $UPDATE -eq 1 ]]; then
        run_one "$tr" > "$base"
        echo "updated  $name"
        updated=$((updated+1))
        continue
    fi
    actual="$(run_one "$tr")"
    if [[ ! -f "$base" ]]; then
        echo "MISSING baseline: $name (run with --update)"
        fail=$((fail+1))
        continue
    fi
    if diff -u "$base" <(printf '%s\n' "$actual") >/tmp/harness_diff.$$; then
        echo "PASS  $name"
        pass=$((pass+1))
    else
        echo "FAIL  $name"
        cat /tmp/harness_diff.$$
        fail=$((fail+1))
    fi
    rm -f /tmp/harness_diff.$$
done

if [[ $UPDATE -eq 1 ]]; then
    echo "baselines updated: $updated"
    exit 0
fi

echo "--- liveness ---"
if "$PY" "$TEST_DIR/check_liveness.py" "$ENGINE" --home "$RODENT4HOME"; then
    echo "PASS  liveness"
    pass=$((pass+1))
else
    echo "FAIL  liveness"
    fail=$((fail+1))
fi

echo "==================================="
echo "transcripts+checks: $pass passed, $fail failed"
[[ $fail -eq 0 ]] && exit 0 || exit 1
