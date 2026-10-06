add_executable(morph_tui_tests
    test_tui_layout.cpp
    test_tui_widgets.cpp
    test_tui_containers.cpp
    test_tui_fields.cpp
    test_tui_lists.cpp
    test_tui_dialog.cpp
    test_tui_table.cpp
    test_tui_loop_executor.cpp
    test_tui_backend.cpp
    test_tui_drag.cpp
    test_tui_scheduler.cpp
)

target_link_libraries(morph_tui_tests
    PRIVATE
        morph::tui
        morph_test_main
)
# The widgets, the layout solver and the session are private to morph_tui
# (src/tui/); their tests include them as "tui/<name>.hpp".
target_include_directories(morph_tui_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)

apply_warnings(morph_tui_tests)

if(DEFINED AF_SANITIZER)
    apply_sanitizers(morph_tui_tests ${AF_SANITIZER})
endif()
if(AF_COVERAGE)
    apply_coverage(morph_tui_tests)
endif()

include(Catch)
# LABELS "tui" is what makes `ctest -L tui` select this suite: Catch2 tags are
# not ctest labels (see tests/net/CMakeLists.txt).
catch_discover_tests(morph_tui_tests DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 120 LABELS "tui")
