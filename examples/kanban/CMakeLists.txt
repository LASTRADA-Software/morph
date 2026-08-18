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
# KanbanAuthorizer) or src/dto/ (Task 8's Login::validate()), so without an
# explicit target_sources() call the rung fails to link with undefined
# kanban::auth::KanbanAuthorizer / kanban::Login::validate() symbols.
# Mirrors bookmarks' own CMakeLists.txt treatment of src/import/ and src/dto/.
# (src/db/schema.cpp needs no equivalent line here -- the glob above already
# covers src/db/*.cpp.)
if(TARGET ladder_kanban_lib)
    target_sources(ladder_kanban_lib PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/auth/kanban_authorizer.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/dto/auth_dto.cpp")
endif()

# Task 5: wires SqliteOfflineQueue/NetworkMonitor/SyncWorker/
# ReconnectCoordinator into BoardBridge::moveTask (gui_lib/board_qml_bridge.
# hpp/.cpp) -- optional, like every other morph::offline_sqlite consumer
# (root CMakeLists.txt's own "SQLite-backed durable offline queue" block,
# which creates morph::offline_sqlite ahead of this rung's own
# add_subdirectory(), specifically so it can be named here). Off by default:
# a configure with MORPH_BUILD_OFFLINE_SQLITE=OFF builds this rung exactly as
# before this task, with no SQLite3 dependency and BoardBridge compiled with
# no offline awareness at all (every new symbol these files add is itself
# `#ifdef MORPH_BUILD_OFFLINE_SQLITE`-guarded -- see board_qml_bridge.hpp's
# own doc comment on that guard).
#
# No new source files to add here: board_qml_bridge.hpp/.cpp and gui/main.cpp
# are pre-existing files morph_add_rung()'s own gui_lib/*.cpp and gui/*.cpp
# globs already pick up. This block only supplies the compile definition
# (MORPH_BUILD_OFFLINE_SQLITE, read by their #ifdef guards) and the
# morph::offline_sqlite link dependency (SQLite3 headers/libs) those guarded
# code paths need to actually build -- and does the same for
# ladder_kanban_tests, since this task's own new test
# (tests/test_board_offline_bridge.cpp, also #ifdef-guarded at file scope)
# lives in that same glob.
if(MORPH_BUILD_OFFLINE_SQLITE)
    if(TARGET ladder_kanban_gui_lib)
        target_compile_definitions(ladder_kanban_gui_lib PUBLIC MORPH_BUILD_OFFLINE_SQLITE)
        target_link_libraries(ladder_kanban_gui_lib PUBLIC morph::offline_sqlite)
    endif()
    if(TARGET ladder_kanban_gui)
        target_compile_definitions(ladder_kanban_gui PRIVATE MORPH_BUILD_OFFLINE_SQLITE)
    endif()
    if(TARGET ladder_kanban_tests)
        target_compile_definitions(ladder_kanban_tests PRIVATE MORPH_BUILD_OFFLINE_SQLITE)
        target_link_libraries(ladder_kanban_tests PRIVATE morph::offline_sqlite)
    endif()
endif()
