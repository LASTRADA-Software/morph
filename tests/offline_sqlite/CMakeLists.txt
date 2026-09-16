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
        morph_test_main
)

apply_warnings(morph_offline_sqlite_tests)

# Was missing entirely, so sqlite_offline_queue.hpp's dedicated suite was never
# instrumented and the file's 57.04% -- the worst in the library -- was measured
# with its own tests absent from the report (morph#403).
if(AF_COVERAGE)
    apply_coverage(morph_offline_sqlite_tests)
endif()

# Was missing for the same reason, with a sharper consequence (morph#542): the
# `clang-asan` leg turns MORPH_BUILD_OFFLINE_SQLITE on precisely because the
# SQLite queue "is where the memory/threading/UB risk actually lives -- a C
# API", then built this suite with zero sanitizer flags and ran it. The job was
# green and had checked nothing.
if(DEFINED AF_SANITIZER)
    apply_sanitizers(morph_offline_sqlite_tests ${AF_SANITIZER})
endif()

include(Catch)
catch_discover_tests(morph_offline_sqlite_tests DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 120)
