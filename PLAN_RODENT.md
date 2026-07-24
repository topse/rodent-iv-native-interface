# Rodent IV libification — working plan for the fork

> **How to use this document.** This is the working plan for a **fork of
> [`nescitus/rodent-iv`](https://github.com/nescitus/rodent-iv)**; it is written for an agent
> working *inside that fork*, and is self-contained — it does not assume access to any other
> repository. Tick the boxes in [Execution phases](#execution-phases) as they land, and amend the
> plan in place when reality disagrees with it — a stale plan is worse than no plan. Items marked
> *verify* are assumptions to check against the sources or a running build before relying on them.
>
> Status: **phases 0–3 done; phase 4 core done** (2026-07-24). CMake build + verification harness
> green (byte-identical) throughout; library-path `exit()` calls removed; all output routed through
> a per-instance `cSink`; engine state is per-instance `EngineContext`, reached via a thread-local
> current-context (macros); `rodent::Engine` + `ProcessLine` exist; `main()` is the stdio adapter;
> read-only tables init once via `std::call_once`; the RNG (book/weakening/taunts) is per-instance
> (4.7 done). The **multi-instance suite passes under ASan and TSan** (`test/multi_instance_test.cpp`
> + `test/run_multi_instance.sh`) — two engines, one with an eval override, produce identical output
> whether run sequentially or concurrently. **Phase 4 is complete**; the one deliberately-skipped
> refinement (`main()` literally constructing a `rodent::Engine`) is documented under phase 4 with
> its rationale (the `threads.ini`/`threadOverride` ordering). Next substantive work is phase 5
> (packaging/CI/docs).
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
    - `cEngine::` **class-static per-search state** — `msMoveTime`, `msMoveNodes`,
      `msSearchDepth`, `msStartTime` (`data.cpp:63-66`). *Verified 2026-07-24 to be a real
      multi-instance correctness hazard, not just a tidiness item:* these hold the live search
      time/node/depth budget and start clock — set in `ParseGo` (`uci.cpp:279-304`, `:334`) and
      read by `CheckTimeout` (`search.cpp:1301`). One shared copy across all instances, so two
      engines searching concurrently would clobber each other's time control (engine A's
      `msStartTime = GetMS()` overwrites B's). **Must become per-instance state in phase 4** — this
      is the top phase-4 correctness fix. Contrast: each `cEngine`'s per-worker search tables
      (`mEvalTT`, `mPawnTT`, `mHistory`, `mEvalStack`, `mKiller`, `mRefutation`, `mPvEng`) are
      already **non-static instance members** → already correct for N instances.
    - `POS::` **class-static tables** — split into two groups (corrected 2026-07-24 after TSan):
      - **Truly write-once, read-only** → stay shared behind `std::call_once`: `msZobPiece`/
        `msZobCastle`/`msZobEp` (zobrist keys) and `cEngine::msLmrSize`. (`static const` members —
        `mscRazorMargin` etc. — are plain constants, no action.)
      - **NOT write-once** → must become per-instance: the castle-configuration set
        `msCastleMask[64]`, `CastleMask_W_QS/KS`/`CastleMask_B_QS/KS`, `CastleFile_RQ/RK` and
        `Castle_W_RQ/K/RK`,`Castle_B_RQ/K/RK`. The audit's earlier "written once in `Init960()`"
        was wrong: `Init960()` is also called from `SetPosition()` (`setboard.cpp:168`), which
        rewrites them (from the FEN castling field) on **every** `position` command. Two instances
        setting positions / searching concurrently race on them, and `msCastleMask`'s
        set-all-then-`&=` transient can be read mid-update → wrong castling rights in the other
        instance's search. Caught by the multi-instance suite under TSan. (~150 refs; the read-only
        `POS::CastleMask[8][8]` const lookup stays put.)
  - **Function-local statics:** only `InputAvailable()` (Windows, above). The `MoveToStr(int)`
    "not thread safe" comment is stale — it uses a local buffer, not a static.
- [x] **4. Instances.** *Core done 2026-07-24.* `EngineContext` + sink are per-`rodent::Engine`;
      `ProcessLine` API added; `main()` is the stdio adapter; read-only tables get their
      `std::call_once` guard; SMP worker threads are instance-owned (each instance's `Engines`
      list, workers propagate `tls_ctx`). Routing done via a thread-local current-context rather
      than the originally-planned `mCtx->` (see 4.2/4.3 note). SMP did **not** need the
      single-threaded fallback. Exe-relative path discovery stays in the adapter, but the library
      still resolves resources through the process-wide Rodent home (host sets it once); explicit
      per-instance paths are a phase-5/refinement item.
      *Done when: harness identical through the adapter, and the multi-instance suite passes —
      N concurrent instances with different personalities, outputs equal to their sequential
      runs, TSan and ASan clean.* — **met**: harness 7/7 byte-identical; `test/run_multi_instance.sh`
      green in plain -O2 (repeated), ASan and TSan (0 races). "Different personalities" is exercised
      via a direct eval override (`PawnValueMg`), because a mid-session `PersonalityFile` load is an
      upstream no-op for eval (see the harness notes); the override is the meaningful differentiator.

  **Phase-4 progress (in flight, 2026-07-24):**
  - Done and harness-green (classic exe byte-identical throughout):
    - 4.1 `cEngine::msMoveTime/msMoveNodes/msSearchDepth/msStartTime` → per-instance `cGlobals`
      (`Glob.moveTime`/`moveNodes`/`searchDepth`/`startTime`).
    - 4.2/4.3 **Routing mechanism = thread-local current-context** (decided with maintainer; the
      plan's `mCtx->` was infeasible because eval helpers are `static` and `POS` methods read
      `Par`). `EngineContext` moved into `rodent.h` with renamed `m*` members; `Glob`/`Par`/`Trans`/
      `Sink`/`GuideBook`/`MainBook`/`Engines`/`EngineSingle` are macros over a
      `thread_local EngineContext* tls_ctx`, set at each thread entry (adapter `main`, worker
      `StartThinkThread`, timer thread, and `rodent::Engine`). ~640 call sites unchanged. The one
      header inline that used `Glob` (`sBook::SetBookName`) moved to `book.cpp`.
    - 4.4 **`rodent::Engine` + `bool ProcessLine(const std::string&)`** (`engine.h`/`engine.cpp`),
      output via a `std::function<void(const char*)>` sink wired into `cSink::onConsole` (public
      type is `OutputSink`, since `Sink` is now a macro; sink is `const char*` not `string_view`
      because the fork is C++14). `UciLoop`'s per-line dispatch extracted to `UciCommand(POS*,char*)`.
    - 4.5 `main.cpp` is now purely the adapter; its library functions (`PrintVersion`,
      `cGlobals::Init/CanReadBook`) and `BB`/`Mask`/`Dist` moved to library TUs so a binary without
      `main.cpp` links. (Not yet main-*over*-`rodent::Engine`; the classic exe still runs its own
      inline init + `UciLoop` — a later purity refinement.)
    - 4.6 Read-only shared tables (`BB`/`Mask`/`Dist`/`POS::Init`/`cEngine::InitSearch`) init once
      via `std::call_once` in `rodent::Engine`.
    - **Multi-instance suite** (`test/multi_instance_test.cpp`): two engines, one with a
      `PawnValueMg` eval override on an asymmetric FEN, run sequentially and concurrently; asserts
      each instance's concurrent output == its sequential output and A≠B. It **found and I fixed**
      two hidden shared-state bugs: `tDepth[]` (Lazy-SMP depth array) and the TT's
      `AllocTrans` `static prev_size` + file-global `aflags0/1` lock arrays (only the first-ever
      engine was allocating a TT) — all now per-instance. Passes reliably at `-O2`.
  - **Hidden shared state the suite forced out, now all fixed and per-instance:**
    - `tDepth[]` (Lazy-SMP depth array) and the TT's `AllocTrans` `static prev_size` + file-global
      `aflags0/1` lock arrays — only the first-ever engine allocated a TT.
    - The **castle-configuration set** (`msCastleMask`, `CastleMask_W/B_*`, `CastleFile_*`,
      `Castle_W/B_*`, ~150 refs) — rewritten by `Init960()` on every `SetPosition`; moved to
      `cGlobals`. TSan-confirmed race.
    - `pers_aliases` / `pers_sets` (personality/personality-set aliases from `basic.ini`) — moved to
      `cGlobals` as `sPersAliases`/`sPersSets`. TSan-confirmed race.
    - **`cGlobals`/`cParam` uninitialized in a per-instance context.** The classic exe's *global*
      `Context` is zero-initialized by static storage, and search-read fields never reset (e.g.
      `Glob.depthReached`) rely on that; a per-`rodent::Engine` context is not static → garbage →
      intermittent early aborts. Fixed by value-initializing `mGlob`/`mPar` in `EngineContext`.
    - **`CheckTimeout` polled `stdin` during search** (for `stop`/`quit`) — correct for the adapter,
      but for an embedded engine the host's stdin spuriously aborted the search (and instances would
      steal each other's input). Gated behind a new `Glob.pollStdin`, set only by `main()`.
  - **Not a bug, noted so it isn't rediscovered:** a deep search produces large output; the *test's*
    `std::regex` normaliser overflowed the stack on it (crash bare -O2, fine under ASan). The test
    now uses a hand-written line normaliser. The engine itself searches to depth ~20 / millions of
    nodes fine, single or concurrent.
  - **4.7 per-instance RNG — done 2026-07-24.** The process-global `rand()`/`srand()` (book move
    selection, Elo weakening, all the taunts) now go through a per-instance xorshift PRNG on
    `cGlobals` (`Glob.Rng()`/`Glob.SeedRng()`, rand()-compatible `[0,RAND_MAX]`). Each
    `rodent::Engine` seeds it with a salted `GetMS()` so concurrent instances draw independent
    sequences even when constructed in the same millisecond; the adapter seeds it in `main()` as
    before. (`init.cpp`'s `mt19937_64` is left alone — it is the deterministic, global-once zobrist
    generator, read-only after `POS::Init`.) Harness byte-identical (these paths aren't exercised:
    book off, full strength, no taunts), NO_THREADS compiles, multi-instance suite still green.
  - **Intentionally NOT done — `main()` constructing a `rodent::Engine`.** The classic exe is
    already a thin adapter in substance (it owns path discovery, GUI detection, stdin polling,
    `setbuf`, exit code and `threads.ini`, and drives the *same* per-instance `EngineContext` +
    `UciCommand` the library uses). Making it literally instantiate a `rodent::Engine` is blocked by
    `threads.ini`: the adapter must set `Glob.threadOverride` *before* `cGlobals::Init` runs (that is
    what forces the SMP thread count and hides the `Threads` option from the GUI), but the `Engine`
    ctor calls `Init` internally — so it would either drop that feature or need the minimal API
    grown with a thread-config parameter. Not worth the API/behaviour risk; left as a future
    refinement if the API grows for other reasons.

- [ ] **5. Packaging, CI, docs.** Library target (static lib + public header) next to the exe
      target; a minimal example embedder (30 lines: two engines, two personalities, one position
      each, printed via sinks); GitHub Actions running build + full harness + sanitized
      multi-instance suite on Linux; README documenting the API, the divergence from upstream,
      and the GPL provenance notes above. Create a initial CLAUDE.md. it shall contain idea of the fork, desisions and requirements. It shall not contain historic informaion. It shall also not contain implementation details, they belong as comments in code. It may contain an architectural overview. Make a review of our changes.
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
  the backstop. *Two confirmed instances (2026-07-24), both scoped into phase 4:* (a) the
  `cEngine::msMoveTime/msMoveNodes/msSearchDepth/msStartTime` per-search timing statics (see the
  phase-4 audit list — the top correctness fix); (b) the **process-global C RNG** — `rand()`/
  `srand()` back book move selection (`book.cpp:294`/`:423`), Elo weakening (`params.cpp:483`) and
  taunts (`taunt.cpp`, which even re-seeds via `srand(time(NULL))`). glibc's `rand()` is
  thread-safe (internally locked) so this is not a crash, but concurrent instances **share one RNG
  sequence** — not independent, and a data-race-y coupling. Give each instance its own RNG in
  phase 4; low urgency for the consumer (presets run `Threads=1` and book is usually off), so
  acceptable to defer to 4b if it entangles.
- **Bench nondeterminism** — checked first thing in phase 0; the fixed-node fallback keeps the
  harness meaningful.
- **Windows-specific code paths** (`LogFileWStr` wide strings, `WINDOWS_BUILD` blocks) — keep
  compiling (CI is Linux-only, so at minimum keep the `#ifdef` structure intact and unmodified
  where possible); full Windows verification is the consumer's MSVC follow-up.
- **Personality loading** reads files during `setoption` (`ReadPersonality`, including a
  `basic.ini` probe — `uci_options.cpp:417`) — after phase 4 these reads must go through the
  instance's configured paths; a missing file must degrade (error line via sink), not crash or
  exit. Add a transcript case for a nonexistent personality file.
