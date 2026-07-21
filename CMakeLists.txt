cmake_minimum_required(VERSION 3.25)

project(morph
    VERSION 0.1.0
    DESCRIPTION "Bridge/Model/Executor async framework"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MORPH_BUILD_TESTS         "Build tests"         ON)
option(MORPH_BUILD_EXAMPLES      "Build examples"      ON)
option(MORPH_BUILD_BANK_EXAMPLE  "Build the SQLite/Lightweight bank example (heavy deps)" OFF)
option(MORPH_BUILD_BANK_GUI      "Build the Qt 6 GUI for the bank example" OFF)
option(MORPH_BUILD_HMAC_EXAMPLES "Build vetted-HMAC adapter examples (libsodium/OpenSSL, heavy deps)" OFF)
option(MORPH_BUILD_FORMS_QML     "Build the shipped Qt/QML forms renderer module (MorphForms) and its demo" OFF)

if(MORPH_BUILD_HMAC_EXAMPLES AND NOT MORPH_BUILD_EXAMPLES)
    message(WARNING "MORPH_BUILD_HMAC_EXAMPLES is ignored: it lives under examples/vetted_hmac, "
                    "which needs MORPH_BUILD_EXAMPLES=ON.")
endif()

if(MORPH_BUILD_FORMS_QML AND EMSCRIPTEN)
    message(WARNING "MORPH_BUILD_FORMS_QML is ignored: the Qt/QML forms renderer needs a "
                    "non-Emscripten toolchain.")
endif()
option(MORPH_BUILD_QT            "Build Qt6 WebSocket backend and tests" OFF)
option(MORPH_BUILD_FUZZERS       "Build libFuzzer harnesses over wire::decode/dispatchExecute (Clang only)" OFF)
option(MORPH_BUILD_LOAD_TESTS    "Build the soak + throughput/latency benchmark targets" OFF)
option(MORPH_BUILD_DOCUMENTATION "Build doxygen docs"  OFF)
option(MORPH_BUILD_CLANG_TIDY   "Enable clang-tidy checks (warnings-as-errors)" OFF)
option(MORPH_ENABLE_STRICT_COMPILATION
    "Enable strict compilation: treat all warnings as errors with extra diagnostics"
    ON
)
# Opt-in production guard: when ON, session_auth.hpp drops the `mac = hmacSha256`
# default argument on TokenIssuer/TokenVerifier/SigningAuthorizer, so any
# construction relying on the reference HMAC's default fails to compile and a
# vetted MacFunction (see examples/vetted_hmac/) must be injected explicitly.
# Keys on a build configuration the deployer chooses, not on CMAKE_BUILD_TYPE,
# so a zero-dependency local/low-stakes deployment can keep the reference impl
# by leaving this off. See docs/spec/security.md, "MAC-primitive recommended
# wiring". Off by default: the standard build is unaffected.
option(MORPH_REQUIRE_VETTED_HMAC
    "Fail to build if a call site relies on the reference hmacSha256 default; forces an explicit vetted MacFunction"
    OFF
)

# Both Qt consumers (the WebSocket backend and the forms QML renderer) need
# Qt6 auto-detected on Windows before their find_package(Qt6 ...) calls run.
if(WIN32 AND (MORPH_BUILD_QT OR MORPH_BUILD_FORMS_QML))
    include(cmake/FindQt.cmake)
endif()

include(cmake/compiler_options.cmake)

# ── clang-tidy integration ─────────────────────────────────────────────────
if(MORPH_BUILD_CLANG_TIDY)
    find_program(CLANG_TIDY_BIN clang-tidy REQUIRED)
    set(CMAKE_CXX_CLANG_TIDY "${CLANG_TIDY_BIN};--warnings-as-errors=*;--header-filter=include/morph/.*")
    message(STATUS "clang-tidy: ${CLANG_TIDY_BIN} (warnings as errors)")
endif()

# Registered before any subdirectory so demo-level add_test calls (examples/
# forms) land in the test set alongside the Catch2 suite.
if(MORPH_BUILD_TESTS)
    enable_testing()
endif()

