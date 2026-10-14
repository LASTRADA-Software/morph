# SPDX-License-Identifier: Apache-2.0
#
# The generic client (spec 4 §3), built from one main for every frontend that is built: morph_client links no
# application and runs remote only; morph_examples_client links every example for local mode (`--app NAME`).

set(_morph_client_qt_quick 0)
set(_morph_client_tui 0)
if(TARGET morph_qt_quick)
    set(_morph_client_qt_quick 1)
endif()
if(TARGET morph_tui)
    set(_morph_client_tui 1)
endif()

foreach(_morph_client IN ITEMS morph_client morph_examples_client)
    if(_morph_client STREQUAL "morph_client")
        add_executable(${_morph_client} main.cpp local_apps.hpp no_local_apps.cpp)
    else()
        add_executable(${_morph_client} main.cpp local_apps.hpp example_apps.cpp)
        target_link_libraries(${_morph_client} PRIVATE gallery_screens)
        # Each ladder rung with screens runs in local mode too, when the ladder builds it.
        set(_morph_client_lims 0)
        if(TARGET ladder_lims_screens)
            target_link_libraries(${_morph_client} PRIVATE morph::ladder_lims_screens
                                  "$<LINK_LIBRARY:WHOLE_ARCHIVE,morph::ladder_lims_lib>")
            set(_morph_client_lims 1)
        endif()
        target_compile_definitions(${_morph_client} PRIVATE MORPH_CLIENT_HAS_LIMS=${_morph_client_lims})
    endif()
    target_link_libraries(${_morph_client} PRIVATE morph::examples_common)
    if(_morph_client_qt_quick)
        target_link_libraries(${_morph_client} PRIVATE morph::qt_quick)
    endif()
    if(_morph_client_tui)
        target_link_libraries(${_morph_client} PRIVATE morph::tui)
    endif()
    target_compile_definitions(${_morph_client} PRIVATE
        MORPH_CLIENT_HAS_QT_QUICK=${_morph_client_qt_quick}
        MORPH_CLIENT_HAS_TUI=${_morph_client_tui})
    apply_warnings(${_morph_client})
    if(DEFINED AF_SANITIZER)
        apply_sanitizers(${_morph_client} ${AF_SANITIZER})
    endif()
    if(AF_COVERAGE)
        apply_coverage(${_morph_client})
    endif()
endforeach()

# The client binary itself, end to end: the gallery opened in local mode under Qt Quick, offscreen, quits with 0 once
# its screen mounted without a problem.
if(MORPH_BUILD_TESTS AND _morph_client_qt_quick)
    add_test(NAME morph_examples_client.gallery_local_offscreen
             COMMAND morph_examples_client --app gallery --ui=qt --exit-after-mount)
    set_tests_properties(morph_examples_client.gallery_local_offscreen PROPERTIES
        TIMEOUT 120 LABELS "gallery" ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
