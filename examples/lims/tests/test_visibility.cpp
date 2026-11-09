// SPDX-License-Identifier: Apache-2.0
//
// Visibility: every list returns only records in scopes where the caller holds a role. A role at a company sees the
// company and all its sites; a role at a site sees that site and, through it, its company.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstdint>
#include <memory>
#include <morph/session/session_auth.hpp>
#include <morph/table/query.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "lims/app/lims_authorizer.hpp"
#include "lims/lab/models/sample_model.hpp"
#include "lims/org/models/org_model.hpp"
#include "lims_test_support.hpp"
#include "testkit/backend_rig.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/pump.hpp"

using lims::test::ScopedPrincipal;
using morph::ladder::testkit::awaitQt;
using morph::ladder::testkit::BackendRig;
using morph::ladder::testkit::DbFixture;
using morph::ladder::testkit::Mode;

namespace {

constexpr std::string_view kSecret = "lims-visibility-test-secret-at-least-32-b";

/// @brief The organisation every case here starts from.
struct Org {
    lims::CompanyId osaka;   ///< A second company, alice's.
    lims::SiteId fieldLab;   ///< A second site of the seeded company.
    lims::SiteId osakaLab;   ///< The only site of the second company.
    lims::SampleId atMain;   ///< A sample at the seeded site.
    lims::SampleId atField;  ///< A sample at the field lab.
    lims::SampleId atOsaka;  ///< A sample at the second company's site.
};

/// @brief alice supervises both companies and creates the rest. bob is an analyst at the field lab only, dave
/// supervises the second company only, erin supervises the seeded company only, and frank is an analyst at the
/// seeded site.
/// @return The ids.
[[nodiscard]] Org setUp() {
    lims::test::grant("alice", lims::LimsRole::Supervisor);
    ScopedPrincipal const alice{"alice"};
    lims::OrgModel org;
    auto const osaka = org.execute(lims::CreateCompany{
        .name = "Osaka branch", .baseCurrency = lims::CurrencyCode::JPY, .fiscalYearStart = lims::Month::April});
    auto const fieldLab = org.execute(lims::CreateSite{.companyId = lims::test::seededCompany(), .name = "Field lab"});
    auto const osakaLab = org.execute(lims::CreateSite{.companyId = osaka.id, .name = "Osaka lab"});
    org.execute(
        lims::AssignRole{.principal = "bob", .role = lims::LimsRole::Analyst, .company = {}, .site = fieldLab.id});
    org.execute(
        lims::AssignRole{.principal = "dave", .role = lims::LimsRole::Supervisor, .company = osaka.id, .site = {}});
    org.execute(lims::AssignRole{
        .principal = "erin", .role = lims::LimsRole::Supervisor, .company = lims::test::seededCompany(), .site = {}});
    org.execute(lims::AssignRole{
        .principal = "frank", .role = lims::LimsRole::Analyst, .company = {}, .site = lims::test::seededSite()});
    lims::SampleModel bench;
    auto const client = bench.execute(lims::RegisterClient{.name = "Waterworks Ltd"});
    auto const atMain = bench.execute(
        lims::RegisterSample{.siteId = lims::test::seededSite(), .clientId = client.clientId, .reference = "WW-MAIN"});
    lims::SampleModel other;
    auto const atField = other.execute(
        lims::RegisterSample{.siteId = fieldLab.id, .clientId = client.clientId, .reference = "WW-FIELD"});
    lims::SampleModel abroad;
    auto const atOsaka = abroad.execute(
        lims::RegisterSample{.siteId = osakaLab.id, .clientId = client.clientId, .reference = "WW-OSAKA"});
    return Org{.osaka = osaka.id,
               .fieldLab = fieldLab.id,
               .osakaLab = osakaLab.id,
               .atMain = atMain.id,
               .atField = atField.id,
               .atOsaka = atOsaka.id};
}

/// @brief The ids of @p rows.
/// @tparam Row A view with an `id`.
/// @param rows The rows.
/// @return Their ids, in order.
template <typename Row>
[[nodiscard]] auto idsOf(std::vector<Row> const& rows) {
    std::vector<decltype(Row::id)> ids;
    for (Row const& row : rows) {
        ids.push_back(row.id);
    }
    return ids;
}

}  // namespace

