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

# Task 17: the attachment HTTP side channel (src/http/attachment_server.cpp)
# is not picked up by morph_add_rung()'s own globs (src/models, src/db,
# src/app only) -- same reason src/auth/ and src/dto/ need the explicit
# target_sources() call above. It needs Qt6::Network for QTcpServer/
# QTcpSocket, which ladder_kanban_lib does not otherwise link (it only pulls
# in Qt6::Core directly); morph::qt's own Qt6::WebSockets dependency brings
# Qt6::Network in transitively for any consumer that already links morph::qt,
# but ladder_kanban_lib is a lower layer than that (models/db/app, no
# transport), so it needs its own direct Qt6::Network link rather than
# reaching for morph::qt just for one header's socket types. Gated on
# MORPH_BUILD_QT: like the WebSocket transport this shares a TokenVerifier
# with, this is a Qt-only server and does not build without Qt in the tree.
if(TARGET ladder_kanban_lib AND MORPH_BUILD_QT)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Network)
    target_sources(ladder_kanban_lib PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/http/attachment_server.cpp")
    target_link_libraries(ladder_kanban_lib PUBLIC Qt6::Network)
endif()

# Task 18: BoardBridge's attachment upload/download (gui_lib/board_qml_bridge.
# hpp/.cpp) uses QNetworkAccessManager to talk to Task 17's HTTP side
# channel -- this needs Qt6::Network too, but on ladder_kanban_gui_lib, a
# different target from the one Task 17 (above) wired it onto.
# ladder_kanban_gui_lib only ever picks up Qt6::Network *transitively* by
# linking morph::ladder_kanban_lib (cmake/morph_add_rung.cmake's
# `if(TARGET ladder_${_rung}_lib) target_link_libraries(...
# morph::ladder_${_rung}_lib)` block) -- and ladder_kanban_lib itself does
# not exist at all under Emscripten (see morph_add_rung.cmake's own
# "native only" comment on that target: persistence lives server-side for
# a WASM client, so there is nothing for gui_lib to link there). The
# result: native builds succeeded by accident (through that transitive
# chain), while the WASM build -- which has no ladder_kanban_lib to
# transit through -- failed with "'QNetworkAccessManager' file not found".
# gui_lib needs this dependency directly and unconditionally, on every
# platform, since QNetworkAccessManager is used by shared presenter/bridge
# code both the desktop and WASM clients link.
if(TARGET ladder_kanban_gui_lib AND MORPH_BUILD_QT)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Network)
    target_link_libraries(ladder_kanban_gui_lib PUBLIC Qt6::Network)
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

# Task 18: TaskDetailPopup.qml's "Attach file"/"Download" affordances use
# QtQuick.Dialogs' FileDialog for local file selection. morph_add_rung()'s
# qt_add_qml_module(ladder_kanban_qml ...) call scans this rung's own .qml
# files for their `import`s and auto-links whatever known QML plugins they
# name (that scan is why Controls/Layouts/Templates already resolve with no
# extra wiring here) -- but only among plugins already visible as a linked
# dependency of ladder_kanban_qml itself. Qt6::QuickDialogs2 is a component
# nothing in this tree links yet (kanban is the first rung to use a
# QtQuick.Dialogs type), so without this the *engine* can still parse and
# AOT-compile TaskDetailPopup.qml (qmltypes alone are enough for that), but
# resolving the type at runtime fails with "module \"QtQuick.Dialogs\" is not
# installed" -- confirmed by linking it onto the consuming ladder_kanban_gui/
# ladder_kanban_tests executables instead, which did not fix it: the plugin
# has to be a dependency of the *QML module* the scan runs against, not of
# whatever later links that module.
if(TARGET ladder_kanban_qml)
    find_package(Qt6 6.5 REQUIRED COMPONENTS QuickDialogs2)
    target_link_libraries(ladder_kanban_qml PUBLIC Qt6::QuickDialogs2)
endif()
