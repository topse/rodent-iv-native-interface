#!/usr/bin/env bash
# Copyright (C) 2026 T. Steinmann (Rodent IV libification fork)
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Full verification for the fork -- everything a change must pass before it is
# considered good. Deliberately a plain shell script with no service or hosting
# dependency: run it on a developer machine after cloning, or from whatever CI a
# host happens to use.
#
#   test/run_all.sh            # everything (sanitizers included; several minutes)
#   test/run_all.sh --quick    # skip the ASan/TSan multi-instance runs
#
# Steps:
#   1. CMake build            library + classic executable + example embedder
#   2. Verification harness   transcripts byte-identical to the committed baselines
#                             + liveness checks (test/run_harness.sh)
#   3. Multi-instance suite   two engines in one process, sequential vs concurrent,
#                             plain / ASan / TSan (test/run_multi_instance.sh)
#   4. Example embedder       the documented public-API example actually runs
#   5. Makefile build         the upstream reference build still compiles
#   6. NO_THREADS build       the no-std::thread configuration still compiles and runs
#
# Needs: a C++14 compiler, cmake, make, python3. Linux is the actively tested
# target; the code stays portable, but this fork adds no platform build wiring.

set -u

QUICK=0
[[ "${1:-}" == "--quick" ]] && QUICK=1

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build}"
CXX="${CXX:-g++}"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

fail=0
step() { echo; echo "=== $* ==="; }
check() { # exit-code label
    if [[ $1 -eq 0 ]]; then echo "OK    $2"; else echo "FAIL  $2"; fail=1; fi
}

step "1. CMake build"
cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release >/dev/null &&
    cmake --build "$BUILD_DIR" -j >/dev/null
check $? "cmake build (library + executable + example)"

step "2. Verification harness"
"$REPO_ROOT/test/run_harness.sh" "$BUILD_DIR/rodentIV"
check $? "harness (transcripts + liveness)"

step "3. Multi-instance suite"
if [[ $QUICK -eq 1 ]]; then
    echo "SKIP  sanitized multi-instance suite (--quick)"
else
    "$REPO_ROOT/test/run_multi_instance.sh"
    check $? "multi-instance (plain + ASan + TSan)"
fi

step "4. Example embedder"
"$BUILD_DIR/rodent_example_embed" "$REPO_ROOT" >"$TMP_DIR/example.log" 2>&1 &&
    grep -q "^\[fischer\] bestmove" "$TMP_DIR/example.log" &&
    grep -q "^\[karpov\] bestmove" "$TMP_DIR/example.log"
check $? "example embedder (both instances returned a move)"

step "5. Makefile build (upstream reference build)"
# EXENAME is overridden so the committed mac/rodentIV binary is left untouched.
make -C "$REPO_ROOT/sources" build EXENAME="$TMP_DIR/rodentIV_make" >/dev/null 2>&1
check $? "make -C sources build"

step "6. NO_THREADS build"
"$CXX" -std=c++14 -O2 -DNDEBUG -DNO_THREADS -fno-rtti -w \
    -I"$REPO_ROOT/include" $(ls "$REPO_ROOT"/sources/src/*.cpp) \
    -o "$TMP_DIR/rodent_nothreads" -lm >/dev/null 2>&1 &&
    printf 'uci\nisready\nquit\n' | "$TMP_DIR/rodent_nothreads" >/dev/null
check $? "NO_THREADS build + smoke run"

echo
echo "==================================="
if [[ $fail -eq 0 ]]; then
    echo "run_all: PASS"
else
    echo "run_all: FAIL"
fi
exit $fail
