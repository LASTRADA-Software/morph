# SPDX-License-Identifier: Apache-2.0
#
# Built only when MORPH_BUILD_OFFLINE_SQLITE=ON (see top-level CMakeLists.txt);
# keeps the SQLite3 dependency out of the default morph_tests target.

add_executable(morph_offline_sqlite_tests
    test_sqlite_offline_queue.cpp
)

target_link_libraries(morph_offline_sqlite_tests
    PRIVATE
        morph::offline_sqlite
        Catch2::Catch2WithMain
)

apply_warnings(morph_offline_sqlite_tests)

include(Catch)
catch_discover_tests(morph_offline_sqlite_tests DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 120)
