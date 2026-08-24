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
    test_model.cpp
    test_logger.cpp
    test_observability.cpp
    test_backend_extra.cpp
    test_registry_extra.cpp
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
    test_shared_instances.cpp
    test_strong_id_keys.cpp
    test_wire_encode_fault.cpp
    test_wire_hardening.cpp
    test_protocol_version.cpp
    test_limit_policy.cpp
    test_action_log.cpp
    test_action_log_phase2.cpp
    test_journal_format_versioning.cpp
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
    test_views.cpp
    test_computed_fields.cpp
    test_forms_rules.cpp
    test_forms_layout.cpp
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
)

target_link_libraries(morph_tests
    PRIVATE
        morph::morph
        Catch2::Catch2WithMain
)

apply_warnings(morph_tests)

# Same local-machine fastcache-cc opt-out examples/ledger/CMakeLists.txt
# already applies to ladder_ledger_lib/ladder_ledger_tests, for the same
# reason: the machine-local fastcache-cc/fastcached compiler-cache launcher
# was observed serving a stale cached object for a genuinely-changed source
# file in this target too (tests/test_ledger_rational_fuzz.cpp -- a debug
# print added directly to the source never appeared in the executed binary,
# reproduced across repeated rebuilds, and the same stale object survived a
# full FastCached service restart). No eviction primitive exists in the tool
# and the daemon's on-disk cache directory is outside this checkout, so this
# target opts out of the launcher entirely rather than risk silently linking
# a stale object again. CI is unaffected (fastcache-cc is only found/enabled
# when a daemon actually answers on the build machine, never true in CI) --
# this is a local-development-experience fix only. Safe to remove once the
# underlying cache bug is fixed upstream (see fastcache-cc --help /
# D:/caching/README.md for the tool this refers to, and
# https://github.com/LASTRADA-Software/fastcached/issues/51 for the filed
# report).
# Gated on fastcache-cc actually being the launcher. Clearing the property
# unconditionally clears *whatever* launcher is configured -- so on a CI runner,
# where the launcher is sccache, this target silently bypassed the compiler
# cache. morph_tests is ~92 TUs and is essentially the whole build of the plain
# matrix legs: `Linux / gcc-debug` compiled 101 TUs while sccache saw 9.
if(MORPH_COMPILER_CACHE_IS_FASTCACHE)
    set_target_properties(morph_tests PROPERTIES
        C_COMPILER_LAUNCHER ""
        CXX_COMPILER_LAUNCHER "")
endif()

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

# ── MORPH_CLIENT_ONLY guard check ────────────────────────────────────────────
# Configure-time proof that MORPH_CLIENT_ONLY actually suppresses the two
# model-owning registrars (registerModelOnce, registerActionOnce) -- see
# docs/spec/core/registry.md, "MORPH_CLIENT_ONLY". Reuses
# MORPH_VETTED_HMAC_GUARD_INCLUDE_DIRS (morph + glaze include dirs; see that
# guard's own comments above for why try_compile needs them forwarded this
# way rather than via LINK_LIBRARIES).
#
# compile_checks/client_only_no_model_link.cpp declares (never defines)
# ClientOnlyModel's constructor and execute(). Two probes, in opposite
# directions, prove the guard does something real rather than the probe being
# vacuously satisfied either way:
#   - WITH MORPH_CLIENT_ONLY defined: must LINK (the model-owning registrars
#     are macroed away, so nothing in the program ever references either
#     undefined symbol).
#   - WITHOUT it (today's default): must FAIL TO LINK (the registrars' lambda
#     bodies do reference them, and the linker cannot resolve either symbol).
unset(MORPH_CLIENT_ONLY_GUARD_SUPPRESSES_LINKAGE CACHE)
try_compile(MORPH_CLIENT_ONLY_GUARD_SUPPRESSES_LINKAGE
    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/compile_checks/client_only_no_model_link.cpp"
    CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${MORPH_VETTED_HMAC_GUARD_INCLUDE_DIRS}"
    CXX_STANDARD 23
    COMPILE_DEFINITIONS "-DMORPH_CLIENT_ONLY"
    OUTPUT_VARIABLE MORPH_CLIENT_ONLY_GUARD_SUPPRESSES_LINKAGE_OUTPUT
)
if(NOT MORPH_CLIENT_ONLY_GUARD_SUPPRESSES_LINKAGE)
    message(FATAL_ERROR
        "MORPH_CLIENT_ONLY guard check failed: "
        "compile_checks/client_only_no_model_link.cpp failed to link with "
        "MORPH_CLIENT_ONLY defined, even though ClientOnlyModel's constructor "
        "and execute() are never called anywhere in that program (the "
        "model-owning registrars must be fully suppressed).\n"
        "--- compiler/linker output ---\n"
        "${MORPH_CLIENT_ONLY_GUARD_SUPPRESSES_LINKAGE_OUTPUT}")
