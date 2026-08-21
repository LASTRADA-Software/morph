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
# bank_lib, the CLI and the tests — is skipped. Only the self-contained WASM GUI
# (its own in-memory backend) is built. See gui_wasm/.
if(EMSCRIPTEN)
    if(MORPH_BUILD_BANK_GUI)
        add_subdirectory(gui_wasm)
    endif()
    return()
endif()

include(FetchContent)

# ── Lightweight ORM (SQLite via ODBC) ────────────────────────────────────────
# Lightweight resolves reflection-cpp / stdexec via CPM and finds yaml-cpp /
# Catch2 / libzip as system CONFIG packages.
set(LIGHTWEIGHT_BUILD_TESTS     OFF CACHE BOOL "" FORCE)
set(LIGHTWEIGHT_BUILD_EXAMPLES  OFF CACHE BOOL "" FORCE)
set(LIGHTWEIGHT_BUILD_TOOLS     OFF CACHE BOOL "" FORCE)
set(LIGHTWEIGHT_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)

FetchContent_Declare(Lightweight
    GIT_REPOSITORY https://github.com/LASTRADA-Software/Lightweight.git
    # Kept in sync with examples/common/CMakeLists.txt's identical pin --
    # see that file's comment for why this is a commit SHA, not a tag.
    GIT_TAG        bbb972a78e1962b968a2c6ad93f7dade736eaa01
    GIT_SHALLOW    FALSE
)
# Lightweight's own install() rules unconditionally reference
# $<TARGET_PDB_FILE:Lightweight> on WIN32 (its CMakeLists.txt), which CMake
# only allows for linker-created artifacts (DLL/EXE) -- invalid whenever
# Lightweight resolves to a static build (LIGHTWEIGHT_BUILD_SHARED=OFF,
# forced by examples/common/CMakeLists.txt when it fetches Lightweight
# first in the same configure), and it fails at generate time even though
# nothing in this tree ever runs `cmake --install`. Skipping install-rule
# generation for just this FetchContent_MakeAvailable call sidesteps the
# bad generator expression without touching Lightweight's vendored
# CMakeLists.txt -- same fix as examples/common/CMakeLists.txt's own
# Lightweight fetch.
set(_morph_saved_skip_install_rules ${CMAKE_SKIP_INSTALL_RULES})
set(CMAKE_SKIP_INSTALL_RULES ON)
FetchContent_MakeAvailable(Lightweight)
set(CMAKE_SKIP_INSTALL_RULES ${_morph_saved_skip_install_rules})
unset(_morph_saved_skip_install_rules)

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
    src/app/app.cpp
)
target_include_directories(bank_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(bank_lib PUBLIC morph::morph Lightweight::Lightweight)
target_compile_features(bank_lib PUBLIC cxx_std_23)
apply_bigobj(bank_lib)
# Note: deliberately NOT calling apply_warnings() here — the third-party ORM
# headers are not -Werror clean and would fail the build.

# ── CLI driver ───────────────────────────────────────────────────────────────
add_executable(bank_cli src/cli/main.cpp)
target_link_libraries(bank_cli PRIVATE bank_lib)
target_compile_features(bank_cli PRIVATE cxx_std_23)
apply_bigobj(bank_cli)

# ── Qt 6 GUI (opt-in) ────────────────────────────────────────────────────────
if(MORPH_BUILD_BANK_GUI)
    add_subdirectory(gui)
endif()

# ── Tests ────────────────────────────────────────────────────────────────────
if(MORPH_BUILD_TESTS)
    find_package(Catch2 3 CONFIG QUIET)
    if(NOT Catch2_FOUND)
        message(WARNING "Catch2 not found; bank example tests will not be built.")
    else()
        add_executable(bank_tests
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
            tests/test_relations.cpp
            tests/test_stateful_account.cpp
        )
        target_include_directories(bank_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
        target_link_libraries(bank_tests PRIVATE bank_lib Catch2::Catch2WithMain)
        target_compile_features(bank_tests PRIVATE cxx_std_23)
        apply_bigobj(bank_tests)

        list(APPEND CMAKE_MODULE_PATH ${Catch2_DIR})
        include(Catch)
        catch_discover_tests(bank_tests DISCOVERY_MODE PRE_TEST)
    endif()
endif()
