/*
Rodent, a UCI chess playing engine derived from Sungorus 1.4
Copyright (C) 2009-2011 Pablo Vazquez (Sungorus author)
Copyright (C) 2011-2019 Pawel Koziol
Copyright (C) 2020-2020 Bernhard C. Maerz
Modified 2026 by T. Steinmann (Rodent IV libification fork)

Rodent is free software: you can redistribute it and/or modify it under the terms of the GNU
General Public License as published by the Free Software Foundation, either version 3 of the
License, or (at your option) any later version.

Rodent is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
General Public License for more details.

You should have received a copy of the GNU General Public License along with this program.
If not, see <http://www.gnu.org/licenses/>.
*/

#include "rodent.h"
#include "book.h"
#include <cstdio>   // setbuf (adapter owns process stdio)
#include <cstdlib>
#include <string>
#ifdef USE_THREADS
    #include <list>
#endif

// Phase 4: EngineContext (defined in rodent.h) bundles all the mutable per-engine
// state. The classic executable owns one instance; main() points its thread's
// tls_ctx at it so the Glob/Par/Trans/Sink/book/Engines macros resolve here. A
// future rodent::Engine will own one context each, for N coexisting instances.
// Read-only lookup tables (BB/Mask/Dist and the magic-move tables) deliberately
// stay process-global -- see their note below and the audit list in PLAN_RODENT.md.
EngineContext Context; // the one instance the classic exe owns
// (BB/Mask/Dist are read-only shared tables; defined in data.cpp so any binary that
//  links the engine but not this adapter -- e.g. the multi-instance test -- has them.
//  PrintVersion() moved to uci.cpp and cGlobals::Init/CanReadBook to data.cpp so this
//  file is purely the standalone-executable adapter over the engine library.)

int main() {

	// This thread drives the one context the classic exe owns; point tls_ctx at it
	// before any engine code (the Glob/Par/... macros) runs. (phase 4)
	tls_ctx = &Context;

	SetRodentHomeDir();
	Glob.threadOverride = 0;
	ReadThreadNumber("threads.ini");

    // catching memory leaks using MS Visual Studio
#if defined(_MSC_VER) && !defined(NDEBUG)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

    Glob.SeedRng(GetMS());
    BB.Init();
    cEngine::InitSearch();
    POS::Init();
    Glob.Init();
	Par.elo = 2800;
	Par.SetSpeed(Par.elo); // no longer part of DefaultWeights
    // Par.DefaultWeights(); will be done later
    Par.InitKingAttackTable();
	Par.use_ponder = false;
    Mask.Init();
    Dist.Init();

    Par.chess960 = false;
	Par.useBook = true;
	Par.verboseBook = false;

    //PrintVersion();

if (Glob.isNoisy) {
#if defined(_WIN32) || defined(_WIN64)
    printfUciOut("info string opening books path is '%ls' (%s)\n", _BOOKSPATH, ChDir(_BOOKSPATH) ? "exists" : "doesn't exist");
    printfUciOut("info string personalities path is '%ls' (%s)\n", _PERSONALITIESPATH, ChDir(_PERSONALITIESPATH) ? "exists" : "doesn't exist");
#else
    printfUciOut("info string opening books path is '%s' (%s)\n", _BOOKSPATH, ChDir(_BOOKSPATH) ? "exists" : "doesn't exist");
    printfUciOut("info string personalities path is '%s' (%s)\n", _PERSONALITIESPATH, ChDir(_PERSONALITIESPATH) ? "exists" : "doesn't exist");
#endif
}

    PrintOverrides(); // print books and pers paths overrides (26/08/17: linux only)

    GuideBook.SetBookName("guide.bin");
    MainBook.SetBookName("rodent.bin");
    ReadPersonality("basic.ini");

    // To make also "setoption name Personality ..." useable in "default.txt"
    // We need set to default values AFTER reading basic.ini
    Par.DefaultWeights();

    CheckGUI();

    // Process-level stdio config belongs to the adapter, not the engine loop
    // (moved out of UciLoop). Unbuffered so a GUI sees output immediately.
    setbuf(stdin, NULL);
    setbuf(stdout, NULL);

    // This adapter owns process stdin, so it polls it for stop/quit mid-search
    // (CheckTimeout). An embedded rodent::Engine leaves this off. (phase 4)
    Glob.pollStdin = true;

    UciLoop();

    return 0; // the adapter owns the process exit code; the library never exits
}