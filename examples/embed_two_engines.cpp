/*
Copyright (C) 2026 T. Steinmann
SPDX-License-Identifier: GPL-3.0-or-later

Part of the Rodent IV libification fork (GPL-3.0-or-later).

Minimal embedder: two rodent::Engine instances in one process, each with its own
personality, each searching its own position on its own thread, each printing
through its own sink. This is the whole public API -- SetResourceDir, the sink,
ProcessLine.

    build/rodent_example_embed [resource-dir]     # default: the current directory

`resource-dir` is the directory holding personalities/ and books/ (the repo root
in a source checkout).
*/

#include "rodent/engine.h"

#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

namespace {

std::mutex g_print; // the two sinks are called from two threads

void RunOne(const char *tag, const char *personality, const char *position) {

    rodent::Engine engine([tag](const char *line) {
        std::lock_guard<std::mutex> guard(g_print);
        // Keep the example's output short: drop the search chatter and the option
        // banner, print the rest (id/uciok/readyok/bestmove).
        const std::string text(line);
        if (text.compare(0, 5, "info ") != 0 && text.compare(0, 7, "option ") != 0)
            std::printf("[%s] %s", tag, line);
    });

    engine.ProcessLine("uci");
    engine.ProcessLine("isready");
    engine.ProcessLine("setoption name UseBook value false"); // book moves are random
    engine.ProcessLine(std::string("setoption name PersonalityFile value ") + personality);
    engine.ProcessLine(position);
    engine.ProcessLine("go depth 10"); // blocks until "bestmove" reaches the sink
    engine.ProcessLine("quit");
}

} // namespace

int main(int argc, char **argv) {

    // Where personalities/ and books/ live. Process-wide, set before any engine.
    rodent::SetResourceDir(argc > 1 ? argv[1] : ".");

    std::thread a(RunOne, "fischer", "fischer.txt", "position startpos");
    std::thread b(RunOne, "karpov", "karpov.txt",
                  "position fen r3r1k1/2p2ppp/p1p1bn2/8/1q2P3/2NPQN2/PPP3PP/R4RK1 b - - 2 15");
    a.join();
    b.join();
    return 0;
}
