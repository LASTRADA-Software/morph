# SPDX-License-Identifier: Apache-2.0
#
# morph::qt_quick's suites (docs/spec/qt_quick/backend.md, "Tests"). Two executables, because a process holds one
# QGuiApplication at a time: morph_qt_quick_tests owns one in main() for the backend, the conformance cases, the
# interpreter-driven cases and the components; morph_qt_quick_frontend_tests owns none, because
# qt_quick::Frontend::run creates its own.

find_package(Qt6 REQUIRED COMPONENTS Test QuickTest)

add_executable(morph_qt_quick_tests
    qt_quick_harness.hpp
    test_qt_quick_main.cpp
    test_qt_quick_widgets.cpp
    test_qt_quick_chart.cpp
    test_qt_quick_conformance.cpp
    test_qt_quick_containers.cpp
    test_qt_quick_overlays.cpp
    test_qt_quick_table.cpp
    test_qt_quick_interpreter.cpp
    test_qt_quick_components.cpp
    test_qt_quick_scheduler.cpp
    test_qt_quick_theme.cpp
    test_qt_quick_automation.cpp
    test_qt_quick_inspector.cpp
)
target_link_libraries(morph_qt_quick_tests
    PRIVATE
        morph::qt_quick
        Qt6::Test
        Qt6::QuickTest
        # Our own main() owns the QGuiApplication, so this cannot link
        # morph_test_main; morph_test_log_level carries the --log-level helper.
        Catch2::Catch2
        morph_test_log_level
)
# The widgets and the context are private to morph_qt_quick (src/qt_quick/); the tests include them as
# "qt_quick/<name>.hpp", and the interpreter harness from tests/.
target_include_directories(morph_qt_quick_tests PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/tests)
set_target_properties(morph_qt_quick_tests PROPERTIES AUTOMOC ON)
apply_warnings(morph_qt_quick_tests)

add_executable(morph_qt_quick_frontend_tests test_qt_quick_frontend.cpp)
target_link_libraries(morph_qt_quick_frontend_tests PRIVATE morph::qt_quick Catch2::Catch2 morph_test_log_level)
apply_warnings(morph_qt_quick_frontend_tests)

foreach(_morph_qq_tests IN ITEMS morph_qt_quick_tests morph_qt_quick_frontend_tests)
    if(DEFINED AF_SANITIZER)
        apply_sanitizers(${_morph_qq_tests} ${AF_SANITIZER})
    endif()
    if(AF_COVERAGE)
        apply_coverage(${_morph_qq_tests})
    endif()
endforeach()
unset(_morph_qq_tests)

include(Catch)
# Qt's libraries must be findable at discovery and at run time on Windows; DL_PATHS prepends to PATH /
# LD_LIBRARY_PATH / DYLD_LIBRARY_PATH. LABELS "qt_quick" is what makes `ctest -L qt_quick` select these suites.
get_target_property(_morph_qq_core_dll Qt6::Core IMPORTED_LOCATION)
cmake_path(GET _morph_qq_core_dll PARENT_PATH _morph_qq_bin_dir)
foreach(_morph_qq_tests IN ITEMS morph_qt_quick_tests morph_qt_quick_frontend_tests)
    catch_discover_tests(${_morph_qq_tests}
        DISCOVERY_MODE PRE_TEST
        DL_PATHS "${_morph_qq_bin_dir}"
        PROPERTIES TIMEOUT 120 LABELS "qt_quick" ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    )
endforeach()
unset(_morph_qq_tests)
unset(_morph_qq_core_dll)
unset(_morph_qq_bin_dir)

# The chart's Canvas paints at the device pixel ratio: its cases run again at a ratio of 2, where a 1 px line must
# still be one solid line of device pixels in its colour.
add_test(NAME qt_quick_chart_scale_2 COMMAND morph_qt_quick_tests "[chart]")
set_tests_properties(qt_quick_chart_scale_2 PROPERTIES LABELS "qt_quick" TIMEOUT 120
                     ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=2")

# A dashed outline paints on a Canvas at the device pixel ratio: its case runs again at a ratio of 2, where the dashes
# must still be broken by gaps.
add_test(NAME qt_quick_dashed_scale_2 COMMAND morph_qt_quick_tests "[dashed]")
set_tests_properties(qt_quick_dashed_scale_2 PROPERTIES LABELS "qt_quick" TIMEOUT 120
                     ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=2")

# Every QML file the renderer compiles passes qmllint with no warning: the MorphUi components the backend
# instantiates, through the module's own lint target, which knows its import paths.
add_test(NAME qt_quick_qmllint
         COMMAND ${CMAKE_COMMAND} -DBUILD_DIR=${CMAKE_BINARY_DIR} -DTARGET=morph_qt_quick_qml_qmllint
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/run_qmllint.cmake)
set_tests_properties(qt_quick_qmllint PROPERTIES LABELS "qt_quick" TIMEOUT 300)
