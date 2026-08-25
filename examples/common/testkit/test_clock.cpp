// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include "clock.hpp"

using namespace std::chrono_literals;

TEST_CASE("morph::ladder::now() reads the real wall clock with no override installed", "[ladder][testkit][clock]") {
    const auto before = ::morph::time::DateTime::now();
    const auto observed = morph::ladder::now();
    const auto after = ::morph::time::DateTime::now();
    REQUIRE(observed.hasValue());
    REQUIRE(*observed >= before);
    REQUIRE(*observed <= after);
}

TEST_CASE("ScopedClockOverride freezes now() at the given instant", "[ladder][testkit][clock]") {
    const ::morph::time::DateTime frozen{std::chrono::year{2030}, std::chrono::month{1},   std::chrono::day{1},
                                         std::chrono::hours{0},   std::chrono::minutes{0}, std::chrono::seconds{0}};
    {
        morph::ladder::ScopedClockOverride guard{frozen};
        REQUIRE(*morph::ladder::now() == frozen);
        REQUIRE(*morph::ladder::now() == frozen);  // stable across repeated reads, not a one-shot
    }
    REQUIRE(*morph::ladder::now() != frozen);  // restored to the real clock after the guard's scope
}

TEST_CASE("ScopedClockOverride freezes now() at a pre-1970 instant", "[ladder][testkit][clock]") {
    // A pre-epoch instant's epoch-ms is negative. The disabled sentinel used
    // to be -1, so any negative override (including this one) fell through
    // to the real wall clock instead of the frozen instant, silently. The
    // sentinel is now INT64_MIN, which no real DateTime a test constructs can
    // ever equal.
    const ::morph::time::DateTime frozen{std::chrono::year{1965}, std::chrono::month{3},   std::chrono::day{12},
                                         std::chrono::hours{0},   std::chrono::minutes{0}, std::chrono::seconds{0}};
    REQUIRE(frozen.value.time_since_epoch().count() < 0);
    morph::ladder::ScopedClockOverride guard{frozen};
    REQUIRE(*morph::ladder::now() == frozen);
}

TEST_CASE("ScopedClockOverride nests: the inner guard wins, the outer resumes on inner's destruction",
          "[ladder][testkit][clock]") {
    const ::morph::time::DateTime outer{std::chrono::year{2030}, std::chrono::month{1},   std::chrono::day{1},
                                        std::chrono::hours{0},   std::chrono::minutes{0}, std::chrono::seconds{0}};
    const ::morph::time::DateTime inner{std::chrono::year{2031}, std::chrono::month{6},   std::chrono::day{15},
                                        std::chrono::hours{12},  std::chrono::minutes{0}, std::chrono::seconds{0}};
    morph::ladder::ScopedClockOverride outerGuard{outer};
    REQUIRE(*morph::ladder::now() == outer);
    {
        morph::ladder::ScopedClockOverride innerGuard{inner};
        REQUIRE(*morph::ladder::now() == inner);
    }
    REQUIRE(*morph::ladder::now() == outer);
}
