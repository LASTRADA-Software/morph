# SPDX-License-Identifier: Apache-2.0
#
# polls — rung 3 of the application ladder (examples/polls/README.md).
# All target wiring lives in morph_add_rung() (cmake/morph_add_rung.cmake);
# this file only pulls in polls-specific sources it doesn't know about, then
# calls it.

cmake_minimum_required(VERSION 3.25)

morph_add_rung(NAME polls)

# morph_add_rung() only globs src/models/*.cpp, src/db/*.cpp and
# src/app/*.cpp into ladder_polls_lib (cmake/morph_add_rung.cmake:91-92)
# — it does not know about this rung's src/auth/ (Tasks 1-10's
# PollsAuthorizer), so without an explicit target_sources() call the rung
# fails to link with undefined polls::auth::PollsAuthorizer symbols.
# Mirrors bookmarks' own CMakeLists.txt treatment of src/import/ and src/dto/.
# (src/db/schema.cpp needs no equivalent line here -- the glob above already
# covers src/db/*.cpp.)
if(TARGET ladder_polls_lib)
    target_sources(ladder_polls_lib PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/auth/polls_authorizer.cpp")
endif()

# ladder_polls_lib is native-only (morph_add_rung.cmake's own comment:
# "ladder_<rung>_gui_wasm never links ladder_<rung>_lib — so this target
# genuinely never needs to build under Emscripten at all"), so the
# target_sources() call above silently no-ops under EMSCRIPTEN. Nothing in
# ladder_polls_gui_wasm references PollsAuthorizer today, so this has not
# yet produced bookmarks' identical undefined-symbol link failure — but the
# same trap is there the moment it does. polls_authorizer.cpp has no
# persistence dependency, so it is equally at home in ladder_polls_gui_lib,
# which does build under Emscripten and is what ladder_polls_gui_wasm links.
# Mirrors bookmarks' own CMakeLists.txt treatment of the identical gap for
# src/dto/auth_dto.cpp.
if(TARGET ladder_polls_gui_lib AND NOT TARGET ladder_polls_lib)
    target_sources(ladder_polls_gui_lib PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/auth/polls_authorizer.cpp")
endif()

# ── The WASM client's server url ────────────────────────────────────────────
# Same mechanism as pastebin's/bookmarks' own CMakeLists.txt — see either
# file's comment. Port 8767 matches ladder_polls_server's own compiled-in
# default (src/server/main.cpp), the next free port after pastebin's 8765 and
# bookmarks' 8766.
if(TARGET ladder_polls_gui_wasm)
    if(NOT DEFINED MORPH_LADDER_POLLS_WASM_SERVER_URL)
        set(MORPH_LADDER_POLLS_WASM_SERVER_URL "ws://127.0.0.1:8767" CACHE STRING
            "URL polls' WASM client connects to; must be a reachable ladder_polls_server.")
    endif()
    target_compile_definitions(ladder_polls_gui_wasm PRIVATE
        MORPH_LADDER_POLLS_WASM_SERVER_URL="${MORPH_LADDER_POLLS_WASM_SERVER_URL}"
    )
endif()
