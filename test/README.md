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
  exits cleanly (code 0); the book path returns a move. These directly guard the
  paths Phase 1 ("no more exit()") changes.

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
| `02_bench` | bench node count = build/search identity (deterministic; note: bench is *personality-invariant*) |
| `03_search_positions` | fixed-depth search over start-moves + 3 FENs; move application |
| `04_eval_option` | eval-weight `setoption` changes the search (locks `setvalue`/`InitPst`/`shouldClear`) |
| `05_personality_load` | `PersonalityFile` + `Personality` load paths run (characterisation — see quirk below) |
| `06_missing_personality` | a nonexistent personality degrades, does not crash/exit |

## Engine quirks these baselines characterise (upstream behaviour, not bugs to fix here)

- **Bench ignores the loaded personality** — it reports the same node count for
  every personality. Bench therefore baselines the build/search, not eval tuning.
- **A mid-session personality load changes only the opening book, not the
  mid-game eval.** With the book off, `05_personality_load` searches are identical
  to the default personality. Direct eval `setoption`s (`04_eval_option`) *do*
  change the search. If the refactor alters either behaviour, the diff catches it.
  (Whether the personality-eval no-op is a latent upstream bug is a question for
  the maintainer; this refactor only preserves current behaviour.)
