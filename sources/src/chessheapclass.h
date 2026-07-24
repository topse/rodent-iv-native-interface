/*
Part of Rodent IV, a UCI chess engine derived from Sungorus 1.4 (GPL-3.0-or-later).
Copyright (C) 2009-2011 Pablo Vazquez; 2011-2019 Pawel Koziol; 2020 Bernhard C. Maerz.
Modified 2026 by T. Steinmann (Rodent IV libification fork).

This program is free software: you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software Foundation,
either version 3 of the License, or (at your option) any later version. See
<http://www.gnu.org/licenses/>.
*/

#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef USE_THREADS
    #include <atomic>
    #include <memory>
#endif


class ChessHeapClass {
    static constexpr int bucket_size_mb = 512;
    static constexpr int num_per_bucket = bucket_size_mb * 1024 * 1024 / sizeof(ENTRY);
    static constexpr int arrays_size = max_tt_size_mb / bucket_size_mb;

    static_assert(sizeof(ENTRY) == 16, "ENTRY size must be 16 bytes.");

    int    bucket_sizs[arrays_size];
    ENTRY *bucket_ptrs[arrays_size];

    unsigned int tt_size;
    unsigned int tt_mask;

    // Per-instance now (phase 4). prev_size was a function-local static in
    // AllocTrans and the aflags[] lock arrays were file-scope globals in trans.cpp,
    // so with N engines only the first allocated its table and all instances shared
    // one set of TT locks. Both are per-instance TT state; each engine gets its own.
    unsigned int prev_size;
#ifdef USE_THREADS
    std::unique_ptr<std::atomic_flag[]> aflags0;
    std::unique_ptr<std::atomic_flag[]> aflags1;
#endif

    bool success;

    void Free() {           // free the allocated memory and zeroize bucket_ptrs[]

        for (int i = 0; i < arrays_size && bucket_ptrs[i]; i++) {
            free(bucket_ptrs[i]);
            bucket_ptrs[i] = NULL;
        }
    }

    void ZeroMem() {        // zeroize the allocated memory

        if (success)
            for (int i = 0; i < arrays_size && bucket_ptrs[i]; i++)
                memset(bucket_ptrs[i], 0, 1024 * 1024 * bucket_sizs[i]);
    }

    ENTRY *MakeAddr(int entry_number) const {     // calculate address of the entry with entry_number

        const int num_of_bucket = entry_number / num_per_bucket;

        return bucket_ptrs[num_of_bucket] + entry_number - num_per_bucket * num_of_bucket;
    }

    bool Alloc(int size_mb) {       // allocate size_mb megabyte of memory and return true on success

        if (size_mb > max_tt_size_mb)
            return false;

        Free();

        success = true;
        for (int i = 0; size_mb > 0 && success; i++) {
            bucket_sizs[i] = size_mb > bucket_size_mb ? bucket_size_mb : size_mb;
            bucket_ptrs[i] = (ENTRY *) malloc(1024 * 1024 * bucket_sizs[i]);
            success = bucket_ptrs[i] != NULL;
            size_mb -= bucket_size_mb;

            if (success)
                printf_debug("allocated: %dMB\n", bucket_sizs[i]);
        }

        if (!success)
            Free();

        return success;
    }

  public:

    int tt_date;

    ChessHeapClass(): bucket_ptrs{}, prev_size{0}, success{false} {};

    ~ChessHeapClass() {

        Free();
    }

    void AllocTrans(unsigned int mbsize);
    void Clear();
    bool Retrieve(U64 key, int *move, int *score, int *flag, int alpha, int beta, int depth, int ply);
    void RetrieveMove(U64 key, int *move);
    void Store(U64 key, int move, int score, int flags, int depth, int ply);
};