// Mutation: let visibleSites ignore company-scoped roles (alice sees no site).
TEST_CASE("visibility: companies and sites are listed for the scopes the caller holds a role in",
          "[lims][visibility]") {
    DbFixture fixture;
    auto const org = setUp();
    lims::OrgModel model;
    {
        ScopedPrincipal const alice{"alice"};
        CHECK(idsOf(model.execute(lims::ListCompanies{}).companies) ==
              std::vector<lims::CompanyId>{lims::test::seededCompany(), org.osaka});
        CHECK(idsOf(model.execute(lims::ListSites{}).sites) ==
              std::vector<lims::SiteId>{lims::test::seededSite(), org.fieldLab, org.osakaLab});
    }
    {
        ScopedPrincipal const bob{"bob"};
        CHECK(idsOf(model.execute(lims::ListCompanies{}).companies) ==
              std::vector<lims::CompanyId>{lims::test::seededCompany()});
        CHECK(idsOf(model.execute(lims::ListSites{}).sites) == std::vector<lims::SiteId>{org.fieldLab});
        for (auto const& assignment : model.execute(lims::ListRoleAssignments{}).assignments) {
            CHECK(assignment.company != org.osaka);
            CHECK(assignment.site != lims::test::seededSite());
        }
    }
    {
        ScopedPrincipal const carol{"carol"};
        CHECK(model.execute(lims::ListCompanies{}).companies.empty());
        CHECK(model.execute(lims::ListSites{}).sites.empty());
        CHECK(model.execute(lims::ListRoleAssignments{}).assignments.empty());
        CHECK(model.execute(lims::ListUsers{}).users.empty());
    }
}

// Mutation: skip the site filter in ListSamples (bob sees the main site's sample).
TEST_CASE("visibility: samples are listed for the sites the caller can see, and paged", "[lims][visibility]") {
    DbFixture fixture;
    auto const org = setUp();
    lims::SampleModel model;
    {
        ScopedPrincipal const bob{"bob"};
        auto const page = model.execute(lims::ListSamples{});
        CHECK(idsOf(page.rows) == std::vector<lims::SampleId>{org.atField});
    }
    {
        ScopedPrincipal const alice{"alice"};
        auto const page = model.execute(lims::ListSamples{});
        CHECK(idsOf(page.rows) == std::vector<lims::SampleId>{org.atMain, org.atField, org.atOsaka});
        lims::ListSamples firstOnly;
        firstOnly.query.page.limit = 1;
        auto const paged = model.execute(firstOnly);
        CHECK(idsOf(paged.rows) == std::vector<lims::SampleId>{org.atMain});
        CHECK(paged.total == std::optional<std::int64_t>{3});
    }
    {
        ScopedPrincipal const carol{"carol"};
        CHECK(model.execute(lims::ListSamples{}).rows.empty());
    }
    CHECK(model.execute(lims::ListSamples{}).rows.empty());
}

// Mutations: let visibleSites match a company-scoped role without comparing the company; let visibleCompanies answer
// every company to anyone holding a company-scoped role; let the site branch of ListRoleAssignments accept every row.
TEST_CASE("visibility: a company-scoped role reaches nothing in another company", "[lims][visibility]") {
    DbFixture fixture;
    auto const org = setUp();
    lims::OrgModel model;
    lims::SampleModel bench;
    {
        ScopedPrincipal const dave{"dave"};
        CHECK(idsOf(model.execute(lims::ListCompanies{}).companies) == std::vector<lims::CompanyId>{org.osaka});
        CHECK(idsOf(model.execute(lims::ListSites{}).sites) == std::vector<lims::SiteId>{org.osakaLab});
        CHECK(idsOf(bench.execute(lims::ListSamples{}).rows) == std::vector<lims::SampleId>{org.atOsaka});
        auto const assignments = model.execute(lims::ListRoleAssignments{}).assignments;
        REQUIRE_FALSE(assignments.empty());
        for (auto const& assignment : assignments) {
            CHECK(assignment.scopeKind == lims::ScopeKind::Company);
            CHECK(assignment.company == org.osaka);
        }
    }
    {
        ScopedPrincipal const erin{"erin"};
        CHECK(idsOf(model.execute(lims::ListCompanies{}).companies) ==
              std::vector<lims::CompanyId>{lims::test::seededCompany()});
        CHECK(idsOf(model.execute(lims::ListSites{}).sites) ==
              std::vector<lims::SiteId>{lims::test::seededSite(), org.fieldLab});
        CHECK(idsOf(bench.execute(lims::ListSamples{}).rows) == std::vector<lims::SampleId>{org.atMain, org.atField});
        auto const assignments = model.execute(lims::ListRoleAssignments{}).assignments;
        REQUIRE_FALSE(assignments.empty());
        for (auto const& assignment : assignments) {
            CHECK(assignment.company != org.osaka);
            CHECK(assignment.site != org.osakaLab);
        }
    }
}

