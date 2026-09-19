# Opt-in throughput/latency benchmark against a trivial echo model (measures
# framework dispatch overhead, not business logic). Built only under
# -DMORPH_BUILD_LOAD_TESTS=ON; never part of the default `morph_tests` target
# or the fast ctest sweep. See docs/spec/testing_strategy.md.
add_executable(morph_bench bench_dispatch_latency.cpp)
target_link_libraries(morph_bench PRIVATE morph::morph morph_test_main)
target_include_directories(morph_bench PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_compile_definitions(morph_bench PRIVATE BENCH_ARTIFACT_DIR="${CMAKE_CURRENT_BINARY_DIR}")
apply_warnings(morph_bench)

# Instrumented under the same deliberate decision as tests/soak (morph#542).
# The numbers a sanitized benchmark reports are not comparable with an
# unsanitized one and are not meant to be: on a sanitizer preset this target is
# a correctness run over the dispatch path, and the throughput figures it
# publishes come from the ordinary presets.
if(DEFINED AF_SANITIZER)
    apply_sanitizers(morph_bench ${AF_SANITIZER})
endif()

include(Catch)
catch_discover_tests(morph_bench DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 60 LABELS "bench")
