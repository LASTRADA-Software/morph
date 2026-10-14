# SPDX-License-Identifier: Apache-2.0
#
# bank on server-defined screens (spec 4 §5): bank_screens is Qt-free and is linked by the generic client
# for local mode and by the tests.

add_library(bank_screens STATIC bank_screens.cpp bank_screens.hpp)
target_include_directories(bank_screens PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# bank's models: over SQLite natively, in memory in a WebAssembly build (the browser has no ODBC).
if(TARGET bank_lib)
    target_link_libraries(bank_screens PUBLIC morph::morph bank_lib)
    # bank_lib links Lightweight PUBLIC with plain include directories; its headers do not meet this project's
    # warnings.
    get_target_property(_bank_lightweight_includes Lightweight::Lightweight INTERFACE_INCLUDE_DIRECTORIES)
    if(_bank_lightweight_includes)
        target_include_directories(bank_screens SYSTEM PUBLIC ${_bank_lightweight_includes})
    endif()
else()
    target_link_libraries(bank_screens PUBLIC morph::morph bank_memory_lib)
endif()
apply_warnings(bank_screens)
apply_bigobj(bank_screens)
if(DEFINED AF_SANITIZER)
    apply_sanitizers(bank_screens ${AF_SANITIZER})
endif()
if(AF_COVERAGE)
    apply_coverage(bank_screens)
endif()

# The screen harness lives in the ladder's testkit, so the screen tests are built with the ladder.
if(MORPH_BUILD_TESTS AND TARGET morph_ladder_testkit)
    include(Catch)
    add_executable(bank_screen_tests tests/test_bank_screens.cpp
        "${PROJECT_SOURCE_DIR}/examples/common/testkit/testkit_main.cpp")
    target_link_libraries(bank_screen_tests PRIVATE bank_screens morph::examples_common morph::ladder_testkit)
    target_include_directories(bank_screen_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../tests")
    target_compile_features(bank_screen_tests PRIVATE cxx_std_23)
    set_target_properties(bank_screen_tests PROPERTIES AUTOMOC ON)
    apply_warnings(bank_screen_tests)
    apply_bigobj(bank_screen_tests)
    if(DEFINED AF_SANITIZER)
        apply_sanitizers(bank_screen_tests ${AF_SANITIZER})
    endif()
    if(AF_COVERAGE)
        apply_coverage(bank_screen_tests)
    endif()
    # Labelled as a rung's are, so the ladder job's `ctest -L ladder` runs them.
    catch_discover_tests(bank_screen_tests DISCOVERY_MODE PRE_TEST TEST_PREFIX "bank.screens."
        PROPERTIES TIMEOUT 120 LABELS "ladder-bank" ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    if(TARGET morph_qt_quick)
        add_executable(bank_render_smoke tests/test_bank_render.cpp)
        target_link_libraries(bank_render_smoke PRIVATE
            bank_screens morph::examples_common morph::qt_quick morph::ladder_testkit morph_test_main)
        target_include_directories(bank_render_smoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../tests")
        apply_warnings(bank_render_smoke)
        if(DEFINED AF_SANITIZER)
            apply_sanitizers(bank_render_smoke ${AF_SANITIZER})
        endif()
        target_compile_features(bank_render_smoke PRIVATE cxx_std_23)
        apply_bigobj(bank_render_smoke)
        get_target_property(_bank_qt_core Qt6::Core IMPORTED_LOCATION)
        if(NOT _bank_qt_core)
            get_target_property(_bank_qt_core Qt6::Core IMPORTED_LOCATION_RELEASE)
        endif()
        set(_bank_dl_paths "")
        if(_bank_qt_core)
            cmake_path(GET _bank_qt_core PARENT_PATH _bank_dl_paths)
        endif()
        catch_discover_tests(bank_render_smoke DISCOVERY_MODE PRE_TEST DL_PATHS "${_bank_dl_paths}"
            TEST_PREFIX "bank.render."
            PROPERTIES TIMEOUT 120 LABELS "ladder-bank" ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()
endif()
