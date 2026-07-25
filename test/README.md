# Verification harness

The behavioural safety net for the libification refactor (see `../CLAUDE.md`).
It proves that a build still behaves byte-for-byte like the committed baseline
captured from the unmodified upstream sources. Every refactor phase must keep it
green.

## Running

```sh
test/run_all.sh                     # everything: build, harness, sanitized multi-instance
                                    # suite, example embedder, Makefile + NO_THREADS builds
test/run_all.sh --quick             # same, without the ASan/TSan runs

test/run_harness.sh                 # just this harness: build/rodentIV, built via CMake if absent
test/run_harness.sh path/to/engine  # verify a specific binary
test/run_harness.sh --update        # recapture baselines (only when a change is intended)

test/run_multi_instance.sh          # just the two-instance suite (plain, ASan, TSan)
```

`run_all.sh` is the entry point to run before calling a change done. It is a plain shell
script on purpose — no dependency on a git host or a CI service; run it locally after
cloning, or from whatever CI a downstream uses.

Exit code is 0 iff every transcript matches its baseline and every liveness check
passes. A clean clone can go straight to `test/run_harness.sh` — it will configure
and build with CMake first.

## How it works

- **`uci_driver.py`** drives a transcript like a well-behaved UCI GUI: after a
  command that has a terminating response (`uci`→`uciok`, `isready`→`readyok`,
  `go`→`bestmove`, `bench`→`nodes searched`) it *waits* for that response before
  sending the next line. A plain `cat transcript | engine` pipe does **not** work:
  Rodent polls stdin during a search, so a queued `quit`/next command aborts the
  in-progress `go` mid-iteration and the output stops being reproducible.
- **`filter.py`** normalises the only nondeterministic fields — `time`, `nps`, and
  the bench summary's ms/nps/score — leaving everything behavioural (depth, nodes,
  score, pv, bestmove, option list) intact.
- **`run_harness.sh`** runs each `transcripts/*.uci` through driver+filter and
  diffs against `baselines/*.txt`, then runs `check_liveness.py`.
- **`check_liveness.py`** covers behaviour that can't be a byte-identical baseline:
  `stop` interrupts a search and the engine stays alive; `quit` during a search
  exits cleanly (code 0); the book path returns a move; a loaded personality
  changes the search. The first three guard the paths Phase 1 ("no more exit()")
  changes; the last guards against a *silent* regression that a baseline alone
  cannot catch, because a no-op personality load produces perfectly plausible
  output and its baseline can be re-captured by someone assuming the change was
  intended. That is exactly how it went unnoticed once — see below.

## Determinism rules (why the transcripts look the way they do)

- **`RODENT4HOME` is set to the repo root** by the runner, so `personalities/`,
  `books/` and `basic.ini` are found identically every run.
- **`UseBook` is off in every search transcript.** Book move selection is seeded
  with `srand(GetMS())` (`sources/src/book.cpp`), i.e. wall-clock random — book
  output is not reproducible and is only exercised by the book *liveness* check.
- **`Threads` is pinned to 1** (also the default) so searches are single-threaded
  and node-deterministic.

## What each transcript locks

| transcript | covers |
|---|---|
| `01_handshake` | `id` strings + full option banner + `readyok` |
| `02_bench` | bench node count = build/search identity (deterministic; run on the default personality) |
| `03_search_positions` | fixed-depth search over start-moves + 3 FENs; move application |
| `04_eval_option` | eval-weight `setoption` changes the search (locks `setvalue`/`InitPst`/`shouldClear`) |
| `05_personality_load` | both `PersonalityFile` and `Personality` loads change the search |
| `06_missing_personality` | a nonexistent personality degrades, does not crash/exit |

## Corrected 2026-07-25: personality loads were never actually exercised

This section used to record two "upstream quirks" — that a mid-session personality
load changes only the opening book and not the evaluation, and that bench is
personality-invariant. **Both were wrong**, and both came from the same place: the
two personality-loading commands in the transcripts were malformed, so no
personality was ever loaded and the baselines simply captured the default one.

- `setoption name PersonalityFile cloe.txt` was missing the `value` keyword.
  `ParseSetoption` splits on `" name "` and `" value "`; without the second it does
  not recognise the option at all and silently does nothing. `06_missing_personality`
  had the same defect, so it was not testing degradation either.
- `setoption name Personality value tal` used the wrong case. The combo value is
  matched with `strcmp` against the aliases in `personalities/basic.ini`, which
  spells it `Tal`. A miss is silent — `Glob.personalityW` is cleared and no load
  happens.

Measured after fixing, same position and depth, one thread: default `cp -86` /
77074 nodes, Cloe `cp -54` / 96079, Tal `cp 58` / 150975. Bench likewise moves
(557513 → 603848 nodes with Tal loaded). Only `05_personality_load.txt` changed
when the baselines were recaptured; the other five stayed byte-identical, which is
what says the engine itself never behaved differently.

The lesson is in the shape of the failure, not the typo: a UCI engine answers an
unrecognised or unmatched `setoption` with silence, so "the command was accepted"
and "the command did nothing" look the same from outside. `check_liveness.py`'s
`personality-affects-search` now asserts the difference directly, and also that a
mis-cased alias still does nothing — the exact matching is the characterised
behaviour, and it should not quietly become fuzzy either.

## Engine quirks these baselines characterise (upstream behaviour, not bugs to fix here)

- **A failed personality load still resets the evaluation weights.**
  `ReadPersonality` calls `Par.DefaultWeights()` before it opens the file, so a
  missing file leaves the default personality rather than the previous one
  (`06_missing_personality`).
- **Book move selection is wall-clock random** (`srand(GetMS())`, `book.cpp`), so
  book output is not reproducible and is only covered by a liveness check.
