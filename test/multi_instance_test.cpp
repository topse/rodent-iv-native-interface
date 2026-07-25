/*
Copyright (C) 2026 T. Steinmann
SPDX-License-Identifier: GPL-3.0-or-later

Part of the Rodent IV libification fork (GPL-3.0-or-later).

Multi-instance verification (phase 4). Proves that two rodent::Engine instances in
one process are independent:
  * each is driven through a node-limited search script;
  * instance A overrides a direct eval term (PawnValueMg), instance B does not, so
    their searches MUST differ -- if the transposition table, personality (Par) or
    run state (Glob) were shared, they would contaminate each other;
  * each script is run once sequentially and once concurrently (each engine on its
    own thread, with its own SMP worker + timer threads). For a correct design the
    concurrent output of an instance equals its own sequential output byte-for-byte
    (after masking the nondeterministic time/nps/hashfull fields, as the harness
    filter does);
  * Stop() ends a would-be-endless search from another thread, and ends it on the
    instance it was called on only -- the cross-thread abort an embedder needs, and
    the one call allowed to run concurrently with that instance's ProcessLine.

Run under ASan and TSan (see run_multi_instance.sh) to catch races on any state
that is still shared behind the scenes.
*/

#include "rodent/engine.h" // public API only -- this also proves the header stands alone

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Replace the digits after `key` (e.g. " time ") with `rep`. Done by hand rather
// than with std::regex, which recurses and overflows the stack on the large output
// a deep search produces.
static std::string MaskField(std::string s, const std::string &key, const char *rep) {
    size_t pos = 0;
    while ((pos = s.find(key, pos)) != std::string::npos) {
        size_t ds = pos + key.size(), de = ds;
        while (de < s.size() && std::isdigit((unsigned char)s[de])) de++;
        if (de > ds) { s.replace(ds, de - ds, rep); pos = ds + std::strlen(rep); }
        else pos = ds;
    }
    return s;
}

