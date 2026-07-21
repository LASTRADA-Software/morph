# Opt-in throughput/latency benchmark against a trivial echo model (measures
# framework dispatch overhead, not business logic). Built only under
# -DMORPH_BUILD_LOAD_TESTS=ON; never part of the default `morph_tests` target
# or the fast ctest sweep. See docs/spec/testing_strategy.md.
add_executable(morph_bench bench_dispatch_latency.cpp)
target_link_libraries(morph_bench PRIVATE morph::morph Catch2::Catch2WithMain)
target_include_directories(morph_bench PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_compile_definitions(morph_bench PRIVATE BENCH_ARTIFACT_DIR="${CMAKE_CURRENT_BINARY_DIR}")
apply_warnings(morph_bench)

include(Catch)
catch_discover_tests(morph_bench DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 60 LABELS "bench")
