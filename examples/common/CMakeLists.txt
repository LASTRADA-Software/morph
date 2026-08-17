# SPDX-License-Identifier: Apache-2.0
#
# Shared ladder infrastructure: the presenter architecture (gui/) and the
# testkit (testkit/). See examples/TESTING.md.

# MORPH_BUILD_QT is required in *every* configure, Emscripten included:
# morph_ladder_app below is AppContext, whose Remote mode is a
# QtWebSocketBackend, and a WASM client is remote-only by rule
# (examples/IMPLEMENTATION.md rule 4's WASM clause). Native configures
# additionally need it for the testkit's BackendRig Socket mode and the
# fault-injection proxy.
if(NOT MORPH_BUILD_QT)
    message(FATAL_ERROR
        "MORPH_BUILD_LADDER requires MORPH_BUILD_QT=ON: AppContext's Remote mode, "
        "the testkit's BackendRig Socket mode and the fault-injection proxy all "
        "need morph::qt (Qt6::WebSockets).")
endif()

# Qt6::WebSockets is required under Emscripten too — morph::qt's own INTERFACE
# links it, so every consumer below (and the WASM spike) needs it present. An
# earlier revision of this file assumed the opposite ("not part of the standard
# Qt-for-WebAssembly module set") and returned before this call; that was never
# tested against a real Emscripten toolchain, and it only deferred the same
# failure to the link. Qt does ship QtWebSockets for wasm; a wasm Qt kit
# installed without that module now fails here, at configure time, with Qt's
# own clear message instead of an undefined-symbol wall.
find_package(Qt6 6.5 REQUIRED COMPONENTS Core WebSockets)
qt_standard_project_setup(REQUIRES 6.5)

