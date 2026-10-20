// SPDX-License-Identifier: Apache-2.0
//
// The forms demo's render smoke (spec 4 §7): every screen of the bundle mounted by the Qt Quick renderer, offscreen,
// in local mode. A refused document, a mount error, an interpreter diagnostic or a Qt warning fails it.

#include <array>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <morph/client/source.hpp>
#include <string>
#include <testkit/render_smoke.hpp>
#include <vector>

#include "forms_screens.hpp"

namespace {

using morph::client::LocalApp;
using morph::client::LocalSetup;
using morph::client::LocalSource;
using morph::examples::testing::runRenderSmoke;
using morph::examples::testing::SmokeReport;

int& argCount() {
    static int count = 1;
    return count;
}

char** argValues() {
    static std::array<char, 19> name{"forms_render_smoke"};
    static std::array<char*, 2> values{name.data(), nullptr};
    return values.data();
}

}  // namespace

// Mutation: drop a screen from the bundle (the mounted list misses it).
TEST_CASE("forms render smoke: local mode, every screen, offscreen", "[forms][render-smoke]") {
    LocalSource source{LocalApp{.name = "forms", .bundle = &lab::screens::bundle, .setUp = {}},
                       LocalSetup{.db = {}, .user = "tech"}};
    SmokeReport const report = runRenderSmoke(argCount(), argValues(), source);
    CHECK(report.mounted ==
          std::vector<std::string>{std::string{lab::screens::kDensity}, std::string{lab::screens::kMeasure},
                                   std::string{lab::screens::kIntake}, std::string{lab::screens::kSamples}});
    // Just before the verdict: unscoped messages attach to the next assertion.
    for (std::string const& failure : report.failures) {
        UNSCOPED_INFO(failure);
    }
    CHECK(report.clean());
}
