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

// msCastleMask, Castle_W/B_*, CastleMask_W/B_* and CastleFile_* are per-instance
// cGlobals members now (Glob.*) -- they are rewritten on every SetPosition, so as
// process-global state they raced across concurrent instances (phase 4).

const int tp_value[7] = { 100, 325, 325, 500, 1000,  0,   0 };
const int ph_value[7] = {   0,   1,   1,   2,    4,  0,   0 }; // any change requires modification in draw.cpp

U64 POS::msZobPiece[12][64];
U64 POS::msZobCastle[16];
U64 POS::msZobEp[8];

const unsigned char POS::CastleMask [8][8] = {
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // K-A
	{0x0C,0x00,0x78,0x74,0x6C,0x5C,0x3C,0x7C}, // K-B
	{0x0A,0x08,0x00,0x70,0x68,0x58,0x38,0x78}, // K-C
	{0x06,0x04,0x00,0x00,0x60,0x50,0x30,0x70}, // K-D
	{0x0E,0x0C,0x08,0x04,0x00,0x40,0x20,0x60}, // K-E
	{0x1E,0x1C,0x18,0x14,0x0C,0x00,0x00,0x40}, // K-F
	{0x3E,0x3C,0x38,0x34,0x2C,0x1C,0x00,0x20}, // K-G
	{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}  // K-H
};

// tDepth[] (SMP per-worker depth tracking) moved into the per-instance cGlobals in
// phase 4: it is one instance's Lazy-SMP coordination, shared by that instance's
// workers -- NOT across instances. As a process-global it made two concurrent
// engines clobber each other's tDepth[0] (caught by the multi-instance test).
// cEngine::msMoveTime/msMoveNodes/msSearchDepth/msStartTime were defined here; the
// per-search budget now lives in the per-instance cGlobals (Glob.moveTime etc.), phase 4.

std::wstring RodentHomeDirWStr;
std::wstring LogFileWStr;
bool SkipBeginningOfLog;

// Read-only after their Init() calls; safe to share process-wide across instances
// (they get a std::call_once guard via rodent::Engine). Defined here in a library
// TU -- not in the adapter main.cpp -- so the multi-instance test links them too.
cBitBoard BB;
cMask     Mask;
cDistance Dist;

// The calling thread's current EngineContext (phase 4). Set at each thread entry:
// the classic exe's main(), rodent::Engine's driving thread, each SMP worker
// (StartThinkThread) and the search timer thread (ParseGo). Null until then, which
// is fine because no engine code runs before some thread sets it.
thread_local EngineContext* tls_ctx = nullptr;

// cGlobals::Init / CanReadBook lived in main.cpp; moved here in phase 4 so they are
// part of the engine library, not the standalone-executable adapter.
void cGlobals::Init() {

	isNoisy = false;
    isTesting = false;
    isBenching = false;
    isTuning = false;
    useTaunting = false;
    printPv = true;
    isReadingPersonality = false;
    usePersonalityFiles = true;
    useBooksFromPers = true;
    showPersonalityFile = false;
    numberOfThreads = 1;
	if (Glob.threadOverride)
		numberOfThreads = Glob.threadOverride;
	timeBuffer = 10; // blitz under Arena would require something like 200, but it's user's job
	game_key = 0;

    // Clearing  and  setting threads  may  be  necessary
    // if we need a compile using a bigger default number
    // of threads for testing purposes

#ifdef USE_THREADS
    if (numberOfThreads > 1) { //-V547 get rid of PVS Studio warning
        Engines.clear();
        for (int i = 0; i < numberOfThreads; i++)
            Engines.emplace_back(i);
    }
#endif

    shouldClear = false;
    goodbye = false; // set true when quit/EOF arrives during a search (see CheckTimeout)
    isConsole = true;
    eloSlider = true;
	multiPv = 1;
    CastleNotation = KingMove;
    useUciPersonalitySet = false;
    personalityW = "";
    personalityB = "";
}

bool cGlobals::CanReadBook() {
    return (useBooksFromPers == isReadingPersonality || !usePersonalityFiles);
}
