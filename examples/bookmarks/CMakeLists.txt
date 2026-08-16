# SPDX-License-Identifier: Apache-2.0
#
# bookmarks — rung 2 of the application ladder (examples/bookmarks/README.md).
# All target wiring lives in morph_add_rung() (cmake/morph_add_rung.cmake);
# this file only pulls in bookmarks-specific dependencies it doesn't know
# about, then calls it.

cmake_minimum_required(VERSION 3.25)

morph_add_rung(NAME bookmarks)

# morph_add_rung() only globs src/models/*.cpp, src/db/*.cpp and
# src/app/*.cpp into ladder_bookmarks_lib (cmake/morph_add_rung.cmake:91-92)
# — it does not know about this rung's src/import/ (Task 11's Netscape
# bookmarks importer) or src/dto/ (Task 12's auth DTO validation), so
# without an explicit target_sources() call the rung fails to link with
# undefined bookmarks::import::parseNetscapeChunk / bookmarks::Login::validate.
# Confirmed against Task 12's independent build (task-12-report.md,
# "Verification" section).
if(TARGET ladder_bookmarks_lib)
    target_sources(ladder_bookmarks_lib PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/import/netscape_bookmarks.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/dto/auth_dto.cpp")
endif()

# ladder_bookmarks_lib is native-only (morph_add_rung.cmake's own comment:
# "ladder_<rung>_gui_wasm never links ladder_<rung>_lib — so this target
# genuinely never needs to build under Emscripten at all"), so the
# target_sources() call above silently no-ops under EMSCRIPTEN and
# auth_dto.cpp is never compiled into anything the WASM GUI links —
# undefined bookmarks::Login::validate() at the ladder_bookmarks_gui_wasm
# link step. auth_dto.cpp has no persistence dependency (pure DTO
# validation), so it is equally at home in ladder_bookmarks_gui_lib, which
# does build under Emscripten and is what ladder_bookmarks_gui_wasm links.
if(TARGET ladder_bookmarks_gui_lib AND NOT TARGET ladder_bookmarks_lib)
    target_sources(ladder_bookmarks_gui_lib PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/dto/auth_dto.cpp")
endif()

# ── The WASM client's server url ────────────────────────────────────────────
# Same mechanism as pastebin's own CMakeLists.txt — see that file's comment.
if(TARGET ladder_bookmarks_gui_wasm)
    if(NOT DEFINED MORPH_LADDER_BOOKMARKS_WASM_SERVER_URL)
        set(MORPH_LADDER_BOOKMARKS_WASM_SERVER_URL "ws://127.0.0.1:8766" CACHE STRING
            "URL bookmarks' WASM client connects to; must be a reachable ladder_bookmarks_server.")
    endif()
    target_compile_definitions(ladder_bookmarks_gui_wasm PRIVATE
        MORPH_LADDER_BOOKMARKS_WASM_SERVER_URL="${MORPH_LADDER_BOOKMARKS_WASM_SERVER_URL}"
    )
endif()
