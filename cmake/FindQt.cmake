# FindQt.cmake
#
# Scans C:/Qt (Windows) for the newest installed Qt6 version and the best
# matching kit for the current compiler/architecture, then prepends its
# lib/cmake directory to CMAKE_PREFIX_PATH so that a subsequent
# find_package(Qt6 ...) succeeds without any hardcoded paths.
#
# On non-Windows platforms this script is a no-op: Qt6 is expected to be
# found via the system cmake package registry (e.g. after
# "apt install qt6-websockets-dev" on Ubuntu/Debian).
#
# Usage in CMakeLists.txt (before find_package):
#   include(cmake/FindQt.cmake)
#   find_package(Qt6 COMPONENTS WebSockets REQUIRED)
#
# Variables set on success (Windows only):
#   QT_AUTODETECTED_ROOT   — e.g. C:/Qt/6.8.3/msvc2022_64
#   QT_AUTODETECTED_VERSION — e.g. 6.8.3

if(NOT WIN32)
    return()
endif()

# ── 1. Locate the Qt installation root ───────────────────────────────────────
# Prefer an explicit override, then QTDIR env var, then scan C:/Qt.

set(_qt_search_roots "")

if(DEFINED QT_ROOT)
    list(APPEND _qt_search_roots "${QT_ROOT}")
endif()

if(DEFINED ENV{QTDIR})
    # QTDIR may point directly to a kit (e.g. C:/Qt/6.8.3/msvc2022_64)
    # or to the version root (C:/Qt/6.8.3). Accept both.
    list(APPEND _qt_search_roots "$ENV{QTDIR}")
endif()

# Default scan location
list(APPEND _qt_search_roots "C:/Qt")

# ── 2. Enumerate version directories ─────────────────────────────────────────

set(_qt_found_cmake_dir "")
set(_qt_found_root "")
set(_qt_found_version "")

# Kit priority list — first match wins.  Ordered from most to least preferred
# for a typical Windows MSVC build.  Add mingw_64 as fallback.
set(_kit_candidates "")

# Determine architecture suffix
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_arch_suffix "_64")
else()
    set(_arch_suffix "_32")
endif()

# Compiler-specific kits (most preferred first)
if(MSVC)
    # Detect MSVC toolset year from CMAKE_CXX_COMPILER_VERSION (e.g. 19.38 → 2022)
    if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "19.30")
        list(APPEND _kit_candidates "msvc2022${_arch_suffix}")
    endif()
    if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "19.20")
        list(APPEND _kit_candidates "msvc2019${_arch_suffix}")
    endif()
    list(APPEND _kit_candidates "msvc2017${_arch_suffix}")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # clang-cl still links against the MSVC runtime; use msvc kits
    list(APPEND _kit_candidates "msvc2022${_arch_suffix}" "msvc2019${_arch_suffix}")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    list(APPEND _kit_candidates "mingw${_arch_suffix}")
endif()
# Universal fallback — try everything present
list(APPEND _kit_candidates
    "msvc2022_64" "msvc2022_32"
    "msvc2019_64" "msvc2019_32"
    "mingw_64"    "mingw_32"
)
list(REMOVE_DUPLICATES _kit_candidates)

foreach(_root IN LISTS _qt_search_roots)
    if(NOT IS_DIRECTORY "${_root}")
        continue()
    endif()

    # Collect all subdirectories that look like version numbers (digit.digit.digit)
    file(GLOB _version_dirs LIST_DIRECTORIES true "${_root}/[0-9]*")

    # Sort descending so we pick the newest version first
    list(SORT _version_dirs COMPARE NATURAL ORDER DESCENDING)

    foreach(_vdir IN LISTS _version_dirs)
        if(NOT IS_DIRECTORY "${_vdir}")
            continue()
        endif()

        get_filename_component(_ver "${_vdir}" NAME)

        # Must start with "6." to qualify as Qt 6
        if(NOT _ver MATCHES "^6\\.")
            continue()
        endif()

        # Try each kit in priority order
        foreach(_kit IN LISTS _kit_candidates)
            set(_cmake_dir "${_vdir}/${_kit}/lib/cmake")
            if(IS_DIRECTORY "${_cmake_dir}" AND IS_DIRECTORY "${_cmake_dir}/Qt6")
                set(_qt_found_cmake_dir "${_cmake_dir}")
                set(_qt_found_root "${_vdir}/${_kit}")
                set(_qt_found_version "${_ver}")
                break()
            endif()
        endforeach()

        if(_qt_found_cmake_dir)
            break()
        endif()
    endforeach()

    if(_qt_found_cmake_dir)
        break()
    endif()
endforeach()

# ── 3. Apply or report ───────────────────────────────────────────────────────

if(_qt_found_cmake_dir)
    message(STATUS "Qt6 auto-detected: ${_qt_found_root} (${_qt_found_version})")
    # Prepend so user-supplied CMAKE_PREFIX_PATH entries still win
    list(PREPEND CMAKE_PREFIX_PATH "${_qt_found_cmake_dir}")
    set(CMAKE_PREFIX_PATH "${CMAKE_PREFIX_PATH}" CACHE STRING
        "CMake prefix search path (auto-populated by FindQt.cmake)" FORCE)
    set(QT_AUTODETECTED_ROOT    "${_qt_found_root}"    CACHE PATH   "Auto-detected Qt kit root" FORCE)
    set(QT_AUTODETECTED_VERSION "${_qt_found_version}" CACHE STRING "Auto-detected Qt version"  FORCE)
else()
    message(STATUS "Qt6 not found under C:/Qt (or QTDIR/QT_ROOT). "
                   "Set QT_ROOT or CMAKE_PREFIX_PATH manually if Qt is installed elsewhere.")
endif()
