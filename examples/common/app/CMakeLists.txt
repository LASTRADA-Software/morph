# SPDX-License-Identifier: Apache-2.0
#
# morph_examples_common: what the generic client and every example's screen tests share (spec 4 §4). Its headers
# name no toolkit. The two remote transports are optional translation units, linked PRIVATE: a Qt WebSocket one
# when morph::qt is built, a raw-socket one when morph::net is. The include root is examples/common, so headers
# read <app/...> and <testkit/...>.

set(_morph_examples_transport_qt 0)
set(_morph_examples_transport_net 0)
add_library(morph_examples_common STATIC
    app_source.cpp
    client_shell.cpp
    transport.cpp
)
add_library(morph::examples_common ALIAS morph_examples_common)
if(TARGET morph_qt_impl)
    target_sources(morph_examples_common PRIVATE transport_qt.cpp)
    target_link_libraries(morph_examples_common PRIVATE morph::qt morph_qt_impl)
    set(_morph_examples_transport_qt 1)
endif()
if(TARGET morph::net)
    target_sources(morph_examples_common PRIVATE transport_net.cpp)
    target_link_libraries(morph_examples_common PRIVATE morph::net)
    set(_morph_examples_transport_net 1)
endif()
target_compile_definitions(morph_examples_common PRIVATE
    MORPH_EXAMPLES_TRANSPORT_QT=${_morph_examples_transport_qt}
    MORPH_EXAMPLES_TRANSPORT_NET=${_morph_examples_transport_net})
target_include_directories(morph_examples_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(morph_examples_common PUBLIC morph::morph)
target_compile_features(morph_examples_common PUBLIC cxx_std_23)
apply_warnings(morph_examples_common)
if(DEFINED AF_SANITIZER)
    apply_sanitizers(morph_examples_common ${AF_SANITIZER})
endif()
if(AF_COVERAGE)
    apply_coverage(morph_examples_common)
endif()

if(MORPH_BUILD_TESTS)
    add_subdirectory(tests)
endif()
