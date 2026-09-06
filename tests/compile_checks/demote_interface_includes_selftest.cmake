# SPDX-License-Identifier: Apache-2.0
#
# Configure-time self-test for
# cmake/morph_demote_interface_includes.cmake's
# morph_demote_interface_includes_to_system() (morph#438).
#
# ── Why a synthetic fixture, and not an assertion about Lightweight ──────────
#
# On Linux, `pkg-config --cflags odbc` is empty -- the same odbc.pc asks for
# `-I${includedir}`, but pkg-config strips it because /usr/include is on its
# system list. Verified on this tree's Linux host: `pkg-config --modversion
# odbc` -> 2.3.14, `--variable=includedir odbc` -> /usr/include, `--cflags
# odbc` -> empty. So `Lightweight`'s INTERFACE_COMPILE_OPTIONS contains no `-I`
# at all here, and a gate written as "assert no `-I` remains in Lightweight's
# interface" would pass on CI whether or not the helper does anything -- it
# would pass identically with the function body deleted.
#
# That is the exact shape of gate this repository has shipped before and been
# burned by (docs/spec/testing_charter.md, "Verify rather than assert"; the
# failure modes recorded in scripts/check_coverage_roots.sh's own header). So
# the helper is checked against a hand-written option list instead, exercising
# every case it claims to handle, with the expected results written out in
# full. This fails if the function body is removed.
#
# It does NOT prove the real Lightweight target is handed a real `-I` on the
# affected machine -- that is macOS-only and is item 7 of morph#438's
# acceptance criteria, for whoever has the hardware. This file proves the
# transform; that proves the input.

# ── The fixture ──────────────────────────────────────────────────────────────
# Every case morph#438 names, in one list:
#   -I/fixture/joined      joined form, one element
#   -I;/fixture/separated  separated form, two elements
#   -Ifixture/relative     joined form with a relative directory
#   -DKEEP_ME=1            a plain option that must survive, in order
#   -include forced.hpp    an option that merely *looks* like -I to a
#                          case-insensitive or prefix-only match, plus an
#                          argument that must not be mistaken for a directory
#   -Wall                  a second plain option, to pin ordering
add_library(morph_demote_selftest_fixture INTERFACE)
set_property(TARGET morph_demote_selftest_fixture PROPERTY INTERFACE_COMPILE_OPTIONS
    "-I/fixture/joined"
    "-DKEEP_ME=1"
    "-I" "/fixture/separated"
    "-include" "forced.hpp"
    "-Ifixture/relative"
    "-Wall")

morph_demote_interface_includes_to_system(morph_demote_selftest_fixture)

get_target_property(_morph_selftest_opts
    morph_demote_selftest_fixture INTERFACE_COMPILE_OPTIONS)
get_target_property(_morph_selftest_sys
    morph_demote_selftest_fixture INTERFACE_SYSTEM_INCLUDE_DIRECTORIES)

set(_morph_selftest_want_opts "-DKEEP_ME=1;-include;forced.hpp;-Wall")
# target_include_directories() resolves a relative directory against
# CMAKE_CURRENT_SOURCE_DIR, which is CMake's documented behaviour and not
# something the helper should be papering over -- pkg-config emits absolute
# paths, so the relative case is here to pin the transform, not the resolution.
set(_morph_selftest_want_sys
    "/fixture/joined;/fixture/separated;${CMAKE_CURRENT_SOURCE_DIR}/fixture/relative")

if(NOT "${_morph_selftest_opts}" STREQUAL "${_morph_selftest_want_opts}")
    message(FATAL_ERROR
        "morph_demote_interface_includes_to_system() self-test failed: residual "
        "INTERFACE_COMPILE_OPTIONS\n"
        "    got:      ${_morph_selftest_opts}\n"
        "    expected: ${_morph_selftest_want_opts}\n"
        "Every non-`-I` option must survive unchanged and in order, and every "
        "`-I` (joined or separated) must be gone. See "
        "cmake/morph_demote_interface_includes.cmake and morph#438.")
endif()

if(NOT "${_morph_selftest_sys}" STREQUAL "${_morph_selftest_want_sys}")
    message(FATAL_ERROR
        "morph_demote_interface_includes_to_system() self-test failed: "
        "INTERFACE_SYSTEM_INCLUDE_DIRECTORIES\n"
        "    got:      ${_morph_selftest_sys}\n"
        "    expected: ${_morph_selftest_want_sys}\n"
        "This is the property morph's three existing SYSTEM demotions read, and "
        "the one an `-isystem` on the command line comes from -- the whole point "
        "of the move. See cmake/morph_demote_interface_includes.cmake and "
        "morph#438.")
