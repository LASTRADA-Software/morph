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
    test_qt_websocket_adversarial.cpp
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
    PROPERTIES TIMEOUT 120
)

# ── QT_NO_SSL compile guard ───────────────────────────────────────────────────
# Configure-time proof that qt_websocket_backend.cpp/.hpp compile against an
# SSL-less Qt (QT_NO_SSL) -- the standard Qt-for-WebAssembly configuration --
# without needing an actual SSL-less Qt install to test against. Qt's own
# <QSslConfiguration> header self-guards on this exact macro
# (`#ifndef QT_NO_SSL ... #endif // QT_NO_SSL`) regardless of how the linked
# Qt was actually built, so forcing the macro against this project's normal
# SSL-enabled Qt reliably reproduces the same "QSslConfiguration is an
# incomplete type" failure an actual SSL-less Qt hits -- confirmed by
# compiling the pre-fix source this way, which fails with exactly that error.
# See qt_websocket_backend.hpp's class doc comment, "SSL-less Qt builds".
#
# get_target_property() rather than generator expressions in CMAKE_FLAGS:
# try_compile() runs its nested configure immediately, at ordinary configure
# time, before generator expressions would be evaluated (same rationale as
# the MORPH_REQUIRE_VETTED_HMAC / MORPH_CLIENT_ONLY guards in
# tests/CMakeLists.txt). Qt6::Core/Network/WebSockets are imported targets
# from find_package(Qt6 ...), so (unlike the plain INTERFACE `morph` target)
# they can be passed via LINK_LIBRARIES directly.
get_target_property(MORPH_QT_NO_SSL_GUARD_MORPH_INCLUDE_DIRS morph INTERFACE_INCLUDE_DIRECTORIES)
get_target_property(MORPH_QT_NO_SSL_GUARD_GLAZE_INCLUDE_DIRS glaze::glaze INTERFACE_INCLUDE_DIRECTORIES)
list(APPEND MORPH_QT_NO_SSL_GUARD_MORPH_INCLUDE_DIRS ${MORPH_QT_NO_SSL_GUARD_GLAZE_INCLUDE_DIRS})
string(REPLACE ";" "\\;" MORPH_QT_NO_SSL_GUARD_MORPH_INCLUDE_DIRS "${MORPH_QT_NO_SSL_GUARD_MORPH_INCLUDE_DIRS}")

unset(MORPH_QT_NO_SSL_GUARD_COMPILES CACHE)
try_compile(MORPH_QT_NO_SSL_GUARD_COMPILES
    SOURCES
        "${CMAKE_SOURCE_DIR}/src/qt/qt_websocket_backend.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../compile_checks/qt_no_ssl_main.cpp"
    LINK_LIBRARIES Qt6::Core Qt6::Network Qt6::WebSockets
    CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${MORPH_QT_NO_SSL_GUARD_MORPH_INCLUDE_DIRS}"
    CXX_STANDARD 23
    COMPILE_DEFINITIONS "-DQT_NO_SSL"
)
if(NOT MORPH_QT_NO_SSL_GUARD_COMPILES)
    message(FATAL_ERROR
        "QT_NO_SSL guard check failed: src/qt/qt_websocket_backend.cpp failed "
        "to compile with QT_NO_SSL defined. An SSL-less Qt build (including "
        "the standard Qt-for-WebAssembly configuration) must still compile "
        "this file -- see qt_websocket_backend.hpp's class doc comment, "
        "\"SSL-less Qt builds\".")
endif()