# ── glaze ────────────────────────────────────────────────────────────────────
# On Windows vcpkg provides glaze. On Linux we fall back to FetchContent so
# that CI does not require vcpkg. The version bound matters: without it an
# older system/user install (e.g. a stray 4.x in ~/.local) silently shadows
# the pinned v7.4.0 and breaks schema generation.
find_package(glaze 7.4 CONFIG QUIET)
if(NOT glaze_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        glaze
        GIT_REPOSITORY https://github.com/stephenberry/glaze.git
        GIT_TAG        v7.4.0
        GIT_SHALLOW    TRUE
    )
    set(glaze_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(glaze)
endif()

# Emscripten's single-threaded build has no pthreads; requiring Threads there
# would force -pthread (SharedArrayBuffer), which we deliberately avoid so the
# WASM bundle can be served from plain GitHub Pages with no COOP/COEP headers.
if(NOT WIN32 AND NOT EMSCRIPTEN)
    find_package(Threads REQUIRED)
endif()

# ── Header-only library target ───────────────────────────────────────────────
add_library(morph INTERFACE)
add_library(morph::morph ALIAS morph)

target_include_directories(morph INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(morph INTERFACE glaze::glaze)
if(NOT WIN32 AND NOT EMSCRIPTEN)
    target_link_libraries(morph INTERFACE Threads::Threads)
endif()
if(MORPH_REQUIRE_VETTED_HMAC)
    target_compile_definitions(morph INTERFACE MORPH_REQUIRE_VETTED_HMAC)
endif()

set_target_properties(morph PROPERTIES
    VERIFY_INTERFACE_HEADER_SETS ON
)

target_sources(morph
    INTERFACE
    FILE_SET HEADERS
    BASE_DIRS include
    FILES
        include/morph/core/logger.hpp
        include/morph/core/observability.hpp
        include/morph/core/executor.hpp
        include/morph/core/strand.hpp
        include/morph/core/completion.hpp
        include/morph/core/model.hpp
        include/morph/core/registry.hpp
        include/morph/core/backend.hpp
        include/morph/core/remote.hpp
        include/morph/core/bridge.hpp
        include/morph/core/wire.hpp
        include/morph/journal/action_log.hpp
        include/morph/journal/journal.hpp
        include/morph/journal/file_action_log.hpp
        include/morph/offline/network_monitor.hpp
        include/morph/offline/offline_queue.hpp
        include/morph/offline/sync_worker.hpp
        include/morph/session/session.hpp
        include/morph/forms/forms.hpp
        include/morph/forms/choice.hpp
        include/morph/forms/i18n.hpp
        include/morph/util/rational.hpp
        include/morph/util/quantity.hpp
        include/morph/util/datetime.hpp
        include/morph/render/i18n.hpp
        include/morph/render/locale_format.hpp
)

# ── Qt/QML forms renderer setup (optional) ───────────────────────────────────
# Ships the reference Qt/QML renderer (MorphForms, src/qt/forms) as a reusable
# component, independent of MORPH_BUILD_EXAMPLES. examples/forms/gui_qml
# (added from examples/forms/CMakeLists.txt, itself still gated on
# MORPH_BUILD_EXAMPLES, added further below in "Demo executable") is a
# *consumer* of this module, not its home -- the find_package/
# qt_standard_project_setup() call and the morph::qt_forms interface target
# below must both exist before the "Demo executable" section runs, since that
# demo's CMakeLists.txt relies on qt_add_library/qt_add_qml_module/
# qt_add_executable already being defined and links against morph::qt_forms
# (a namespaced target, which -- unlike a plain target name such as
# morph_forms_moduleplugin below -- CMake requires to already exist at the
# point it is named). add_subdirectory(src/qt/forms) itself (which builds the
# actual MorphForms module/plugin, plus a Catch2-linked test executable) is
# deferred to just after the "Tests" section further below, since Catch2 is
# only found/fetched there and its test executable names Catch2::Catch2
# directly.
if(MORPH_BUILD_FORMS_QML AND NOT EMSCRIPTEN)
    find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick)
    qt_standard_project_setup(REQUIRES 6.5)

    # Header-only, model-agnostic controller core (no Q_OBJECT -- Qt cannot
    # register a class template for QML); an app's own QObject/QML_ELEMENT
    # controller subclass forwards to it. See
    # include/morph/qt/forms/forms_controller_core.hpp.
    add_library(morph_qt_forms INTERFACE)
    add_library(morph::qt_forms ALIAS morph_qt_forms)
    target_link_libraries(morph_qt_forms INTERFACE morph Qt6::Core)
    target_sources(morph_qt_forms
        INTERFACE
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/morph/qt/forms/forms_controller_core.hpp
    )
endif()

# ── Demo executable ──────────────────────────────────────────────────────────
if(MORPH_BUILD_EXAMPLES)
    # The console demo uses the thread-pool executor; skip it for the
    # single-threaded WASM build (which only wants the bank GUI target).
    if(NOT EMSCRIPTEN)
        add_executable(morph_example src/main.cpp)
        target_link_libraries(morph_example PRIVATE morph)
        apply_warnings(morph_example)

        if(DEFINED AF_SANITIZER)
            apply_sanitizers(morph_example ${AF_SANITIZER})
        endif()
        if(AF_COVERAGE)
            apply_coverage(morph_example)
        endif()

        add_subdirectory(examples/forms)
    endif()

    if(MORPH_BUILD_BANK_EXAMPLE)
        add_subdirectory(examples/bank)
    endif()

    if(MORPH_BUILD_HMAC_EXAMPLES)
        add_subdirectory(examples/vetted_hmac)
    endif()
endif()

# ── Tests ────────────────────────────────────────────────────────────────────
if(MORPH_BUILD_TESTS)
    find_package(Catch2 CONFIG QUIET)
    if(NOT Catch2_FOUND)
        include(FetchContent)
        FetchContent_Declare(
            Catch2
            GIT_REPOSITORY https://github.com/catchorg/Catch2.git
            GIT_TAG        v3.8.1
            GIT_SHALLOW    TRUE
        )
        FetchContent_MakeAvailable(Catch2)
    endif()

    add_subdirectory(tests)
endif()

# ── Qt/QML forms renderer (optional) ─────────────────────────────────────────
# The actual MorphForms module/plugin (src/qt/forms), deferred to here (after
# Catch2 is found/fetched above) since its own CMakeLists.txt links a Catch2
# test executable directly against Catch2::Catch2 when MORPH_BUILD_TESTS is
# ON -- the same condition that guards both. Qt6 Quick/Qml was already found
# and morph::qt_forms already created earlier (before "Demo executable"), so
# examples/forms/gui_qml (a consumer, added above) only forward-references
# the plain (non-namespaced) morph_forms_moduleplugin target this creates,
# which CMake resolves once this subdirectory is processed.
if(MORPH_BUILD_FORMS_QML AND NOT EMSCRIPTEN)
    add_subdirectory(src/qt/forms)
endif()

# ── Fuzz harnesses (optional) ────────────────────────────────────────────────
if(MORPH_BUILD_FUZZERS)
    add_subdirectory(tests/fuzz)
endif()

# ── Soak + load/latency benchmark targets (optional) ─────────────────────────
if(MORPH_BUILD_LOAD_TESTS)
    if(NOT MORPH_BUILD_TESTS)
        message(FATAL_ERROR
            "MORPH_BUILD_LOAD_TESTS requires MORPH_BUILD_TESTS=ON (the soak/bench "
            "targets link Catch2, fetched under that option).")
    endif()
    add_subdirectory(tests/soak)
    add_subdirectory(tests/bench)
endif()

# ── Qt WebSocket backend (optional) ─────────────────────────────────────────
if(MORPH_BUILD_QT)
    find_package(Qt6 COMPONENTS WebSockets REQUIRED)

    # Header-only interface for consumers
    add_library(morph_qt INTERFACE)
    add_library(morph::qt ALIAS morph_qt)
    target_link_libraries(morph_qt INTERFACE morph Qt6::WebSockets)
    target_sources(morph_qt
        INTERFACE
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/morph/qt/qt_executor.hpp
            include/morph/qt/qt_tls.hpp
            include/morph/qt/qt_websocket_server.hpp
            include/morph/qt/qt_websocket_backend.hpp
    )

    # Compiled implementation (QObject subclass needs MOC via a .cpp file)
    add_library(morph_qt_impl STATIC
        src/qt/qt_websocket_server.cpp
        src/qt/qt_websocket_backend.cpp
        include/morph/qt/qt_websocket_server.hpp
    )
    target_link_libraries(morph_qt_impl PUBLIC morph_qt)
    set_target_properties(morph_qt_impl PROPERTIES AUTOMOC ON)

    if(MORPH_BUILD_TESTS)
        add_subdirectory(tests/qt)
    endif()

    if(MORPH_BUILD_EXAMPLES)
        add_subdirectory(examples/qt_tls_client)
    endif()
endif()

# ── Documentation ───────────────────────────────────────────────────────────
if(MORPH_BUILD_DOCUMENTATION)
    add_subdirectory(docs)
endif()
