# Copyright (C) 2026 T. Steinmann (Rodent IV libification fork)
# SPDX-License-Identifier: GPL-3.0-or-later
#
# CMake build for the classic Rodent IV executable.
#
# This mirrors the default build produced by sources/Makefile so the binary
# behaves byte-for-byte identically (the verification harness in test/ depends
# on that). The Makefile remains the reference build and keeps working; this is
# added alongside it.
#
# Makefile default build (sources/Makefile):
#   g++ -std=c++14 -O3 -DNDEBUG -fno-rtti -finline-functions -fprefetch-loop-arrays
#       -g -w -Wfatal-errors -pipe -DBOOKPATH=... src/*.cpp -o ../mac/rodentIV   (-s -lm)
#
# Notes on faithful mirroring:
#   * USE_THREADS is auto-enabled inside rodent.h (NO_THREADS is not defined), so
#     the engine uses std::thread and needs a threads library -> Threads::Threads.
#   * popcount resolves to __builtin_popcountll under GCC/Clang when no -march /
#     -mpopcnt / -mssse3 is given (bitboard.cpp). We deliberately pass no -march so
#     the codegen path -- and the engine's "id name" build suffix -- stay identical
#     to the Makefile and portable to ARM/Android.
#   * -DBOOKPATH is passed by the Makefile but is unused in the sources, so it is
#     intentionally not defined here.
#   * -g/-w/-pipe/-s do not affect behaviour and are omitted; the flags that affect
#     code generation are reproduced below.

cmake_minimum_required(VERSION 3.13)

project(RodentIV LANGUAGES CXX)

# Default to an optimized build (matches the Makefile's -O3 -DNDEBUG) unless the
# user picks a config explicitly.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

set(CMAKE_CXX_STANDARD 14)            # Makefile: -std=c++14
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)          # -std=c++14 is strict ISO, not gnu++14

# All translation units, matching the Makefile's "src/*.cpp".
file(GLOB RODENT_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/sources/src/*.cpp")

add_executable(rodent ${RODENT_SOURCES})
set_target_properties(rodent PROPERTIES OUTPUT_NAME rodentIV)

# Codegen flags from the Makefile. CMake's Release config already supplies
# -O3 -DNDEBUG; we add the remaining semantic flags. The full set is gated on
# GCC (the Makefile's reference compiler); Clang gets the portable subset.
target_compile_options(rodent PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-fno-rtti;-finline-functions;-fprefetch-loop-arrays>
    $<$<CXX_COMPILER_ID:Clang,AppleClang>:-fno-rtti>)

# USE_THREADS is on by default in rodent.h -> link a threads library.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
target_link_libraries(rodent PRIVATE Threads::Threads)

# Makefile links -lm; harmless no-op where libm is part of libc.
if(UNIX)
    target_link_libraries(rodent PRIVATE m)
endif()
