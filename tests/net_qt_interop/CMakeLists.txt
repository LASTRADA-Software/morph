add_executable(morph_net_qt_interop_tests
    test_net_qt_interop.cpp
)

target_link_libraries(morph_net_qt_interop_tests
    PRIVATE
        morph::net
        morph_qt_impl
        Catch2::Catch2  # custom main() owns QCoreApplication, matching tests/qt
        morph_test_log_level
)

set_target_properties(morph_net_qt_interop_tests PROPERTIES AUTOMOC ON)
apply_warnings(morph_net_qt_interop_tests)

# Was missing entirely: this suite drives morph::net against Qt's transport and
# was never instrumented (morph#403).
if(AF_COVERAGE)
    apply_coverage(morph_net_qt_interop_tests)
endif()

# Likewise missing entirely (morph#542). This suite drives morph::net -- raw
# sockets, an I/O thread and a hand-rolled frame reader -- against Qt's
# transport, which is the part of the tree a sanitizer has most to say about.
if(DEFINED AF_SANITIZER)
    apply_sanitizers(morph_net_qt_interop_tests ${AF_SANITIZER})
endif()

include(Catch)

get_target_property(_qt_core_dll Qt6::Core IMPORTED_LOCATION)
cmake_path(GET _qt_core_dll PARENT_PATH _qt_bin_dir)

catch_discover_tests(morph_net_qt_interop_tests
    DISCOVERY_MODE POST_BUILD
    DL_PATHS "${_qt_bin_dir}"
    PROPERTIES TIMEOUT 120
)
