#!/usr/bin/env bash
#
# Copyright (C) 2026 T. Steinmann
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build and run the multi-instance verification (phase 4): two rodent::Engine
# instances, one with a PawnValueMg eval override, driven sequentially and then
# concurrently; each instance's concurrent output must equal its own sequential run,
# and the two instances must differ. Built and run in three modes:
#   plain -O2   -- functional + determinism (repeated to shake out flakiness)
#   ASan        -- memory errors
#   TSan        -- data races on any still-shared state
#
# Links every sources/src/*.cpp EXCEPT main.cpp (the standalone adapter), plus
# test/multi_instance_test.cpp. Needs RODENT4HOME so personalities/books/basic.ini
# resolve (set here to the repo root, like run_harness.sh).

set -u
TEST_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$TEST_DIR/.." && pwd)"
SRC_DIR="$REPO_ROOT/sources/src"
OUT_DIR="${TMPDIR:-/tmp}/rodent_mi.$$"
mkdir -p "$OUT_DIR"
trap 'rm -rf "$OUT_DIR"' EXIT

export RODENT4HOME="$REPO_ROOT/"

CXX="${CXX:-g++}"
STD="-std=c++14"
SRCS=$(ls "$SRC_DIR"/*.cpp | grep -v '/main.cpp$')
TEST_SRC="$TEST_DIR/multi_instance_test.cpp"
COMMON="-fno-rtti -pthread -I$SRC_DIR"

fail=0

build() { # name, extra flags
    local name="$1"; shift
    echo ">> building $name"
    if ! "$CXX" $STD $COMMON "$@" $SRCS "$TEST_SRC" -o "$OUT_DIR/$name" -lm; then
        echo "   BUILD FAILED ($name)"; fail=1; return 1
    fi
}

run_reps() { # name, reps, [env...]
    local name="$1" reps="$2"; shift 2
    local ok=0 bad=0 i rc
    for ((i = 1; i <= reps; i++)); do
        env "$@" "$OUT_DIR/$name" >"$OUT_DIR/$name.log" 2>&1
        rc=$?
        if [ $rc -eq 0 ]; then ok=$((ok + 1)); else bad=$((bad + 1)); fi
    done
    echo "   $name: $ok/$reps ok"
    if [ $bad -ne 0 ]; then
        echo "   ---- last failing/last run output ----"; cat "$OUT_DIR/$name.log"; fail=1
    fi
}

# 1. plain -O2, repeated (determinism / flakiness)
if build mi -O2 -DNDEBUG; then
    run_reps mi 10
fi

# 2. ASan
if build mi_asan -O1 -g -fsanitize=address -fno-omit-frame-pointer; then
    run_reps mi_asan 3 ASAN_OPTIONS=detect_leaks=0
fi

# 3. TSan
if build mi_tsan -O1 -g -fsanitize=thread -fno-omit-frame-pointer; then
    run_reps mi_tsan 3 TSAN_OPTIONS=halt_on_error=0
fi

echo "==================================="
if [ $fail -eq 0 ]; then
    echo "multi-instance suite: PASS (plain + ASan + TSan)"
else
    echo "multi-instance suite: FAIL"
fi
exit $fail
