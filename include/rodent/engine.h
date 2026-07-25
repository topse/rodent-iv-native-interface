/*
Copyright (C) 2026 T. Steinmann
SPDX-License-Identifier: GPL-3.0-or-later

Part of the Rodent IV libification fork (GPL-3.0-or-later). Rodent IV is derived
from Sungorus 1.4; see sources/src/rodent.h for the upstream copyright block and
LICENSE for the full license text.

This is the fork's ONE public header. It deliberately includes nothing from
sources/src: the engine's internal header defines short macros (Glob, Par, Sink,
Trans, ...) that would collide with a host application's identifiers, so the
implementation is hidden behind a pointer.
*/

#ifndef RODENT_ENGINE_H
#define RODENT_ENGINE_H

#include <functional>
#include <memory>
#include <string>

namespace rodent {

// Where the engine looks for `personalities/` (including basic.ini) and `books/`.
//
// Process-wide, not per-instance: personality and book loading resolves relative
// paths through this directory, and temporarily changes the process working
// directory while reading a file. Set it once, before constructing any Engine, and
// do not change it while engines are running.
//
// A relative directory is made absolute against the current working directory at
// the time of the call (the engine changes that directory while reading a file, so
// a relative one would not survive the first load). A missing personality file is
// not fatal: the engine reports it on its sink and keeps running with built-in
// defaults.
void SetResourceDir(const std::string &dir);

// The resource directory as currently set, with a trailing separator.
std::string ResourceDir();

// One embeddable Rodent engine instance.
//
// Construct it with an output sink -- every line the engine would have printed
// goes there -- then drive it one UCI line at a time with ProcessLine(). Nothing
// in the library touches stdin/stdout/stderr or terminates the process: errors
// become output lines and a degraded-but-alive instance.
//
// N instances coexist in one process, each with its own personality,
// transposition table, opening books and SMP workers. Drive each instance from
// its own thread; a single instance is not internally synchronised, so calls for
// one instance must be serialised (ProcessLine blocks until the engine is done
// with the line, including for `go`). Stop() is the one exception -- see below.
class Engine {
public:
    // Receives one null-terminated line, usually newline-terminated, exactly as
    // the classic executable would have written it to stdout. The pointer is
    // valid only for the duration of the call. Called on the thread that drives
    // the instance -- including from inside ProcessLine("go ...") for the info
    // lines and the bestmove.
    //
    // Named OutputSink rather than Sink because the engine's internal header
    // uses `Sink` as a macro.
    using OutputSink = std::function<void(const char *line)>;

    explicit Engine(OutputSink sink);
    ~Engine();

    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;

    // Feed one UCI line in Rodent's dialect -- standard UCI plus the engine's own
    // options, e.g. "setoption name PersonalityFile value fischer.txt". The line
    // must not contain a newline.
    //
    // Returns false iff the session ended: the line was `quit`, or a `quit`
    // arrived while the engine was searching. The caller decides what that means
    // -- destroying the instance is the usual answer; the object stays valid
    // either way.
    bool ProcessLine(const std::string &line);

    // Abort a running search. The blocked ProcessLine("go ...") returns once the
    // search has unwound and its `bestmove` has reached the sink, exactly as if
    // the search had run out of time.
    //
    // This is the ONLY method that may be called while another thread is inside
    // ProcessLine for the same instance: it just raises this instance's atomic
    // abort flag -- the same one the destructor raises -- and touches nothing
    // else. There is no `stop` UCI command to feed to ProcessLine instead; the
    // classic executable reaches the flag by polling stdin from inside the
    // search, which an embedded instance neither can nor should do.
    //
    // A no-op when no search is running: every `go` clears the flag on entry, so
    // a Stop() raised while idle cannot leak into the next search.
    void Stop();

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

} // namespace rodent

#endif // RODENT_ENGINE_H
