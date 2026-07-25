# Rodent IV — libification fork

A fork of [nescitus/rodent-iv](https://github.com/nescitus/rodent-iv) restructured the way
Stockfish is structured: an **embeddable engine class** with line-based input and callback
output — no stdio, no `exit()`, no process-global mutable state — while the **classic
executable keeps building and behaving identically**, reduced to a thin stdin/stdout adapter
over that class.

The intended consumer is an application that embeds engines in-process (one instance per
concurrent role) rather than spawning them as child processes.

**The chess is untouched.** Search, evaluation and personalities are upstream's; every
refactoring step is verified to leave the engine's output byte-for-byte unchanged, apart from
the deliberate fork marker in the `id name` string.

## Building

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

produces

| target | output | what it is |
|---|---|---|
| `rodent_engine` (alias `rodent::engine`) | `build/librodent_engine.a` | the engine as a static library; public header in `include/` |
| `rodent` | `build/rodentIV` | the classic UCI executable (stdio adapter over the library) |
| `rodent_example_embed` | `build/rodent_example_embed` | the example below |

Upstream's `sources/Makefile` still builds the executable (`make -C sources build`) and
remains the reference build the CMake flags mirror.

To embed the library in another CMake project, `add_subdirectory()` this repository and link
`rodent::engine`; `-DRODENT_BUILD_EXE=OFF -DRODENT_BUILD_EXAMPLES=OFF` builds the library
alone.

## Using the library

```cpp
#include "rodent/engine.h"

rodent::SetResourceDir("/path/to/rodent");     // holds personalities/ and books/

rodent::Engine engine([](const char *line) {   // every engine output line arrives here
    std::printf("%s", line);
});

engine.ProcessLine("uci");
engine.ProcessLine("setoption name PersonalityFile value fischer.txt");
engine.ProcessLine("position startpos moves e2e4");
engine.ProcessLine("go depth 10");             // returns after "bestmove" reached the sink
```

`examples/embed_two_engines.cpp` is the complete version: two instances, two personalities,
one position each, running concurrently on their own threads. Run it with the directory that
holds `personalities/` and `books/`:

```sh
build/rodent_example_embed .
```

The whole API is `include/rodent/engine.h` — read it, it is short. The contract:

- **Output.** Everything the engine would have printed goes to the instance's sink, on the
  thread that drives the instance. The library never touches `stdin`/`stdout`/`stderr`.
- **Input.** `ProcessLine()` takes one UCI line in Rodent's dialect and returns `false` iff
  the session ended (`quit`, or a quit that arrived during a search). `go …` blocks until the
  search is finished and `bestmove` has been delivered.
- **Errors never kill the host.** A missing personality or book, or a failed allocation,
  becomes an output line and a degraded-but-alive instance. The library contains no `exit()`
  or `abort()`.
- **Stopping a search.** `Stop()` aborts the running search; the blocked `go …` returns once
  it has unwound and delivered its `bestmove`. It is the one method that may be called from
  another thread while `ProcessLine` is blocked, because it only raises the instance's atomic
  abort flag. There is no `stop` UCI *command* to feed to `ProcessLine`: the classic executable
  reaches the same flag by polling stdin from inside the search, which an embedded instance
  neither can nor should do — so an embedder that offers UCI to its own callers translates
  `stop` into `Stop()` itself.
- **Instances are independent.** Each has its own personality, transposition table, opening
  books, search state and SMP workers. Drive each instance from its own thread; one instance
  is not internally synchronised, so calls for the *same* instance must be serialised (`Stop()`
  excepted, above).
- **Resource paths are process-wide** (`SetResourceDir`), not per-instance — see Limitations.
- The library exposes a C++ API only; a C ABI belongs in the consumer's FFI shim.

## Verification

The refactor's safety net is mechanical behavioural identity, and it is the fork's main
contribution besides the API:

```sh
test/run_all.sh            # build + harness + sanitized multi-instance suite + example
test/run_all.sh --quick    # same, without the ASan/TSan runs
test/run_harness.sh        # just the transcript/liveness harness
```

- **Transcript identity** — scripted UCI sessions replayed through a line-driven driver and
  diffed against baselines captured from the unmodified upstream build. Any behavioural
  change shows up as a diff.
- **Bench identity** — the search's node count at a fixed depth must not move.
- **Liveness** — `stop` interrupts a search, `quit` during a search exits cleanly, the book
  returns a move.
- **Multi-instance suite** — two engines in one process, run sequentially and then
  concurrently; each instance's concurrent output must equal its own sequential output, under
  ASan and TSan.

`test/README.md` explains the determinism rules the transcripts depend on (book off, pinned
thread count, filtered time/nps fields) and the upstream quirks they characterise.

There is no hosted CI configuration here on purpose: `test/run_all.sh` is a plain shell
script with no dependency on any particular git host, and is meant to be run on a developer
machine after cloning — or from whatever CI a downstream happens to use. It needs a C++14
compiler, CMake, make and python3. Linux is the actively built and tested target; Windows and
Android stay supported at the source level (the code keeps compiling) but this fork adds no
platform build wiring.

## What diverged from upstream

Everything below is structural. None of it changes how the engine plays.

- **No `exit()` in library code.** Quit, EOF and quit-during-search unify on the existing
  deferred `Glob.goodbye` flag; the UCI loop returns to `main()`, which owns the exit code.
- **One output seam.** All output funnels through a per-instance sink object instead of
  `printf`/`vfprintf(stdout)` scattered across the sources. The classic executable's sink
  writes to `stdout` exactly as before, log file behaviour included.
- **Per-instance state.** The former process-globals (run state, personality, transposition
  table, books, SMP workers, RNG, per-search time control, castling configuration, …) live in
  one `EngineContext` per instance, reached through a thread-local current-context; the
  read-only lookup tables stay shared and are initialised once per process.
- **`rodent::Engine`** with the sink + `ProcessLine`/`Stop` API described above, plus the public
  header that keeps the engine's internal macros out of a host's translation units.
- **`main()` is an adapter** — path discovery, GUI detection, `setbuf`, stdin polling and the
  exit code; the engine library carries none of it.
- **Build and test infrastructure**: the CMake build (library + executable + example), the
  verification harness, and the multi-instance suite.

`CLAUDE.md` records the requirements, architecture and standing decisions behind that work,
for anyone (or anything) changing the fork.

## Limitations and known quirks

- **Resource paths are process-wide.** `SetResourceDir` sets one directory for all instances,
  and personality/book loading temporarily changes the process working directory while it
  reads a file (upstream behaviour). Two instances loading personalities *at the same moment*
  therefore share that process state; loading them before the engines start searching, or one
  at a time, is safe. A relative directory is anchored at the current one when you call
  `SetResourceDir`, so it survives those working-directory changes. Per-instance explicit
  paths are the next API refinement.
- **A mid-session personality load changes only the opening book, not the evaluation** —
  upstream behaviour, characterised by the harness, not "fixed" here. Direct eval
  `setoption`s do change the search.
- **Bench is personality-invariant** for the same reason.
- **Book move selection is time-seeded random**, so book output is not reproducible; the
  harness covers it with a liveness check and turns the book off elsewhere.
- **Windows and Android are not built or tested here** — no devices. The `#ifdef` structure
  for both is kept intact; macOS/iOS are not prevented by design but equally untested.

## License and provenance

Rodent IV is **GPL-3.0-or-later**, derived from Sungorus 1.4. This fork stays GPL-3, keeps
every upstream copyright header, and marks each modified upstream file with a
`Modified 2026 by T. Steinmann (Rodent IV libification fork)` line after the copyright block
(GPL §5a). Files authored by the fork carry `Copyright (C) 2026 T. Steinmann` and an
`SPDX-License-Identifier: GPL-3.0-or-later` tag. See `LICENSE` for the full text.

- Upstream: [nescitus/rodent-iv](https://github.com/nescitus/rodent-iv) — Pawel Koziol,
  Bernhard C. Maerz; based on Sungorus 1.4 by Pablo Vazquez.
- Fork maintainer: T. Steinmann.
- The engine identifies itself as `Rodent IV 0.33 native API fork` so binaries built from this
  tree are attributable. That string is the one output difference from upstream.

## Upstream's notes for contributors

Kept from upstream's README, still worth reading before touching the engine itself:

1. It's easier to work with small patches. They are more readable and cause less problems in
   case of an error.
2. Starting with bugfixes reduces headache. Fixing things before adding some great idea might
   accidentally do something good to implementation of Your idea as well. Going the other way
   round causes bugs to breed.
3. If fixing a bug required writing some diagnostic code, then diagnostic code stays (perhaps
   under an `#ifdef`) because it might help with yet another bug.
4. With a non-functional patch, please test that it is non-functional: run bench command and
   compare node counts before and after the patch.
5. With an Elo-gaining patch, please run a test of at least 1000 games, preferably more.
6. With a feature-adding patch it gets more difficult. Strength is not the main goal of Rodent
   project, but creating personalities requires some Elo to be thrown to the burner, sometimes
   in hundreds. And I would like most personalities to be able to beat Fruit 2.1 to ensure
   they are of decent GM strength. Default Rodent should at least keep 2900-3000 CCRL rating.
   Improvement is nice, but optional.
7. Treat Rodent like legacy software. A lot of things in there require refactoring, code
   quality is worse than chess quality, so tread carefully. I had no training in IT when I
   started this project. It taught me enough to get a job - and to notice that some of my code
   sucks.
8. Don't overdo automatic tuning. Rodent III lost a lot of style because of that, and I had to
   revert many changes.
9. Please don't remove quirks and features. They make Rodent what it is.
10. Enjoy. Rodent is meant to be fun.
