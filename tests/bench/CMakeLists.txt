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

# Allocation census for one local dispatch (morph#572). A binary of its own,
# not a case in morph_bench, because it replaces the global operator new: the
# replacement is process-wide and would perturb every other measurement in the
# same binary. Deliberately **not** registered with ctest -- it asserts nothing
# by default (an allocation count is toolchain-specific, so no ceiling is right
# everywhere), and a test that cannot fail is noise in a suite. Run it by hand:
#   ./build/tests/bench/morph_bench_alloc [--attribute]
# See the file's own header comment for the method, and
# docs/spec/testing_strategy.md.
add_executable(morph_bench_alloc bench_dispatch_allocations.cpp)
target_link_libraries(morph_bench_alloc PRIVATE morph::morph)
apply_warnings(morph_bench_alloc)
# -Wmismatched-new-delete pairs a `new` the compiler inlined with the
# `std::free` inside the *replaced* `operator delete` and calls it a mismatch.
# It is not one: replacing the global allocation functions is this program's
# entire purpose, and forwarding them to malloc/free is how such a replacement
# is written. Scoped to this one target rather than relaxed anywhere else.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(morph_bench_alloc PRIVATE -Wno-mismatched-new-delete)
endif()
if(DEFINED AF_SANITIZER)
    apply_sanitizers(morph_bench_alloc ${AF_SANITIZER})
endif()
