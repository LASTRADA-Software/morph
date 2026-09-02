# SPDX-License-Identifier: Apache-2.0
#
# pastebin — rung 1 of the application ladder (examples/pastebin/README.md).
# All target wiring lives in morph_add_rung() (cmake/morph_add_rung.cmake);
# this file only pulls in pastebin-specific dependencies morph_add_rung()
# itself doesn't know about, then calls it.

cmake_minimum_required(VERSION 3.25)

morph_add_rung(NAME pastebin)

# ── The WASM client's server url ────────────────────────────────────────────
# A page served from a static bundle has no argv to read a --server flag from,
# so the url the browser client connects to is a build-time constant. Same
# mechanism and same shape as the rung-0 spike's own
# MORPH_LADDER_WASM_SPIKE_SERVER_URL (examples/common/wasm_spike/CMakeLists.txt),
# under a per-rung name so several rungs' WASM clients can point at their own
# servers in one Emscripten configure. Guarded on the target rather than on
# EMSCRIPTEN directly: morph_add_rung() creates it only under Emscripten, and
# only when its prerequisites are met (it announces every skip).
if(TARGET ladder_pastebin_gui_wasm)
    if(NOT DEFINED MORPH_LADDER_PASTEBIN_WASM_SERVER_URL)
        set(MORPH_LADDER_PASTEBIN_WASM_SERVER_URL "ws://127.0.0.1:8765" CACHE STRING
            "URL pastebin's WASM client connects to; must be a reachable ladder_pastebin_server.")
    endif()
    target_compile_definitions(ladder_pastebin_gui_wasm PRIVATE
        MORPH_LADDER_PASTEBIN_WASM_SERVER_URL="${MORPH_LADDER_PASTEBIN_WASM_SERVER_URL}"
    )
endif()


