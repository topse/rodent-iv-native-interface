# Copyright (C) 2026 T. Steinmann (Rodent IV libification fork)
# SPDX-License-Identifier: GPL-3.0-or-later
#
# CMake build for the Rodent IV libification fork:
#
#   rodent_engine (alias rodent::engine)  static library, public header in include/
#   rodent (rodentIV)                     the classic UCI executable, a thin adapter
#                                         over the library (sources/src/main.cpp)
#   rodent_example_embed                  minimal embedder, see examples/
#
# The executable must stay byte-for-byte identical in behaviour to the upstream
# build, which is what the verification harness in test/ checks; both targets are
# therefore compiled with the same flags, mirroring sources/Makefile's default
# build (the Makefile remains the reference build and keeps working):
#
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

option(RODENT_BUILD_EXE "Build the classic rodentIV executable" ON)
option(RODENT_BUILD_EXAMPLES "Build the example embedder" ON)
option(RODENT_INSTALL "Generate install rules for the library" ON)

# Default to an optimized build (matches the Makefile's -O3 -DNDEBUG) unless the
# user picks a config explicitly.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

set(CMAKE_CXX_STANDARD 14)            # Makefile: -std=c++14
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)          # -std=c++14 is strict ISO, not gnu++14

# Codegen flags from the Makefile, applied to every target that compiles engine
# code. CMake's Release config already supplies -O3 -DNDEBUG; these are the
# remaining semantic flags. The full set is gated on GCC (the Makefile's reference
# compiler); Clang gets the portable subset.
set(RODENT_CODEGEN_FLAGS
    $<$<CXX_COMPILER_ID:GNU>:-fno-rtti;-finline-functions;-fprefetch-loop-arrays>
    $<$<CXX_COMPILER_ID:Clang,AppleClang>:-fno-rtti>)

# ---------------------------------------------------------------------------
# The engine library: every translation unit except the executable's main.cpp.
# ---------------------------------------------------------------------------
file(GLOB RODENT_LIB_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/sources/src/*.cpp")
list(REMOVE_ITEM RODENT_LIB_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/sources/src/main.cpp")

add_library(rodent_engine STATIC ${RODENT_LIB_SOURCES})
add_library(rodent::engine ALIAS rodent_engine)

target_include_directories(rodent_engine
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/sources/src)

target_compile_options(rodent_engine PRIVATE ${RODENT_CODEGEN_FLAGS})

# USE_THREADS is on by default in rodent.h -> link a threads library. PUBLIC
# because the engine spawns its own search and timer threads, so an embedder's
# link line needs it too.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
target_link_libraries(rodent_engine PUBLIC Threads::Threads)

# Makefile links -lm; harmless no-op where libm is part of libc.
if(UNIX)
    target_link_libraries(rodent_engine PUBLIC m)
endif()

# ---------------------------------------------------------------------------
# The classic executable: the stdio adapter over the engine.
# ---------------------------------------------------------------------------
if(RODENT_BUILD_EXE)
    add_executable(rodent "${CMAKE_CURRENT_SOURCE_DIR}/sources/src/main.cpp")
    set_target_properties(rodent PROPERTIES OUTPUT_NAME rodentIV)
    # The adapter uses the engine's internal header, not the public API (see
    # CLAUDE.md on why it does not construct a rodent::Engine).
    target_include_directories(rodent PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/sources/src)
    target_compile_options(rodent PRIVATE ${RODENT_CODEGEN_FLAGS})
    target_link_libraries(rodent PRIVATE rodent_engine)
endif()

# ---------------------------------------------------------------------------
# Example embedder (public API only).
# ---------------------------------------------------------------------------
if(RODENT_BUILD_EXAMPLES)
    add_executable(rodent_example_embed
                   "${CMAKE_CURRENT_SOURCE_DIR}/examples/embed_two_engines.cpp")
    target_link_libraries(rodent_example_embed PRIVATE rodent::engine)
endif()

# ---------------------------------------------------------------------------
# Install rules (library + public header). Installing the executable together
# with its personalities/books data stays the Makefile's job.
# ---------------------------------------------------------------------------
if(RODENT_INSTALL)
    include(GNUInstallDirs)
    install(TARGETS rodent_engine ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/rodent"
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
endif()
