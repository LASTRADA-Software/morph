add_executable(morph_net_tests
    test_base64.cpp
    test_sha1.cpp
    test_ws_handshake.cpp
    test_tcp_socket.cpp
    test_ws_frame.cpp
    test_handshake_over_socket.cpp
    test_socket_server.cpp
    test_socket_backend.cpp
)

target_link_libraries(morph_net_tests
    PRIVATE
        morph::net
        Catch2::Catch2WithMain
)

apply_warnings(morph_net_tests)

if(DEFINED AF_SANITIZER)
    apply_sanitizers(morph_net_tests ${AF_SANITIZER})
endif()
if(AF_COVERAGE)
    apply_coverage(morph_net_tests)
endif()

include(Catch)
catch_discover_tests(morph_net_tests DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 120)
