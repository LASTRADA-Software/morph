// SPDX-License-Identifier: Apache-2.0
//
// Role assignments (lims/org): a principal holds a role in a scope, a company or one of its sites. A role held at a
// company holds at every site of it; roles themselves are not hierarchical (README decision 16).

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "lims/org/core/errors.hpp"
#include "lims/org/db/org_entity.hpp"
#include "lims/org/dto/role_dto.hpp"
#include "lims/org/models/org_model.hpp"
#include "lims_test_support.hpp"
#include "migration_probe.hpp"
#include "testkit/db_fixture.hpp"

using lims::test::ScopedPrincipal;
using morph::ladder::testkit::DbFixture;

namespace {

/// @brief Whether @p roles holds @p role.
/// @param roles The roles.
/// @param role The role looked for.
/// @return It does.
[[nodiscard]] bool holds(std::vector<lims::LimsRole> const& roles, lims::LimsRole role) {
    return std::ranges::find(roles, role) != roles.end();
}

}  // namespace

// A named namespace: Lightweight reflects records, which needs types with linkage.
namespace lims_role_migration_fixtures {

/// @brief `lims_operators` as lab wrote it before roles were scoped.
struct LegacyOperatorRecord {
    static constexpr std::string_view TableName =
        "lims_operators";  // NOLINT(readability-identifier-naming): the ORM reads this spelling
    Light::Field<std::uint64_t, Light::PrimaryKey::ServerSideAutoIncrement, Light::SqlRealName{"id"}> id;
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"principal"}> principal;
    Light::Field<int, Light::SqlRealName{"role"}> role{0};
    Light::Field<Light::SqlAnsiString<64>, Light::SqlRealName{"granted_by"}> grantedBy;
    Light::Field<std::int64_t, Light::SqlRealName{"granted_at"}> grantedAt{0};
};

}  // namespace lims_role_migration_fixtures

// Mutations: drop the supervisor check once one exists (mallory's grant goes through); create a second row for a
// repeated grant (bob's assignment ids differ).
TEST_CASE("org roles: the first assignment bootstraps a lab with no supervisor; later ones need one",
          "[lims][org][roles]") {
    DbFixture fixture;
    lims::OrgModel org;
    {
        ScopedPrincipal const alice{"alice"};
        CHECK_FALSE(lims::OrgModel::supervisorExists());
        auto const first = org.execute(lims::AssignRole{.principal = "alice",
                                                        .role = lims::LimsRole::Supervisor,
                                                        .company = lims::test::seededCompany(),
                                                        .site = {}});
        CHECK(first.scopeKind == lims::ScopeKind::Company);
        CHECK(first.company == lims::test::seededCompany());
        CHECK(first.grantedBy == "alice");
        CHECK(lims::OrgModel::supervisorExists());
    }
    {
        ScopedPrincipal const mallory{"mallory"};
        CHECK_THROWS_AS(org.execute(lims::AssignRole{.principal = "mallory",
                                                     .role = lims::LimsRole::Supervisor,
                                                     .company = lims::test::seededCompany(),
                                                     .site = {}}),
                        lims::Forbidden);
    }
    {
        ScopedPrincipal const alice{"alice"};
        auto const bob = org.execute(lims::AssignRole{
            .principal = "bob", .role = lims::LimsRole::Verifier, .company = {}, .site = lims::test::seededSite()});
        auto const again = org.execute(lims::AssignRole{
            .principal = "bob", .role = lims::LimsRole::Verifier, .company = {}, .site = lims::test::seededSite()});
        CHECK(again.id == bob.id);
        CHECK(org.execute(lims::ListRoleAssignments{}).assignments.size() == 2);
    }
    CHECK_THROWS_AS(
        org.execute(lims::AssignRole{
            .principal = "bob", .role = lims::LimsRole::Verifier, .company = lims::test::seededCompany(), .site = {}}),
        lims::EmptyPrincipalError);
}

