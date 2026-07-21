# Opt-in soak tests: switchBackend/reconnect churn over many cycles, checking
# resource stability rather than a single-shot outcome. Built only under
# -DMORPH_BUILD_LOAD_TESTS=ON; never part of the default `morph_tests` target
# or the fast ctest sweep. See docs/spec/testing_strategy.md.
#
# Task 4 adds a second source file (test_soak_reconnect_churn.cpp) to this
# same target -- listing only this task's file for now keeps this task
# independently buildable.
add_executable(morph_soak
    test_soak_switch_backend.cpp
    test_soak_reconnect_churn.cpp
)
target_link_libraries(morph_soak PRIVATE morph::morph Catch2::Catch2WithMain)
target_include_directories(morph_soak PRIVATE ${CMAKE_SOURCE_DIR}/tests)
apply_warnings(morph_soak)

include(Catch)
catch_discover_tests(morph_soak DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 300 LABELS "soak")
