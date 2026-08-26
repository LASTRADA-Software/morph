# SPDX-License-Identifier: Apache-2.0
#
# ledger — rung 5 of the application ladder (examples/ledger/README.md).
# All target wiring lives in morph_add_rung() (cmake/morph_add_rung.cmake);
# this file only pulls in ledger-specific sources it doesn't know about, then
# calls it.

cmake_minimum_required(VERSION 3.25)

morph_add_rung(NAME ledger)

# No target_sources() extras: unlike polls' src/auth/ (PollsAuthorizer), this
# rung has no source directory morph_add_rung()'s globs (src/models/*.cpp,
# src/db/*.cpp, src/app/*.cpp) don't already cover -- src/app/app.cpp
# (ledger::app::App, the report runner) is picked up by the third of those.
# Add one here, mirroring polls' own CMakeLists.txt treatment of src/auth/,
# if a later task introduces such a directory.
#
# There is deliberately no src/server/ yet, so morph_add_rung() emits no
# ladder_ledger_server target: this rung installs no authorizer, and an
# unauthenticated financial server is not something to ship (morph#242). See
# examples/ledger/include/ledger/app/app.hpp.

# gui/*.cpp and gui_wasm/*.cpp don't exist yet (Task 22 wires the desktop
# client) -- morph_add_rung() already skips ladder_ledger_gui and
# ladder_ledger_gui_wasm silently when their source directories are empty, so
# there is nothing to comment out here. The WASM client's server-url cache
# variable (see polls' own CMakeLists.txt) has its equivalent block added
# alongside gui_wasm/ in that later task.

# Local workaround: the machine-local fastcache-cc/fastcached compiler-cache
# launcher (cmake/CompileCache.cmake) was observed serving a stale, empty
# object for src/db/schema.cpp.obj regardless of the file's actual content --
# a HIT against a key that does not vary with the translation unit's content,
# reproduced by invoking fastcache-cc directly (bypassing ninja) with both
# direct and non-direct (FASTCACHE_NO_DIRECT=1) modes; only changing the
# object's *output path* produced a fresh key. No eviction primitive exists
# in the tool and the daemon's on-disk cache directory is outside this
# checkout, so this target opts out of the launcher entirely rather than
# risk silently linking a stale schema migration object again. Safe to
# remove once the underlying cache bug is fixed upstream (see fastcache-cc
# --help / D:/caching/README.md for the tool this refers to).
#
# Same bug hit tests/test_ledger_schema.cpp.obj (Task 5): a rebuild after
# adding a second TEST_CASE to that file kept linking a stale object with
# only the original TEST_CASE (verified via `strings` on the linked exe and
# `--list-tests`, not just a passing/stale ctest result), even after
# deleting the .obj by hand and reinvoking ninja -- the launcher re-served
# the same stale bytes on the very next build. ladder_ledger_tests opts out
# for the same reason.
if(TARGET ladder_ledger_lib)
    if(MORPH_COMPILER_CACHE_IS_FASTCACHE)
        set_target_properties(ladder_ledger_lib PROPERTIES
            C_COMPILER_LAUNCHER ""
            CXX_COMPILER_LAUNCHER "")
    endif()
endif()
if(TARGET ladder_ledger_tests)
    if(MORPH_COMPILER_CACHE_IS_FASTCACHE)
        set_target_properties(ladder_ledger_tests PROPERTIES
            C_COMPILER_LAUNCHER ""
            CXX_COMPILER_LAUNCHER "")
    endif()
endif()
