# CLAUDE.md — working notes for this repository

## What this fork is for

Upstream Rodent IV is a standalone UCI executable: it owns `stdin`/`stdout`, calls `exit()`,
and keeps its state in process globals. This fork restructures it into an **embeddable engine
library** — one class per engine instance, line-based input, callback output — while the
classic executable keeps building and behaving exactly as before, reduced to a thin adapter
over that library.

The consumer this is built for embeds several engine instances in one process (one per
concurrent role) and drives them over FFI. That is the design pressure behind every rule
below: no process-wide state, no process death, no stdio.

## Hard requirements

1. **Behavioural identity.** The chess — search, evaluation, personalities, node counts — must
   not change. Structural refactoring only. If a change moves the bench node count or any
   transcript baseline, it is a bug until proven otherwise; a baseline that *should* move is
   re-captured together with the reason, in the same commit.
2. **The verification harness stays green.** `test/run_all.sh` (or at minimum
   `test/run_harness.sh`) must pass before any change is considered done. Baselines are
   re-captured (`test/run_harness.sh --update`) only when a behaviour change is *intended* and
   explained.
3. **Nothing in the library touches the process.** No `stdin`/`stdout`/`stderr`, no `exit()`
   or `abort()`, no process-global mutable state. Errors become output lines on the instance's
   sink and a degraded-but-alive instance. Process-level concerns — path discovery, GUI
   detection, `setbuf`, polling stdin, the exit code — belong to the adapter or the host.
4. **N instances coexist.** Any state a search reads or writes must be per-instance. Shared
   state is acceptable only if it is written once at startup and read-only afterwards, behind
   a one-time init.
5. **Portability without platform wiring.** Targets are Windows, Linux and Android; macOS/iOS
   are not prevented but not implemented. Keep every `#ifdef` branch compiling and structurally
   intact. Add no Android/NDK, MSVC or platform-specific build files — that wiring belongs to
   the consumer application. Linux is the only actively built and tested target here.
6. **GPL-3 hygiene.** Every modified upstream file keeps its copyright block and carries a
   `Modified 2026 by T. Steinmann (Rodent IV libification fork)` line; new files carry their
   own copyright plus an SPDX tag. C++14, no exceptions to the standard level.
7. **No dependency on a particular git host or CI service.** Verification is plain shell
   scripts that run after cloning, on any machine.

## Architecture

```
   host application
        |  rodent::Engine(sink) + ProcessLine("go depth 10")
        v
   include/rodent/engine.h        the only public header: opaque, macro-free
        |
        v
   engine library (sources/src/*.cpp minus main.cpp)
        |  per-instance EngineContext: run state, personality, transposition table,
        |  books, SMP workers, RNG, output sink
        |  shared, read-only: bitboards, masks, distances, zobrist keys, magics
        v
   sources/src/main.cpp           the classic executable: stdio adapter only
```

- **The context.** All mutable engine state lives in one context object per instance. Engine
  code reaches its context through the thread that runs it, so every thread executing engine
  code — the driving thread, the search workers, the timer — must be pointed at its instance's
  context on entry. This is what keeps upstream's ~640 unqualified state references working
  unchanged; it is also the rule to remember when adding a thread.
- **The output seam.** One sink object per instance is the single place allowed to produce
  output. The library formats a line and hands it over; where it goes (a host callback, the
  console, the log file) is the sink's business, not the caller's.
- **Read-only tables** are shared process-wide and initialised exactly once. Anything that is
  rewritten during play (e.g. per-position castling configuration) is *not* one of these — it
  belongs in the context.
- **The public header stays independent of the engine's internal header**, which defines short
  macros (`Glob`, `Par`, `Sink`, …) that would collide with host code.
- **The adapter drives the engine's internals directly** rather than constructing a
  `rodent::Engine`: it has to force the SMP thread count (and hide the `Threads` option from
  the GUI) *before* engine init runs, which the constructor does internally. It stays a thin,
  engine-logic-free file regardless.

## Standing decisions

These are settled; revisit them deliberately, not by accident.

- **State is reached through the running thread, not through an engine pointer.** Passing a
  context pointer down would have meant rewriting every state reference, and it is not even
  available in the static evaluation helpers or in `POS`'s methods. The thread-local context
  keeps upstream's code shape — at the price of the entry rule above.
- **Shared process-wide, initialised once:** bitboards, masks, distances, magic-move tables,
  zobrist keys, the LMR reduction table. **Everything else is per-instance**, including things
  that look constant but are not — the castling configuration is rebuilt from every position,
  the per-search time/node/depth budget belongs to one search, and the RNG must not be shared
  or two instances draw from one sequence.
- **The RNG is the engine's own**, not the C library's: book move selection, Elo weakening and
  taunts all draw from the per-instance generator.
- **Resource paths are process-wide** and personality/book loading changes the process working
  directory while it reads (upstream behaviour). Per-instance explicit paths are the known next
  API refinement; until then a host calls `rodent::SetResourceDir` once before constructing
  engines.
- **The build mirrors upstream's Makefile flags and passes no `-march`** — the codegen path and
  the engine's build suffix stay identical and portable to ARM/Android. The Makefile stays a
  working reference build.
- **The `id name` carries the fork marker** (`Rodent IV 0.33 native API fork`) so binaries are
  attributable. Keep it stable; changing it changes the `01_handshake` baseline.

## Working rules

- `test/README.md` explains why the transcripts look the way they do (book off, pinned thread
  count, filtered `time`/`nps`). Respect those determinism rules when adding cases.
- New shared state is the main hazard: prefer a member of the context, and let the
  multi-instance suite under TSan be the judge.
- Explain *why* in code comments where upstream's structure is surprising; keep this file free
  of implementation detail and of history.
- Upstream quirks are characterised, not fixed: personality loads that do not affect the
  evaluation, personality-invariant bench, time-seeded book randomness. Changing them is a
  separate, deliberate decision with the maintainer.
