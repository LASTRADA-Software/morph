// SPDX-License-Identifier: Apache-2.0
//
// ledger's render smoke (spec 4 §7): the screen of the bundle mounted by the Qt Quick renderer, offscreen, in local
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

#include "ledger_screens.hpp"

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
    static std::array<char, 20> name{"ledger_render_smoke"};
    static std::array<char*, 2> values{name.data(), nullptr};
    return values.data();
}

}  // namespace

// Mutation: drop the screen from the bundle (the mounted list is empty).
TEST_CASE("ledger render smoke: local mode, every screen, offscreen", "[ledger][render-smoke]") {
    DbFixture fixture;
    LocalAppSource source{LocalApp{.name = "ledger",
                                   .bundle = &ledger::screens::bundle,
                                   .setUp = [](LocalSetup const&) { ledger::screens::prepareLocal(); }},
                          LocalSetup{.db = {}, .user = "alice"}};
    SmokeReport const report = runRenderSmoke(argCount(), argValues(), source);
    CHECK(report.mounted ==
          std::vector<std::string>{std::string{ledger::screens::kLogin}, std::string{ledger::screens::kLedger}});
    // Just before the verdict: unscoped messages attach to the next assertion.
    for (std::string const& failure : report.failures) {
        UNSCOPED_INFO(failure);
    }
    CHECK(report.clean());
}
