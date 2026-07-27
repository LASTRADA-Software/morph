include(${CMAKE_SOURCE_DIR}/cmake/pinned_facts.cmake)
morph_generate_pinned_facts_header(
    ${CMAKE_SOURCE_DIR}/docs/spec/pinned_facts.toml
    ${CMAKE_BINARY_DIR}/generated/pinned_facts_generated.hpp
)

add_executable(morph_tests
    test_executor.cpp
    test_example.cpp
    test_executor_extra.cpp
    test_strand.cpp
    test_strand_extra.cpp
    test_strand_race.cpp
    test_completion.cpp
    test_completion_extra.cpp
    test_model.cpp
    test_logger.cpp
    test_observability.cpp
    test_backend_extra.cpp
    test_registry_extra.cpp
    test_bridge_local.cpp
    test_bridge_remote.cpp
    test_bridge_execute_json.cpp
    test_remote_extra.cpp
    test_remote_connection_scope.cpp
    test_action_validation.cpp
    test_security_fixes.cpp
    test_bridge_lifetime.cpp
    test_dispatch_di.cpp
    test_handler_binding.cpp
    test_switch_backend.cpp
    test_network_monitor.cpp
    test_offline_queue.cpp
    test_file_offline_queue.cpp
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
    test_wire_hardening.cpp
    test_protocol_version.cpp
    test_limit_policy.cpp
    test_action_log.cpp
    test_action_log_phase2.cpp
    test_journal_format_versioning.cpp
    test_outbox.cpp
    test_rational.cpp
    test_quantity.cpp
    test_quantity_forms.cpp
    test_flows_apps.cpp
    test_views.cpp
    test_computed_fields.cpp
    test_forms_rules.cpp
    test_forms_layout.cpp
    test_widget_hints.cpp
    test_datetime.cpp
    test_forms_i18n.cpp
    test_render_i18n.cpp
    test_render_locale_format.cpp
    test_forms_conformance_corpus.cpp
    test_session_auth.cpp
    test_policy_hardening.cpp
    test_register_authorization.cpp
    test_opaque_model_ids.cpp
    test_graceful_shutdown.cpp
    test_pinned_facts.cpp
    test_version.cpp
)

target_link_libraries(morph_tests
    PRIVATE
        morph::morph
        Catch2::Catch2WithMain
)

apply_warnings(morph_tests)

# Forwards CMakeLists.txt's project(VERSION ...) into tests/test_version.cpp
# so it can static_assert the checked-in include/morph/version.hpp constants
# never drift from the build system's declared version.
target_compile_definitions(morph_tests PRIVATE
    MORPH_CMAKE_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    MORPH_CMAKE_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    MORPH_CMAKE_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

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
catch_discover_tests(morph_tests DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 120)

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