// Mutation: accept both a company and a site (the assignment is created).
TEST_CASE("org roles: an assignment names exactly one scope, and the scope exists", "[lims][org][roles]") {
    DbFixture fixture;
    ScopedPrincipal const alice{"alice"};
    lims::OrgModel org;
    CHECK_THROWS_AS(org.execute(lims::AssignRole{.principal = "bob",
                                                 .role = lims::LimsRole::Analyst,
                                                 .company = lims::test::seededCompany(),
                                                 .site = lims::test::seededSite()}),
                    lims::ValidationError);
    CHECK_THROWS_AS(
        org.execute(lims::AssignRole{.principal = "bob", .role = lims::LimsRole::Analyst, .company = {}, .site = {}}),
        lims::ValidationError);
    CHECK_THROWS_AS(
        org.execute(lims::AssignRole{
            .principal = {}, .role = lims::LimsRole::Analyst, .company = lims::test::seededCompany(), .site = {}}),
        lims::ValidationError);
    CHECK_THROWS_AS(
        org.execute(lims::AssignRole{
            .principal = "bob", .role = lims::LimsRole::Analyst, .company = {}, .site = lims::SiteId{999}}),
        lims::NotFound);
    CHECK_THROWS_AS(
        org.execute(lims::AssignRole{
            .principal = "bob", .role = lims::LimsRole::Analyst, .company = lims::CompanyId{999}, .site = {}}),
        lims::NotFound);
}

// Mutations: ignore company-scoped assignments for a site (bob's company role is missing at the field lab); let a
// site role cover its company (carol is a verifier at the company).
TEST_CASE("org roles: a company role holds at its sites, and a site role holds at that site only",
          "[lims][org][roles]") {
    DbFixture fixture;
    ScopedPrincipal const alice{"alice"};
    lims::OrgModel org;
    org.execute(lims::AssignRole{
        .principal = "alice", .role = lims::LimsRole::Supervisor, .company = lims::test::seededCompany(), .site = {}});
    auto const fieldLab = org.execute(lims::CreateSite{.companyId = lims::test::seededCompany(), .name = "Field lab"});
    auto const other = org.execute(lims::CreateCompany{
        .name = "Osaka branch", .baseCurrency = lims::CurrencyCode::JPY, .fiscalYearStart = lims::Month::April});

    org.execute(lims::AssignRole{
        .principal = "bob", .role = lims::LimsRole::Verifier, .company = lims::test::seededCompany(), .site = {}});
    org.execute(
        lims::AssignRole{.principal = "carol", .role = lims::LimsRole::Verifier, .company = {}, .site = fieldLab.id});

    CHECK(holds(lims::OrgModel::rolesIn("bob", lims::Scope{fieldLab.id}), lims::LimsRole::Verifier));
    CHECK(holds(lims::OrgModel::rolesIn("bob", lims::Scope{lims::test::seededSite()}), lims::LimsRole::Verifier));
    CHECK_FALSE(holds(lims::OrgModel::rolesIn("bob", lims::Scope{other.id}), lims::LimsRole::Verifier));

    CHECK(holds(lims::OrgModel::rolesIn("carol", lims::Scope{fieldLab.id}), lims::LimsRole::Verifier));
    CHECK_FALSE(
        holds(lims::OrgModel::rolesIn("carol", lims::Scope{lims::test::seededSite()}), lims::LimsRole::Verifier));
    CHECK_FALSE(
        holds(lims::OrgModel::rolesIn("carol", lims::Scope{lims::test::seededCompany()}), lims::LimsRole::Verifier));
    CHECK(holds(lims::OrgModel::rolesAnywhere("carol"), lims::LimsRole::Verifier));
}

TEST_CASE("org roles: roles are not hierarchical", "[lims][org][roles]") {
    DbFixture fixture;
    ScopedPrincipal const alice{"alice"};
    lims::OrgModel org;
    org.execute(lims::AssignRole{
        .principal = "alice", .role = lims::LimsRole::Supervisor, .company = lims::test::seededCompany(), .site = {}});
    // A supervisor is not implicitly a verifier: the second pair of eyes on somebody's own work is never granted
    // by implication.
    auto const held = lims::OrgModel::rolesIn("alice", lims::Scope{lims::test::seededSite()});
    REQUIRE(held.size() == 1);
    CHECK(held.front() == lims::LimsRole::Supervisor);
}

