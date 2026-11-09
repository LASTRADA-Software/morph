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

// Mutations: drop changes<RoleAssignmentView> from AssignRole's effects (the count stays at 0); answer the last
// supervisor's revoke with success (no refusal is shown and the count drops).
TEST_CASE("lims org screens: the first supervisor assigns themselves, then a verifier, and the last supervisor stays",
          "[lims-screens][org]") {
    DbFixture fixture;
    ScreenHarness harness{
        limsApp(), std::string{lims::screens::kAccess}, RigMode::Local, {}, LocalSetup{.db = {}, .user = "tech"}};
    REQUIRE(harness.waitForText("assignmentCount", "0 role assignments"));

    harness.editField("assignForm", "principal", "tech");
    harness.chooseField("assignForm", "role", R"("Supervisor")");
    harness.editField("assignForm", "company", "1");
    REQUIRE(harness.submitForm("assignForm"));
    REQUIRE(harness.waitForText("assignmentCount", "1 role assignments"));
    CHECK(harness.waitForText("userCount", "1 users"));

    harness.editField("assignForm", "principal", "bob");
    harness.chooseField("assignForm", "role", R"("Verifier")");
    REQUIRE(harness.submitForm("assignForm"));
    REQUIRE(harness.waitForText("assignmentCount", "2 role assignments"));
    CHECK(harness.waitForText("userCount", "2 users"));

    // The first row is tech's supervisor assignment, the only one: revoking it is refused, in words.
    // A table is not a column, so the button is looked up by its test id over the whole screen.
    REQUIRE(harness.waitFor([&harness] { return harness.find("Button", "revoke").has_value(); }));
    harness.click("revoke");
    CHECK(harness.waitFor([&harness] {
        auto const text = harness.text("error");
        return text && text->find("last supervisor") != std::string::npos;
    }));
    CHECK(harness.waitForText("assignmentCount", "2 role assignments"));
}
