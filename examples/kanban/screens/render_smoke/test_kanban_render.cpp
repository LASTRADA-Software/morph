// SPDX-License-Identifier: Apache-2.0
//
// kanban's render smoke (spec 4 §7): every screen of the bundle mounted by the Qt Quick renderer, offscreen, in local
// mode over the rung's test database. A refused document, a mount error, an interpreter diagnostic or a Qt warning
// fails it.

#include <app/app_source.hpp>
#include <array>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <testkit/db_fixture.hpp>
#include <testkit/render_smoke.hpp>
#include <vector>

#include "kanban_screens.hpp"

namespace {

using morph::examples::LocalApp;
using morph::examples::LocalAppSource;
using morph::examples::LocalSetup;
using morph::examples::testing::runRenderSmoke;
using morph::examples::testing::SmokeReport;
using morph::ladder::testkit::DbFixture;

int& argCount() {
    static int count = 1;
    return count;
}

char** argValues() {
    static std::array<char, 20> name{"kanban_render_smoke"};
    static std::array<char*, 2> values{name.data(), nullptr};
    return values.data();
}

}  // namespace

// Mutation: drop the screen from the bundle (the mounted list is empty).
TEST_CASE("kanban render smoke: local mode, every screen, offscreen", "[kanban][render-smoke]") {
    DbFixture fixture;
    LocalAppSource source{LocalApp{.name = "kanban",
                                   .bundle = &kanban::screens::bundle,
                                   .setUp = [](LocalSetup const&) { kanban::screens::prepareLocal(); }},
                          LocalSetup{.db = {}, .user = "alice"}};
    SmokeReport const report = runRenderSmoke(argCount(), argValues(), source);
    CHECK(report.mounted == std::vector<std::string>{std::string{kanban::screens::kLogin},
                                                     std::string{kanban::screens::kProjects},
                                                     std::string{kanban::screens::kBoard}});
    // Just before the verdict: unscoped messages attach to the next assertion.
    for (std::string const& failure : report.failures) {
        UNSCOPED_INFO(failure);
    }
    CHECK(report.clean());
}