// Mask nondeterministic output so a search compares equal across runs. Beyond the
// time/nps/hashfull fields the harness filters, we also drop the bare "info depth N"
// *progress markers* the worker prints when it starts an iteration: whether the next
// iteration's marker is emitted before the search ends is a thread-scheduling
// artifact, not part of the search result. The completed "info depth N ... nodes ...
// pv ..." lines and the bestmove -- what actually matters -- are kept, so any real
// cross-instance contamination still fails the test. Line-based (no std::regex).
static std::string Normalize(const std::string &in) {
    std::string out;
    out.reserve(in.size());
    size_t start = 0;
    while (start <= in.size()) {
        size_t nl = in.find('\n', start);
        size_t len = (nl == std::string::npos) ? in.size() - start : nl - start;
        std::string line = in.substr(start, len);

        // Drop bare "info depth <N>" progress-marker lines.
        bool bareMarker = false;
        const std::string pfx = "info depth ";
        if (line.compare(0, pfx.size(), pfx) == 0) {
            size_t i = pfx.size(), j = i;
            while (j < line.size() && std::isdigit((unsigned char)line[j])) j++;
            size_t k = j;
            while (k < line.size() && (line[k] == ' ' || line[k] == '\t')) k++;
            if (j > i && k == line.size()) bareMarker = true;
        }

        if (!bareMarker) {
            line = MaskField(line, " time ", "T");
            line = MaskField(line, " nps ", "N");
            line = MaskField(line, " hashfull ", "H");
            out += line;
            if (nl != std::string::npos) out += '\n';
        }

        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return out;
}

// Drive one fresh engine through a script, returning everything it emitted. The
// sink is called from the engine's own threads (worker/timer), so it is guarded by
// a mutex; the engine is destroyed (joining those threads) before out/m go away.
static std::string RunScript(const std::vector<std::string> &lines) {
    std::string out;
    std::mutex m;
    {
        rodent::Engine eng([&](const char *s) {
            std::lock_guard<std::mutex> g(m);
            out += s;
        });
        for (const auto &l : lines)
            eng.ProcessLine(l);
    }
    return out;
}

// Whether `haystack` contains `needle`. Spelled out so the checks below read as
// assertions rather than as npos comparisons.
static bool Contains(const std::string &haystack, const char *needle) {
    return haystack.find(needle) != std::string::npos;
}

// `go infinite` on one instance, Stop() from another thread, while a second
// instance searches alongside it.
//
// Two things are proved at once, and both matter to an embedder: the search really
// does end on Stop() (an infinite search has no other way out), and Stop() reaches
// exactly one instance -- B is driven to completion untouched. The wait is bounded
// by a future, so a Stop() that does not work fails the test instead of hanging the
// suite forever.
static bool StopChecks() {

    std::string outA, outB;
    std::mutex mA, mB;
    bool ok = true;

    auto check = [&](bool cond, const char *msg) {
        std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", msg);
        if (!cond)
            ok = false;
    };

    rodent::Engine engB([&](const char *s) {
        std::lock_guard<std::mutex> g(mB);
        outB += s;
    });
    rodent::Engine engA([&](const char *s) {
        std::lock_guard<std::mutex> g(mA);
        outA += s;
    });

    for (rodent::Engine *e : {&engA, &engB}) {
        e->ProcessLine("uci");
        e->ProcessLine("isready");
        e->ProcessLine("setoption name UseBook value false");
        e->ProcessLine("position startpos");
    }

    // A searches forever on its own thread; B runs a bounded search on ours.
    std::packaged_task<void()> searchA([&] { engA.ProcessLine("go infinite"); });
    std::future<void> doneA = searchA.get_future();
    std::thread threadA(std::move(searchA));

    engB.ProcessLine("go nodes 100000");

    // Let A actually get into the search before aborting it: a Stop() raised before
    // `go` runs would be cleared on entry (uci.cpp resets the flag), and the test
    // would hang for the right reason but look like the wrong one.
    for (int waited = 0; waited < 200; ++waited) {
        {
            std::lock_guard<std::mutex> g(mA);
            if (Contains(outA, "info depth "))
                break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    engA.Stop();
    const bool returned =
        doneA.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
    if (returned)
        threadA.join();
    else
        threadA.detach(); // the process is failing anyway; do not block on the join

    check(returned, "Stop(): the infinite search returned");
    if (returned) {
        std::lock_guard<std::mutex> g(mA);
        check(Contains(outA, "bestmove"), "Stop(): a bestmove was still delivered");
    }
    {
        std::lock_guard<std::mutex> g(mB);
        check(Contains(outB, "bestmove"),
              "Stop(): the other instance finished its own search");
    }

    // Only meaningful once the aborted thread is gone: ~Engine joins A's workers.
    return ok && returned;
}

int main() {

    // Host responsibility (not the library's): resolve the resource directory once.
    // run_multi_instance.sh points RODENT4HOME at the repo root.
    const char *home = std::getenv("RODENT4HOME");
    rodent::SetResourceDir(home ? home : ".");

    const std::vector<std::string> common_head = {
        "uci",
        "isready",
        "setoption name UseBook value false",
    };
    const std::vector<std::string> common_tail = {
        // Asymmetric middlegame position (from test/transcripts/04_eval_option): a
        // material/structure imbalance so a PawnValueMg change actually shifts the
        // evaluation. startpos is symmetric, so doubling the pawn value there cancels
        // out and would not distinguish the two instances.
        "position fen r3r1k1/2p2ppp/p1p1bn2/8/1q2P3/2NPQN2/PPP3PP/R4RK1 b - - 2 15",
        // Node-limited (not depth/time): aborts at an exact node count with
        // moveTime=99999999, so the search is deterministic and independent of wall
        // clock / CPU load -- the right signal for a concurrent-vs-sequential compare.
        // (Depth-limited "go depth N" turned out to still be time-sensitive here.)
        "go nodes 100000",
    };

    auto make_script = [&](bool override_eval) {
        std::vector<std::string> s = common_head;
        if (override_eval)
            s.push_back("setoption name PawnValueMg value 200");
        for (const auto &l : common_tail)
            s.push_back(l);
        return s;
    };

    const std::vector<std::string> scriptA = make_script(true);  // modified eval
    const std::vector<std::string> scriptB = make_script(false); // default eval

    // Sequential reference runs.
    const std::string seqA = Normalize(RunScript(scriptA));
    const std::string seqB = Normalize(RunScript(scriptB));

    // Concurrent runs: each engine on its own thread.
    std::string conA, conB;
    std::thread tA([&] { conA = Normalize(RunScript(scriptA)); });
    std::thread tB([&] { conB = Normalize(RunScript(scriptB)); });
    tA.join();
    tB.join();

    // Dump all four captures for offline diffing when debugging.
    if (const char *dir = getenv("MI_DUMP_DIR")) {
        auto dump = [&](const char *name, const std::string &s) {
            std::string path = std::string(dir) + "/" + name;
            if (FILE *f = std::fopen(path.c_str(), "w")) { std::fputs(s.c_str(), f); std::fclose(f); }
        };
        dump("seqA.txt", seqA); dump("conA.txt", conA);
        dump("seqB.txt", seqB); dump("conB.txt", conB);
    }

    int failures = 0;
    auto check = [&](bool cond, const char *msg) {
        std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", msg);
        if (!cond)
            ++failures;
    };

    std::printf("multi-instance checks:\n");
    check(!seqA.empty() && !seqB.empty(), "both instances produced output");
    check(seqA == conA, "A: concurrent output == sequential output");
    check(seqB == conB, "B: concurrent output == sequential output");
    check(seqA != seqB, "A != B (eval override applied; no cross-instance contamination)");

    if (!StopChecks())
        ++failures;

    if (failures) {
        std::printf("FAILED (%d)\n", failures);
        // Dump a small diff hint on failure.
        std::printf("--- seqA (first 400) ---\n%.400s\n", seqA.c_str());
        std::printf("--- conA (first 400) ---\n%.400s\n", conA.c_str());
        return 1;
    }
    std::printf("OK\n");
    return 0;
}
