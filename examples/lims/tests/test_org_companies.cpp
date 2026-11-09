// SPDX-License-Identifier: Apache-2.0
//
// Companies and sites (lims/org): the scopes every later record and role is judged in. The first org migration seeds
// one company and one site, because every record that existed before companies did needs one to belong to.

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <catch2/catch_test_macros.hpp>
#include <glaze/glaze.hpp>
#include <morph/core/registry.hpp>
#include <string>

#include "lims/org/core/errors.hpp"
#include "lims/org/db/org_entity.hpp"
#include "lims/org/dto/company_dto.hpp"
#include "lims/org/models/org_model.hpp"
#include "lims_test_support.hpp"
#include "testkit/db_fixture.hpp"

using lims::test::ScopedPrincipal;
using morph::ladder::testkit::DbFixture;

TEST_CASE("org: the first migration seeds one company with one site", "[lims][org]") {
    DbFixture fixture;
    ScopedPrincipal const alice{"alice"};
    lims::OrgModel org;

    auto const companies = org.execute(lims::ListCompanies{}).companies;
    REQUIRE(companies.size() == 1);
    CHECK(companies.front().id == lims::test::seededCompany());
    CHECK(companies.front().name == "Main laboratory");
    CHECK(companies.front().baseCurrency == lims::CurrencyCode::EUR);
    CHECK(companies.front().fiscalYearStart == lims::Month::January);

    auto const sites = org.execute(lims::ListSites{}).sites;
    REQUIRE(sites.size() == 1);
    CHECK(sites.front().id == lims::test::seededSite());
    CHECK(sites.front().companyId == lims::test::seededCompany());
    CHECK(sites.front().name == "Main site");
}

// Mutation: store the base currency as EUR whatever is asked (the JPY company reads back EUR).
TEST_CASE("org: a company keeps its base currency and fiscal-year start", "[lims][org]") {
    DbFixture fixture;
    lims::test::grant("alice", lims::LimsRole::Supervisor);
    ScopedPrincipal const alice{"alice"};
    lims::OrgModel org;

    auto const created = org.execute(lims::CreateCompany{
        .name = "Osaka branch", .baseCurrency = lims::CurrencyCode::JPY, .fiscalYearStart = lims::Month::April});
    REQUIRE(created.id.hasValue());
    CHECK(created.baseCurrency == lims::CurrencyCode::JPY);
    CHECK(created.fiscalYearStart == lims::Month::April);

    auto const companies = org.execute(lims::ListCompanies{}).companies;
    REQUIRE(companies.size() == 2);
    CHECK(companies.back().name == "Osaka branch");
    CHECK(companies.back().baseCurrency == lims::CurrencyCode::JPY);
    CHECK(companies.back().fiscalYearStart == lims::Month::April);
}

// Mutation: skip the company lookup (a site is created under company 999).
TEST_CASE("org: a site belongs to a company that exists", "[lims][org]") {
    DbFixture fixture;
    lims::test::grant("alice", lims::LimsRole::Supervisor);
    ScopedPrincipal const alice{"alice"};
    lims::OrgModel org;

    auto const site = org.execute(lims::CreateSite{.companyId = lims::test::seededCompany(), .name = "Field lab"});
    CHECK(site.companyId == lims::test::seededCompany());
    REQUIRE(lims::OrgModel::findSite(site.id).has_value());
    CHECK(lims::OrgModel::findSite(site.id)->name == "Field lab");
    CHECK_FALSE(lims::OrgModel::findSite(lims::SiteId{999}).has_value());

    CHECK_THROWS_AS(org.execute(lims::CreateSite{.companyId = lims::CompanyId{999}, .name = "Nowhere"}),
                    lims::NotFound);
    CHECK_THROWS_AS(org.execute(lims::CreateSite{.companyId = lims::test::seededCompany(), .name = {}}),
                    lims::ValidationError);
    CHECK_THROWS_AS(org.execute(lims::CreateCompany{
                        .name = {}, .baseCurrency = lims::CurrencyCode::EUR, .fiscalYearStart = lims::Month::January}),
                    lims::ValidationError);
}

TEST_CASE("org: creating a company or a site needs an authenticated principal", "[lims][org]") {
    DbFixture fixture;
    lims::OrgModel org;
    CHECK_THROWS_AS(
        org.execute(lims::CreateCompany{
            .name = "Anonymous", .baseCurrency = lims::CurrencyCode::EUR, .fiscalYearStart = lims::Month::January}),
        lims::EmptyPrincipalError);
    CHECK_THROWS_AS(org.execute(lims::CreateSite{.companyId = lims::test::seededCompany(), .name = "Anonymous"}),
                    lims::EmptyPrincipalError);
}

// NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): each key is checked before it is read.
TEST_CASE("org: the company and site forms are served with explicit submit and closed option sets", "[lims][org]") {
    std::string const document = morph::model::detail::defaultDispatcher().schemasJson("OrgModel");
    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, document));
    for (char const* const actionType : {"CreateCompany", "CreateSite"}) {
        INFO("action type: " << actionType);
        REQUIRE(dom.contains(actionType));
        REQUIRE(dom[actionType].contains("x-submitMode"));
        CHECK(dom[actionType]["x-submitMode"].get<std::string>() == "explicit");
    }
    std::string const company = glz::write_json(dom["CreateCompany"]).value_or(std::string{});
    CHECK(company.contains("JPY"));
    CHECK(company.contains("December"));
}
// NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)

// Mutation: lift the 128-byte bound (a 129-byte name is accepted).
TEST_CASE("org: a company or site name of 128 bytes is accepted and one of 129 is refused", "[lims][org]") {
    DbFixture fixture;
    lims::test::grant("alice", lims::LimsRole::Supervisor);
    ScopedPrincipal const alice{"alice"};
    lims::OrgModel org;
    std::string const longest(128, 'a');
    std::string const tooLong(129, 'b');

    CHECK(org.execute(lims::CreateCompany{.name = longest}).name == longest);
    CHECK_THROWS_AS(org.execute(lims::CreateCompany{.name = tooLong}), lims::ValidationError);
    CHECK(org.execute(lims::CreateSite{.companyId = lims::test::seededCompany(), .name = longest}).name == longest);
    CHECK_THROWS_AS(org.execute(lims::CreateSite{.companyId = lims::test::seededCompany(), .name = tooLong}),
                    lims::ValidationError);
}

// Mutation: drop the company-name check (a second "Main laboratory" is created).
TEST_CASE("org: a company name is unique", "[lims][org]") {
    DbFixture fixture;
    lims::test::grant("alice", lims::LimsRole::Supervisor);
    ScopedPrincipal const alice{"alice"};
    lims::OrgModel org;
    CHECK_THROWS_AS(org.execute(lims::CreateCompany{.name = "Main laboratory"}), lims::Conflict);
    CHECK(org.execute(lims::ListCompanies{}).companies.size() == 1);
    CHECK(org.execute(lims::CreateCompany{.name = "Osaka branch"}).id.hasValue());
}

// Mutation: drop the site-name check (a second "Main site" is created in the company).
TEST_CASE("org: a site name is unique within its company, not across companies", "[lims][org]") {
    DbFixture fixture;
    lims::test::grant("alice", lims::LimsRole::Supervisor);
    ScopedPrincipal const alice{"alice"};
    lims::OrgModel org;
    CHECK_THROWS_AS(org.execute(lims::CreateSite{.companyId = lims::test::seededCompany(), .name = "Main site"}),
                    lims::Conflict);
    CHECK(org.execute(lims::ListSites{}).sites.size() == 1);

    auto const osaka = org.execute(lims::CreateCompany{.name = "Osaka branch"});
    auto const sameName = org.execute(lims::CreateSite{.companyId = osaka.id, .name = "Main site"});
    CHECK(sameName.companyId == osaka.id);
    CHECK(org.execute(lims::ListSites{}).sites.size() == 2);
}

// Mutation: cast the stored integers without the range check (the corrupt row reads back as a value outside the enum).
TEST_CASE("org: a company row storing a currency or month outside its enumeration is refused on read", "[lims][org]") {
    DbFixture fixture;
    ScopedPrincipal const alice{"alice"};
    lims::OrgModel org;
    {
        Lightweight::DataMapper mapper;
        lims::db::CompanyRecord row;
        row.name = Lightweight::SqlAnsiString<128>{"Corrupt currency"};
        row.baseCurrency = 99;
        mapper.Create(row);
    }
    CHECK_THROWS_AS(org.execute(lims::ListCompanies{}), lims::ValidationError);
    {
        Lightweight::DataMapper mapper;
        for (auto& row : mapper.Query<lims::db::CompanyRecord>().All()) {
            if (row.baseCurrency.Value() == 99) {
                row.baseCurrency = 0;
                row.fiscalYearStart = 12;
                mapper.Update(row);
            }
        }
    }
    CHECK_THROWS_AS(org.execute(lims::ListCompanies{}), lims::ValidationError);
}
