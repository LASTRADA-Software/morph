# SPDX-License-Identifier: Apache-2.0
#
# Bank example: a feature-rich demo app built on morph (async UI<->model
# bridge) with a SQLite persistence layer provided by the LASTRADA Lightweight
# ORM. Intended to be enabled from the top-level build with
# -DMORPH_BUILD_BANK_EXAMPLE=ON (it pulls a heavy dependency tree, so it is off
# by default).

cmake_minimum_required(VERSION 3.25)

# Allow standalone configuration (examples/bank as its own project) as well as
# being add_subdirectory()'d from the morph root.
if(NOT TARGET morph::morph)
    message(FATAL_ERROR
        "examples/bank expects the morph::morph target. Configure from the repository root "
        "with -DMORPH_BUILD_BANK_EXAMPLE=ON instead of configuring examples/bank directly.")
endif()

# ── WebAssembly build ────────────────────────────────────────────────────────
# In an Emscripten configure, the native persistence layer (Lightweight ORM over
# ODBC) cannot exist in the browser, so the whole native stack — Lightweight,
# bank_lib, the CLI and the tests — is skipped. bank_memory_lib takes its place:
# the models over an in-memory store, under the same class names as the native
# ones (memory/include shadows bank/models), which the generic client's bank
# screens link in the browser demo.
if(EMSCRIPTEN)
    add_library(bank_memory_lib STATIC
        memory/src/models/auth_model_wasm.cpp
        memory/src/models/account_model_wasm.cpp
        memory/src/models/customer_model_wasm.cpp
        memory/src/models/transaction_model_wasm.cpp
        memory/src/models/card_model_wasm.cpp
        memory/src/models/payee_model_wasm.cpp
        memory/src/models/payment_model_wasm.cpp
        memory/src/models/loan_model_wasm.cpp
        src/core/money.cpp)
    target_include_directories(bank_memory_lib BEFORE PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/memory/include  # shadow models + bank/wasm store
        ${CMAKE_CURRENT_SOURCE_DIR}/include)        # shared bank/dto, bank/core
    target_link_libraries(bank_memory_lib PUBLIC morph::morph)
    target_compile_features(bank_memory_lib PUBLIC cxx_std_23)
    apply_bigobj(bank_memory_lib)
    return()
endif()


# ── Lightweight ORM (SQLite via ODBC) ────────────────────────────────────────
# Lightweight resolves reflection-cpp / stdexec via CPM and finds yaml-cpp /
# Catch2 / libzip as system CONFIG packages.
set(LIGHTWEIGHT_BUILD_TESTS     OFF CACHE BOOL "" FORCE)
set(LIGHTWEIGHT_BUILD_EXAMPLES  OFF CACHE BOOL "" FORCE)
set(LIGHTWEIGHT_BUILD_TOOLS     OFF CACHE BOOL "" FORCE)
set(LIGHTWEIGHT_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)

# Kept in sync with examples/common/CMakeLists.txt's identical pin -- see that
# file's comment for why this is a commit SHA, not a tag.

# Lightweight's own install() rules unconditionally reference
# $<TARGET_PDB_FILE:Lightweight> on WIN32 (its CMakeLists.txt), which CMake
# only allows for linker-created artifacts (DLL/EXE) -- invalid whenever
# Lightweight resolves to a static build (LIGHTWEIGHT_BUILD_SHARED=OFF,
# forced by examples/common/CMakeLists.txt when it fetches Lightweight
# first in the same configure), and it fails at generate time even though
# nothing in this tree ever runs `cmake --install`. Skipping install-rule
# generation for just this CPMAddPackage call sidesteps the
# bad generator expression without touching Lightweight's vendored
# CMakeLists.txt -- same fix as examples/common/CMakeLists.txt's own
# Lightweight fetch.
set(_morph_saved_skip_install_rules ${CMAKE_SKIP_INSTALL_RULES})
set(CMAKE_SKIP_INSTALL_RULES ON)
morph_use_cpm(Lightweight)
CPMAddPackage(
    NAME Lightweight
    GITHUB_REPOSITORY LASTRADA-Software/Lightweight
    GIT_TAG 17c4e70d7fb4ead01376be0f46f201ad41c9a701)
set(CMAKE_SKIP_INSTALL_RULES ${_morph_saved_skip_install_rules})
unset(_morph_saved_skip_install_rules)

