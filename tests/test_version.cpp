// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <morph/version.hpp>

// Cross-checks the checked-in include/morph/version.hpp constants against
// CMakeLists.txt's `project(morph VERSION ...)` field, which
// tests/CMakeLists.txt forwards as MORPH_CMAKE_VERSION_{MAJOR,MINOR,PATCH}
// compile definitions (see docs/spec/VERSIONING.md, "Current version"). A
// mismatch here means someone bumped one without the other.
#ifndef MORPH_CMAKE_VERSION_MAJOR
#error "MORPH_CMAKE_VERSION_MAJOR is not defined - tests/CMakeLists.txt must forward PROJECT_VERSION_MAJOR"
#endif

static_assert(morph::version::kMajor == MORPH_CMAKE_VERSION_MAJOR,
              "morph::version::kMajor (version.hpp) has drifted from CMakeLists.txt's project(VERSION ...)");
static_assert(morph::version::kMinor == MORPH_CMAKE_VERSION_MINOR,
              "morph::version::kMinor (version.hpp) has drifted from CMakeLists.txt's project(VERSION ...)");
static_assert(morph::version::kPatch == MORPH_CMAKE_VERSION_PATCH,
              "morph::version::kPatch (version.hpp) has drifted from CMakeLists.txt's project(VERSION ...)");

static_assert(morph::version::kMajor == 0);
static_assert(morph::version::kMinor == 1);
static_assert(morph::version::kPatch == 0);
static_assert(MORPH_VERSION == MORPH_MAKE_VERSION(0, 1, 0));

TEST_CASE("morph::version::kString matches the packed integer components", "[version]") {
    REQUIRE(morph::version::kString == "0.1.0");
}

TEST_CASE("MORPH_MAKE_VERSION packs components so later versions compare greater", "[version]") {
    REQUIRE(MORPH_MAKE_VERSION(1, 2, 3) > MORPH_MAKE_VERSION(1, 2, 2));
    REQUIRE(MORPH_MAKE_VERSION(1, 2, 3) > MORPH_MAKE_VERSION(1, 1, 99));
    REQUIRE(MORPH_MAKE_VERSION(2, 0, 0) > MORPH_MAKE_VERSION(1, 99, 99));
}
