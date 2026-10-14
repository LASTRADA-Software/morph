// SPDX-License-Identifier: Apache-2.0
//
// bank's render smoke (spec 4 §7): every screen of the bundle mounted by the Qt Quick renderer, offscreen,
// in local mode, signed in as a customer with no accounts. A refused document, a mount error, an interpreter
// diagnostic or a Qt warning fails it.

#include <app/app_source.hpp>
#include <array>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <testkit/render_smoke.hpp>
#include <vector>

#include "bank_screens.hpp"
#include "bank_test_support.hpp"

namespace {

using morph::examples::LocalApp;
using morph::examples::LocalAppSource;
using morph::examples::LocalSetup;
using morph::examples::testing::runRenderSmoke;
using morph::examples::testing::SmokeReport;

int& argCount() {
    static int count = 1;
    return count;
}

char** argValues() {
    static std::array<char, 18> name{"bank_render_smoke"};
    static std::array<char*, 2> values{name.data(), nullptr};
    return values.data();
}

}  // namespace

// Mutation: drop a screen from the bundle (the mounted list misses it).
TEST_CASE("bank render smoke: local mode, every screen, offscreen", "[bank][render-smoke]") {
    bank::testing::ensurePrincipal("smoke");
    LocalAppSource source{LocalApp{.name = "bank",
                                   .bundle = &bank::screens::bundle,
                                   .setUp = [](LocalSetup const&) { bank::testing::ensureDatabase(); }},
                          LocalSetup{.db = {}, .user = "smoke"}};
    SmokeReport const report = runRenderSmoke(argCount(), argValues(), source);
    CHECK(report.mounted ==
          std::vector<std::string>{std::string{bank::screens::kLogin}, std::string{bank::screens::kAccounts},
                                   std::string{bank::screens::kMove}, std::string{bank::screens::kCards},
                                   std::string{bank::screens::kPayees}, std::string{bank::screens::kLoans}});
    // Just before the verdict: unscoped messages attach to the next assertion.
    for (std::string const& failure : report.failures) {
        UNSCOPED_INFO(failure);
    }
    CHECK(report.clean());
}
