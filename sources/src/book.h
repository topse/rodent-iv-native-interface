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

#pragma once

#include <cstdio>
#include <cstring>

struct polyglot_move {
    U64 key;
    int move;
    int weight;
    int n;
    int learn;
};

struct sBook {
  private:
    FILE *bookFile;
    int bookSizeInEntries;
    unsigned char *bookMemory;
    int bookMemoryPos;
    int FindPos(U64 key);
    bool IsInfrequent(int val, int max_freq);
    void ClosePolyglot();
    void OpenPolyglot();
    void ReadEntry(polyglot_move *entry, int n);
    U64 GetPolyglotKey(POS *p);
    U64 ReadInteger(int size);
  public:
    char bookName[256];
    sBook(): bookFile(NULL), bookMemory(NULL) {}
    // Defined out-of-line in book.cpp: it uses Glob, which is a per-instance
    // macro (see rodent.h) not visible in this header (phase 4).
    void SetBookName(const char *name);
    bool Success() const { return bookFile || bookMemory; }
    int GetPolyglotMove(POS *p, bool print_output);
    ~sBook() { ClosePolyglot(); }
};

// GuideBook and MainBook are per-instance members of EngineContext, reached
// through the macros in rodent.h -- no global declarations here any more.
