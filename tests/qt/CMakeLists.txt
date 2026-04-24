# ── Helper binaries for the process-separation tests ────────────────────────
# qt_test_server / qt_test_client are spawned by the unit test as separate
# processes so the WebSocket layer is exercised across a real OS boundary.

add_executable(qt_test_server qt_test_server_main.cpp qt_test_models.hpp)
target_link_libraries(qt_test_server PRIVATE morph_qt_impl)
apply_warnings(qt_test_server)

add_executable(qt_test_client qt_test_client_main.cpp qt_test_models.hpp)
target_link_libraries(qt_test_client PRIVATE morph_qt_impl)
apply_warnings(qt_test_client)

# ── Catch2 tests ─────────────────────────────────────────────────────────────

add_executable(morph_qt_tests
    test_qt_websocket.cpp
)

target_link_libraries(morph_qt_tests
    PRIVATE
        morph_qt_impl
        Catch2::Catch2  # we provide our own main() to control QCoreApplication lifetime
)

set_target_properties(morph_qt_tests PROPERTIES AUTOMOC ON)
apply_warnings(morph_qt_tests)

# Make sure the helpers exist before the test binary tries to spawn them.
add_dependencies(morph_qt_tests qt_test_server qt_test_client)

target_compile_definitions(morph_qt_tests PRIVATE
    TESTS_CERTS_DIR="${CMAKE_SOURCE_DIR}/tests/certs"
    QT_TEST_SERVER_BIN="$<TARGET_FILE:qt_test_server>"
    QT_TEST_CLIENT_BIN="$<TARGET_FILE:qt_test_client>"
)

include(Catch)

# Qt DLLs must be findable at both test discovery and run time on Windows.
# DL_PATHS prepends to PATH (Windows) / LD_LIBRARY_PATH (Linux) / DYLD_LIBRARY_PATH (macOS).
get_target_property(_qt_core_dll Qt6::Core IMPORTED_LOCATION)
cmake_path(GET _qt_core_dll PARENT_PATH _qt_bin_dir)

catch_discover_tests(morph_qt_tests
    DISCOVERY_MODE POST_BUILD
    DL_PATHS "${_qt_bin_dir}"
)