endif()

unset(MORPH_CLIENT_ONLY_GUARD_DEFAULT_NEEDS_LINKAGE CACHE)
try_compile(MORPH_CLIENT_ONLY_GUARD_DEFAULT_NEEDS_LINKAGE
    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/compile_checks/client_only_no_model_link.cpp"
    CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${MORPH_VETTED_HMAC_GUARD_INCLUDE_DIRS}"
    CXX_STANDARD 23
    OUTPUT_VARIABLE MORPH_CLIENT_ONLY_GUARD_DEFAULT_NEEDS_LINKAGE_OUTPUT
)
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
endif()

# The two try_compile() checks above only prove the *registration-suppression*
# half of the guard (static-init-time link resolution). The other half --
# that Bridge::executeVia's localOp actually throws std::logic_error at
# *runtime* under MORPH_CLIENT_ONLY, instead of silently calling
# Model::execute -- needs the probe to actually run, not just link. try_run()
# compiles AND executes compile_checks/client_only_runtime_throw.cpp,
# capturing its exit code (0 = the expected std::logic_error was caught).
unset(MORPH_CLIENT_ONLY_RUNTIME_THROW_COMPILED CACHE)
unset(MORPH_CLIENT_ONLY_RUNTIME_THROW_EXITCODE CACHE)
try_run(MORPH_CLIENT_ONLY_RUNTIME_THROW_EXITCODE MORPH_CLIENT_ONLY_RUNTIME_THROW_COMPILED
    "${CMAKE_CURRENT_BINARY_DIR}/client_only_runtime_throw_check"
    "${CMAKE_CURRENT_SOURCE_DIR}/compile_checks/client_only_runtime_throw.cpp"
    CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${MORPH_VETTED_HMAC_GUARD_INCLUDE_DIRS}"
    CXX_STANDARD 23
    COMPILE_DEFINITIONS "-DMORPH_CLIENT_ONLY"
    # CMAKE_THREAD_LIBS_INIT (a raw linker-flag string set by the outer
    # project's find_package(Threads REQUIRED) at CMakeLists.txt:102, e.g.
    # "-lpthread" on Linux, empty on platforms needing nothing extra) rather
    # than the Threads::Threads *target*: unlike Qt6::Core/Network/WebSockets
    # (an exported find_package(Qt6) target, used the same way by the
    # MORPH_QT_NO_SSL_GUARD_COMPILES check above), the CMake-bundled
    # FindThreads module's imported target does not reliably resolve inside
    # try_run()'s isolated scratch project on every platform -- confirmed by
    # a real CI failure on Windows/MSVC ("Target ... links to: Threads::Threads
    # ... but the target was not found").
    LINK_LIBRARIES "${CMAKE_THREAD_LIBS_INIT}"
    COMPILE_OUTPUT_VARIABLE MORPH_CLIENT_ONLY_RUNTIME_THROW_COMPILE_OUTPUT
    RUN_OUTPUT_VARIABLE MORPH_CLIENT_ONLY_RUNTIME_THROW_RUN_OUTPUT
)
if(NOT MORPH_CLIENT_ONLY_RUNTIME_THROW_COMPILED)
    message(FATAL_ERROR
        "MORPH_CLIENT_ONLY guard check failed: "
        "compile_checks/client_only_runtime_throw.cpp failed to compile with "
        "MORPH_CLIENT_ONLY defined.\n"
        "--- compiler output ---\n"
        "${MORPH_CLIENT_ONLY_RUNTIME_THROW_COMPILE_OUTPUT}")
