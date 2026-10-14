# SPDX-License-Identifier: Apache-2.0
#
# The gallery (spec 1 §8): one screen that uses every node kind, its model, and the server that serves both.
# gallery_screens is Qt-free and is linked by the server, by the generic client for local mode, and by the tests.

add_library(gallery_screens STATIC src/gallery.cpp include/gallery/gallery.hpp include/gallery/screens.hpp)
target_include_directories(gallery_screens PUBLIC include)
target_link_libraries(gallery_screens PUBLIC morph::morph morph::examples_common)
apply_warnings(gallery_screens)
if(DEFINED AF_SANITIZER)
    apply_sanitizers(gallery_screens ${AF_SANITIZER})
endif()
if(AF_COVERAGE)
    apply_coverage(gallery_screens)
endif()

# Servers stay on Qt (spec 4 §1).
if(TARGET morph_qt_impl)
    add_executable(gallery_server src/server/main.cpp)
    target_link_libraries(gallery_server PRIVATE gallery_screens morph::qt morph_qt_impl)
    apply_warnings(gallery_server)
    if(DEFINED AF_SANITIZER)
        apply_sanitizers(gallery_server ${AF_SANITIZER})
    endif()
endif()

if(MORPH_BUILD_TESTS)
    include(Catch)
    add_executable(gallery_tests tests/test_gallery.cpp)
    target_link_libraries(gallery_tests PRIVATE gallery_screens morph_test_main)
    apply_warnings(gallery_tests)
    if(DEFINED AF_SANITIZER)
        apply_sanitizers(gallery_tests ${AF_SANITIZER})
    endif()
    if(AF_COVERAGE)
        apply_coverage(gallery_tests)
    endif()
    catch_discover_tests(gallery_tests DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 120 LABELS "gallery")

    # The render smoke runs the Qt Quick renderer offscreen; it needs the Qt WebSocket server for its remote case.
    if(TARGET morph_qt_quick AND TARGET morph_qt_impl)
        add_executable(gallery_render_smoke tests/test_gallery_render.cpp)
        target_link_libraries(gallery_render_smoke PRIVATE gallery_screens morph::qt_quick morph::qt morph_qt_impl
                                                           morph_test_main)
        apply_warnings(gallery_render_smoke)
        if(DEFINED AF_SANITIZER)
            apply_sanitizers(gallery_render_smoke ${AF_SANITIZER})
        endif()
        get_target_property(_gallery_qt_core Qt6::Core IMPORTED_LOCATION)
        if(NOT _gallery_qt_core)
            get_target_property(_gallery_qt_core Qt6::Core IMPORTED_LOCATION_RELEASE)
        endif()
        set(_gallery_dl_paths "")
        if(_gallery_qt_core)
            cmake_path(GET _gallery_qt_core PARENT_PATH _gallery_dl_paths)
        endif()
        catch_discover_tests(gallery_render_smoke
            DISCOVERY_MODE PRE_TEST
            DL_PATHS "${_gallery_dl_paths}"
            PROPERTIES TIMEOUT 120 LABELS "gallery" ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()
endif()