# ── morph_ladder_gui: presenters, Qt6::Core only, no Catch2 ─────────────────
# Deliberately does NOT link morph::qt/morph_qt_impl (and so not
# Qt6::WebSockets): examples/TESTING.md's "Presenter architecture" rule 1
# requires presenters to instantiate under a plain QCoreApplication. The one
# piece of shared gui/ code that genuinely needs the WebSocket backend —
# AppContext, for its Remote mode — lives in morph_ladder_app below instead.
#
# apply_coverage() (every ladder target below, when AF_COVERAGE is ON —
# see IMPLEMENTATION.md rule 5): AUTOMOC's generated mocs_compilation.cpp
# lives under the build tree, so scripts/coverage.sh's source-path filter
# (which only ever names source-tree paths, e.g. examples/common) already
# excludes moc output from the completeness bar — nothing extra needed here.
add_library(morph_ladder_gui STATIC
    gui/presenter.cpp
    gui/event_poller.cpp
)
add_library(morph::ladder_gui ALIAS morph_ladder_gui)
target_include_directories(morph_ladder_gui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(morph_ladder_gui PUBLIC morph::morph Qt6::Core)
target_compile_features(morph_ladder_gui PUBLIC cxx_std_23)
set_target_properties(morph_ladder_gui PROPERTIES AUTOMOC ON)
apply_warnings(morph_ladder_gui)
if(AF_COVERAGE)
    apply_coverage(morph_ladder_gui)
endif()

# ── morph_ladder_app: AppContext — the deployment-mode-choosing layer ───────
# Split out of morph_ladder_gui so that target can stay Qt6::Core-only (see
# its comment above). A rung's gui_lib links morph::ladder_gui; the shells
# that actually pick a backend (gui/, gui_wasm/, tests/) also link this.
add_library(morph_ladder_app STATIC
    gui/app_context.cpp
)
add_library(morph::ladder_app ALIAS morph_ladder_app)
target_include_directories(morph_ladder_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(morph_ladder_app PUBLIC morph::morph morph::qt morph_qt_impl Qt6::Core)
target_compile_features(morph_ladder_app PUBLIC cxx_std_23)
# No Q_OBJECT here today (AppContext is a plain class); AUTOMOC is set to match
# the other ladder targets' convention so adding one later needs no CMake edit.
set_target_properties(morph_ladder_app PROPERTIES AUTOMOC ON)
apply_warnings(morph_ladder_app)
if(AF_COVERAGE)
    apply_coverage(morph_ladder_app)
endif()

# ── WebAssembly build ────────────────────────────────────────────────────────
# Everything above this line builds under Emscripten and is exactly what a WASM
# client needs: the presenter base (morph_ladder_gui) and the deployment-mode
# layer (morph_ladder_app, i.e. AppContext in its Remote shape). Everything
# below does not and never will — morph_ladder_testkit and ladder_common_tests
# need Catch2 (MORPH_BUILD_TESTS is never part of a WASM configure, mirroring
# examples/bank/CMakeLists.txt's own EMSCRIPTEN early return) and the
# Lightweight ORM speaks ODBC, which does not exist in a browser
# (examples/IMPLEMENTATION.md rule 4's WASM clause).
#
# Rung 0 returned *before* the two targets above as well, which left every
# rung's gui_wasm target with no morph::ladder_gui/morph::ladder_app to link —
# flagged as a known gap when morph_add_rung() shipped (task 8) and closed
# here, when rung 1's WASM client became the first real consumer.
if(EMSCRIPTEN)
    add_subdirectory(wasm_spike)
    return()
endif()

if(NOT MORPH_BUILD_TESTS)
    message(FATAL_ERROR
        "MORPH_BUILD_LADDER requires MORPH_BUILD_TESTS=ON: Catch2 backs the "
        "ladder testkit (morph_ladder_testkit) and ladder_common_tests.")
endif()

# ── Lightweight ORM (hoisted here once; TESTING.md "Build system and CI") ───
include(FetchContent)
set(LIGHTWEIGHT_BUILD_TESTS     OFF CACHE BOOL "" FORCE)
set(LIGHTWEIGHT_BUILD_EXAMPLES  OFF CACHE BOOL "" FORCE)
set(LIGHTWEIGHT_BUILD_TOOLS     OFF CACHE BOOL "" FORCE)
set(LIGHTWEIGHT_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
# Lightweight defaults to a shared library on Windows (LIGHTWEIGHT_BUILD_SHARED_DEFAULT
# in its own CMakeLists.txt), which means its classes need dll-interface annotations
# they don't have -- any target that both links Lightweight and calls apply_warnings()
# (ladder_common_tests, every rung's gui_lib, transitively through ladder_<rung>_lib)
# fails under /WX on Lightweight's own C4251/C4275. Forcing a static build sidesteps
# the DLL export boundary (and its warnings) entirely instead of punching warning
# holes through every consumer target.
set(LIGHTWEIGHT_BUILD_SHARED    OFF CACHE BOOL "" FORCE)
FetchContent_Declare(Lightweight
    GIT_REPOSITORY https://github.com/LASTRADA-Software/Lightweight.git
    GIT_TAG        v0.20260625.0
    GIT_SHALLOW    TRUE
)
# Lightweight's own install() rules unconditionally reference
# $<TARGET_PDB_FILE:Lightweight> on WIN32 (its CMakeLists.txt), which CMake
# only allows for linker-created artifacts (DLL/EXE) -- invalid for the
# static build LIGHTWEIGHT_BUILD_SHARED=OFF above now produces, and it fails
# at generate time even though nothing in this tree ever runs `cmake
# --install`. Skipping install-rule generation for just this
# FetchContent_MakeAvailable call sidesteps the bad generator expression
# without touching Lightweight's vendored CMakeLists.txt.
set(_morph_saved_skip_install_rules ${CMAKE_SKIP_INSTALL_RULES})
set(CMAKE_SKIP_INSTALL_RULES ON)
FetchContent_MakeAvailable(Lightweight)
set(CMAKE_SKIP_INSTALL_RULES ${_morph_saved_skip_install_rules})
unset(_morph_saved_skip_install_rules)

find_package(Catch2 3 CONFIG QUIET)
if(NOT Catch2_FOUND)
    message(FATAL_ERROR "Catch2 not found; MORPH_BUILD_TESTS=ON should have fetched it already (see root CMakeLists.txt).")
endif()

# ── morph_ladder_testkit: pump/fixtures/rig/fault-proxy/interleaver ─────────
# strand_interleaver.hpp (DeterministicExecutor), db_fixture.hpp and
# db_fault_fixture.hpp are fully header-defined and have no .cpp: none is a
# QObject, none needs MOC, and the library already links a non-empty TU
# (fault_proxy.cpp), so content-free placeholder TUs would be dead weight.
add_library(morph_ladder_testkit STATIC
    testkit/fault_proxy.cpp
)
add_library(morph::ladder_testkit ALIAS morph_ladder_testkit)
target_include_directories(morph_ladder_testkit PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(morph_ladder_testkit PUBLIC
    morph::morph morph::qt morph_qt_impl morph::ladder_gui morph::ladder_app
    Catch2::Catch2 Qt6::WebSockets Lightweight::Lightweight
)
target_compile_features(morph_ladder_testkit PUBLIC cxx_std_23)
set_target_properties(morph_ladder_testkit PROPERTIES AUTOMOC ON)
# Lightweight's headers are not -Werror clean (same caveat as bank/CMakeLists.txt) —
# do not apply_warnings() here.
if(AF_COVERAGE)
    apply_coverage(morph_ladder_testkit)
endif()

# ── ladder_common_tests: the testkit's own self-test suite ──────────────────
add_executable(ladder_common_tests
    testkit/testkit_main.cpp
    testkit/test_pump.cpp
    testkit/test_clock.cpp
    testkit/test_db_fixture.cpp
    testkit/test_db_fault_fixture.cpp
    testkit/test_db_busy_fixture.cpp
    testkit/test_db_pool_drain.cpp
    testkit/test_backend_rig.cpp
    testkit/test_presenter.cpp
    testkit/test_event_poller.cpp
    testkit/test_fault_proxy.cpp
    testkit/test_strand_interleaver.cpp
    testkit/test_wasm_registration_path_native.cpp
    testkit/test_action_driver.cpp
    testkit/test_offline_rig.cpp
)
target_link_libraries(ladder_common_tests PRIVATE morph::ladder_testkit)
# morph::ladder_testkit links Lightweight::Lightweight PUBLIC (above), and
# Lightweight's own target_include_directories() call is plain PUBLIC, not
# SYSTEM (its CMakeLists.txt) — so without this, apply_warnings() below
# (-Werror included) applies in full to every Lightweight header this
# target transitively sees. Same fix, same rationale, as
# cmake/morph_add_rung.cmake's identical block for ladder_<rung>_gui_lib/
# ladder_<rung>_tests.
get_target_property(_lightweight_includes Lightweight::Lightweight INTERFACE_INCLUDE_DIRECTORIES)
if(_lightweight_includes)
    target_include_directories(ladder_common_tests SYSTEM PRIVATE ${_lightweight_includes})
endif()
unset(_lightweight_includes)
target_compile_features(ladder_common_tests PRIVATE cxx_std_23)
set_target_properties(ladder_common_tests PROPERTIES AUTOMOC ON)
apply_warnings(ladder_common_tests)
if(AF_COVERAGE)
    apply_coverage(ladder_common_tests)
endif()

include(Catch)
get_target_property(_qt_core_dll Qt6::Core IMPORTED_LOCATION)
cmake_path(GET _qt_core_dll PARENT_PATH _qt_bin_dir)
# RESOURCE_LOCK: catch_discover_tests registers every TEST_CASE as its own
# ctest test, so `ctest -j` would happily run two of them concurrently — and
# DbFixture resets *one* real, shared on-disk database by dropping its tables
# (testkit/db_fixture.hpp), which two concurrent cases would do to each other
# mid-test. No preset sets parallel jobs today, so this is prophylactic; the
# lock is on the whole binary rather than the DB-touching cases only because
# catch_discover_tests applies PROPERTIES uniformly and this suite is ~5s.
catch_discover_tests(ladder_common_tests
    DISCOVERY_MODE POST_BUILD
    DL_PATHS "${_qt_bin_dir}"
    # Exactly one value per property name. catch_discover_tests forwards
    # PROPERTIES as a flat CMake list through a `-D VAR=a;b;c` command line,
    # where a list separator and a literal semicolon are indistinguishable and
    # no escaping survives — so a multi-value `LABELS "ladder;ladder-0"` does
    # not produce a two-label test, it shifts every following name/value pair
    # by one. That is what this call used to do: `ladder-0` became a property
    # *name* whose value was `TIMEOUT`, and neither the second label nor the
    # timeout was ever applied (`ctest --show-only=json-v1` shows it).
    PROPERTIES LABELS ladder TIMEOUT 120 RESOURCE_LOCK morph_ladder_test_db
)

# The per-rung label (`ladder-0` here) has to be applied outside
# catch_discover_tests for the reason above. Tests only exist once ctest reads
# the generated file, so this runs as a second TEST_INCLUDE_FILES entry —
# appended after catch_discover_tests' own, hence processed after it. Three
# details are forced by ctest's script mode rather than chosen:
#   * it iterates `<target>_TESTS`, the variable the discovery file leaves
#     behind — the DIRECTORY `TESTS` property is a configure-time property and
#     reads back empty here;
#   * that list interleaves per-test JSON metadata with the names, and
#     `if(TEST ...)` always answers false in script mode, so the JSON entries
#     are filtered by pattern instead;
#   * it uses `set_tests_properties`, not `set_property(TEST ... APPEND ...)`,
#     which errors with "TEST names that do not exist" here. That call
#     *replaces* LABELS, so it restates `ladder` alongside `ladder-0`. The
#     `LABELS ladder` above stays as the floor: CI filters on it, and it keeps
#     working even if this post-pass is ever dropped.
file(GENERATE
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/ladder_common_tests_rung_label.cmake"
    CONTENT [[
foreach(_ladder_test IN LISTS ladder_common_tests_TESTS)
    if(NOT _ladder_test MATCHES "\"class-name\"")
        set_tests_properties("${_ladder_test}" PROPERTIES LABELS "ladder;ladder-0")
    endif()
endforeach()
]]
)
set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
    "${CMAKE_CURRENT_BINARY_DIR}/ladder_common_tests_rung_label.cmake")
