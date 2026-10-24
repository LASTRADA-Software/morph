# SPDX-License-Identifier: Apache-2.0
#
# The LIMS gallery: ten laboratory screens from design mockups, as ui/1 documents, with no backend.
# lims_gallery_screens is Qt-free and is linked by the generic client for local mode and by the tests.
# A design is one file under src/screens/ and one test under tests/; add both to the lists below.

add_library(lims_gallery_screens STATIC
    src/lims_gallery.cpp
    src/screens/designs.cpp
    src/screens/index.cpp
    src/screens/esignature.cpp
    src/screens/layout.hpp
    include/lims_gallery/lims_gallery.hpp
    include/lims_gallery/screens.hpp)
target_include_directories(lims_gallery_screens PUBLIC include)
target_link_libraries(lims_gallery_screens PUBLIC morph::morph morph::client)
apply_warnings(lims_gallery_screens)
if(DEFINED AF_SANITIZER)
    apply_sanitizers(lims_gallery_screens ${AF_SANITIZER})
endif()
if(AF_COVERAGE)
    apply_coverage(lims_gallery_screens)
endif()

if(MORPH_BUILD_TESTS)
    include(Catch)
    add_executable(lims_gallery_tests
        tests/gallery_harness.hpp
        tests/test_lims_gallery.cpp
        tests/test_esignature.cpp)
    target_link_libraries(lims_gallery_tests PRIVATE lims_gallery_screens morph_test_main)
    # The screen harness is the ladder testkit's.
    target_include_directories(lims_gallery_tests PRIVATE ${PROJECT_SOURCE_DIR}/examples/common)
    apply_warnings(lims_gallery_tests)
    if(DEFINED AF_SANITIZER)
        apply_sanitizers(lims_gallery_tests ${AF_SANITIZER})
    endif()
    if(AF_COVERAGE)
        apply_coverage(lims_gallery_tests)
    endif()
    catch_discover_tests(lims_gallery_tests DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 120 LABELS "lims-gallery")

    # The render smoke runs the Qt Quick renderer offscreen.
    if(TARGET morph_qt_quick)
        add_executable(lims_gallery_render_smoke tests/test_lims_gallery_render.cpp)
        target_link_libraries(lims_gallery_render_smoke PRIVATE lims_gallery_screens morph::qt_quick morph_test_main)
        target_include_directories(lims_gallery_render_smoke PRIVATE ${PROJECT_SOURCE_DIR}/examples/common)
        apply_warnings(lims_gallery_render_smoke)
        if(DEFINED AF_SANITIZER)
            apply_sanitizers(lims_gallery_render_smoke ${AF_SANITIZER})
        endif()
        get_target_property(_lims_gallery_qt_core Qt6::Core IMPORTED_LOCATION)
        if(NOT _lims_gallery_qt_core)
            get_target_property(_lims_gallery_qt_core Qt6::Core IMPORTED_LOCATION_RELEASE)
        endif()
        set(_lims_gallery_dl_paths "")
        if(_lims_gallery_qt_core)
            cmake_path(GET _lims_gallery_qt_core PARENT_PATH _lims_gallery_dl_paths)
        endif()
        catch_discover_tests(lims_gallery_render_smoke
            DISCOVERY_MODE PRE_TEST
            DL_PATHS "${_lims_gallery_dl_paths}"
            PROPERTIES TIMEOUT 120 LABELS "lims-gallery" ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()
endif()
