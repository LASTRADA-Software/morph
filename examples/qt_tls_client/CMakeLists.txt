# SPDX-License-Identifier: Apache-2.0
#
# Worked example: authenticated TLS peer verification over the Qt WebSocket
# transport. Demonstrates tlsPinnedConfig() accepting a server whose
# certificate is pinned and rejecting a second server that presents a
# different certificate (a stand-in MITM), and contrasts both with
# tlsInsecureNoVerify() connecting to either. See README.md and
# docs/spec/security.md ("Transport security").

add_executable(morph_qt_tls_example main.cpp)
target_link_libraries(morph_qt_tls_example PRIVATE morph_qt_impl)
apply_warnings(morph_qt_tls_example)
apply_bigobj(morph_qt_tls_example)

target_compile_definitions(morph_qt_tls_example PRIVATE
    QT_TLS_EXAMPLE_CERTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/certs"
)

if(MORPH_BUILD_TESTS)
    add_test(NAME qt_tls_example_runs COMMAND morph_qt_tls_example)
    set_tests_properties(qt_tls_example_runs PROPERTIES TIMEOUT 60)
endif()
