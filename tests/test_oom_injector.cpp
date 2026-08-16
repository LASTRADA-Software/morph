// SPDX-License-Identifier: Apache-2.0
//
// Self-test for morph::testkit::OomInjector (oom_injector.hpp), the same way
// scripts/test_check_deprecated_markers.sh and
// scripts/test_check_test_type_names.sh self-test their own checkers before
// anything else relies on them: a fault-injection seam nobody tests reports
// "it works" whether or not it actually fires the failure it claims to.

#include <catch2/catch_test_macros.hpp>
#include <new>
#include <stdexcept>
#include <string>

#include "oom_injector.hpp"

TEST_CASE("OomInjector: an allocation below the threshold succeeds normally", "[testkit][oom-injector]") {
    morph::testkit::OomInjector inject{/*minSize=*/1024};
    // A short string is well within SSO on every supported STL; even if it
    // weren't, its heap buffer is far below the 1024-byte threshold above.
    std::string small = "short";
    CHECK(small == "short");
}

TEST_CASE("OomInjector: the first allocation at or above the threshold throws std::bad_alloc",
          "[testkit][oom-injector]") {
    // 256 bytes defeats SSO on every supported standard library (libstdc++,
    // libc++, and MSVC's implementations all inline at most ~23 bytes).
    std::string source(256, 'x');
    morph::testkit::OomInjector inject{/*minSize=*/128};
    std::string target;
    REQUIRE_THROWS_AS(target = source, std::bad_alloc);
}

TEST_CASE("OomInjector: is one-shot -- only the first matching allocation fails, not every later one",
          "[testkit][oom-injector]") {
    std::string source(256, 'x');
    morph::testkit::OomInjector inject{/*minSize=*/128};
    std::string first;
    REQUIRE_THROWS_AS(first = source, std::bad_alloc);
    // The injector fired once and disarmed itself; a second copy of the same
    // large string must now succeed normally, in the same scope.
    std::string second;
    REQUIRE_NOTHROW(second = source);
    CHECK(second == source);
}

TEST_CASE("OomInjector: disarms when its scope ends, even if the matching allocation never happened",
          "[testkit][oom-injector]") {
    {
        morph::testkit::OomInjector inject{/*minSize=*/128};
        // Deliberately do not perform any allocation >= 128 bytes here.
    }
    // The armed-but-never-triggered injector must not leak into later code.
    std::string source(256, 'x');
    std::string target;
    REQUIRE_NOTHROW(target = source);
    CHECK(target == source);
}

TEST_CASE("OomInjector: two instances on the same thread cannot be active at once", "[testkit][oom-injector]") {
    morph::testkit::OomInjector outer{/*minSize=*/128};
    REQUIRE_THROWS_AS((morph::testkit::OomInjector{/*minSize=*/128}), std::logic_error);
}