endif()

# ── Idempotence ──────────────────────────────────────────────────────────────
# Both FetchContent_MakeAvailable(Lightweight) sites call the helper and only
# one of them defines the target, so the second call must be a no-op rather
# than duplicating directories or eating a surviving option.
morph_demote_interface_includes_to_system(morph_demote_selftest_fixture)

get_target_property(_morph_selftest_opts2
    morph_demote_selftest_fixture INTERFACE_COMPILE_OPTIONS)
get_target_property(_morph_selftest_sys2
    morph_demote_selftest_fixture INTERFACE_SYSTEM_INCLUDE_DIRECTORIES)

if(NOT "${_morph_selftest_opts2}" STREQUAL "${_morph_selftest_want_opts}"
   OR NOT "${_morph_selftest_sys2}" STREQUAL "${_morph_selftest_want_sys}")
    message(FATAL_ERROR
        "morph_demote_interface_includes_to_system() self-test failed: the "
        "second call was not a no-op\n"
        "    options: ${_morph_selftest_opts2}\n"
        "    system:  ${_morph_selftest_sys2}\n"
        "Both FetchContent_MakeAvailable(Lightweight) sites call it; whichever "
        "configures first defines the target, so the other must be harmless.")
endif()

# ── The no-`-I` case ─────────────────────────────────────────────────────────
# This is what the function sees on Linux, and what it will see everywhere once
# the fix lands upstream. It must leave the options alone and add no system
# include directories at all -- an empty `target_include_directories(SYSTEM
# INTERFACE)` would be harmless, but a spurious "" entry would not be.
add_library(morph_demote_selftest_clean INTERFACE)
set_property(TARGET morph_demote_selftest_clean PROPERTY INTERFACE_COMPILE_OPTIONS
    "-DKEEP_ME=1" "-Wall")

morph_demote_interface_includes_to_system(morph_demote_selftest_clean)

get_target_property(_morph_selftest_clean_opts
    morph_demote_selftest_clean INTERFACE_COMPILE_OPTIONS)
get_target_property(_morph_selftest_clean_sys
    morph_demote_selftest_clean INTERFACE_SYSTEM_INCLUDE_DIRECTORIES)

if(NOT "${_morph_selftest_clean_opts}" STREQUAL "-DKEEP_ME=1;-Wall"
   OR _morph_selftest_clean_sys)
    message(FATAL_ERROR
        "morph_demote_interface_includes_to_system() self-test failed: a target "
        "with no `-I` was modified\n"
        "    options: ${_morph_selftest_clean_opts}\n"
        "    system:  ${_morph_selftest_clean_sys}\n"
        "This is the shape every target on Linux has, and the shape Lightweight "
        "will have everywhere once the fix lands upstream.")
endif()

# ── The all-`-I` case ────────────────────────────────────────────────────────
# When nothing survives, INTERFACE_COMPILE_OPTIONS must be unset rather than
# left holding one empty string element -- an empty option reaches the compiler
# as an empty argument.
add_library(morph_demote_selftest_only_includes INTERFACE)
set_property(TARGET morph_demote_selftest_only_includes
    PROPERTY INTERFACE_COMPILE_OPTIONS "-I/fixture/only")

morph_demote_interface_includes_to_system(morph_demote_selftest_only_includes)

get_target_property(_morph_selftest_only_opts
    morph_demote_selftest_only_includes INTERFACE_COMPILE_OPTIONS)
get_target_property(_morph_selftest_only_sys
    morph_demote_selftest_only_includes INTERFACE_SYSTEM_INCLUDE_DIRECTORIES)

if(_morph_selftest_only_opts OR NOT "${_morph_selftest_only_sys}" STREQUAL "/fixture/only")
    message(FATAL_ERROR
        "morph_demote_interface_includes_to_system() self-test failed: a target "
        "whose options were all `-I`\n"
        "    options: ${_morph_selftest_only_opts} (expected: unset)\n"
        "    system:  ${_morph_selftest_only_sys} (expected: /fixture/only)")
endif()

unset(_morph_selftest_opts)
unset(_morph_selftest_opts2)
unset(_morph_selftest_sys)
unset(_morph_selftest_sys2)
unset(_morph_selftest_want_opts)
unset(_morph_selftest_want_sys)
unset(_morph_selftest_clean_opts)
unset(_morph_selftest_clean_sys)
unset(_morph_selftest_only_opts)
unset(_morph_selftest_only_sys)
