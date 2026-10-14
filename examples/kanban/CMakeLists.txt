# SPDX-License-Identifier: Apache-2.0
#
# kanban — rung 4 of the application ladder (examples/kanban/README.md).
# All target wiring lives in morph_add_rung() (cmake/morph_add_rung.cmake);
# this file only adds the sources it does not glob.

cmake_minimum_required(VERSION 3.25)

morph_add_rung(NAME kanban)

# morph_add_rung() globs src/models, src/db and src/app; the authorizer and the
# auth DTO's validation live outside those.
if(TARGET ladder_kanban_lib)
    target_sources(ladder_kanban_lib PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/auth/kanban_authorizer.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/dto/auth_dto.cpp")
endif()

# The attachment HTTP side channel needs QTcpServer/QTcpSocket. ladder_kanban_lib
# sits below the transport and does not link morph::qt, so it links Qt6::Network
# itself. Qt-only, like the WebSocket transport it shares a TokenVerifier with.
if(TARGET ladder_kanban_lib AND MORPH_BUILD_QT)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Network)
    target_sources(ladder_kanban_lib PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/http/attachment_server.cpp")
    target_link_libraries(ladder_kanban_lib PUBLIC Qt6::Network)
endif()