# Same demotion as examples/common/CMakeLists.txt's own Lightweight fetch, and
# it has to be here too: whichever of the two sites configures first is the one
# that actually defines the target, so only calling it from the other would
# leave the `-I` in place whenever bank configures first. The helper is
# idempotent, so calling it twice is a no-op the second time.
include(${PROJECT_SOURCE_DIR}/cmake/morph_demote_interface_includes.cmake)
morph_demote_lightweight_odbc_includes()

# ── Bank domain library ──────────────────────────────────────────────────────
add_library(bank_lib STATIC
    src/core/money.cpp
    src/db/schema.cpp
    src/models/account_model.cpp
    src/models/customer_model.cpp
    src/models/auth_model.cpp
    src/models/transaction_model.cpp
    src/models/payee_model.cpp
    src/models/payment_model.cpp
    src/models/card_model.cpp
    src/models/loan_model.cpp
    src/models/budget_model.cpp
    src/models/notification_model.cpp
    src/models/statement_model.cpp
    src/offline/lightweight_offline_queue.cpp
    src/app/app.cpp
)
target_include_directories(bank_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(bank_lib PUBLIC morph::morph Lightweight::Lightweight)
target_compile_features(bank_lib PUBLIC cxx_std_23)
apply_bigobj(bank_lib)
# Note: deliberately NOT calling apply_warnings() here — the third-party ORM
# headers are not -Werror clean and would fail the build.
#
# apply_sanitizers() is a separate question and the answer is yes.
# Until this line, `ladder_bank_server` was the *only* bank target that carried
# an AF_SANITIZER block, so a sanitizer configure with
# -DMORPH_BUILD_BANK_EXAMPLE=ON built bank_lib, bank_cli and every bank test
# binary with no instrumentation at all — and, worse than being uncovered,
# would have failed scripts/check_sanitizer_instrumentation.sh, because those
# test binaries *are* ctest commands. examples/TESTING.md states the
# convention this now follows: every target gets the block, not just the ones
# someone remembered.
#
# Instrumenting bank_lib does mean instrumenting the vendored Lightweight ORM
# headers it compiles in; ci.yml's `ladder-sanitizers` job is the leg that runs
# it, with every rung.
#
# All of the blocks below are required together, not one of them optional. An
# instrumented bank_lib with an uninstrumented executable does not merely go
# unchecked, it fails to link: measured by removing ladder_bank_tests' block alone,
#
#     undefined reference to `__ubsan_handle_type_mismatch_v1_abort'
#     clang++: error: linker command failed with exit code 1
if(DEFINED AF_SANITIZER)
    apply_sanitizers(bank_lib ${AF_SANITIZER})
endif()
if(AF_COVERAGE)
    apply_coverage(bank_lib)
endif()

# ── CLI driver ───────────────────────────────────────────────────────────────
add_executable(bank_cli src/cli/main.cpp)
target_link_libraries(bank_cli PRIVATE bank_lib)
target_compile_features(bank_cli PRIVATE cxx_std_23)
apply_bigobj(bank_cli)
if(DEFINED AF_SANITIZER)
    apply_sanitizers(bank_cli ${AF_SANITIZER})
endif()
# bank_lib is instrumented, and an executable that links an instrumented library must carry the profile runtime.
if(AF_COVERAGE)
    apply_coverage(bank_cli)
endif()

# ── ladder_bank_server: standalone server binary ─────────────────────────────
# Named `ladder_bank_server` because that is the name every rung's server has
# and the name the scenario tooling derives from a rung name
# (`run_scenarios.py`'s `RungSpec.binary`, `find_server()`). Bank is rung 2 --
# see examples/rungs.txt and examples/LADDER.md.
#
# Written out here rather than obtained by calling morph_add_rung(): the macro's
# server links `morph::ladder_<rung>_lib`, a target only the macro creates, and
# bank's library is `bank_lib`, built against a different dependency set
# (Lightweight/ODBC, and without apply_warnings()). Everything else mirrors the
# macro's block line for line. Bank is listed in examples/rungs.txt without
# calling the macro, which that file allows.
#
# Gated on Qt because the transport is morph::qt's QtWebSocketServer, exactly
# as the macro's version is. The gate is the option rather than
# `TARGET morph::qt`, since a build without the ladder adds examples/bank from
# the root before morph::qt exists.
if(MORPH_BUILD_QT)
    # Qt's imported targets are directory-scoped, so naming Qt6::Core below
    # needs a find_package in *this* directory even though the root already ran
    # one. WebSockets comes along because morph::qt's INTERFACE link
    # requirements name it.
    find_package(Qt6 REQUIRED COMPONENTS Core WebSockets)
    add_executable(ladder_bank_server src/server/main.cpp)
    target_link_libraries(ladder_bank_server PRIVATE bank_lib morph::qt morph_qt_impl Qt6::Core)
    target_compile_features(ladder_bank_server PRIVATE cxx_std_23)
    apply_bigobj(ladder_bank_server)
    # No apply_warnings(), for the same reason bank_lib skips it: this target
    # links bank_lib and pulls the ORM's headers in through its model headers,
    # which are not -Werror clean.
    if(AF_COVERAGE)
        apply_coverage(ladder_bank_server)
    endif()
    if(DEFINED AF_SANITIZER)
        apply_sanitizers(ladder_bank_server ${AF_SANITIZER})
    endif()
endif()

# ── Tests ────────────────────────────────────────────────────────────────────
if(MORPH_BUILD_TESTS)
    find_package(Catch2 3 CONFIG QUIET)
    if(NOT Catch2_FOUND)
        message(WARNING "Catch2 not found; bank example tests will not be built.")
    else()
        add_executable(ladder_bank_tests
            tests/test_account.cpp
            tests/test_auth.cpp
            tests/test_transaction.cpp
            tests/test_payee.cpp
            tests/test_payment.cpp
            tests/test_card.cpp
            tests/test_loan.cpp
            tests/test_budget.cpp
            tests/test_notification.cpp
            tests/test_statement.cpp
            tests/test_remote.cpp
            tests/test_offline.cpp
            tests/test_lightweight_offline_queue.cpp
            tests/test_relations.cpp
            tests/test_stateful_account.cpp
        )
        # ${PROJECT_SOURCE_DIR}/tests is morph's own test directory, and it is
        # on the include path for exactly one header:
        # tests/offline_queue_conformance.hpp, the implementation-agnostic
        # IOfflineQueue suite every shipped queue is measured against. The
        # Lightweight-backed queue in this example is measured against the same
        # suite, unedited -- that is the whole point of the conformance suite,
        # and a copy of it next to this example would measure nothing.
        target_include_directories(ladder_bank_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/tests
            ${PROJECT_SOURCE_DIR}/tests)
        target_link_libraries(ladder_bank_tests PRIVATE bank_lib morph_test_main)
        target_compile_features(ladder_bank_tests PRIVATE cxx_std_23)
        apply_bigobj(ladder_bank_tests)
        # A ctest command, so this block is not optional: without it the
        # binary runs on a sanitizer leg carrying nothing, and
        # scripts/check_sanitizer_instrumentation.sh fails the job rather than
        # letting it report a clean run over an uninstrumented suite.
        if(DEFINED AF_SANITIZER)
            apply_sanitizers(ladder_bank_tests ${AF_SANITIZER})
        endif()
        if(AF_COVERAGE)
            apply_coverage(ladder_bank_tests)
        endif()

        list(APPEND CMAKE_MODULE_PATH ${Catch2_DIR})
        include(Catch)
        # The label `ladder-bank`, as on bank's screen suites, and the prefix
        # `bank.`: CTest's -L is a regular expression, so `-L ladder` (the
        # ladder legs) and `-L bank` both select these cases, and the prefix
        # keeps their names distinct from every other rung's.
        # CMakePresets.json's base-test sets `noTestsAction: error`, so a label
        # that stopped matching fails the leg instead of running nothing.
        #
        # No RESOURCE_LOCK on any of the three calls, and that is a decision
        # rather than an oversight. catch_discover_tests registers one ctest
        # case per TEST_CASE and ctest runs each as its own process; when
        # bank's cases all named one fixed SQLite path under
        # temp_directory_path() and each deleted it on the way in,
        # `ctest -j 12 -L bank` failed 21 of 21 with
        # `HY000 (10) - [SQLite]disk I/O error (10)`. The ladder's remedy for
        # the same hazard is RESOURCE_LOCK morph_ladder_test_db (see
        # cmake/morph_add_rung.cmake's own comment on it), which serialises the
        # cases. Bank does not need to buy correctness with parallelism: no
        # case here reads state another case wrote -- every one already started
        # from an empty schema -- so tests/unique_test_database.hpp gives each
        # *process* a database directory of its own instead, and all 21 pass
        # concurrently. Measured: 0/21 at -j 12 before, 21/21 at -j 12 after,
        # in 1.1s against the same suite's 5.7s serial -- which is what a lock
        # would have pinned every developer run to.
        catch_discover_tests(ladder_bank_tests
            DISCOVERY_MODE PRE_TEST
            TEST_PREFIX "bank."
            PROPERTIES LABELS "ladder-bank")
    endif()
endif()
