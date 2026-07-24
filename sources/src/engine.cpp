/*
Copyright (C) 2026 T. Steinmann
SPDX-License-Identifier: GPL-3.0-or-later

Part of the Rodent IV libification fork (GPL-3.0-or-later). See rodent.h for the
upstream Rodent / Sungorus copyright and the full license text.
*/

#include "engine.h"
#include "book.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <utility>

namespace rodent {

// Read-only lookup tables (bitboards, masks, distances, the LMR reduction table and
// the zobrist/castle tables in POS) are shared by all instances and initialised
// exactly once per process -- the Stockfish Bitboards::init() shape. (phase 4.6)
static std::once_flag g_sharedTablesOnce;

static void InitSharedTables() {
    std::call_once(g_sharedTablesOnce, [] {
        BB.Init();
        cEngine::InitSearch();
        POS::Init();
        Mask.Init();
        Dist.Init();
    });
}

Engine::Engine(OutputSink sink) {

    // This thread now drives this instance's context; every Glob/Par/Trans/Sink/...
    // access below resolves to mContext.
    tls_ctx = &mContext;

    InitSharedTables();

    // Route the engine's console output to the caller's sink.
    mContext.mSink.onConsole = std::move(sink);

    // Per-instance init, mirroring the classic exe's main() -- but not the
    // process-level path discovery, GUI detection or stdio config, which belong to
    // the host, nor srand()/thread-file reads.
    Glob.Init();

    // Seed this instance's per-instance RNG with a distinct value, so concurrent
    // engines draw independent book/weakening sequences even when constructed in the
    // same millisecond. (The classic exe seeds the same way in main().)
    static std::atomic<unsigned> s_seedSalt{0x9E3779B9u};
    Glob.SeedRng((unsigned)GetMS() ^ s_seedSalt.fetch_add(0x9E3779B9u, std::memory_order_relaxed));

    Par.elo = 2800;
    Par.SetSpeed(Par.elo); // no longer part of DefaultWeights
    Par.InitKingAttackTable();
    Par.use_ponder = false;
    Par.chess960 = false;
    Par.useBook = true;
    Par.verboseBook = false;

    GuideBook.SetBookName("guide.bin");
    MainBook.SetBookName("rodent.bin");
    ReadPersonality("basic.ini");

    // Default values must be set AFTER reading basic.ini (see main()).
    Par.DefaultWeights();

    Trans.AllocTrans(16);

    mPos.SetPosition(START_POS);
}

Engine::~Engine() {
    // Abort any in-flight search so the worker/timer threads (joined by ~cEngine
    // during mContext destruction) return promptly instead of finishing the search.
    mContext.mGlob.abortSearch = true;

    // Don't leave this thread pointing at a context that is about to be destroyed.
    if (tls_ctx == &mContext)
        tls_ctx = nullptr;
}

bool Engine::ProcessLine(const std::string &line) {

    // Re-assert this thread's context on every call: the caller may drive several
    // instances, and UciCommand + its worker/timer threads read it.
    tls_ctx = &mContext;

    char command[4096];
    std::strncpy(command, line.c_str(), sizeof(command) - 1);
    command[sizeof(command) - 1] = '\0';

    printfUciIn("%s\n", command); // log-only echo, matching the classic ReadLine()

    return UciCommand(&mPos, command);
}

} // namespace rodent
