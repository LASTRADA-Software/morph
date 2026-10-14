# SPDX-License-Identifier: Apache-2.0
#
# examples_common_tests: the foundation, headless. Sources open over a FakeAppContext and the shell mounts on
# RecordingBackend, so nothing here needs a display or a frontend.

add_executable(examples_common_tests
    probe_app.cpp
    test_client_args.cpp
    test_sources.cpp
    test_wait.cpp
)
target_link_libraries(examples_common_tests PRIVATE morph::examples_common morph_test_main)
apply_warnings(examples_common_tests)
if(DEFINED AF_SANITIZER)
    apply_sanitizers(examples_common_tests ${AF_SANITIZER})
endif()
if(AF_COVERAGE)
    apply_coverage(examples_common_tests)
endif()

include(Catch)
catch_discover_tests(examples_common_tests DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 120 LABELS "examples-common")
