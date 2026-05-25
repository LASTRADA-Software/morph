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
option(MORPH_BUILD_QT            "Build Qt6 WebSocket backend and tests" OFF)
option(MORPH_BUILD_DOCUMENTATION "Build doxygen docs"  OFF)

include(cmake/compiler_options.cmake)

# ── glaze ────────────────────────────────────────────────────────────────────
# On Windows vcpkg provides glaze. On Linux we fall back to FetchContent so
# that CI does not require vcpkg.
find_package(glaze CONFIG QUIET)
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

if(NOT WIN32)
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
if(NOT WIN32)
    target_link_libraries(morph INTERFACE Threads::Threads)
endif()

set_target_properties(morph PROPERTIES
    VERIFY_INTERFACE_HEADER_SETS ON
)

target_sources(morph
    INTERFACE
    FILE_SET HEADERS
    BASE_DIRS include
    FILES
        include/morph/logger.hpp
        include/morph/executor.hpp
        include/morph/strand.hpp
        include/morph/completion.hpp
        include/morph/model.hpp
        include/morph/registry.hpp
        include/morph/backend.hpp
        include/morph/remote.hpp
        include/morph/bridge.hpp
        include/morph/network_monitor.hpp
        include/morph/offline_queue.hpp
        include/morph/sync_worker.hpp
        include/morph/session.hpp
        include/morph/wire.hpp
)

# ── Demo executable ──────────────────────────────────────────────────────────
if(MORPH_BUILD_EXAMPLES)
    add_executable(morph_example src/main.cpp)
    target_link_libraries(morph_example PRIVATE morph)
    apply_warnings(morph_example)

    if(DEFINED AF_SANITIZER)
        apply_sanitizers(morph_example ${AF_SANITIZER})
    endif()
    if(AF_COVERAGE)
        apply_coverage(morph_example)
    endif()
endif()

# ── Tests ────────────────────────────────────────────────────────────────────
if(MORPH_BUILD_TESTS)
    # Under MSan every library Catch2 touches must also be MSan-instrumented.
    # The system apt package is not, so we always build from source in that mode.
    set(_catch2_use_fetch OFF)
    find_package(Catch2 CONFIG QUIET)
    if(NOT Catch2_FOUND)
        set(_catch2_use_fetch ON)
    endif()

    if(_catch2_use_fetch)
        include(FetchContent)
        FetchContent_Declare(
            Catch2
            GIT_REPOSITORY https://github.com/catchorg/Catch2.git
            GIT_TAG        v3.8.1
            GIT_SHALLOW    TRUE
        )
        FetchContent_MakeAvailable(Catch2)
    endif()

    enable_testing()
    add_subdirectory(tests)
endif()

# ── Qt WebSocket backend (optional) ─────────────────────────────────────────
if(MORPH_BUILD_QT)
    if(WIN32)
        include(cmake/FindQt.cmake)
    endif()
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
endif()

# ── Documentation ───────────────────────────────────────────────────────────
if(MORPH_BUILD_DOCUMENTATION)
    add_subdirectory(docs)
endif()
