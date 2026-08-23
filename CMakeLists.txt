cmake_minimum_required(VERSION 3.25)

project(morph
    VERSION 0.1.0
    DESCRIPTION "Bridge/Model/Executor async framework"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Global, build-wide: the Lightweight ORM (fetched into examples/common's
# ladder testkit and examples/bank) includes <Windows.h> from SqlStatement.hpp
# with no NOMINMAX/WIN32_LEAN_AND_MEAN guard of its own, so its `min`/`max`
# macros leak into every translation unit that (transitively) includes it and
# break any later std::min/std::max/std::numeric_limits<T>::min() call in the
# same TU (e.g. examples/common/clock.hpp). Defined here, before any
# subdirectory is added, so it reaches every target that could end up in such
# a TU, not just the ones that call apply_warnings().
if(WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

option(MORPH_BUILD_TESTS         "Build tests"         ON)
option(MORPH_BUILD_EXAMPLES      "Build examples"      ON)
option(MORPH_BUILD_BANK_EXAMPLE  "Build the SQLite/Lightweight bank example (heavy deps)" OFF)
option(MORPH_BUILD_BANK_GUI      "Build the Qt 6 GUI for the bank example" OFF)
option(MORPH_BUILD_HMAC_EXAMPLES "Build vetted-HMAC adapter examples (libsodium/OpenSSL, heavy deps)" OFF)
option(MORPH_BUILD_FORMS_QML     "Build the shipped Qt/QML forms renderer module (MorphForms) and its demo" OFF)

# The application ladder (examples/LADDER.md): a shared testkit + GUI
# architecture consumed by every ladder rung. Off by default like the other
# heavy-dependency example options; needs MORPH_BUILD_QT and MORPH_BUILD_TESTS
# (checked inside examples/common/CMakeLists.txt with a clear FATAL_ERROR).
option(MORPH_BUILD_LADDER "Build the application ladder's shared testkit/GUI infrastructure and enabled rungs" OFF)

# Cache list of rungs to build when MORPH_BUILD_LADDER=ON. "all" builds every
# rung with a CMakeLists.txt under examples/<rung>/; a semicolon-separated
# subset (e.g. "pastebin;bookmarks") builds only those. Rung 0 has no rung
# folders yet, so this option exists but has nothing to select until rung 1
# lands (see examples/TESTING.md, "Build system and CI").
set(MORPH_LADDER_RUNGS "all" CACHE STRING "Semicolon-separated list of ladder rungs to build, or \"all\"")

if(MORPH_BUILD_HMAC_EXAMPLES AND NOT MORPH_BUILD_EXAMPLES)
    message(WARNING "MORPH_BUILD_HMAC_EXAMPLES is ignored: it lives under examples/vetted_hmac, "
                    "which needs MORPH_BUILD_EXAMPLES=ON.")
endif()

option(MORPH_BUILD_QT            "Build Qt6 WebSocket backend and tests" OFF)
option(MORPH_BUILD_NET           "Build the morph::net raw-socket WebSocket transport (POSIX only; see docs/spec/core/backend.md)" OFF)
option(MORPH_BUILD_FUZZERS       "Build libFuzzer harnesses over wire::decode/dispatchExecute (Clang only)" OFF)
option(MORPH_BUILD_LOAD_TESTS    "Build the soak + throughput/latency benchmark targets" OFF)
# Opt-in reference durable IOfflineQueue backed by SQLite (raw sqlite3 C API).
# Resolved via CMake's bundled FindSQLite3 module against a system SQLite3 dev
# package -- deliberately NOT added to vcpkg.json, so vcpkg-toolchain presets
# do not install it unconditionally. Off by default: the standard build tree
# never needs SQLite3 to be present. See docs/spec/offline/offline.md,
# "SqliteOfflineQueue".
option(MORPH_BUILD_OFFLINE_SQLITE "Build the SQLite-backed durable IOfflineQueue reference implementation (needs SQLite3)" OFF)
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

# Opt-in linker-footprint guard for a pure client -- one that dispatches every
# action to a remote peer and never constructs a model locally. Registering a
# model normally emits two registrars (registerModelOnce, registerActionOnce)
# whose bodies call the model's constructor and Model::execute -- pulling in
# implementations (a database driver, a native UI framework, an OS-specific
# API) a client target may not even have a link path for, and definitely never
# calls. The third registrar (registerActionExecutorOnce) routes through
# Bridge::executeVia -> IBackend::execute (fully abstract) and needs only the
# action's JSON codecs plus Model::execute's *declaration* -- never its body
# -- so it stays emitted regardless. See docs/spec/core/registry.md,
# "MORPH_CLIENT_ONLY".
#
# On the INTERFACE of the morph target, not per-consumer: the macro changes
# which registrars a model header emits, so two translation units disagreeing
# about it would violate ODR. Off by default: the standard build is unaffected.
#
# NEVER enable this for a process that hosts models (a server, or any Bridge
# running LocalBackend) -- it would silently register nothing, failing at
# runtime with "unknown model type" instead of at compile/link time.
option(MORPH_CLIENT_ONLY
    "Suppress the model-owning registrars so a pure remote client need not link model constructor/execute bodies. NEVER enable for a process that hosts models."
    OFF
)

# Both Qt consumers (the WebSocket backend and the forms QML renderer) need
# Qt6 auto-detected on Windows before their find_package(Qt6 ...) calls run.
if(WIN32 AND (MORPH_BUILD_QT OR MORPH_BUILD_FORMS_QML))
    include(cmake/FindQt.cmake)
endif()

include(cmake/compiler_options.cmake)

# Compiler-cache launcher selection: fastcache-cc (when a fastcached daemon
# answers) -> sccache -> ccache -> none. Included before any target or fetched
# dependency (FetchContent glaze below) so those get cached too. See
# cmake/CompileCache.cmake for the full selection logic; it declares its own
# USE_COMPILER_CACHE/FASTCACHE_ADDR options.
include(cmake/CompileCache.cmake)

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
if(MORPH_CLIENT_ONLY)
    target_compile_definitions(morph INTERFACE MORPH_CLIENT_ONLY)
endif()

set_target_properties(morph PROPERTIES
    VERIFY_INTERFACE_HEADER_SETS ON
)

target_sources(morph
    INTERFACE
    FILE_SET HEADERS
    BASE_DIRS include
    FILES
        include/morph/version.hpp
        include/morph/core/logger.hpp
        include/morph/core/observability.hpp
        include/morph/core/executor.hpp
        include/morph/core/strand.hpp
        include/morph/core/completion.hpp
        include/morph/core/timeout_scheduler.hpp
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
        include/morph/offline/file_offline_queue.hpp
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

# ── SQLite-backed durable offline queue: library target (optional) ──────────
# Only the morph::offline_sqlite target itself is created here, deliberately
# ahead of "Application ladder"'s add_subdirectory(examples) further below:
# examples/kanban/CMakeLists.txt links morph::offline_sqlite (a namespaced
# target, which CMake requires to already exist at the point it is named --
# see the identical note on morph::qt_forms vs the plain
# morph_forms_moduleplugin just below). The tests/offline_sqlite subdirectory
# that also depends on MORPH_BUILD_OFFLINE_SQLITE stays deferred to its
# original spot, after the "Tests" section, since it links
# Catch2::Catch2WithMain and that target only exists once Catch2 has been
# found/fetched there.
if(MORPH_BUILD_OFFLINE_SQLITE)
    find_package(SQLite3 REQUIRED)

    # CMake's FindSQLite3 locates the header by searching for sqlite3.h, and on
    # macOS finds it in the SDK's *bare* /usr/include -- the whole C library's
    # include directory, not a SQLite-specific prefix. Propagating that through
    # an imported target's INTERFACE_INCLUDE_DIRECTORIES turns it into
    # `-isystem <sdk>/usr/include`, which puts it *ahead of libc++* in the
    # search order. Every translation unit that reaches a SQLite header then
    # compiles the C library's own headers without their prerequisites:
    #
    #     .../MacOSX.sdk/usr/include/_types.h:46:9:
    #         error: unknown type name '__uint32_t'
    #
    # AppleClang is unaffected, because that directory is already where it
    # looks; only a non-Apple clang breaks -- which is the compiler this
    # project's own -Weverything guidance points at, so `morph_offline_sqlite`
    # could not be built by the developers most likely to build it (morph#172).
    #
    # The repair is to *drop* the directory, not to relocate it: sqlite3.h is
    # already on the compiler's default search path via -isysroot, at the
    # correct position, so it resolves with no flag at all. Measured both ways
    # on Homebrew clang 22 -- the same TU compiles clean with no flag and
    # produces 20 errors with the -isystem.
    #
    # Scoped to a directory that is a C-library catch-all (detected by stdio.h
    # living in it), so a real SQLite prefix -- Homebrew's, MacPorts', or one
    # given explicitly via -DSQLite3_INCLUDE_DIR -- is still honoured.
    function(_morph_strip_catchall_includes in_var out_var)
        set(_kept "")
        foreach(_dir IN LISTS ${in_var})
            if(APPLE AND EXISTS "${_dir}/stdio.h" AND EXISTS "${_dir}/sqlite3.h")
                message(STATUS
                    "morph: dropping catch-all SQLite3 include dir '${_dir}' -- it is the C library's own "
                    "include directory, and forcing it ahead of libc++ breaks every TU that reaches a "
                    "SQLite header (morph#172). sqlite3.h still resolves via the SDK default search path.")
            else()
                list(APPEND _kept "${_dir}")
            endif()
        endforeach()
        set(${out_var} "${_kept}" PARENT_SCOPE)
    endfunction()

    _morph_strip_catchall_includes(SQLite3_INCLUDE_DIRS _morph_sqlite_includes)

    # FindSQLite3 reports success but does not always leave `SQLite3::SQLite3`
    # resolvable at generate time (observed on the ubuntu-24.04 runner, which
    # fails with "the link interface ... contains SQLite3::SQLite3 but the
    # target was not found" while printing "Found SQLite3" moments earlier).
    # Synthesise the target from the variables the module *does* set, and
    # promote it to GLOBAL either way so every subdirectory that consumes
    # morph::offline_sqlite can see it.
    if(NOT TARGET SQLite3::SQLite3)
        add_library(SQLite3::SQLite3 UNKNOWN IMPORTED GLOBAL)
        set_target_properties(SQLite3::SQLite3 PROPERTIES
            IMPORTED_LOCATION "${SQLite3_LIBRARIES}"
            INTERFACE_INCLUDE_DIRECTORIES "${_morph_sqlite_includes}")
    else()
        set_target_properties(SQLite3::SQLite3 PROPERTIES IMPORTED_GLOBAL TRUE)
        # The module built the target itself, so strip the same directory from
        # the interface it populated -- otherwise the filter above only covers
        # the branch where we synthesise the target.
        get_target_property(_morph_sqlite_target_includes SQLite3::SQLite3 INTERFACE_INCLUDE_DIRECTORIES)
        if(_morph_sqlite_target_includes)
            _morph_strip_catchall_includes(_morph_sqlite_target_includes _morph_sqlite_target_kept)
            set_target_properties(SQLite3::SQLite3 PROPERTIES
                INTERFACE_INCLUDE_DIRECTORIES "${_morph_sqlite_target_kept}")
        endif()
    endif()

    add_library(morph_offline_sqlite INTERFACE)
    add_library(morph::offline_sqlite ALIAS morph_offline_sqlite)
    target_link_libraries(morph_offline_sqlite INTERFACE morph SQLite3::SQLite3)
    target_sources(morph_offline_sqlite
        INTERFACE
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
            include/morph/offline/sqlite_offline_queue.hpp
    )
endif()

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
# Emscripten builds this too. MorphForms is a plain Qt Quick QML module over
# header-only morph code — nothing in it is host-only — and a WASM ladder
# client has to render the *same* schema-driven Main.qml the desktop client
# does (examples/TESTING.md's "same client code"), which imports MorphForms.
# This block used to carry a `NOT EMSCRIPTEN` guard plus a "needs a
# non-Emscripten toolchain" warning, written when no WASM target consumed the
# renderer; that claim was never tested. Its one host-only piece, the QuickTest
# suite, is guarded inside src/qt/forms/CMakeLists.txt instead.
if(MORPH_BUILD_FORMS_QML)
    # 6.5 is a hard floor, not a preference: qt_standard_project_setup's
    # REQUIRES keyword and QQmlApplicationEngine::loadFromModule (used by the
    # demo) both arrive in 6.5. Stating it here turns "your Qt is too old" into
    # one clear configure-time message instead of a confusing missing-member
    # compile error deep in the build (Ubuntu 24.04 still ships 6.4.2).
    find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Qml Quick)
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
        add_subdirectory(examples/concepts)
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

# ── Application ladder (optional) ───────────────────────────────────────────
# Deferred to here (after the Tests section above), the same way
# MORPH_BUILD_FORMS_QML's src/qt/forms subdirectory is deferred further below:
# examples/common/CMakeLists.txt calls find_package(Catch2 3 CONFIG QUIET) and
# treats "not found" as a hard FATAL_ERROR (its own Catch2 does not get
# fetched -- it relies on MORPH_BUILD_TESTS=ON having already resolved one).
# Adding examples/ before this point would let that find_package() run before
# the Tests section's FetchContent fallback ever executes, breaking the
# no-system-Catch2 case even though MORPH_BUILD_TESTS=ON.
if(MORPH_BUILD_LADDER)
    add_subdirectory(examples)
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
if(MORPH_BUILD_FORMS_QML)
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

# ── morph::net raw-socket WebSocket transport (optional, POSIX only) ───────
if(MORPH_BUILD_NET)
    if(WIN32)
        message(WARNING "MORPH_BUILD_NET is ignored on Windows: morph::net is POSIX-only "
                        "(BSD sockets) today; Winsock2 support is documented future work.")
    else()
        add_library(morph_net INTERFACE)
        add_library(morph::net ALIAS morph_net)
        target_link_libraries(morph_net INTERFACE morph)
        target_sources(morph_net
            INTERFACE
            FILE_SET HEADERS
            BASE_DIRS include
            FILES
                include/morph/net/detail/base64.hpp
                include/morph/net/detail/sha1.hpp
                include/morph/net/detail/ws_handshake.hpp
                include/morph/net/detail/tcp_socket.hpp
                include/morph/net/detail/ws_frame.hpp
                include/morph/net/socket_server.hpp
                include/morph/net/socket_backend.hpp
        )

        if(MORPH_BUILD_TESTS)
            add_subdirectory(tests/net)
            if(MORPH_BUILD_QT)
                add_subdirectory(tests/net_qt_interop)
            endif()
        endif()
    endif()
endif()

# ── SQLite-backed durable offline queue: tests (optional) ───────────────────
# The morph::offline_sqlite target itself is created much earlier (see
# "SQLite-backed durable offline queue: library target" above, before
# "Application ladder"'s add_subdirectory(examples)) -- only this suite's own
# subdirectory stays deferred to here, since it links Catch2::Catch2WithMain
# and Catch2 is only found/fetched in the "Tests" section just above.
if(MORPH_BUILD_OFFLINE_SQLITE AND MORPH_BUILD_TESTS)
    add_subdirectory(tests/offline_sqlite)
endif()

# ── Documentation ───────────────────────────────────────────────────────────
if(MORPH_BUILD_DOCUMENTATION)
    add_subdirectory(docs)
endif()
