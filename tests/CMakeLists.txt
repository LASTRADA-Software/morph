# ── The SYSTEM-include demotion helper's own self-test ──────────────────────
# cmake/morph_demote_interface_includes.cmake moves unixODBC's `-I` out of the
# fetched Lightweight target's INTERFACE_COMPILE_OPTIONS. On Linux that flag is
# empty, so nothing observable here would change if the helper's body were
# deleted -- the self-test hands it a synthetic fixture instead and
# FATAL_ERRORs unless the transform is exactly right. Configure-time, no build,
# and it runs wherever tests are configured rather than only on the platform
# that has the symptom.
include(${CMAKE_SOURCE_DIR}/cmake/morph_demote_interface_includes.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/compile_checks/demote_interface_includes_selftest.cmake)

include(${CMAKE_SOURCE_DIR}/cmake/pinned_facts.cmake)
morph_generate_pinned_facts_header(
    ${CMAKE_SOURCE_DIR}/docs/spec/pinned_facts.toml
    ${CMAKE_BINARY_DIR}/generated/pinned_facts_generated.hpp
)

add_executable(morph_tests
    oom_injector.cpp
    test_oom_injector.cpp
    test_executor.cpp
    test_example.cpp
    test_executor_extra.cpp
    test_strand.cpp
    test_strand_extra.cpp
    test_strand_race.cpp
    test_completion.cpp
    test_completion_extra.cpp
    test_completion_multi_handler.cpp
    test_completion_promise.cpp
    test_completion_value_contract.cpp
    test_coroutine_client.cpp
    test_coroutine_model.cpp
    test_async_delay.cpp
    test_model.cpp
    test_logger.cpp
    test_observability.cpp
    test_backend_extra.cpp
    test_backend_registration_surface.cpp
    test_registry_extra.cpp
    test_registration_phase.cpp
    test_registry_schema_forgery.cpp
    test_registration_qualified_types.cpp
    test_registration_same_line.cpp
    test_principal.cpp
    test_bridge_local.cpp
    test_bridge_remote.cpp
    test_bridge_pending_calls.cpp
    test_bridge_execute_json.cpp
    test_bridge_pending_calls.cpp
    test_remote_extra.cpp
    test_remote_connection_scope.cpp
    test_remote_step_interleaving.cpp
    test_remote_execute_ordering.cpp
    test_execute_order_gate.cpp
    test_action_validation.cpp
    test_action_wire_rejection.cpp
    test_security_fixes.cpp
    test_bridge_lifetime.cpp
    test_client_execute_deadline.cpp
    test_timeout_scheduler.cpp
    test_dispatch_di.cpp
    test_handler_binding.cpp
    test_switch_backend.cpp
    test_async_registration.cpp
    test_network_monitor.cpp
    test_forms_boolean_anyof_wire.cpp
    test_forms_dom_access.cpp
    test_forms_exact_bounds.cpp
    test_offline_queue.cpp
    test_file_offline_queue.cpp
    test_replay_ledger.cpp
    test_sync_worker.cpp
    test_reconnect_coordinator.cpp
    test_offline_integration.cpp
    test_conflict_resolution.cpp
    test_bridge_fixes.cpp
    test_completion_branches.cpp
    test_issues.cpp
    test_concurrency_invariants.cpp
    test_subscription.cpp
    test_coverage_gaps.cpp
    test_coverage_extra.cpp
    test_coverage_push95.cpp
    test_server_limits.cpp
    test_shared_instances.cpp
    test_strong_id_keys.cpp
    test_wire_encode_fault.cpp
    test_wire_hardening.cpp
    test_wire_omitted_fields.cpp
    test_remote_reply_envelopes.cpp
    test_reply_router.cpp
    test_wire_schemas.cpp
    test_protocol_version.cpp
    test_limit_policy.cpp
    test_action_log.cpp
    test_action_log_phase2.cpp
    test_journal_format_versioning.cpp
    test_journal_payload_evolution.cpp
    test_outbox.cpp
    test_rational_checked.cpp
    test_rational_wire_policy.cpp
    test_rational.cpp
    test_ledger_rational_fuzz.cpp
    test_quantity.cpp
    test_tagged.cpp
    test_quantity_forms.cpp
    test_quantity_decode_validation.cpp
    test_nested_forms.cpp
    test_flows_apps.cpp
    test_sections.cpp
    test_views.cpp
    test_computed_fields.cpp
    test_forms_rules.cpp
    test_forms_rule_agreement.cpp
    test_forms_rule_corpus.cpp
    test_forms_layout.cpp
    test_forms_field_bounds.cpp
    test_forms_display_unit.cpp
    test_forms_blank_as.cpp
    test_forms_instance_constraints.cpp
    test_widget_hints.cpp
    test_datetime.cpp
    test_forms_i18n.cpp
    test_render_i18n.cpp
    test_render_locale_format.cpp
    test_forms_conformance_corpus.cpp
    test_session_auth.cpp
    test_policy_hardening.cpp
    test_register_authorization.cpp
    test_register_session.cpp
    test_control_byte_escaping.cpp
    test_opaque_model_ids.cpp
    test_graceful_shutdown.cpp
    test_pinned_facts.cpp
    test_version.cpp
    test_callback_scope.cpp
    test_file_io_ops.cpp
    test_extension_bag_spike.cpp
)

