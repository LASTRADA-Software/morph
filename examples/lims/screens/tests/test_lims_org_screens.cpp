// SPDX-License-Identifier: Apache-2.0
//
// lims' administration screens on the screen harness: companies and sites, and users and their role assignments.
// Every form is the action's own schema, and every list refreshes from the declared effects of the action that
// changed it.

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <testkit/db_fixture.hpp>
#include <testkit/screen_harness.hpp>

#include "lims/org/dto/role_dto.hpp"
#include "lims_screens.hpp"
#include "lims_test_support.hpp"

namespace {

using morph::client::LocalApp;
using morph::client::LocalSetup;
using morph::examples::testing::RigMode;
using morph::examples::testing::ScreenHarness;
using morph::ladder::testkit::DbFixture;

LocalApp limsApp() { return LocalApp{.name = "lims", .bundle = &lims::screens::bundle, .setUp = {}}; }

void reportProblems(ScreenHarness const& harness) {
    for (std::string const& problem : harness.problems()) {
        UNSCOPED_INFO(problem);
    }
}

}  // namespace

// Mutations: drop changes<CompanyView> from CreateCompany's effects (the count stays at 1 company); drop
// changes<SiteView> from CreateSite's (the count stays at 1 site).
TEST_CASE("lims org screens: a supervisor creates a company and a site, and both lists show them",
          "[lims-screens][org]") {
    DbFixture fixture;
    lims::test::grant("tech", lims::LimsRole::Supervisor);
    ScreenHarness harness{
        limsApp(), std::string{lims::screens::kCompanies}, RigMode::Local, {}, LocalSetup{.db = {}, .user = "tech"}};
    REQUIRE(harness.waitForText("companyCount", "1 companies"));
    REQUIRE(harness.waitForText("siteCount", "1 sites"));

    harness.editField("companyForm", "name", "Osaka branch");
    harness.chooseField("companyForm", "baseCurrency", R"("JPY")");
    harness.chooseField("companyForm", "fiscalYearStart", R"("April")");
    REQUIRE(harness.submitForm("companyForm"));
    CHECK(harness.waitForText("companyCount", "2 companies"));

    harness.editField("siteForm", "companyId", "1");
    harness.editField("siteForm", "name", "Field lab");
    REQUIRE(harness.submitForm("siteForm"));
    CHECK(harness.waitForText("siteCount", "2 sites"));
    reportProblems(harness);
    CHECK(harness.problems().empty());
}

// Mutations: drop changes<RoleAssignmentView> from RevokeRole's effects (the count stays at 3); make every row's
// button send the table's current first row's id (pressing carol's button revokes the first row instead, so carol's
// row stays and the first row goes).
TEST_CASE("lims org screens: a revoke drops its own row, and the last supervisor's revoke is refused in words",
          "[lims-screens][org]") {
    DbFixture fixture;
    lims::test::grant("bob", lims::LimsRole::Supervisor);
    lims::test::grant("carol", lims::LimsRole::Verifier);
    ScreenHarness harness{
        limsApp(), std::string{lims::screens::kAccess}, RigMode::Local, {}, LocalSetup{.db = {}, .user = "bob"}};
    REQUIRE(harness.waitForText("assignmentCount", "3 role assignments"));
    REQUIRE(harness.waitFor([&harness] { return harness.findAll("Button", "revoke").size() == 3; }));
    auto const shown = [&harness](std::string_view who) {
        return harness.backend().find("Text", "text", who).has_value();
    };
    REQUIRE(shown("lab-supervisor"));
    REQUIRE(shown("bob"));
    REQUIRE(shown("carol"));

    // Rows are the assignments in id order: the granter's, bob's, carol's. The third button is carol's, not the first.
    harness.clickNth("revoke", 2);
    CHECK(harness.waitForText("assignmentCount", "2 role assignments"));
    CHECK(harness.waitFor([&] { return !shown("carol"); }));
    CHECK(shown("lab-supervisor"));
    CHECK(shown("bob"));
    CHECK(harness.findAll("Button", "revoke").size() == 2);

    // The granter's row may go while bob remains a supervisor; the row left is then the last supervisor's, refused.
    harness.clickNth("revoke", 0);
    CHECK(harness.waitForText("assignmentCount", "1 role assignments"));
    CHECK(harness.waitForText("userCount", "1 users"));
    harness.click("revoke");
    CHECK(harness.waitFor([&harness] {
        auto const text = harness.text("error");
        return text && text->find("last supervisor") != std::string::npos;
    }));
    CHECK(shown("bob"));
    reportProblems(harness);
}

// A form reports a refusal in its result line whether or not it shows its status, so the refusal is read there.
// Mutation: make lab-lead a supervisor (the form succeeds and no refusal appears).
TEST_CASE("lims org screens: a refused create-company form names the missing supervisor role in its result",
          "[lims-screens][org]") {
    DbFixture fixture;
    lims::test::grant("lab-lead", lims::LimsRole::Verifier);
    ScreenHarness harness{limsApp(),
                          std::string{lims::screens::kCompanies},
                          RigMode::Local,
                          {},
                          LocalSetup{.db = {}, .user = "lab-lead"}};
    REQUIRE(harness.waitForText("companyCount", "1 companies"));

    harness.editField("companyForm", "name", "Osaka branch");
    harness.chooseField("companyForm", "baseCurrency", R"("JPY")");
    harness.chooseField("companyForm", "fiscalYearStart", R"("April")");
    REQUIRE(harness.submitForm("companyForm"));
    CHECK(harness.waitFor([&harness] {
        auto const text = harness.text("form_result");
        return text && text->find("does not hold the supervisor role") != std::string::npos;
    }));
}
