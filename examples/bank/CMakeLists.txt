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

# Same demotion as examples/common/CMakeLists.txt's own Lightweight fetch, and
# it has to be here too: whichever of the two sites configures first is the one
# that actually defines the target, so only calling it from the other would
# leave the `-I` in place whenever bank configures first. The helper is
# idempotent, so calling it twice is a no-op the second time (morph#438).
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

# ── ladder_bank_server: standalone server binary ─────────────────────────────
# Named `ladder_bank_server` because that is the name the scenario tooling
# derives from a rung name (`run_scenarios.py`'s `RungSpec.binary`,
# `find_server()`); the prefix is that tool's convention, not a claim to a rung
# number, which issue #87 leaves to the maintainer.
#
# Written out here rather than obtained by calling morph_add_rung(). That macro
# emits this exact block (cmake/morph_add_rung.cmake, "ladder_<rung>_server"),
# but it links `morph::ladder_<rung>_lib` -- a target only the macro itself
# creates -- and bank's library is `bank_lib`, built above against a different
# dependency set (Lightweight/ODBC, and deliberately without apply_warnings()).
# More decisively, calling the macro is not available to us: examples/rungs.txt
# documents that "omitting a rung here while examples/<rung>/CMakeLists.txt
# calls morph_add_rung() is a hard configure error, by design", so the call
# would force bank into that file -- and with it into ladder CI, the sanitizer
# and WASM matrices, codecov components and coverage.sh. That is the numbering
# decision #87 reserves for the maintainer, so this stays a local target: it
# gives bank a server without asserting anything about bank's slot in the
# ladder. Everything else here mirrors the macro's block line for line.
#
# Gated on Qt because the transport is morph::qt's QtWebSocketServer, exactly
# as the macro's version is; MORPH_BUILD_BANK_GUI is *not* the gate, since this
# is a headless server that needs Qt6::Core and no GUI.
#
# The gate is the *option*, not `TARGET morph::qt`: the root CMakeLists adds
# examples/bank (line ~523) well before it creates morph::qt (line ~596), so a
# TARGET check here is false even when Qt is on. Naming the target in
# target_link_libraries is fine regardless -- CMake resolves link names at
# generate time, by which point the alias exists. bank/gui/CMakeLists.txt
# depends on the same ordering.
if(MORPH_BUILD_QT)
    # Qt's imported targets are directory-scoped, so naming Qt6::Core below
    # needs a find_package in *this* directory even though the root already ran
    # one -- the same call bank_gui_qml_tests makes further down for the same
    # reason. WebSockets comes along because morph::qt's INTERFACE link
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

        # ── bank_gui_tests: the GUI's own suite ──────────────────────────────
        # A second binary rather than more sources in bank_tests: bank_tests
        # links bank_lib and nothing Qt, and it is built in every
        # -DMORPH_BUILD_BANK_EXAMPLE=ON configure, including the ones with no
        # Qt at all. The controllers only exist when MORPH_BUILD_BANK_GUI=ON
        # (that is the option that adds gui/), so anything holding one has to
        # be gated on the same switch.
        #
        # tests/gui/, not tests/: bank lists its test sources by hand rather
        # than globbing them, so a file in tests/ that is deliberately absent
        # from bank_tests reads as an oversight. The subdirectory says which
        # binary a file belongs to.
        if(TARGET bank_gui_lib)
            add_executable(bank_gui_tests
                tests/gui/test_bank_qml_surface.cpp
                # QmlSurfaceAudit, compiled in rather than linked from
                # morph::ladder_testkit. That library is only created by
                # MORPH_BUILD_LADDER=ON, an option entirely independent of
                # MORPH_BUILD_BANK_EXAMPLE — bank is not a ladder rung (it is
                # absent from examples/rungs.txt and never calls
                # morph_add_rung), so requiring the ladder's whole shared
                # testkit to be configured before the bank GUI can be tested
                # would couple two option trees that have nothing else to do
                # with each other. It would also over-link: morph_ladder_testkit
                # pulls in morph::qt, Qt6::WebSockets, ladder_gui and ladder_app,
                # while the audit itself needs Qt6::Core and nothing more.
                # This is the same translation unit, not a copy of it, so the
                # audit still has exactly one implementation.
                ${PROJECT_SOURCE_DIR}/examples/common/testkit/qml_surface.cpp
            )
            target_include_directories(bank_gui_tests PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/tests
                ${PROJECT_SOURCE_DIR}/examples/common)
            target_link_libraries(bank_gui_tests PRIVATE bank_gui_lib Catch2::Catch2WithMain)
            target_compile_features(bank_gui_tests PRIVATE cxx_std_23)
            # The audit reads gui/qml/*.qml out of the source tree at run time,
            # and ctest runs the binary from the build directory. Same macro,
            # same value (the repo root) and same reason as every ladder rung's
            # test binary gets from morph_add_rung().
            target_compile_definitions(bank_gui_tests PRIVATE
                MORPH_LADDER_SOURCE_ROOT="${PROJECT_SOURCE_DIR}")
            apply_bigobj(bank_gui_tests)

            catch_discover_tests(bank_gui_tests DISCOVERY_MODE PRE_TEST)

            # ── bank_gui_qml_tests: the QML layer, with a live engine ────────
            # A third binary, and the reason is the second one's defining
            # property: bank_gui_tests links Qt6::Core and nothing else, needs
            # no platform plugin and no display, and its own file header says
            # so -- that is what lets the surface audit run in any configure
            # that can compile a QObject. Linking Qt6::Quick into it to gain
            # one test would cost that, for every run of the audit, forever.
            #
            # This target exists because some of bank's GUI logic is *in* the
            # QML and is invisible from C++. MoveMoneyPage's account picker is
            # the worked example (morph#296): the controller is entirely
            # self-consistent when driven directly, and the defect is a
            # ComboBox whose `currentIndex` no binding restores after its model
            # is replaced. Only a live engine over the shipped .qml file can
            # see that, so only a live engine can hold the regression.
            #
            # The .qml files are loaded from the source tree by URL rather than
            # from the BankGui QML module, which lives inside the bank_gui
            # *executable* and cannot be linked. QML's implicit import of a
            # component's own directory resolves the sibling types (Panel,
            # Picker, Field, AppButton) with no qmldir, so this is the same
            # file the desktop client ships, not a copy.
            find_package(Qt6 REQUIRED COMPONENTS Gui Qml Quick QuickControls2)
            add_executable(bank_gui_qml_tests
                tests/gui/test_bank_gui_qml_behaviour.cpp
                # The Qt-owning Catch2 main, in place of Catch2WithMain, with
                # MORPH_LADDER_TESTKIT_GUI_APP below upgrading its owned
                # application object to QGuiApplication. Two reasons, both
                # load-bearing: Qt Quick cannot instantiate an item without a
                # platform integration, and BankClient's QtExecutor captures
                # `QCoreApplication::instance()` when it is *constructed*, so
                # the application has to exist before the first BankClient.
                ${PROJECT_SOURCE_DIR}/examples/common/testkit/testkit_main.cpp
            )
            target_include_directories(bank_gui_qml_tests PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/tests
                ${PROJECT_SOURCE_DIR}/examples/common)
            target_link_libraries(bank_gui_qml_tests PRIVATE
                bank_gui_lib Catch2::Catch2
                Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2)
            target_compile_features(bank_gui_qml_tests PRIVATE cxx_std_23)
            target_compile_definitions(bank_gui_qml_tests PRIVATE
                MORPH_LADDER_SOURCE_ROOT="${PROJECT_SOURCE_DIR}"
                MORPH_LADDER_TESTKIT_GUI_APP)
            apply_bigobj(bank_gui_qml_tests)

            # offscreen, because the runner may have no display and this suite
            # never looks at a pixel -- it reads property values off the items
            # the engine created. The same variable the ladder's own GUI legs
            # set for ctest (examples/TESTING.md).
            catch_discover_tests(bank_gui_qml_tests
                DISCOVERY_MODE PRE_TEST
                PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        endif()
    endif()
endif()
