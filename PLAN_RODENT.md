# Rodent IV libification — working plan for the fork

> **How to use this document.** This is the working plan for a **fork of
> [`nescitus/rodent-iv`](https://github.com/nescitus/rodent-iv)**; it is written for an agent
> working *inside that fork*, and is self-contained — it does not assume access to any other
> repository. Tick the boxes in [Execution phases](#execution-phases) as they land, and amend the
> plan in place when reality disagrees with it — a stale plan is worse than no plan. Items marked
> *verify* are assumptions to check against the sources or a running build before relying on them.
>
> Status: **phases 0–3 done** (2026-07-24). CMake build + verification harness green; baselines
> captured; library-path `exit()` calls removed (clean loop termination); all output routed
> through one global `cSink`; all mutable engine state consolidated into one `EngineContext` that
> `main()` owns (former globals are reference aliases into it for now). Next up is phase 4
> (instances: `EngineContext`/sink become per-`rodent::Engine`, `ProcessLine` API, adapter split).
>
> Target platforms: **Windows, Linux, Android** (iOS/macOS not prevented by design but not
> built — no devices). Scope is **portability-only**: keep every platform compiling, add no
> Android/MSVC build wiring or platform CI. Linux is the only actively built + tested target
> here.

## Goal

Restructure Rodent IV the way Stockfish is structured: an **embeddable engine class** with
line-based input and callback output — no stdio, no `exit()`, no process-global mutable state —
while the **classic executable keeps building and behaving identically**, reduced to a thin
stdin/stdout adapter over that class.

End-state contract the library must satisfy (this is what the eventual consumer — a GPL-3
Flutter chess app that embeds engines in-process over FFI, one instance per concurrent role —
will build its shim on):

- **C++ API, one class per engine instance** (name it e.g. `rodent::Engine`):
  - constructor takes an output sink (`std::function<void(std::string_view line)>` or a small
    listener interface) and explicit resource paths (personalities dir, books dir, optional log
    file) — **no exe-relative discovery inside the library**;
  - `bool ProcessLine(const std::string&)` accepting full UCI (Rodent's dialect, including
    `setoption name PersonalityFile …` / `Personality …`), returning `false` on `quit`;
  - every line the engine used to print goes through the instance's sink; **nothing** in the
    library touches `stdin`/`stdout`/`stderr` or calls `exit()`/`abort()`;
  - errors (unreadable personality, missing book, allocation failure) become output lines and a
    degraded-but-alive instance, never process death.
- **Multi-instance**: N instances coexist in one process, each driven from its own caller
  thread, each with its own personality, transposition table and books. Read-only lookup tables
  (bitboards, masks, magics, distance) may stay process-global behind `std::call_once` init —
  the Stockfish `Bitboards::init()` shape.
- **The adapter executable**: `main()` constructs one instance with a stdout sink and a
  `fgets`-loop feeding `ProcessLine`, and carries the process-level things the library sheds
  (`setbuf`, exe-location discovery for default paths, GUI detection, exit codes). Same UCI
  behaviour as upstream — provable, see the harness.
- The library **does not** need to provide a C ABI — the consumer's shim adds that on top, as it
  does for Stockfish and Lc0.

## License and provenance

Rodent IV is **GPL-3.0-or-later** (derived from Sungorus). The fork stays GPL-3. **Fork
maintainer: T. Steinmann.**

- keep every existing copyright header; add `Modified 2026 by T. Steinmann (Rodent IV
  libification fork)` after the copyright block of **each modified upstream file** (GPL §5a).
  Apply it in every phase to the files that phase touches, and backfill earlier-phase files.
  New files the fork authors carry their own header (`Copyright (C) 2026 T. Steinmann` +
  `SPDX-License-Identifier: GPL-3.0-or-later`). *Done for phases 0–1's files.*
- the fork README states what diverged from upstream and why, and links back to
  `nescitus/rodent-iv`;
- the engine's `id name` string gains a fork marker (e.g. "Rodent IV 0.33 (lib)") so binaries
  are attributable — *decide with the maintainer, then keep it stable*.

## Why upstream cannot be embedded as-is (the inventory to dismantle)

Verified against the sources 2026-07-23; all in `sources/src/`:

- **stdio, hardwired.** `ReadLine()` does `fgets(stdin)` (`uci.cpp:36`); *all* output funnels
  through the `printfUciIn/Out/Add` macros (`rodent.h:1092-1094`) into `printfLog()` →
  `vfprintf(stdout, …)` (`util.cpp:409`), which also appends to a global log file
  (`LogFileWStr`). `UciLoop()` calls `setbuf` on both stdio streams.
- **`exit()` in library paths**: `uci.cpp:41` (EOF), `uci.cpp:125`, `uci.cpp:379`, `uci.cpp:418`
  (quit paths), `search.cpp:1287` (quit during search). In-process, each kills the host.
- **Mutable process globals** (defined in `main.cpp`): `Glob` (`cGlobals` — run state and
  option flags), `Par` (`cParam` — **the personality**, mutated by every `setoption`), `Trans`
  (`ChessHeapClass` — the transposition table), `GuideBook`/`MainBook` (`sBook`), and the SMP
  worker list `Engines` (`std::list<cEngine>`) / `EngineSingle`. Plus the log path, GUI
  detection state, and an unknown number of function-local `static`s — a dedicated audit item.
- **Read-only-after-init tables** (may stay global): `BB` (`cBitBoard`), `Mask`, `Dist`, the
  magic-move tables. *Verify* each really is write-once-then-read-only before leaving it global.

## The verification harness — built first, used at every phase

The refactor's safety net is behavioural identity, proven mechanically:

1. **Bench identity.** `bench <depth>` exists (`uci.cpp:117`, `cEngine::Bench`) and reports
   total nodes. Baseline: nodes searched at a fixed depth, single-threaded (default `Threads=1`).
   *Verified 2026-07-24:* two consecutive runs are node-identical (`1591057` @ depth 10), so bench
   stays. **Correction to the original plan:** bench is **personality-invariant** — every
   personality yields the *same* node count (bench uses the default eval regardless of the loaded
   personality), so benching "2–3 contrasting personalities" is a no-op and was dropped.
   Personality/eval coverage lives in the transcript suite instead (see below). Any phase that
   changes the bench node count has changed the engine and must be fixed or explained here.
2. **Transcript identity.** A scripted UCI session suite in `test/`, run through a small
   **line-driven driver** (`test/uci_driver.py`) that waits for `bestmove`/`uciok`/`readyok`
   before sending the next command. **A plain `cat | exe` pipe does not work:** Rodent polls
   stdin during search, so a queued `quit` aborts the in-progress `go` mid-iteration (the
   `search.cpp:1287` exit path) and output stops being reproducible. `test/filter.py` normalises
   the nondeterministic `time`/`nps` fields. Baselines are captured from the **unmodified build**
   and committed under `test/baselines/`; every later phase must reproduce them byte-identically.
   Determinism constraints discovered in phase 0 and baked into the suite:
   - **`RODENT4HOME`** is set to the repo root so personalities/books/basic.ini resolve identically.
   - **`UseBook` off in every search transcript** — book move selection is `srand(GetMS())`
     (`book.cpp:423`), i.e. wall-clock random and unreproducible. Book is covered by a *liveness*
     check only (a move is returned), not by byte-diff.
   - **Personality eval is not applied to a mid-session search** — loading a personality
     (`PersonalityFile`/`Personality`) changes only the opening book, not the mid-game eval; with
     book off, those searches equal the default. Direct eval `setoption`s *do* change the search
     (`04_eval_option` locks that path). The suite characterises current behaviour; whether the
     personality-eval no-op is a latent upstream bug is a maintainer question, out of scope here.
   - Coverage: `01_handshake`, `02_bench`, `03_search_positions`, `04_eval_option`,
     `05_personality_load`, `06_missing_personality` (degrade-not-crash), plus `stop`/quit-in-search
     liveness in `test/check_liveness.py`. See `test/README.md`.
3. **Multi-instance suite** (exists from phase 4): N instances in one process on N threads,
   different personalities, searching concurrently; each instance's output must equal its own
   single-instance sequential run. Built and run under **TSan and ASan**.

## Execution phases

Each phase ends with the harness green and the exe behaving identically. Small commits inside a
phase are encouraged — bench identity per commit makes bisecting trivial.

- [x] **0. Fork hygiene, build, baselines.** *Done 2026-07-24.* Added root `CMakeLists.txt` for
      the classic exe (Makefile still works). Mirrored defines, *verified* against the Makefile
      default build: **`USE_THREADS` is ON** (auto-enabled in `rodent.h` — `NO_THREADS` unset;
      needs a threads lib → `Threads::Threads`); popcount resolves to `__builtin_popcountll` under
      GCC/Clang with **no `-march`** (kept out so the `id name` build suffix and ARM/Android
      portability stay intact); `-std=c++14 -O3 -DNDEBUG -fno-rtti -finline-functions
      -fprefetch-loop-arrays`; `-DBOOKPATH` is defined by the Makefile but **unused** in the
      sources, so omitted. CMake and Makefile binaries are behaviourally identical across the whole
      harness (same `uci` banner, same bench `1591057`@d10, all transcripts). Harness + filter +
      liveness written (`test/`), baselines captured and committed.
      *Done when: clean clone → CMake build → harness green against committed baselines, and a
      second full harness run produces zero diff.* — met: `rm -rf build && test/run_harness.sh`
      builds and passes 6/6 transcripts + 3/3 liveness, twice, zero diff.
- [x] **1. No more `exit()`.** *Done 2026-07-24.* All five library-path `exit(0)` calls removed
      and unified onto the existing deferred `Glob.goodbye` flag: `ReadLine` now returns `bool`
      (false on EOF) instead of exiting; `CheckTimeout` sets `abortSearch`+`goodbye` on quit **or**
      EOF for both threaded and non-threaded builds (the `#ifndef USE_THREADS` exit is gone);
      `ParseGo`'s two `goodbye`→`exit` blocks are gone; `UciLoop` breaks on EOF, on the `quit`
      token, and on `goodbye` after a search, then returns to `main`, which `return 0`s. `setbuf`
      moved from `UciLoop` to `main` (adapter owns process stdio); `goodbye` initialised in
      `Glob.Init()`. Clean shutdown is safe because `~cEngine()` already joins its worker.
      Files: `uci.cpp`, `search.cpp`, `main.cpp`, `rodent.h`.
      *Done when: harness identical; `quit`, EOF and quit-during-search all terminate the exe
      cleanly (exit code 0) without `exit()` inside `sources/src` minus `main.cpp`.* — met: harness
      6/6 + liveness green; idle-quit / EOF / quit-in-search / **EOF-in-search** all exit 0; `grep`
      finds no `exit(`/`abort(` in `sources/src`; NO_THREADS build still compiles and runs.
- [x] **2. The output seam.** *Done 2026-07-24.* Added a `cSink` class (one global instance
      `Sink`, console `FILE*` defaulting to `stdout` + the existing `LogFileWStr` log behaviour)
      with a single `Emit(toConsole, logPrefix, text)` method — the lone place in the library that
      writes to the console. `printfLog` now formats once into a `std::string` (via `VFormat`,
      which also removes the old two-`vfprintf`/one-`va_list` aliasing bug) and calls `Sink.Emit`;
      the `printfUciIn/Out/Add` macros are unchanged on top of it. The ~40 bare `printf()` calls in
      the library (bench, board/param dumps, tuning, string self-tests) became `printfCon()`
      (console-only, no log — exactly their old behaviour). Files: `rodent.h`, `util.cpp`,
      `main.cpp`, `uci.cpp`, `bitboard.cpp`, `eval.cpp`, `params.cpp`, `tuning.cpp`,
      `stringfunctions.cpp`.
      *Done when: transcripts byte-identical (unfiltered this time — the sink must not even
      reorder flushes); zero direct `printf`/`fprintf(stdout…)`/`std::cout` left in library
      code (grep gate), `bench` unchanged.* — met: harness 6/6 + liveness green (handshake, which
      has no time/nps, is an unfiltered byte-identical check through the sink); grep gate clean
      (only the sink's `fputs(text, console)` and `main`'s `setbuf` touch stdout); bench node count
      unchanged; NO_THREADS still compiles.
- [x] **3. The context object — still one instance.** *Done 2026-07-24.* Added
      `struct EngineContext` in `main.cpp` bundling the mutable engine state (`Sink`, `Glob`,
      `Par`, `Trans`, `GuideBook`, `MainBook`, and the `Engines`/`EngineSingle` SMP workers); one
      global instance `Context` that `main()` owns. **Approach decided with the maintainer
      (deviates from the original "rewrite all ~590 references" wording):** the former globals
      become non-owning **reference aliases** into `Context` (`cParam& Par = Context.Par;` …), so
      all ~590 use sites are unchanged and the harness stays byte-identical. `Trans`'s definition
      moved out of `trans.cpp` into `Context`; the header `extern`s became `extern …&`. This
      consolidates the storage (the point that makes phase 4's per-instance possible) with near-zero
      risk; **the per-reference routing to `mCtx->…` is folded into phase 4** (done once, when the
      context becomes per-instance — no double-churn). Files: `main.cpp`, `rodent.h`, `book.h`,
      `trans.cpp`, `data.cpp`.
      *Done when: harness identical; `main.cpp` owns the one `EngineContext`; no mutable
      namespace-scope variable left outside it (audit list committed to this file).* — met with the
      alias caveat above (residual names are aliases, not storage); harness 6/6 + liveness green,
      bench unchanged, NO_THREADS compiles.

  **Phase-3 mutable-state audit** (every namespace-scope mutable global + function-local static):
  - **Moved into `EngineContext` (storage consolidated):** `Glob`, `Par`, `Trans`, `GuideBook`,
    `MainBook`, `Sink`, `Engines`/`EngineSingle`. (Names remain as reference aliases until phase 4.)
  - **Read-only after init → stay process-global (justified):** `BB`, `Mask`, `Dist` (commented in
    `main.cpp`); the magic-move tables `magicmovesbdb`/`magicmovesrdb` (in `magicmoves.cpp`, which is
    third-party non-GPL code — left untouched); `CastleFile_RQ`/`_RK` (written once in `Init960()`,
    commented in `data.cpp`); the `const` data/taunt tables. These match the Stockfish
    `Bitboards::init()` shape and get their `std::call_once` guard in phase 4.
  - **Deferred to phase 4 with a clear owner:**
    - `tDepth[MAX_THREADS]` (SMP per-worker depth) — folds into the per-instance engine with
      `Engines` (commented in `data.cpp`).
    - `RodentHomeDirWStr` and `InputAvailable()`'s Windows `static`s (`init`/`pipe`/`inh`) — path
      discovery / console-input detection → move into the **adapter** (phase 4 already scopes this).
    - `LogFileWStr`, `SkipBeginningOfLog` — log configuration → fold into the (then per-instance)
      `cSink`.
    - `cEngine::` and `POS::` **class-static** members (e.g. `msMoveTime`, `Castle_*`) — already
      class-scoped; become per-instance members when instances are introduced in phase 4.
  - **Function-local statics:** only `InputAvailable()` (Windows, above). The `MoveToStr(int)`
    "not thread safe" comment is stale — it uses a local buffer, not a static.
- [ ] **4. Instances.** `EngineContext` + sink become per-`rodent::Engine`; `ProcessLine` API;
      `main()` becomes the stdio adapter; exe-relative path discovery (`RodentHome`, GUI
      detection, `IsProcessRunning`) moves into the adapter, the library takes explicit paths.
      Read-only tables get their `std::call_once` guard. SMP worker threads (`USE_THREADS`)
      become instance-owned. **Fallback if SMP entanglement explodes the diff:** land phase 4
      single-threaded-per-instance (document it here), re-enable instance-owned SMP as phase
      4b — the consumer's presets mostly run `Threads=1` anyway.
      *Done when: harness identical through the adapter, and the multi-instance suite passes —
      N concurrent instances with different personalities, outputs equal to their sequential
      runs, TSan and ASan clean.*
- [ ] **5. Packaging, CI, docs.** Library target (static lib + public header) next to the exe
      target; a minimal example embedder (30 lines: two engines, two personalities, one position
      each, printed via sinks); GitHub Actions running build + full harness + sanitized
      multi-instance suite on Linux; README documenting the API, the divergence from upstream,
      and the GPL provenance notes above.
      *Done when: CI is green from a clean clone and the example embedder runs as documented.*

Out of scope for this fork (they belong to the consumer app, later, and only once this plan is
done): the FFI C-ABI shim, submodule pinning, preset selection among the 39 personalities,
Android/MSVC build wiring. Keep the code as portable as it is today (it already builds for
Windows/Linux/Android per upstream's README) — just add nothing platform-specific.

## Risks

- **SMP state entanglement** (`Engines` list, thread coordination in `search.cpp`) — the phase 4
  fallback exists precisely for this; do not let it stall the whole plan.
- **Hidden shared state** that bench identity cannot see in single-instance runs — the
  concurrent-vs-sequential equality check in the multi-instance suite is the detector; TSan is
  the backstop.
- **Bench nondeterminism** — checked first thing in phase 0; the fixed-node fallback keeps the
  harness meaningful.
- **Windows-specific code paths** (`LogFileWStr` wide strings, `WINDOWS_BUILD` blocks) — keep
  compiling (CI is Linux-only, so at minimum keep the `#ifdef` structure intact and unmodified
  where possible); full Windows verification is the consumer's MSVC follow-up.
- **Personality loading** reads files during `setoption` (`ReadPersonality`, including a
  `basic.ini` probe — `uci_options.cpp:417`) — after phase 4 these reads must go through the
  instance's configured paths; a missing file must degrade (error line via sink), not crash or
  exit. Add a transcript case for a nonexistent personality file.