// Mutation: drop the creator's assignment from CreateCompany (alice cannot see the company she created).
TEST_CASE("visibility: a company's creator is its first supervisor", "[lims][visibility]") {
    DbFixture fixture;
    auto const org = setUp();
    auto const held = lims::OrgModel::rolesIn("alice", lims::Scope{org.osaka});
    REQUIRE(held.size() == 1);
    CHECK(held.front() == lims::LimsRole::Supervisor);
    CHECK(lims::OrgModel::rolesIn("bob", lims::Scope{org.osaka}).empty());
}

TEST_CASE("visibility: the org lists are scoped over every backend", "[lims][visibility][matrix]") {
    auto const mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    auto const org = setUp();
    morph::session::TokenIssuer const issuer{std::string{kSecret}, morph::session::hmacSha256};
    BackendRig rig{mode, 1,
                   std::make_shared<lims::auth::LimsAuthorizer>(std::string{kSecret}, [](std::string_view principal) {
                       return lims::OrgModel::rolesAnywhere(principal);
                   })};
    morph::session::Context bob;
    bob.principal = "bob";
    bob.token = issuer.issue(
        morph::session::SessionToken{.principal = "bob", .issuedAtMs = 0, .expiresAtMs = 4102444800000, .roles = {}});
    rig.bridge(0).setDefaultSession(bob);
    auto lister = rig.client<lims::OrgModel>(0);
    auto const companies = awaitQt(lister.execute(lims::ListCompanies{}));
    CHECK(idsOf(companies.companies) == std::vector<lims::CompanyId>{lims::test::seededCompany()});
    auto const sites = awaitQt(lister.execute(lims::ListSites{}));
    CHECK(idsOf(sites.sites) == std::vector<lims::SiteId>{org.fieldLab});
    auto const assignments = awaitQt(lister.execute(lims::ListRoleAssignments{}));
    REQUIRE_FALSE(assignments.assignments.empty());
    for (auto const& assignment : assignments.assignments) {
        CHECK(assignment.company != org.osaka);
        if (assignment.scopeKind == lims::ScopeKind::Site) {
            CHECK(assignment.site == org.fieldLab);
        }
    }
}

TEST_CASE("visibility: the sample list is scoped over every backend", "[lims][visibility][matrix]") {
    auto const mode = GENERATE(Mode::Local, Mode::LocalSingleThread, Mode::Socket);
    CAPTURE(mode);
    DbFixture fixture;
    auto const org = setUp();
    morph::session::TokenIssuer const issuer{std::string{kSecret}, morph::session::hmacSha256};
    BackendRig rig{mode, 1,
                   std::make_shared<lims::auth::LimsAuthorizer>(std::string{kSecret}, [](std::string_view principal) {
                       return lims::OrgModel::rolesAnywhere(principal);
                   })};
    morph::session::Context bob;
    bob.principal = "bob";
    bob.token = issuer.issue(
        morph::session::SessionToken{.principal = "bob", .issuedAtMs = 0, .expiresAtMs = 4102444800000, .roles = {}});
    rig.bridge(0).setDefaultSession(bob);
    auto lister = rig.client<lims::SampleModel>(0);
    auto const page = awaitQt(lister.execute(lims::ListSamples{}));
    CHECK(idsOf(page.rows) == std::vector<lims::SampleId>{org.atField});
}
