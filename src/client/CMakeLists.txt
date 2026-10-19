# SPDX-License-Identifier: Apache-2.0
#
# morph::client, the generic client's source and shell (docs/spec/client/client.md): where its screens come from (a
# server or an in-process application), the shell it mounts them in, and the application host a shell of an
# application's own mounts with. Its headers name no toolkit. The two remote transports are optional translation
# units, linked PRIVATE: a Qt WebSocket one when morph_qt_impl is built, a raw-socket one when morph::net is.

set(_morph_client_transport_qt 0)
set(_morph_client_transport_net 0)
add_library(morph_client STATIC
    args.cpp
    shell.cpp
    source.cpp
    transport.cpp
)
add_library(morph::client ALIAS morph_client)
target_sources(morph_client
    PUBLIC
    FILE_SET HEADERS
    BASE_DIRS ${PROJECT_SOURCE_DIR}/include
    FILES
        ${PROJECT_SOURCE_DIR}/include/morph/client/args.hpp
        ${PROJECT_SOURCE_DIR}/include/morph/client/shell.hpp
        ${PROJECT_SOURCE_DIR}/include/morph/client/source.hpp
        ${PROJECT_SOURCE_DIR}/include/morph/client/transport.hpp
)
# Same standalone-compile check the core target gets.
set_target_properties(morph_client PROPERTIES VERIFY_INTERFACE_HEADER_SETS ON)
if(TARGET morph_qt_impl)
    target_sources(morph_client PRIVATE transport_qt.cpp)
    target_link_libraries(morph_client PRIVATE morph::qt morph_qt_impl)
    set(_morph_client_transport_qt 1)
endif()
if(TARGET morph::net)
    target_sources(morph_client PRIVATE transport_net.cpp)
    target_link_libraries(morph_client PRIVATE morph::net)
    set(_morph_client_transport_net 1)
endif()
target_compile_definitions(morph_client PRIVATE
    MORPH_CLIENT_TRANSPORT_QT=${_morph_client_transport_qt}
    MORPH_CLIENT_TRANSPORT_NET=${_morph_client_transport_net})
target_link_libraries(morph_client PUBLIC morph)
target_compile_features(morph_client PUBLIC cxx_std_23)
apply_warnings(morph_client)
# Every consumer of this archive carries an AF_SANITIZER block of its own, which is what makes instrumenting a static
# library safe (see morph_qt_impl).
if(DEFINED AF_SANITIZER)
    apply_sanitizers(morph_client ${AF_SANITIZER})
endif()
if(AF_COVERAGE)
    apply_coverage(morph_client)
endif()
unset(_morph_client_transport_qt)
unset(_morph_client_transport_net)