// Mutations: allow revoking the last supervisor (the bootstrap reopens); skip the supervisor check (bob revokes).
TEST_CASE("org roles: a supervisor revokes an assignment, but never the last supervisor's", "[lims][org][roles]") {
    DbFixture fixture;
    lims::OrgModel org;
    ScopedPrincipal const alice{"alice"};
    auto const boss = org.execute(lims::AssignRole{
        .principal = "alice", .role = lims::LimsRole::Supervisor, .company = lims::test::seededCompany(), .site = {}});
    auto const bob = org.execute(lims::AssignRole{
        .principal = "bob", .role = lims::LimsRole::Verifier, .company = lims::test::seededCompany(), .site = {}});
    {
        ScopedPrincipal const asBob{"bob"};
        CHECK_THROWS_AS(org.execute(lims::RevokeRole{.assignmentId = boss.id}), lims::Forbidden);
    }
    CHECK(org.execute(lims::RevokeRole{.assignmentId = bob.id}).assignmentId == bob.id);
    CHECK(lims::OrgModel::rolesAnywhere("bob").empty());
    CHECK_THROWS_AS(org.execute(lims::RevokeRole{.assignmentId = boss.id}), lims::Conflict);
    CHECK_THROWS_AS(org.execute(lims::RevokeRole{.assignmentId = lims::RoleAssignmentId{999}}), lims::NotFound);
    CHECK(lims::OrgModel::supervisorExists());
}

TEST_CASE("org roles: users are the principals holding an assignment, with the roles they hold",
          "[lims][org][roles]") {
    DbFixture fixture;
    ScopedPrincipal const alice{"alice"};
    lims::OrgModel org;
    org.execute(lims::AssignRole{
        .principal = "alice", .role = lims::LimsRole::Supervisor, .company = lims::test::seededCompany(), .site = {}});
    org.execute(lims::AssignRole{
        .principal = "bob", .role = lims::LimsRole::Analyst, .company = {}, .site = lims::test::seededSite()});
    org.execute(lims::AssignRole{
        .principal = "bob", .role = lims::LimsRole::Verifier, .company = lims::test::seededCompany(), .site = {}});
    auto const users = org.execute(lims::ListUsers{}).users;
    REQUIRE(users.size() == 2);
    CHECK(users[0].id == "alice");
    CHECK(users[1].id == "bob");
    CHECK(users[1].roles == std::vector<lims::LimsRole>{lims::LimsRole::Analyst, lims::LimsRole::Verifier});
}

// Mutation: copy no rows in the lab migration (bob holds nothing after it).
TEST_CASE("org roles: assignments made before roles were scoped migrate to the seeded company",
          "[lims][org][roles][migration]") {
    using lims_role_migration_fixtures::LegacyOperatorRecord;
    DbFixture fixture;
    lims::test::MigrationProbe probe{20261109000002};
    {
        Lightweight::DataMapper mapper;
        LegacyOperatorRecord bob;
        bob.principal = Lightweight::SqlAnsiString<64>{"bob"};
        bob.role = static_cast<int>(lims::LimsRole::Verifier);
        bob.grantedBy = Lightweight::SqlAnsiString<64>{"alice"};
        mapper.Create(bob);
    }
    probe.applyRest();
    CHECK(holds(lims::OrgModel::rolesIn("bob", lims::Scope{lims::test::seededCompany()}), lims::LimsRole::Verifier));
    CHECK(holds(lims::OrgModel::rolesIn("bob", lims::Scope{lims::test::seededSite()}), lims::LimsRole::Verifier));
    Lightweight::DataMapper mapper;
    CHECK_THROWS(mapper.Query<LegacyOperatorRecord>().All());
}

// Mutations: replace the role range check with `if (false)` (the first CHECK_THROWS_AS fails); the same for the scope
// kind check (the second one fails).
TEST_CASE("org roles: a stored role or scope kind outside its enumeration is refused on read", "[lims][org][roles]") {
    DbFixture fixture;
    ScopedPrincipal const alice{"alice"};
    lims::OrgModel org;
    lims::test::grant("bob", lims::LimsRole::Verifier);
    Lightweight::DataMapper mapper;
    auto rows =
        mapper.Query<lims::db::RoleAssignmentRecord>()
            .Where(Lightweight::FieldNameOf<&lims::db::RoleAssignmentRecord::principal>, "=", std::string{"bob"})
            .All();
    REQUIRE(rows.size() == 1);
    auto row = rows.front();

    row.role = 99;
    mapper.Update(row);
    CHECK_THROWS_AS(org.execute(lims::ListRoleAssignments{}), lims::ValidationError);
    CHECK_THROWS_AS(lims::OrgModel::rolesAnywhere("bob"), lims::ValidationError);

    row.role = static_cast<int>(lims::LimsRole::Verifier);
    row.scopeKind = 7;
    mapper.Update(row);
    CHECK_THROWS_AS(org.execute(lims::ListRoleAssignments{}), lims::ValidationError);
    CHECK_THROWS_AS(lims::OrgModel::rolesIn("bob", lims::Scope{lims::test::seededCompany()}), lims::ValidationError);
}
