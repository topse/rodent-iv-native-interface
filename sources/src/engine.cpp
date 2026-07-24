/*
Copyright (C) 2026 T. Steinmann
SPDX-License-Identifier: GPL-3.0-or-later

Part of the Rodent IV libification fork (GPL-3.0-or-later). See rodent.h for the
upstream Rodent / Sungorus copyright and the full license text.

Implementation of the public API in include/rodent/engine.h. The engine's state
(EngineContext) and the board (POS) live in Impl, so the public header stays free
of rodent.h and its Glob/Par/Sink macros.
*/

#include "rodent/engine.h"

// Platform headers first, like rodenthome.cpp does: rodent.h defines short macros
// (Glob, Par, Trans, ...) that must not be let loose on a system header.
#if defined(_WIN32) || defined(_WIN64)
    #include <windows.h>
#else
    #include <unistd.h>
#endif

#include "rodent.h"
#include "book.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <utility>

namespace rodent {

// Read-only lookup tables (bitboards, masks, distances, the LMR reduction table and
// the zobrist tables in POS) are shared by all instances and initialised exactly
// once per process -- the Stockfish Bitboards::init() shape. (phase 4.6)
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

void SetResourceDir(const std::string &dir) {

    std::wstring path = Str2WStr(dir);

    // A relative directory is anchored at the current working directory *now*,
    // because the engine changes the process working directory while it reads a
    // personality or book file: a relative resource directory would resolve for the
    // first load and then silently stop resolving. (The classic executable never hit
    // this -- its home directory comes from the executable's own absolute path.)
    if (!path.empty() && !isabsolute(dir.c_str())) {
#if defined(_WIN32) || defined(_WIN64)
        wchar_t cwd[1024];
        if (GetCurrentDirectoryW((DWORD)(sizeof(cwd) / sizeof(cwd[0])), cwd)) {
            std::wstring base(cwd);
            PathEndSlash(base);
            path = base + path;
        }
#else
        char cwd[1024];
        if (getcwd(cwd, sizeof(cwd))) {
            std::wstring base = CStr2WStr(cwd);
            PathEndSlash(base);
            path = base + path;
        }
#endif
    }

    if (!path.empty())
        PathEndSlash(path);

    RodentHomeDirWStr = path;
}

std::string ResourceDir() {
    return WStr2Str(RodentHomeDirWStr);
}

// Everything the public header must not see.
struct Engine::Impl {
    EngineContext ctx;
    POS           pos;
};

Engine::Engine(OutputSink sink): mImpl(new Impl) {

    // This thread now drives this instance's context; every Glob/Par/Trans/Sink/...
    // access below resolves to mImpl->ctx.
    tls_ctx = &mImpl->ctx;

    InitSharedTables();

    // The part of POS::Init() that is per-instance rather than shared: the standard
    // castle configuration lives in this context, so InitSharedTables() only set it
    // up for whichever instance happened to win the call_once.
    POS::InitCastleDefaults();

    // Route the engine's console output to the caller's sink.
    mImpl->ctx.mSink.onConsole = std::move(sink);

    // Per-instance init, mirroring the classic exe's main() -- but not the
    // process-level path discovery, GUI detection or stdio config, which belong to
    // the host, nor the thread-count file read.
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

    mImpl->pos.SetPosition(START_POS);
}

Engine::~Engine() {
    // Abort any in-flight search so the worker/timer threads (joined by ~cEngine
    // during the context's destruction) return promptly instead of finishing the
    // search.
    mImpl->ctx.mGlob.abortSearch = true;

    // Don't leave this thread pointing at a context that is about to be destroyed.
    if (tls_ctx == &mImpl->ctx)
        tls_ctx = nullptr;
}

bool Engine::ProcessLine(const std::string &line) {

    // Re-assert this thread's context on every call: the caller may drive several
    // instances, and UciCommand + its worker/timer threads read it.
    tls_ctx = &mImpl->ctx;

    char command[4096];
    std::strncpy(command, line.c_str(), sizeof(command) - 1);
    command[sizeof(command) - 1] = '\0';

    printfUciIn("%s\n", command); // log-only echo, matching the classic ReadLine()

    return UciCommand(&mImpl->pos, command);
}

} // namespace rodent