endif()
if(NOT MORPH_CLIENT_ONLY_RUNTIME_THROW_EXITCODE EQUAL 0)
    message(FATAL_ERROR
        "MORPH_CLIENT_ONLY guard check failed: "
        "compile_checks/client_only_runtime_throw.cpp did not observe the "
        "expected std::logic_error from Bridge::executeVia's localOp under "
        "MORPH_CLIENT_ONLY (exit code ${MORPH_CLIENT_ONLY_RUNTIME_THROW_EXITCODE}).\n"
        "--- program output ---\n"
        "${MORPH_CLIENT_ONLY_RUNTIME_THROW_RUN_OUTPUT}")
endif()

# Issue #61: BRIDGE_REGISTER_ACTION_FOR_CLIENT lets a MORPH_CLIENT_ONLY client
# register an action's ActionTraits (JSON codecs + an explicitly-named Result
# type) without ever needing the real model's complete class body -- see
# docs/spec/core/registry.md, "BRIDGE_REGISTER_ACTION_FOR_CLIENT".
# compile_checks/client_only_facade_no_model_header.cpp forward-declares (never
# defines) ClientOnlyFacadeModel and proves the resulting ActionTraits
# specialisation still round-trips JSON correctly. try_run(), not try_compile():
# this probe's main() actually exercises toJson/fromJson, not just links.
unset(MORPH_CLIENT_ONLY_FACADE_COMPILED CACHE)
unset(MORPH_CLIENT_ONLY_FACADE_EXITCODE CACHE)
try_run(MORPH_CLIENT_ONLY_FACADE_EXITCODE MORPH_CLIENT_ONLY_FACADE_COMPILED
    "${CMAKE_CURRENT_BINARY_DIR}/client_only_facade_check"
    "${CMAKE_CURRENT_SOURCE_DIR}/compile_checks/client_only_facade_no_model_header.cpp"
    CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${MORPH_VETTED_HMAC_GUARD_INCLUDE_DIRS}"
    CXX_STANDARD 23
    COMPILE_DEFINITIONS "-DMORPH_CLIENT_ONLY"
    LINK_LIBRARIES "${CMAKE_THREAD_LIBS_INIT}"
    COMPILE_OUTPUT_VARIABLE MORPH_CLIENT_ONLY_FACADE_COMPILE_OUTPUT
    RUN_OUTPUT_VARIABLE MORPH_CLIENT_ONLY_FACADE_RUN_OUTPUT
)
if(NOT MORPH_CLIENT_ONLY_FACADE_COMPILED)
    message(FATAL_ERROR
        "BRIDGE_REGISTER_ACTION_FOR_CLIENT guard check failed: "
        "compile_checks/client_only_facade_no_model_header.cpp failed to "
        "compile/link with MORPH_CLIENT_ONLY defined, even though "
        "ClientOnlyFacadeModel is never a complete type anywhere in this "
        "program (BRIDGE_REGISTER_ACTION_FOR_CLIENT must not require model "
        "completeness).\n"
        "--- compiler/linker output ---\n"
        "${MORPH_CLIENT_ONLY_FACADE_COMPILE_OUTPUT}")
endif()
if(NOT MORPH_CLIENT_ONLY_FACADE_EXITCODE EQUAL 0)
    message(FATAL_ERROR
        "BRIDGE_REGISTER_ACTION_FOR_CLIENT guard check failed: "
        "compile_checks/client_only_facade_no_model_header.cpp did not "
        "round-trip its action through toJson/fromJson correctly "
        "(exit code ${MORPH_CLIENT_ONLY_FACADE_EXITCODE}).\n"
        "--- program output ---\n"
        "${MORPH_CLIENT_ONLY_FACADE_RUN_OUTPUT}")
endif()
