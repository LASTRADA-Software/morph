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
        morph_test_main
)

apply_warnings(morph_net_tests)

if(DEFINED AF_SANITIZER)
    apply_sanitizers(morph_net_tests ${AF_SANITIZER})
endif()
if(AF_COVERAGE)
    apply_coverage(morph_net_tests)
endif()

include(Catch)
# LABELS "net" is what makes `ctest -L net` mean anything (morph#771).
#
# Catch2's own tags -- `[net][tcp]`, `[net][handshake][socket]` -- are not
# ctest labels: no `catch_discover_tests` call in this repository passes
# `ADD_TAGS_AS_LABELS`, so nothing translates a tag into a label. Before this
# property existed, `ctest -L net` matched zero tests and *exited 0* having
# run nothing, which is precisely the shape AGENTS.md names first: a filter
# reporting success while measuring nothing. Measured on ce1f6193, bare ctest
# against this build directory: `Total Tests: 0`, then `No tests were found!!!`
# and exit 0. (Through a test preset it is less silent but no more useful --
# `base-test` sets `noTestsAction: error`, so `ctest --preset gcc-debug -L net`
# exited 8. The bare form is the one written into developer and agent briefs.)
#
# The same shape as tests/fuzz (`LABELS "fuzz"`), tests/bench (`LABELS
# "bench"`) and tests/soak (`LABELS "soak"`): one value, applied uniformly to
# every discovered test by `catch_discover_tests`' PROPERTIES. Keep it in step
# with the suite's directory name -- the label is only useful as long as the
# name someone guesses is the name that is set.
catch_discover_tests(morph_net_tests DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 120 LABELS "net")
