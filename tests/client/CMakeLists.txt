# SPDX-License-Identifier: Apache-2.0
#
# morph_client_tests: the generic client's sources and shell, headless. Sources open over a FakeAppContext and the
# shell mounts on RecordingBackend, so nothing here needs a display or a frontend. The ladder testkit's headers
# (examples/common/testkit) give the fake context and the waits.

add_executable(morph_client_tests
    probe_app.cpp
    test_client_args.cpp
    test_sign_in.cpp
    test_sources.cpp
    test_wait.cpp
)
target_include_directories(morph_client_tests PRIVATE ${PROJECT_SOURCE_DIR}/examples/common)
target_link_libraries(morph_client_tests PRIVATE morph::client morph_test_main)
apply_warnings(morph_client_tests)
if(DEFINED AF_SANITIZER)
    apply_sanitizers(morph_client_tests ${AF_SANITIZER})
endif()
if(AF_COVERAGE)
    apply_coverage(morph_client_tests)
endif()

include(Catch)
catch_discover_tests(morph_client_tests DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 120 LABELS "client")
