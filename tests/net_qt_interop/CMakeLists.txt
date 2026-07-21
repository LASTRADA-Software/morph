add_executable(morph_net_qt_interop_tests
    test_net_qt_interop.cpp
)

target_link_libraries(morph_net_qt_interop_tests
    PRIVATE
        morph::net
        morph_qt_impl
        Catch2::Catch2  # custom main() owns QCoreApplication, matching tests/qt
)

set_target_properties(morph_net_qt_interop_tests PROPERTIES AUTOMOC ON)
apply_warnings(morph_net_qt_interop_tests)

include(Catch)

get_target_property(_qt_core_dll Qt6::Core IMPORTED_LOCATION)
cmake_path(GET _qt_core_dll PARENT_PATH _qt_bin_dir)

catch_discover_tests(morph_net_qt_interop_tests
    DISCOVERY_MODE POST_BUILD
    DL_PATHS "${_qt_bin_dir}"
    PROPERTIES TIMEOUT 120
)
