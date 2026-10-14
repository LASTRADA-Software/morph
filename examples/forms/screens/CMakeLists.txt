# SPDX-License-Identifier: Apache-2.0
#
# The forms demo on server-defined screens (spec 4 §5): forms_screens is Qt-free and is linked by the generic client
# for local mode and by the tests.

add_library(forms_screens STATIC forms_screens.cpp forms_screens.hpp)
target_include_directories(forms_screens PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(forms_screens PUBLIC morph::morph)
apply_warnings(forms_screens)
apply_bigobj(forms_screens)
if(DEFINED AF_SANITIZER)
    apply_sanitizers(forms_screens ${AF_SANITIZER})
endif()
if(AF_COVERAGE)
    apply_coverage(forms_screens)
endif()

# The screen harness lives in the ladder's testkit, so the screen tests are built with the ladder.
if(MORPH_BUILD_TESTS AND TARGET morph_ladder_testkit)
    include(Catch)
    add_executable(forms_screen_tests tests/test_forms_screens.cpp
        "${PROJECT_SOURCE_DIR}/examples/common/testkit/testkit_main.cpp")
    target_link_libraries(forms_screen_tests PRIVATE forms_screens morph::examples_common morph::ladder_testkit)
    target_compile_features(forms_screen_tests PRIVATE cxx_std_23)
    set_target_properties(forms_screen_tests PROPERTIES AUTOMOC ON)
    apply_warnings(forms_screen_tests)
    apply_bigobj(forms_screen_tests)
    if(DEFINED AF_SANITIZER)
        apply_sanitizers(forms_screen_tests ${AF_SANITIZER})
    endif()
    if(AF_COVERAGE)
        apply_coverage(forms_screen_tests)
    endif()
    catch_discover_tests(forms_screen_tests DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 120 LABELS "forms")

    if(TARGET morph_qt_quick)
        add_executable(forms_render_smoke tests/test_forms_render.cpp)
        target_link_libraries(forms_render_smoke PRIVATE
            forms_screens morph::examples_common morph::qt_quick morph::ladder_testkit morph_test_main)
        apply_warnings(forms_render_smoke)
        if(DEFINED AF_SANITIZER)
            apply_sanitizers(forms_render_smoke ${AF_SANITIZER})
        endif()
        target_compile_features(forms_render_smoke PRIVATE cxx_std_23)
        apply_bigobj(forms_render_smoke)
        get_target_property(_forms_qt_core Qt6::Core IMPORTED_LOCATION)
        if(NOT _forms_qt_core)
            get_target_property(_forms_qt_core Qt6::Core IMPORTED_LOCATION_RELEASE)
        endif()
        set(_forms_dl_paths "")
        if(_forms_qt_core)
            cmake_path(GET _forms_qt_core PARENT_PATH _forms_dl_paths)
        endif()
        catch_discover_tests(forms_render_smoke DISCOVERY_MODE PRE_TEST DL_PATHS "${_forms_dl_paths}"
            PROPERTIES TIMEOUT 120 LABELS "forms" ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()
endif()
