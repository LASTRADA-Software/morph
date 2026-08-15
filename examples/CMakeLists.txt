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
set(_morph_known_rungs pastebin bookmarks polls kanban)
foreach(_rung ${_morph_known_rungs})
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_rung}/CMakeLists.txt")
        continue()
    endif()
    if(MORPH_LADDER_RUNGS STREQUAL "all" OR _rung IN_LIST MORPH_LADDER_RUNGS)
        add_subdirectory(${_rung})
    endif()
endforeach()
