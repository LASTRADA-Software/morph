// SPDX-License-Identifier: Apache-2.0
//
// The application shell on the screen harness: the scope switcher hands every screen its context.

#include <algorithm>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <expected>
#include <memory>
#include <morph/session/session_auth.hpp>
#include <morph/ui/author.hpp>
#include <morph/ui/service.hpp>
#include <morph/ui/shell_parts.hpp>
#include <string>
#include <string_view>
#include <testkit/db_fixture.hpp>
#include <testkit/screen_harness.hpp>
#include <utility>
#include <vector>

#include "lims/app/lims_authorizer.hpp"
#include "lims/ledger/dto/account_dto.hpp"
#include "lims/ledger/models/ledger_model.hpp"
#include "lims/org/dto/auth_dto.hpp"
#include "lims/org/models/auth_model.hpp"
#include "lims/org/models/org_model.hpp"
#include "lims_screens.hpp"
#include "lims_shell_parts.hpp"
#include "lims_test_support.hpp"
#include "scenario/org_scenario.hpp"
#include "scenario/scenario_client.hpp"
#include "screen_test_support.hpp"
#include "shell_probe_bundles.hpp"
#include "shell_screens.hpp"

namespace {

namespace ui = morph::ui::author;
using lims::shelltest::kProbeHome;
using lims::shelltest::kProbeLogin;
using lims::shelltest::kScoped;
using lims::shelltest::modulesBundle;
using lims::shelltest::scopedBundle;
using morph::examples::testing::RigMode;
using morph::examples::testing::ScreenHarness;

/// @brief Seeds the companies the shell switches between: the seeded one with one account, and "Osaka branch" with
/// two.
void seedAccounts() {
    lims::test::grant("acc", lims::LimsRole::Accountant);
    lims::test::grant("sam", lims::LimsRole::Supervisor);
    lims::CompanyId osaka;
    {
        lims::test::ScopedPrincipal const sam{"sam"};
        osaka = lims::OrgModel{}
                    .execute(lims::CreateCompany{.name = "Osaka branch",
                                                 .baseCurrency = lims::CurrencyCode::JPY,
                                                 .fiscalYearStart = lims::Month::April})
                    .id;
        lims::OrgModel{}.execute(
            lims::AssignRole{.principal = "acc", .role = lims::LimsRole::Accountant, .company = osaka, .site = {}});
    }
    lims::test::ScopedPrincipal const acc{"acc"};
    lims::LedgerModel ledger;
    ledger.execute(lims::CreateAccount{.company = lims::test::seededCompany(),
                                       .code = "1200",
                                       .name = "Receivables",
                                       .type = lims::AccountType::Asset});
    for (std::string const code : {"9000", "9100"}) {
        ledger.execute(lims::CreateAccount{
            .company = osaka, .code = code, .name = "Account " + code, .type = lims::AccountType::Asset});
    }
}

/// @brief Whether the widget of @p testId shows @p expected.
/// @param harness The harness.
/// @param testId The widget.
/// @param expected The text.
/// @return Whether it does within the wait budget.
bool shows(ScreenHarness& harness, std::string_view testId, std::string_view expected) {
    return harness.waitForText(testId, expected);
}

}  // namespace

// Mutations: read the seeded company in the probe's query whatever is chosen (the list stays at "1 accounts"; the
// next case is that probe); name the seeded company in the page head's eyebrow whatever is chosen (it stays
// "Ledger · Main laboratory").
TEST_CASE("lims shell: choosing a company in the switcher changes a screen's list to that company's records",
          "[lims-screens][shell]") {
    morph::ladder::testkit::DbFixture fixture;
    seedAccounts();
    ScreenHarness harness{
        morph::client::LocalApp{.name = "lims-shell", .bundle = [] { return scopedBundle(true); }, .setUp = {}},
        std::string{kScoped},
        RigMode::Local,
        {},
        morph::client::LocalSetup{.db = {}, .user = "acc"}};
    CHECK(shows(harness, "accountCount", "1 accounts"));
    CHECK(shows(harness, "context", "1"));
    CHECK(shows(harness, "head_eyebrow", "Ledger · Main laboratory"));
    auto const menu = harness.backend().find("Menu", "testId", "shell_context");
    REQUIRE(menu.has_value());
    CHECK(harness.backend().prop(*menu, "label").contains("Main laboratory"));
    harness.backend().chooseMenuEntry(*menu, {1});
    CHECK(shows(harness, "accountCount", "2 accounts"));
    CHECK(shows(harness, "context", "2"));
    CHECK(shows(harness, "head_eyebrow", "Ledger · Osaka branch"));
    lims::screens::test::reportProblems(harness);
    CHECK(harness.problems().empty());
}

// Mutation: the same probe with its query reading the seeded company, which the switcher cannot change.
TEST_CASE("lims shell: a screen that does not read the context stays where it was", "[lims-screens][shell]") {
    morph::ladder::testkit::DbFixture fixture;
    seedAccounts();
    ScreenHarness harness{
        morph::client::LocalApp{.name = "lims-shell", .bundle = [] { return scopedBundle(false); }, .setUp = {}},
        std::string{kScoped},
        RigMode::Local,
        {},
        morph::client::LocalSetup{.db = {}, .user = "acc"}};
    REQUIRE(shows(harness, "accountCount", "1 accounts"));
    auto const menu = harness.backend().find("Menu", "testId", "shell_context");
    REQUIRE(menu.has_value());
    harness.backend().chooseMenuEntry(*menu, {1});
    CHECK(shows(harness, "context", "2"));
    CHECK(harness.text("accountCount") == "1 accounts");
}
