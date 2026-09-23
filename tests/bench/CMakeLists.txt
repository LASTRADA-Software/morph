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
# same binary. Run it by hand for the numbers and the per-allocation breakdown:
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

# ── The allocation budget, as a ctest gate ───────────────────────────────────
#
# morph#572 asks for "a benchmark that *fails* when the budget regresses", and
# names why: `tests/bench/bench_dispatch_latency.json` is produced and never
# diffed, which is the "control that measures nothing" failure AGENTS.md warns
# about. A census nobody compares is the same thing with extra steps, so the
# census is compared here.
#
# **The ceiling is a per-toolchain figure, and that is why it is a cache
# variable rather than a constant.** An allocation count depends on the
# standard library's small-object thresholds, not only on morph's code: a
# number that is right on libstdc++ can be wrong on MSVC's STL, where
# `std::function`'s inline buffer is larger. So the default below is the
# figure measured on the toolchains that actually run this target in CI, and
# anything else can set its own. Setting it to 0 disables the gate.
#
# Re-measured on this tree with morph#572's Part A, Part B and Part C all
# applied -- Part B landed in morph#743 and took the figure from 14.06 to 8.06,
# which is why the ceiling below is 9.0 and no longer 15.0. 20 processes per
# configuration, plus one run of 24 concurrent processes on twelve cores:
#
#   clang 22.1.8 / libstdc++ 16.2.1, Release, idle       : 8.06, 20 of 20
#   clang 22.1.8 / libstdc++ 16.2.1, Debug,   idle       : 8.06, 20 of 20
#   clang 22.1.8 / libstdc++ 16.2.1, Debug,   24-way     : 8.06, 24 of 24
#   gcc 16.2.1   / libstdc++ 16.2.1, Debug,   idle       : 8.06, 20 of 20
#
# The CI legs that build this target are Linux `clang-debug` and `gcc-debug`
# (.github/workflows/ci.yml, the "every optional feature" matrix), so the Debug
# rows are the ones the default has to hold for.
#
# **There is no spread to leave headroom for, and that is recent.** Before the
# worker gate this file's benchmark now uses, the figure was bimodal -- ~17 on
# an idle machine and ~13 on a loaded one, for the same binary -- because
# whether the caller beat the pool thread to attaching its handlers decided
# which `CompletionState` path they took. That is morph#687, and a ceiling
# under it would have been either flaky or blind: the pre-fix loaded mode
# (13.06) sits *below* the post-fix idle mode (14.06), so no single number
# could have separated a regression from a busy runner. With the race pinned
# the figure is exact, so 9.0 is not a tolerance band -- it is "one more
# allocation per call than we spend now", which is the smallest regression
# worth a red build.
#
# **Why this number was moved rather than left alone.** 15.0 was set against a
# 14.06 figure; once Part B took the figure to 8.06 the same constant tolerated
# a complete regression back to the pre-Part-B path without a word, which is a
# gate holding a line nobody is standing on any more. Whoever next reduces the
# figure should move this constant in the same commit, for the same reason.
#
# **Shown firing, not assumed to.** Adding a single `new char[8]` to
# `Bridge::executeVia`'s steady-state path (clang-debug,
# MORPH_BUILD_LOAD_TESTS=ON) takes the census to 9.06 and turns
# `bench.alloc_budget` red -- ctest exit 8, and from the case's own output:
#
#   heap allocations total     : 1812 (9.06 per call)
#   FAIL: 9.06 allocations per call exceeds --budget=9.00
#
# With the injection removed the same ctest case passes at 8.06. One extra
# allocation per call is therefore the resolution of this gate, which is what
# "one more allocation than we spend now" is meant to mean -- and note that the
# same 9.06 passed silently under the 15.0 this replaces.
#
# MSVC's STL is excluded by default rather than guessed at: nothing in CI
# measures it, and a ceiling nobody has measured is exactly the kind of control
# this gate exists to be the opposite of.
if(MSVC)
    set(_morph_alloc_budget_default 0)
else()
    set(_morph_alloc_budget_default 9.0)
endif()
set(MORPH_ALLOC_BUDGET_PER_CALL "${_morph_alloc_budget_default}" CACHE STRING
    "Ceiling, in heap allocations per local execute round trip, that morph_bench_alloc enforces as a ctest case. 0 disables the gate. See tests/bench/CMakeLists.txt for how the default was measured.")

if(NOT MORPH_ALLOC_BUDGET_PER_CALL EQUAL 0)
    # --lookup-budget=0 is exact, not a ceiling with headroom: morph#572's
    # Part C made `ActionDispatcher`'s key lookups allocate nothing at all, and
    # morph#699 did the same for `journal::PayloadMigrationRegistry::find`, for
    # ids of any length. "Nothing at all" is a property that either holds or
    # has regressed. There is no toolchain on which a transparent lookup
    # allocates, so this half needs no per-toolchain default.
    #
    # --id-length-budget covers morph#699's third registry,
    # `ActionExecuteRegistry`, which cannot be probed on its own -- it
    # dispatches whatever it finds -- so what is gated is the gap between an
    # `executeJson` over ids past libstdc++'s SSO buffer and one over ids
    # inside it. 0.5 rather than 0 is measured, not padded: the figure reads
    # 0.01 because of a deterministic two-allocation one-off that follows
    # census order rather than id length, and reverting either half of
    # morph#699 takes it to 3.00. The binary's own comment at the check
    # carries the measurement. Like the lookup half it is toolchain-independent
    # -- it is a difference between two runs of the same build.
    add_test(NAME bench.alloc_budget
             COMMAND morph_bench_alloc "--budget=${MORPH_ALLOC_BUDGET_PER_CALL}" "--lookup-budget=0"
                     "--id-length-budget=0.5")
    set_tests_properties(bench.alloc_budget PROPERTIES TIMEOUT 120 LABELS "bench")
endif()
