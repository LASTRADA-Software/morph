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

# Local workaround: the machine-local fastcache-cc/fastcached compiler-cache
# launcher (cmake/CompileCache.cmake) served a stale ladder_pastebin_tests
# object for tests/test_gui_qml_smoke.cpp after a TEST_CASE was added to it --
# reproduced by compiling the same source directly with clang-cl (bypassing
# fastcache-cc entirely) and finding the new TEST_CASE name present in that
# object but absent from the one ninja linked, even after a from-scratch
# rebuild of just that translation unit. Same bug and same symptom as
# ledger's src/db/schema.cpp.obj / tests/test_ledger_schema.cpp.obj (see that
# file's identical comment), tracked upstream at
# https://github.com/LASTRADA-Software/fastcached/issues/51. Safe to remove
# once that is fixed. Gated on fastcache-cc actually being the launcher --
# clearing the property unconditionally would clear *whatever* launcher is
# configured, silently bypassing sccache on CI (this bug is local-development
# only; fastcache-cc is only selected when a daemon actually answers).
if(MORPH_COMPILER_CACHE_IS_FASTCACHE)
    foreach(_pastebin_target IN ITEMS ladder_pastebin_lib ladder_pastebin_gui_lib ladder_pastebin_tests)
        if(TARGET ${_pastebin_target})
            set_target_properties(${_pastebin_target} PROPERTIES
                C_COMPILER_LAUNCHER ""
                CXX_COMPILER_LAUNCHER "")
        endif()
    endforeach()
endif()
