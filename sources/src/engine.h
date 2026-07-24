/*
Copyright (C) 2026 T. Steinmann
SPDX-License-Identifier: GPL-3.0-or-later

Part of the Rodent IV libification fork (GPL-3.0-or-later). See rodent.h for the
upstream Rodent / Sungorus copyright and the full license text.
*/

#pragma once

#include <functional>
#include <string>

#include "rodent.h"

namespace rodent {

// One embeddable Rodent engine instance. Construct it with an output sink -- every
// line the engine would have printed is delivered there -- then drive it one UCI
// line at a time with ProcessLine(). Nothing here touches stdin/stdout or exits the
// process. N instances coexist in one process, each with its own personality,
// transposition table and books; drive each instance from its own thread.
//
// The sink receives a null-terminated line (usually newline-terminated, matching
// what the classic exe would write). The pointer is valid only for the duration of
// the call. (std::string_view would be the natural type, but this fork targets
// C++14, which predates it.)
//
// Resource paths (personalities/, books/, basic.ini) currently resolve through the
// process-wide Rodent home directory, which the host sets once (SetRodentHomeDir(),
// or the RODENT4HOME environment variable) before constructing engines. Per-instance
// explicit paths are a later refinement.
class Engine {
public:
    // Named OutputSink, not Sink: rodent.h has a `Sink` macro (the per-instance
    // output object) that would otherwise clobber the identifier.
    using OutputSink = std::function<void(const char *line)>;

    explicit Engine(OutputSink sink);
    ~Engine();

    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;

    // Feed one UCI line (Rodent dialect, e.g. "go depth 8" or
    // "setoption name PersonalityFile value foo.txt"). Returns false iff the line
    // was "quit" (or quit/EOF was seen mid-search); the caller decides what to do.
    bool ProcessLine(const std::string &line);

private:
    EngineContext mContext;
    POS           mPos;
};

} // namespace rodent