target_link_libraries(morph_tests
    PRIVATE
        morph::morph
        morph_test_main
)

apply_warnings(morph_tests)

# Several individual test files (schema/rule template instantiation over many
# action/form types, e.g. test_quantity_forms.cpp) have independently pushed
# their .obj's COFF section count past the 32-bit SN_LOFF format's limit
# under MSVC Debug (no /Og folding, full /Zi debug info), aborting with C1128
# ("number of sections exceeded object file format limit") -- three separate
# unrelated branches hit this on that one file alone, each adding only a
# little more template-instantiation weight. Rather than chase this file by
# file as it recurs, /bigobj is applied to the whole target: harmless on
# Release and on other compilers, and it costs nothing on the files that
# don't need it.
target_compile_options(morph_tests PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/bigobj>
)

# Forwards CMakeLists.txt's project(VERSION ...) into tests/test_version.cpp
# so it can static_assert the checked-in include/morph/version.hpp constants
# never drift from the build system's declared version.
target_compile_definitions(morph_tests PRIVATE
    MORPH_CMAKE_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    MORPH_CMAKE_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    MORPH_CMAKE_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

# The shared x-rules corpus. One file, two readers: this suite and
# src/qt/forms/tests/tst_DynamicFormRuleCorpus.qml, both pointed at it from
# CMake so neither can quietly grow its own copy. It lives beside the QML half
# because that half cannot be handed a path at runtime; this half can.
target_compile_definitions(morph_tests PRIVATE
    MORPH_FORMS_RULE_CORPUS="${CMAKE_CURRENT_SOURCE_DIR}/../src/qt/forms/tests/data/rule_corpus.json"
    MORPH_FORMS_INSTANCE_BOUNDS="${CMAKE_CURRENT_SOURCE_DIR}/../src/qt/forms/tests/data/instance_bounds.json")

target_include_directories(morph_tests PRIVATE ${CMAKE_BINARY_DIR}/generated)

# MORPH_REQUIRE_VETTED_HMAC is a *deployment* switch aimed at application call
# sites; it rides in on morph::morph's INTERFACE compile definitions. morph's
# own suite deliberately exercises the reference hmacSha256 through its default
# argument (test_session_auth.cpp, test_security_fixes.cpp,
# test_policy_hardening.cpp, test_register_authorization.cpp), so the inherited
# definition is undefined again for this target only. Without this, configuring
# the documented `-DMORPH_REQUIRE_VETTED_HMAC=ON` would fail to build unless
# tests were also switched off — which would leave the option itself untested.
# The guard's own correctness is proven independently by the three try_compile
# probes below, which set/omit the macro per probe regardless of the option.
#
# COMPILE_OPTIONS rather than COMPILE_DEFINITIONS: CMake's compile rule expands
# <DEFINES> before <FLAGS>, so a `-U` in the flags reliably wins over the `-D`
# inherited from the interface target.
if(MORPH_REQUIRE_VETTED_HMAC)
    target_compile_options(morph_tests PRIVATE
        "$<IF:$<CXX_COMPILER_ID:MSVC>,/UMORPH_REQUIRE_VETTED_HMAC,-UMORPH_REQUIRE_VETTED_HMAC>")
endif()

# /w14062 (enabled project-wide) only fires when an enum switch has no
# `default`; test_pinned_facts.cpp's exhaustive enum-cardinality switches keep
# a `default:` (so GCC/Clang's -Wswitch-default does not fire), which would
# otherwise exempt them from MSVC's check. /w14061 fires even with a default
# present. Scoped to this one file rather than project-wide -- see the plan's
# Global Constraints for why.
set_source_files_properties(test_pinned_facts.cpp PROPERTIES
    COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:MSVC>:/w14061>"
)

if(DEFINED AF_SANITIZER)
    apply_sanitizers(morph_tests ${AF_SANITIZER})
endif()
if(AF_COVERAGE)
    apply_coverage(morph_tests)
endif()

include(Catch)
# Every test is sub-second in practice; cap each at 120s so a hung/deadlocked
# test fails fast instead of stalling the whole CI job (observed: a single test
# hanging blocked a Windows runner for over half an hour).
#
# That number was never chosen with any particular test in mind, which is
# fine while every test is sub-second and wrong for the one that is not: the
# longest, most load-sensitive case in the binary was governed by the same
# ceiling as the cheapest. So the blanket stays and `[slow]` is
# registered separately -- DISCOVERY_MODE
# PRE_TEST defers discovery to ctest invocation time, so a per-test TIMEOUT
# cannot be set with set_tests_properties() here (nothing is named that test
# yet at configure time); excluding a tag and giving it its own
# catch_discover_tests() call is the mechanism that is actually available.
#
# (The `equation()` depth test no longer needs the tag: at 70,000 nodes it blew
# past 120s under TSan because of O(n^2) string building, and with that
# rendering linear it is back under the blanket cap. The tag is reused
# here for a different test and a different reason.)
catch_discover_tests(morph_tests DISCOVERY_MODE PRE_TEST TEST_SPEC "~[slow]" PROPERTIES TIMEOUT 120)

# `[slow]`: `StrandExecutor never runs two tasks for one key
# concurrently under contention`. Not slow because it computes anything --
# 0.14 s on an idle box -- but because the strand serialises 3200 tasks per
# iteration and every handoff is a thread wakeup that has to wait its turn on
# the run queue. That cost is set by how contended the host is, and it climbs
# steeply. Measured on this tree, 12 cores, clang 22.1.8 Release, load made
# with plain spin loops, whole-case wall clock, one run each:
#
#     run queue  1 (idle)  ->    0.14 s
#     run queue 14         ->   23.8 s
#     run queue 27         ->  170.5 s
#     run queue 38         ->  396.4 s   <- load average 31-38 at measurement
#
# Every one of those runs *passed*: 40 assertions, `inFlight 1, maxInFlight 1`
# throughout. What this tag answers is the case being killed while passing, at
# load average 29-48 -- above the 38 measured here, and the curve is
# superlinear, so the fitted figure at a run queue of ~50 is roughly 730 s.
#
# 900 s is therefore ~2.3x the measured 396 s and ~1.2x that extrapolation. It
# is also the number this file already uses for its other load-sensitive case
# (`forms_schema_generation_is_not_route_count_sensitive`, below), chosen there
# for the same reason: a slow shared runner can be several times a workstation.
#
# What this deliberately does **not** do is reduce `kIterations`. Twenty
# iterations is this case's detection power for a rare interleaving; trading it
# away to fit a ceiling would make the case cheaper and worse at the only thing
# it exists for. See the comment on `kIterations` in test_strand_race.cpp.
#
# And what it does not buy is immunity: a host oversubscribed past roughly 4.5x
# will exceed 900 s too. The ceiling is here to catch a *deadlock* -- which is
# unbounded, not merely large -- and it still does, in 15 minutes instead of 2,
# for one test out of 3044.
catch_discover_tests(morph_tests DISCOVERY_MODE PRE_TEST TEST_SPEC "[slow]" PROPERTIES TIMEOUT 900)

# ── Two-binary journal-path skew test ───────────────────────────────────────
# The executable form of the journal's data-at-rest contract: an
# old build records a journal, a new build replays it, and a shape that changed
# in between must not decode to a plausible default in silence.
#
# tests/test_journal_payload_evolution.cpp already simulates an older build
# inside one process, via a struct it never registers. That proves the gate
# fires; it cannot prove anything about two separately compiled programs, since
# there is only ever one compilation. compile_checks/journal_skew_probe.cpp is
# compiled twice, into two executables that share no translation unit, and the
# "new" one asserts against a journal file the "old" one actually wrote.
#
# NOTE (deviation from the issue's suggestion): two real targets plus ordered
# ctest cases, not the try_compile()/try_run() pattern the other
# MORPH_CLIENT_ONLY gates use. Those gates need try_compile because they assert
# a *link failure*, and because they must define a macro the main build does
# not. Neither applies here: two ordinary executables differing only in a
# -D is exactly what add_executable()/target_compile_definitions() are for, and
# building them at build time lets ninja parallelise them instead of adding two
# serial multi-minute compiles to every configure.
#
# NOTE (scope): neither binary is built with MORPH_CLIENT_ONLY. That macro
# suppresses the model-owning registrars, so a client-only binary cannot
# execute a model and therefore cannot journal anything. It is the right gate
# for the *wire*-path skew test, which needs a per-action fingerprint exchanged
# at `hello` before there is anything to assert against, and no such exchange
# exists. This covers the journal path, which ships today.
set(MORPH_JOURNAL_SKEW_DIR "${CMAKE_CURRENT_BINARY_DIR}/journal_skew")
file(MAKE_DIRECTORY "${MORPH_JOURNAL_SKEW_DIR}")

foreach(_skew_role old new)
    string(TOUPPER "${_skew_role}" _skew_role_upper)
    add_executable(morph_journal_skew_${_skew_role} compile_checks/journal_skew_probe.cpp)
    target_compile_definitions(morph_journal_skew_${_skew_role}
        PRIVATE MORPH_SKEW_ROLE_${_skew_role_upper})
    target_link_libraries(morph_journal_skew_${_skew_role} PRIVATE morph::morph)
    morph_suppress_test_dialogs(morph_journal_skew_${_skew_role})
    apply_warnings(morph_journal_skew_${_skew_role})
    if(DEFINED AF_SANITIZER)
        apply_sanitizers(morph_journal_skew_${_skew_role} ${AF_SANITIZER})
    endif()
endforeach()
unset(_skew_role)
unset(_skew_role_upper)

# FIXTURES_SETUP/FIXTURES_REQUIRED rather than a bare ordering: ctest then
# refuses to run the replay case at all if the writer failed, instead of
# reporting a second, confusing failure about a journal file that was never
# produced. Both are also serialised against each other by the fixture, which
# matters under `ctest -j`: they share one journal file.
add_test(NAME journal_skew_old_build_writes
    COMMAND morph_journal_skew_old "${MORPH_JOURNAL_SKEW_DIR}")
set_tests_properties(journal_skew_old_build_writes PROPERTIES
    FIXTURES_SETUP morph_journal_skew_fixture
    TIMEOUT 120)

add_test(NAME journal_skew_new_build_replays
    COMMAND morph_journal_skew_new "${MORPH_JOURNAL_SKEW_DIR}")
set_tests_properties(journal_skew_new_build_replays PROPERTIES
    FIXTURES_REQUIRED morph_journal_skew_fixture
    TIMEOUT 120)

# ── forms: schema generation must not be route-count sensitive ──────────────
# compile_checks/forms_dag_probe.cpp is compiled
# twice — once as a type graph with one route to each node, once as a DAG with
# 6,561 routes through the same number of types at the same depth — and
# compile_checks/forms_dag_budget.cmake asserts the second costs no more than
# MAX_RATIO_PERCENT of the first. Both files' headers say why it is a ratio and
# what was measured; the short version is that this fixture took 26.8 CPU-s at
# master @ a9cb5649 and 3.0 with the fix, against a 2.7-second control that the
# fix leaves alone.
#
# A ctest case rather than a configure-time try_compile: it costs ~12 seconds of
# compiling, which does not belong in every configure, and unlike the
# MORPH_CLIENT_ONLY gates it asserts nothing about whether a program builds —
# only about how long it takes, which is a thing to measure when someone asks
# for test results, not when they ask for a build system.
#
# Gated on a GCC/Clang driver: the script passes `-I`, `-std=` and
# `-fsyntax-only` literally, which clang-cl and MSVC spell differently. The
# pathology is compiler-independent (it is an instantiation count), so one
# front end proving it is enough; nothing here is skipped silently, because a
# skipped case reports as "Not Run" rather than as a pass.
if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$" AND NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    # Genex-free include list: this runs under `cmake -P`, where nothing expands
    # a $<BUILD_INTERFACE:...>. morph's own include directory is named directly
    # rather than unwrapped from the target, so a change to how that target is
    # declared cannot quietly leave this pointing at an installed copy.
    set(MORPH_FORMS_DAG_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/include")
    get_target_property(_morph_forms_dag_glaze_includes glaze::glaze INTERFACE_INCLUDE_DIRECTORIES)
    foreach(_dir IN LISTS _morph_forms_dag_glaze_includes)
        string(REGEX REPLACE "^\\$<BUILD_INTERFACE:(.*)>$" "\\1" _dir "${_dir}")
        if(NOT _dir MATCHES "\\$<")
            list(APPEND MORPH_FORMS_DAG_INCLUDE_DIRS "${_dir}")
        endif()
    endforeach()
    unset(_dir)
    unset(_morph_forms_dag_glaze_includes)
    # Pipe-joined: a `;` inside one add_test() argument is an argument separator.
    string(REPLACE ";" "|" MORPH_FORMS_DAG_INCLUDE_DIRS "${MORPH_FORMS_DAG_INCLUDE_DIRS}")

    add_test(NAME forms_schema_generation_is_not_route_count_sensitive
        COMMAND ${CMAKE_COMMAND}
            "-DMORPH_DAG_CXX=${CMAKE_CXX_COMPILER}"
            "-DMORPH_DAG_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/compile_checks/forms_dag_probe.cpp"
            "-DMORPH_DAG_INCLUDES=${MORPH_FORMS_DAG_INCLUDE_DIRS}"
            "-DMORPH_DAG_FLAGS=-std=c++23|-fsyntax-only"
            "-DMORPH_DAG_MAX_RATIO_PERCENT=300"
            "-DMORPH_DAG_RUNS=2"
            "-DMORPH_DAG_MIN_CONTROL_MS=250"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/compile_checks/forms_dag_budget.cmake")
    # Generous enough that a regression reports the ratio it measured rather
    # than dying as a timeout: the unfixed fixture took ~27 s per run here, and
    # a slow shared runner can be several times that.
    set_tests_properties(forms_schema_generation_is_not_route_count_sensitive PROPERTIES
        TIMEOUT 900
        RUN_SERIAL TRUE)
endif()

# ── MORPH_REQUIRE_VETTED_HMAC guard check ────────────────────────────────────
# Configure-time proof that the default-argument-removal mechanism in
# session_auth.hpp (TokenIssuer/TokenVerifier/SigningAuthorizer) actually
# gates on MORPH_REQUIRE_VETTED_HMAC, independent of whatever value the
# top-level MORPH_REQUIRE_VETTED_HMAC option currently has — each try_compile
# defines (or omits) the macro just for that one probe, via COMPILE_DEFINITIONS.
# See docs/spec/security.md, "MAC-primitive recommended wiring".
#
# unset(... CACHE) first: try_compile()'s result variable is stored as a cache
# entry, so without clearing it a stale PASS/FAIL from an earlier configure
# would survive unchanged even if session_auth.hpp regresses.
#
# NOTE (deviation from the plan): try_compile()'s LINK_LIBRARIES only accepts
# system libraries and *Imported* targets from the calling project (see the
# try_compile() docs) — `morph` is a plain (non-imported) INTERFACE library
# defined in this same project, so `LINK_LIBRARIES morph::morph` fails with
# "the target was not found" (confirmed experimentally). Since morph and glaze
# are both header-only for this probe's purposes, we instead read their
# INTERFACE_INCLUDE_DIRECTORIES via get_target_property() — which works for
# any target, imported or not, because it queries the calling project's own
# configure state rather than the try_compile scratch project — and forward
# them as an old-style `-DINCLUDE_DIRECTORIES=...` CMAKE_FLAGS entry (the
# generated scratch project turns that into `include_directories(...)`, per
# the try_compile() docs). Semicolons inside the combined list must be
# escaped so the outer CMAKE_FLAGS list isn't split apart.
get_target_property(MORPH_VETTED_HMAC_GUARD_INCLUDE_DIRS morph INTERFACE_INCLUDE_DIRECTORIES)
get_target_property(MORPH_VETTED_HMAC_GUARD_GLAZE_INCLUDE_DIRS glaze::glaze INTERFACE_INCLUDE_DIRECTORIES)
list(APPEND MORPH_VETTED_HMAC_GUARD_INCLUDE_DIRS ${MORPH_VETTED_HMAC_GUARD_GLAZE_INCLUDE_DIRS})
# core-cpp's headers, spelled out rather than read from core::base: its
# include directories are $<BUILD_INTERFACE:...> generator expressions, which
# a try_compile() here would receive unevaluated. `src/` holds the headers and
# the binary directory's `include/` the generated <core/Config.hpp>.
list(APPEND MORPH_VETTED_HMAC_GUARD_INCLUDE_DIRS "${core-cpp_SOURCE_DIR}/src" "${core-cpp_BINARY_DIR}/include")
string(REPLACE ";" "\\;" MORPH_VETTED_HMAC_GUARD_INCLUDE_DIRS "${MORPH_VETTED_HMAC_GUARD_INCLUDE_DIRS}")

unset(MORPH_VETTED_HMAC_GUARD_BLOCKS_DEFAULT CACHE)
try_compile(MORPH_VETTED_HMAC_GUARD_BLOCKS_DEFAULT
    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/compile_checks/vetted_hmac_guard_uses_default.cpp"
    CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${MORPH_VETTED_HMAC_GUARD_INCLUDE_DIRS}"
    CXX_STANDARD 23
    COMPILE_DEFINITIONS "-DMORPH_REQUIRE_VETTED_HMAC"
)
if(MORPH_VETTED_HMAC_GUARD_BLOCKS_DEFAULT)
    message(FATAL_ERROR
        "MORPH_REQUIRE_VETTED_HMAC guard check failed: "
        "compile_checks/vetted_hmac_guard_uses_default.cpp compiled even with "
        "MORPH_REQUIRE_VETTED_HMAC defined (expected a compile failure — the "
        "default-argument-removal #ifdef in session_auth.hpp is broken).")
endif()

unset(MORPH_VETTED_HMAC_GUARD_ALLOWS_EXPLICIT CACHE)
try_compile(MORPH_VETTED_HMAC_GUARD_ALLOWS_EXPLICIT
    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/compile_checks/vetted_hmac_guard_explicit_mac.cpp"
    CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${MORPH_VETTED_HMAC_GUARD_INCLUDE_DIRS}"
    CXX_STANDARD 23
    COMPILE_DEFINITIONS "-DMORPH_REQUIRE_VETTED_HMAC"
)
if(NOT MORPH_VETTED_HMAC_GUARD_ALLOWS_EXPLICIT)
    message(FATAL_ERROR
        "MORPH_REQUIRE_VETTED_HMAC guard check failed: "
        "compile_checks/vetted_hmac_guard_explicit_mac.cpp failed to compile "
        "with MORPH_REQUIRE_VETTED_HMAC defined, even though it passes an "
        "explicit MacFunction (the guard must not block explicit injection).")
endif()

unset(MORPH_VETTED_HMAC_GUARD_DEFAULT_WORKS_UNGATED CACHE)
try_compile(MORPH_VETTED_HMAC_GUARD_DEFAULT_WORKS_UNGATED
    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/compile_checks/vetted_hmac_guard_uses_default.cpp"
    CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${MORPH_VETTED_HMAC_GUARD_INCLUDE_DIRS}"
    CXX_STANDARD 23
)
if(NOT MORPH_VETTED_HMAC_GUARD_DEFAULT_WORKS_UNGATED)
    message(FATAL_ERROR
        "MORPH_REQUIRE_VETTED_HMAC guard check failed: "
        "compile_checks/vetted_hmac_guard_uses_default.cpp failed to compile "
        "without MORPH_REQUIRE_VETTED_HMAC defined (regression: the default "
        "hmacSha256 argument must keep working when the option is off).")
endif()

# ── MORPH_CLIENT_ONLY guard check ────────────────────────────────────────────
# Proof that MORPH_CLIENT_ONLY actually suppresses the two model-owning
# registrars (registerModelOnce, registerActionOnce) -- see
# docs/spec/core/registry.md, "MORPH_CLIENT_ONLY".
#
# compile_checks/client_only_no_model_link.cpp declares (never defines)
# ClientOnlyModel's constructor and execute(). Two probes, in opposite
# directions, prove the guard does something real rather than the probe being
# vacuously satisfied either way:
#   - WITH MORPH_CLIENT_ONLY defined: must LINK (the model-owning registrars
#     are macroed away, so nothing in the program ever references either
#     undefined symbol).
#   - WITHOUT it (today's default): must FAIL TO LINK, and must fail on
#     ClientOnlyModel's symbols (the registrars' lambda bodies do reference
#     them, and the linker cannot resolve either).
#
# The probes that must link, and the two that must also run, are ordinary
# build-time targets rather than try_compile()/try_run(): morph's headers use
# core-cpp's compiled modules (core::net's event loop, through
# TimeoutScheduler), and a try_compile() scratch project can link only
# imported targets -- core-cpp's are built by this project, after configure.
# A probe that fails to link now fails the build rather than the configure;
# the run probes are ctest cases. The probes link morph's dependencies
# directly rather than morph::morph, whose interface carries MORPH_CLIENT_ONLY
# when that option is on, so each probe decides the macro for itself.
add_library(morph_guard_probe_deps INTERFACE)
target_include_directories(morph_guard_probe_deps INTERFACE "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(morph_guard_probe_deps INTERFACE
    glaze::glaze core::base core::async core::net core::platform
    $<TARGET_NAME_IF_EXISTS:Threads::Threads>)

add_executable(morph_client_only_guard_links compile_checks/client_only_no_model_link.cpp)
target_compile_definitions(morph_client_only_guard_links PRIVATE MORPH_CLIENT_ONLY)
target_link_libraries(morph_client_only_guard_links PRIVATE morph_guard_probe_deps)
morph_suppress_test_dialogs(morph_client_only_guard_links)
add_test(NAME morph_client_only_guard_links COMMAND morph_client_only_guard_links)
set_tests_properties(morph_client_only_guard_links PROPERTIES TIMEOUT 60)

# The negative probe stays a configure-time try_compile(): the link failure is
# the assertion. Its scratch project cannot link core-cpp either, so a failure
# alone would prove nothing; the check is that the linker named
# ClientOnlyModel -- the symbols the default registrars reference.
unset(MORPH_CLIENT_ONLY_GUARD_DEFAULT_NEEDS_LINKAGE CACHE)
try_compile(MORPH_CLIENT_ONLY_GUARD_DEFAULT_NEEDS_LINKAGE
    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/compile_checks/client_only_no_model_link.cpp"
    CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${MORPH_VETTED_HMAC_GUARD_INCLUDE_DIRS}"
    CXX_STANDARD 23
    OUTPUT_VARIABLE MORPH_CLIENT_ONLY_GUARD_DEFAULT_NEEDS_LINKAGE_OUTPUT
)
# GNU ld: "undefined reference to `ClientOnlyModel::...", lld: "undefined
# symbol: ClientOnlyModel::...", MSVC: "unresolved external symbol ...
# ClientOnlyModel...", ld64: "ClientOnlyModel::..., referenced from".
set(_morph_client_only_unresolved
    "(undefined reference to|undefined symbol:|unresolved external symbol)[^\n]*ClientOnlyModel|ClientOnlyModel[^\n]*referenced from")
if(MORPH_CLIENT_ONLY_GUARD_DEFAULT_NEEDS_LINKAGE)
    message(FATAL_ERROR
        "MORPH_CLIENT_ONLY guard check failed: "
        "compile_checks/client_only_no_model_link.cpp linked successfully "
        "WITHOUT MORPH_CLIENT_ONLY defined (expected a link failure -- the "
        "probe's undefined constructor/execute() should be referenced by "
        "the default (non-client-only) registrars; if it links anyway, this "
        "probe is not actually proving anything about the guard).\n"
        "--- compiler/linker output ---\n"
        "${MORPH_CLIENT_ONLY_GUARD_DEFAULT_NEEDS_LINKAGE_OUTPUT}")
elseif(NOT MORPH_CLIENT_ONLY_GUARD_DEFAULT_NEEDS_LINKAGE_OUTPUT MATCHES "${_morph_client_only_unresolved}")
    message(FATAL_ERROR
        "MORPH_CLIENT_ONLY guard check failed: "
        "compile_checks/client_only_no_model_link.cpp failed to link WITHOUT "
        "MORPH_CLIENT_ONLY defined, as it must -- but the linker did not name "
        "ClientOnlyModel, so it failed for some other reason and proves "
        "nothing about the guard.\n"
        "--- compiler/linker output ---\n"
        "${MORPH_CLIENT_ONLY_GUARD_DEFAULT_NEEDS_LINKAGE_OUTPUT}")
endif()
unset(_morph_client_only_unresolved)

# The link probe above only proves the *registration-suppression* half of the
# guard (static-init-time link resolution). The other half -- that
# Bridge::executeVia's localOp actually throws std::logic_error at *runtime*
# under MORPH_CLIENT_ONLY, instead of silently calling Model::execute -- needs
# the probe to run: compile_checks/client_only_runtime_throw.cpp exits 0 when
# the expected std::logic_error was caught.
add_executable(morph_client_only_runtime_throw compile_checks/client_only_runtime_throw.cpp)
target_compile_definitions(morph_client_only_runtime_throw PRIVATE MORPH_CLIENT_ONLY)
target_link_libraries(morph_client_only_runtime_throw PRIVATE morph_guard_probe_deps)
morph_suppress_test_dialogs(morph_client_only_runtime_throw)
add_test(NAME morph_client_only_runtime_throw COMMAND morph_client_only_runtime_throw)
set_tests_properties(morph_client_only_runtime_throw PROPERTIES TIMEOUT 60)

# BRIDGE_REGISTER_ACTION_FOR_CLIENT lets a MORPH_CLIENT_ONLY client
# register an action's ActionTraits (JSON codecs + an explicitly-named Result
# type) without ever needing the real model's complete class body -- see
# docs/spec/core/registry.md, "BRIDGE_REGISTER_ACTION_FOR_CLIENT".
# compile_checks/client_only_facade_no_model_header.cpp forward-declares (never
# defines) ClientOnlyFacadeModel and proves the resulting ActionTraits
# specialisation still round-trips JSON correctly: it builds, links and exits 0.
add_executable(morph_client_only_facade compile_checks/client_only_facade_no_model_header.cpp)
target_compile_definitions(morph_client_only_facade PRIVATE MORPH_CLIENT_ONLY)
target_link_libraries(morph_client_only_facade PRIVATE morph_guard_probe_deps)
morph_suppress_test_dialogs(morph_client_only_facade)
add_test(NAME morph_client_only_facade COMMAND morph_client_only_facade)
set_tests_properties(morph_client_only_facade PROPERTIES TIMEOUT 60)

# Instrumented like every other executable here: on a sanitizer leg core-cpp's
# modules are instrumented too (root CMakeLists.txt), and an uninstrumented
# program linking them fails with the sanitizer runtime undefined.
if(DEFINED AF_SANITIZER)
    foreach(_morph_guard_probe IN ITEMS morph_client_only_guard_links morph_client_only_runtime_throw
                                        morph_client_only_facade)
        apply_sanitizers(${_morph_guard_probe} ${AF_SANITIZER})
    endforeach()
    unset(_morph_guard_probe)
endif()

# ── Windows dialog canary ────────────────────────────────────────────────────
# Proves that a morph test executable dies on a failed assert() or an abort()
# instead of waiting on a dialog nobody will click, and that an invalid CRT
# parameter is handled rather than raising one. Its main() asks for nothing:
# linking core::testing_dialogs, as every test executable here does, is what
# installs the suppression. Modelled on core-cpp's WindowsDialogCanary, and
# judged the same way: by the marker each mode prints before it fails
# (PASS_REGULAR_EXPRESSION, which also makes ctest ignore the exit status), not
# by WILL_FAIL, which would score a run that never reached its assertion as a
# pass. The TIMEOUT is what catches a dialog: a run still waiting when it
# expires is waiting for a click.
if(WIN32 AND TARGET core-cpp-testing_dialogs)
    add_executable(morph_windows_dialog_canary windows_dialog_canary.cpp)
    morph_suppress_test_dialogs(morph_windows_dialog_canary)
    foreach(_mode IN ITEMS assert abort invalid-parameter)
        add_test(NAME morph_windows_dialog_canary.${_mode} COMMAND morph_windows_dialog_canary ${_mode})
        # `assert` and `abort` must kill the process; `invalid-parameter` must
        # survive, because the suppression installs a no-op handler, so the one
        # marker is the failure for two modes and the pass for the third.
        set(_pass "failing by ${_mode}")
        set(_fail "CONTINUED AFTER FAILURE")
        if(_mode STREQUAL "invalid-parameter")
            set(_pass "CONTINUED AFTER FAILURE")
            set(_fail "")
        endif()
        # A Debug CRT writes abort()'s message to stderr, and the suppression
        # keeps it there rather than dropping it: the abort run must show it. A
        # Release UCRT writes none, and a multi-config generator does not say
        # which it builds, so those keep the "failing by" marker.
        if(_mode STREQUAL "abort" AND CMAKE_BUILD_TYPE STREQUAL "Debug")
            set(_pass "abort\\(\\) has been called")
        endif()
        set_tests_properties(morph_windows_dialog_canary.${_mode} PROPERTIES
            PASS_REGULAR_EXPRESSION "${_pass}"
            FAIL_REGULAR_EXPRESSION "${_fail}"
            TIMEOUT 60
            SKIP_RETURN_CODE 77
            LABELS "canary")
    endforeach()
    unset(_mode)
    unset(_pass)
    unset(_fail)
endif()
