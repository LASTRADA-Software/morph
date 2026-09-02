# SPDX-License-Identifier: Apache-2.0
#
# ledger — rung 5 of the application ladder (examples/ledger/README.md).
# All target wiring lives in morph_add_rung() (cmake/morph_add_rung.cmake);
# this file only pulls in ledger-specific sources it doesn't know about, then
# calls it.

cmake_minimum_required(VERSION 3.25)

morph_add_rung(NAME ledger)

# morph_add_rung() only globs src/models/*.cpp, src/db/*.cpp and
# src/app/*.cpp into ladder_ledger_lib (cmake/morph_add_rung.cmake) -- it
# does not know about this rung's src/dto/ (Login's principal-charset
# validation, morph#242) or src/auth/ (none needed: LedgerAuthorizer is
# header-only, unlike PollsAuthorizer's split .cpp -- see
# ledger/auth/ledger_authorizer.hpp's own doc comment). Without an explicit
# target_sources() call the rung fails to link with undefined
# ledger::Login::validate. Mirrors bookmarks' own CMakeLists.txt treatment of
# its identical src/dto/auth_dto.cpp gap.
if(TARGET ladder_ledger_lib)
    target_sources(ladder_ledger_lib PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/dto/auth_dto.cpp")
endif()

# ladder_ledger_lib is native-only (morph_add_rung.cmake's own comment:
# "ladder_<rung>_gui_wasm never links ladder_<rung>_lib -- so this target
# genuinely never needs to build under Emscripten at all"), so the
# target_sources() call above silently no-ops under EMSCRIPTEN. auth_dto.cpp
# has no persistence dependency, so it is equally at home in
# ladder_ledger_gui_lib, which does build under Emscripten and is what
# ladder_ledger_gui_wasm links. Mirrors bookmarks' own CMakeLists.txt
# treatment of the identical gap.
if(TARGET ladder_ledger_gui_lib AND NOT TARGET ladder_ledger_lib)
    target_sources(ladder_ledger_gui_lib PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/dto/auth_dto.cpp")
endif()

# gui_wasm/*.cpp doesn't exist yet -- morph_add_rung() already skips
# ladder_ledger_gui_wasm silently when its source directory is empty, so
# there is nothing to comment out here. The WASM client's server-url cache
# variable (see bookmarks'/polls' own CMakeLists.txt) has its equivalent
# block added alongside gui_wasm/ in a later task, if this rung ever grows
# one.

