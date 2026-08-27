# SPDX-License-Identifier: Apache-2.0
#
# lims — rung 6 of the application ladder (examples/lims/README.md).
# All target wiring lives in morph_add_rung() (cmake/morph_add_rung.cmake);
# this file only calls it.

cmake_minimum_required(VERSION 3.25)

morph_add_rung(NAME lims)

# Local workaround: the machine-local fastcache-cc/fastcached compiler-cache
# launcher (cmake/CompileCache.cmake) served a stale ladder_lims_tests object
# for tests/test_gui_qml_smoke.cpp after two TEST_CASEs were added to it --
# reproduced by compiling the same source directly with clang-cl (bypassing
# fastcache-cc entirely) and finding the new TEST_CASE names present in that
# object but absent from the one ninja linked, even after a from-scratch
# rebuild of just that translation unit. Same bug and same symptom as
# ledger's src/db/schema.cpp.obj / tests/test_ledger_schema.cpp.obj (see this
# file's identical comment there), tracked upstream at
# https://github.com/LASTRADA-Software/fastcached/issues/51. Safe to remove
# once that is fixed. Gated on fastcache-cc actually being the launcher --
# clearing the property unconditionally would clear *whatever* launcher is
# configured, silently bypassing sccache on CI (this bug is local-development
# only; fastcache-cc is only selected when a daemon actually answers).
if(MORPH_COMPILER_CACHE_IS_FASTCACHE)
    foreach(_lims_target IN ITEMS ladder_lims_lib ladder_lims_gui_lib ladder_lims_tests)
        if(TARGET ${_lims_target})
            set_target_properties(${_lims_target} PROPERTIES
                C_COMPILER_LAUNCHER ""
                CXX_COMPILER_LAUNCHER "")
        endif()
    endforeach()
endif()
