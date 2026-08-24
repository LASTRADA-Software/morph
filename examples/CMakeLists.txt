# SPDX-License-Identifier: Apache-2.0
#
# The application ladder (examples/LADDER.md). Orchestrates the shared
# infrastructure (common/) and, once MORPH_LADDER_RUNGS names them, the
# individual rung apps. Reached only when MORPH_BUILD_LADDER=ON (see the root
# CMakeLists.txt).

cmake_minimum_required(VERSION 3.25)

if(NOT TARGET morph::morph)
    message(FATAL_ERROR
        "examples/ (the ladder) expects the morph::morph target. Configure from the "
        "repository root with -DMORPH_BUILD_LADDER=ON instead of configuring "
        "examples/ directly.")
endif()

# PROJECT_SOURCE_DIR, not CMAKE_SOURCE_DIR: the latter is the *top-level*
# source dir, which is not morph's own root when morph is embedded via
# add_subdirectory() in a parent project.
include(${PROJECT_SOURCE_DIR}/cmake/morph_add_rung.cmake)

add_subdirectory(common)

# Rung directories register themselves here as they gain CMakeLists.txt files
# (rung 1 onward). MORPH_LADDER_RUNGS == "all" or a semicolon list selects
# which are configured — see examples/TESTING.md, "Build system and CI".
# No rung exists yet at rung 0, so this loop currently has nothing to do; it
# is real, working selection logic (not a placeholder) that the first rung's
# CMakeLists.txt addition activates without needing to touch this file again.
#
# The rung names themselves are NOT written here. They live in
# examples/rungs.txt, which CI's path-filter steps and scripts/coverage.sh also
# read — those run on a bare checkout, before any configure, so a CMake
# variable is unreachable to them. Hand-copying the list into each of them is
# what produced morph#179: ci.yml's filter had stopped at kanban, so rungs 5
# and 6 matched nothing and skipped the ladder jobs entirely.
set(_morph_rung_file "${CMAKE_CURRENT_SOURCE_DIR}/rungs.txt")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_morph_rung_file}")
if(NOT EXISTS "${_morph_rung_file}")
    message(FATAL_ERROR
        "examples/rungs.txt is missing. It is the ladder's authoritative rung "
        "list; without it no rung can be configured.")
endif()
# Whole-line comments only, and every remaining line must be a bare rung name.
# Anything else is an error rather than something to skip: a line this parser
# quietly ignored would drop a rung from the list, which is precisely the
# silent-omission failure the file exists to prevent.
file(STRINGS "${_morph_rung_file}" _morph_rung_lines)
set(_morph_known_rungs "")
foreach(_line IN LISTS _morph_rung_lines)
    string(STRIP "${_line}" _line)
    if(_line STREQUAL "" OR _line MATCHES "^#")
        continue()
    endif()
    if(NOT _line MATCHES "^[A-Za-z0-9_]+$")
        message(FATAL_ERROR
            "examples/rungs.txt: '${_line}' is not a bare rung name. One name "
            "per line; comments must occupy a whole line.")
    endif()
    list(APPEND _morph_known_rungs "${_line}")
endforeach()
if(NOT _morph_known_rungs)
    message(FATAL_ERROR
        "examples/rungs.txt named no rungs. An empty list configures no rung "
        "at all and would do so silently — see the file's own header.")
endif()

# This list is the one whose staleness must fail loudly, because every other
# consumer derives from it. A rung directory that calls morph_add_rung() but is
# not named in rungs.txt would otherwise be skipped in silence: it would never
# build, never be tested, and never appear in any CI path filter — and nothing
# would say so. Refuse to configure instead.
file(GLOB _morph_rung_cmakelists CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/*/CMakeLists.txt")
foreach(_candidate IN LISTS _morph_rung_cmakelists)
    file(READ "${_candidate}" _candidate_text)
    # Anchored at line start so the identical token inside a comment (as in
    # examples/common/CMakeLists.txt) is not mistaken for a call.
    if(_candidate_text MATCHES "(^|\n)morph_add_rung\\(NAME[ \t]+([A-Za-z0-9_]+)")
        set(_declared "${CMAKE_MATCH_2}")
        if(NOT _declared IN_LIST _morph_known_rungs)
            message(FATAL_ERROR
                "${_candidate} declares the ladder rung '${_declared}', but "
                "'${_declared}' is not listed in examples/rungs.txt. Add it "
                "there: that file is the single authority every consumer "
                "reads (CI path filters, scripts/coverage.sh), and a rung "
                "missing from it is a rung nothing builds or tests.")
        endif()
    endif()
endforeach()

foreach(_rung ${_morph_known_rungs})
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_rung}/CMakeLists.txt")
        continue()
    endif()
    if(MORPH_LADDER_RUNGS STREQUAL "all" OR _rung IN_LIST MORPH_LADDER_RUNGS)
        add_subdirectory(${_rung})
    endif()
endforeach()
