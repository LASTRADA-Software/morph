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
target_link_libraries(morph_soak PRIVATE morph::morph morph_test_main)
target_include_directories(morph_soak PRIVATE ${CMAKE_SOURCE_DIR}/tests)
apply_warnings(morph_soak)

# morph#542 left the soak/bench decision explicit rather than assumed. Both are
# instrumented: they are opt-in (-DMORPH_BUILD_LOAD_TESTS=ON), so no default
# sanitizer leg pays for them, and switchBackend/reconnect churn over many
# cycles is exactly the shape of test whose finding is a leak or a race rather
# than a failed assertion -- which is the finding only a sanitizer reports.
if(DEFINED AF_SANITIZER)
    apply_sanitizers(morph_soak ${AF_SANITIZER})
endif()

include(Catch)
catch_discover_tests(morph_soak DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 300 LABELS "soak")
