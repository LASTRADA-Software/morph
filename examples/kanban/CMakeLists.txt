# SPDX-License-Identifier: Apache-2.0
#
# kanban — rung 4 of the application ladder (examples/kanban/README.md).
# All target wiring lives in morph_add_rung() (cmake/morph_add_rung.cmake);
# this file only pulls in kanban-specific sources it doesn't know about, then
# calls it.

cmake_minimum_required(VERSION 3.25)

morph_add_rung(NAME kanban)

# morph_add_rung() only globs src/models/*.cpp, src/db/*.cpp and
# src/app/*.cpp into ladder_kanban_lib (cmake/morph_add_rung.cmake:91-92)
# — it does not know about this rung's src/auth/ (Tasks 1-10's
# KanbanAuthorizer), so without an explicit target_sources() call the rung
# fails to link with undefined kanban::auth::KanbanAuthorizer symbols.
# Mirrors bookmarks' own CMakeLists.txt treatment of src/import/ and src/dto/.
# (src/db/schema.cpp needs no equivalent line here -- the glob above already
# covers src/db/*.cpp.)
if(TARGET ladder_kanban_lib)
    target_sources(ladder_kanban_lib PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/auth/kanban_authorizer.cpp")
endif()
