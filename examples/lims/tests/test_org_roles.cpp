// SPDX-License-Identifier: Apache-2.0
//
// Role assignments (lims/org): a principal holds a role in a scope, a company or one of its sites. A role held at a
// company holds at every site of it; roles themselves are not hierarchical (README decision 16).

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/SqlStatement.hpp>
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <morph/core/action_guard.hpp>
#include <morph/session/session_auth.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lims/org/core/errors.hpp"
#include "lims/org/db/org_entity.hpp"
#include "lims/org/dto/role_dto.hpp"
#include "lims/org/models/auth_model.hpp"
#include "lims/org/models/org_model.hpp"
#include "lims_test_support.hpp"
#include "migration_probe.hpp"
#include "testkit/db_fixture.hpp"
#include "testkit/storage_owner.hpp"

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
        CHECK_THROWS_AS(::morph::model::executeChecked(org, lims::AssignRole{.principal = "mallory",
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
    CHECK_THROWS_AS(::morph::model::executeChecked(org, lims::AssignRole{.principal = "bob",
                                                                         .role = lims::LimsRole::Verifier,
                                                                         .company = lims::test::seededCompany(),
                                                                         .site = {}}),
                    lims::EmptyPrincipalError);
}

// Mutations: drop `notOnSelf` from Authorization<AssignRole> (alice grants herself InvoiceApprover); refuse the
// bootstrap too (the first assignment is Forbidden); route CreateCompany's appointment through AssignRole.
TEST_CASE("org roles: a supervisor does not assign themselves a role, except the bootstrap and a new company",
          "[lims][org][roles]") {
    DbFixture fixture;
    lims::OrgModel org;
    {
        ScopedPrincipal const alice{"alice"};
        auto const boot = org.execute(lims::AssignRole{.principal = "alice",
                                                       .role = lims::LimsRole::Supervisor,
                                                       .company = lims::test::seededCompany(),
                                                       .site = {}});
        CHECK(boot.principal == "alice");
        CHECK_THROWS_AS(::morph::model::executeChecked(org, lims::AssignRole{.principal = "alice",
                                                                             .role = lims::LimsRole::InvoiceApprover,
                                                                             .company = lims::test::seededCompany(),
                                                                             .site = {}}),
                        lims::Forbidden);
        CHECK_THROWS_AS(::morph::model::executeChecked(org, lims::AssignRole{.principal = "alice",
                                                                             .role = lims::LimsRole::Accountant,
                                                                             .company = {},
                                                                             .site = lims::test::seededSite()}),
                        lims::Forbidden);
        auto const osaka = org.execute(lims::CreateCompany{
            .name = "Osaka branch", .baseCurrency = lims::CurrencyCode::JPY, .fiscalYearStart = lims::Month::April});
        auto const held = lims::OrgModel::rolesIn("alice", lims::Scope{osaka.id});
        CHECK(std::ranges::find(held, lims::LimsRole::Supervisor) != held.end());
        auto const everywhere = lims::OrgModel::rolesAnywhere("alice");
        CHECK(std::ranges::find(everywhere, lims::LimsRole::InvoiceApprover) == everywhere.end());
        org.execute(lims::AssignRole{.principal = "dave",
                                     .role = lims::LimsRole::Supervisor,
                                     .company = lims::test::seededCompany(),
                                     .site = {}});
    }
    ScopedPrincipal const dave{"dave"};
    CHECK(org.execute(lims::AssignRole{.principal = "alice",
                                       .role = lims::LimsRole::InvoiceApprover,
                                       .company = lims::test::seededCompany(),
                                       .site = {}})
              .principal == "alice");
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
    CHECK_THROWS_AS(::morph::model::executeChecked(org, lims::AssignRole{.principal = "bob",
                                                                         .role = lims::LimsRole::Analyst,
                                                                         .company = {},
                                                                         .site = lims::SiteId{999}}),
                    lims::NotFound);
    CHECK_THROWS_AS(::morph::model::executeChecked(org, lims::AssignRole{.principal = "bob",
                                                                         .role = lims::LimsRole::Analyst,
                                                                         .company = lims::CompanyId{999},
                                                                         .site = {}}),
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
        CHECK_THROWS_AS(::morph::model::executeChecked(org, lims::RevokeRole{.assignmentId = boss.id}),
                        lims::Forbidden);
    }
    CHECK(org.execute(lims::RevokeRole{.assignmentId = bob.id}).assignmentId == bob.id);
    CHECK(lims::OrgModel::rolesAnywhere("bob").empty());
    CHECK_THROWS_AS(::morph::model::executeChecked(org, lims::RevokeRole{.assignmentId = boss.id}), lims::Conflict);
    CHECK_THROWS_AS(::morph::model::executeChecked(org, lims::RevokeRole{.assignmentId = lims::RoleAssignmentId{999}}),
                    lims::Forbidden);
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

// Mutations: copy no rows in the lab migration (the table is empty); copy only the first row (bob's second role is
// missing); put the rows at no company (the scope check fails).
TEST_CASE("org roles: assignments made before roles were scoped migrate to the seeded company",
          "[lims][org][roles][migration]") {
    using lims_role_migration_fixtures::LegacyOperatorRecord;
    DbFixture fixture;
    lims::test::MigrationProbe probe{20261109000002};
    {
        Lightweight::DataMapper mapper;
        for (auto const& [principal, role] : {std::pair{std::string_view{"alice"}, lims::LimsRole::Supervisor},
                                              std::pair{std::string_view{"bob"}, lims::LimsRole::Analyst},
                                              std::pair{std::string_view{"bob"}, lims::LimsRole::Verifier}}) {
            LegacyOperatorRecord row;
            row.principal = Lightweight::SqlAnsiString<64>{principal};
            row.role = static_cast<int>(role);
            row.grantedBy = Lightweight::SqlAnsiString<64>{"alice"};
            mapper.Create(row);
        }
    }
    probe.applyRest();
    // The migrated alice supervises the seeded company, so she sees all three assignments.
    ScopedPrincipal const alice{"alice"};
    lims::OrgModel org;
    auto const assignments = org.execute(lims::ListRoleAssignments{}).assignments;
    REQUIRE(assignments.size() == 3);
    for (auto const& assignment : assignments) {
        CHECK(assignment.scopeKind == lims::ScopeKind::Company);
        CHECK(assignment.company == lims::test::seededCompany());
    }
    CHECK(
        holds(lims::OrgModel::rolesIn("alice", lims::Scope{lims::test::seededCompany()}), lims::LimsRole::Supervisor));
    auto const bob = lims::OrgModel::rolesIn("bob", lims::Scope{lims::test::seededCompany()});
    CHECK(bob.size() == 2);
    CHECK(holds(bob, lims::LimsRole::Analyst));
    CHECK(holds(bob, lims::LimsRole::Verifier));
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

// Mutation: skip the supervisor appointment in CreateCompany (alice holds nothing at Osaka and bob's assignment is
// refused).
TEST_CASE("org roles: creating a company appoints its creator supervisor there", "[lims][org][roles]") {
    DbFixture fixture;
    ScopedPrincipal const alice{"alice"};
    lims::OrgModel org;
    org.execute(lims::AssignRole{
        .principal = "alice", .role = lims::LimsRole::Supervisor, .company = lims::test::seededCompany(), .site = {}});
    auto const osaka = org.execute(lims::CreateCompany{
        .name = "Osaka branch", .baseCurrency = lims::CurrencyCode::JPY, .fiscalYearStart = lims::Month::April});

    CHECK(holds(lims::OrgModel::rolesIn("alice", lims::Scope{osaka.id}), lims::LimsRole::Supervisor));
    auto const assignments = org.execute(lims::ListRoleAssignments{}).assignments;
    REQUIRE(assignments.size() == 2);
    CHECK(assignments.back().company == osaka.id);
    CHECK(assignments.back().grantedBy == "alice");

    auto const bob = org.execute(
        lims::AssignRole{.principal = "bob", .role = lims::LimsRole::Analyst, .company = osaka.id, .site = {}});
    CHECK(bob.company == osaka.id);
    {
        // Roles are not hierarchical: mallory, a supervisor at neither company, cannot assign at Osaka.
        ScopedPrincipal const mallory{"mallory"};
        CHECK_THROWS_AS(::morph::model::executeChecked(org, lims::AssignRole{.principal = "mallory",
                                                                             .role = lims::LimsRole::Supervisor,
                                                                             .company = osaka.id,
                                                                             .site = {}}),
                        lims::Forbidden);
    }
}

// Mutation: drop the transaction around CreateCompany (the company stays behind without its supervisor).
TEST_CASE("org roles: a company whose supervisor cannot be appointed is not created", "[lims][org][roles]") {
    DbFixture fixture;
    ScopedPrincipal const alice{"alice"};
    lims::OrgModel org;
    {
        // The statement that fails is the one the model really issues.
        Lightweight::SqlStatement stmt;
        (void)stmt.ExecuteDirect(
            "CREATE TRIGGER morph_fail_insert BEFORE INSERT ON \"org_role_assignments\" BEGIN SELECT RAISE(ABORT, "
            "'injected'); END");
    }
    CHECK_THROWS(::morph::model::executeChecked(org, lims::CreateCompany{.name = "Osaka branch",
                                                                         .baseCurrency = lims::CurrencyCode::JPY,
                                                                         .fiscalYearStart = lims::Month::April}));
    {
        Lightweight::SqlStatement stmt;
        (void)stmt.ExecuteDirect("DROP TRIGGER morph_fail_insert");
    }
    // Read straight from the tables: alice holds no role, so no list would show her anything.
    Lightweight::DataMapper mapper;
    CHECK(mapper.Query<lims::db::CompanyRecord>().All().size() == 1);
    CHECK(mapper.Query<lims::db::RoleAssignmentRecord>().All().empty());
}

// Mutations: count supervisors across the whole table (the revoke at Osaka goes through because the seeded company
// has one); count site-scoped supervisors (carol, supervisor at the field lab, lets alice's revoke through).
TEST_CASE("org roles: a company's last supervisor cannot be revoked, whoever else supervises elsewhere",
          "[lims][org][roles]") {
    DbFixture fixture;
    ScopedPrincipal const alice{"alice"};
    lims::OrgModel org;
    org.execute(lims::AssignRole{
        .principal = "alice", .role = lims::LimsRole::Supervisor, .company = lims::test::seededCompany(), .site = {}});
    auto const osaka = org.execute(lims::CreateCompany{
        .name = "Osaka branch", .baseCurrency = lims::CurrencyCode::JPY, .fiscalYearStart = lims::Month::April});
    auto const osakaLab = org.execute(lims::CreateSite{.companyId = osaka.id, .name = "Osaka lab"});
    auto const aliceAtOsaka = [&] {
        for (auto const& assignment : org.execute(lims::ListRoleAssignments{}).assignments) {
            if (assignment.company == osaka.id) {
                return assignment;
            }
        }
        FAIL("alice holds no assignment at Osaka");
        return lims::RoleAssignmentView{};
    }();

    // Another company has a supervisor, and so does a site of this one; neither is this company's.
    org.execute(lims::AssignRole{
        .principal = "carol", .role = lims::LimsRole::Supervisor, .company = {}, .site = osakaLab.id});
    CHECK_THROWS_AS(::morph::model::executeChecked(org, lims::RevokeRole{.assignmentId = aliceAtOsaka.id}),
                    lims::Conflict);
    CHECK(holds(lims::OrgModel::rolesIn("alice", lims::Scope{osaka.id}), lims::LimsRole::Supervisor));

    // A second company-scope supervisor makes the first one's revoke legitimate.
    org.execute(
        lims::AssignRole{.principal = "dave", .role = lims::LimsRole::Supervisor, .company = osaka.id, .site = {}});
    CHECK(org.execute(lims::RevokeRole{.assignmentId = aliceAtOsaka.id}).assignmentId == aliceAtOsaka.id);
    CHECK_FALSE(holds(lims::OrgModel::rolesIn("alice", lims::Scope{osaka.id}), lims::LimsRole::Supervisor));
}

// Mutation: count only company-scope supervisors in the "anywhere" rule (the bootstrap at the site is revoked and
// the next caller bootstraps again).
TEST_CASE("org roles: the last supervisor anywhere cannot be revoked, even one scoped to a site",
          "[lims][org][roles]") {
    DbFixture fixture;
    lims::OrgModel org;
    ScopedPrincipal const alice{"alice"};
    auto const atSite = org.execute(lims::AssignRole{
        .principal = "alice", .role = lims::LimsRole::Supervisor, .company = {}, .site = lims::test::seededSite()});
    CHECK_THROWS_AS(::morph::model::executeChecked(org, lims::RevokeRole{.assignmentId = atSite.id}), lims::Conflict);
    CHECK(lims::OrgModel::supervisorExists());

    // A second supervisor, anywhere, makes the first one's revoke legitimate.
    org.execute(lims::AssignRole{
        .principal = "bob", .role = lims::LimsRole::Supervisor, .company = {}, .site = lims::test::seededSite()});
    CHECK(org.execute(lims::RevokeRole{.assignmentId = atSite.id}).assignmentId == atSite.id);
}

// Mutation: decode every supervisor row's scope kind (the corrupt row at Osaka makes the revoke at the seeded
// company throw).
TEST_CASE("org roles: a corrupt supervisor row at another company does not block a revoke here",
          "[lims][org][roles]") {
    DbFixture fixture;
    lims::OrgModel org;
    ScopedPrincipal const alice{"alice"};
    org.execute(lims::AssignRole{
        .principal = "alice", .role = lims::LimsRole::Supervisor, .company = lims::test::seededCompany(), .site = {}});
    auto const dave = org.execute(lims::AssignRole{
        .principal = "dave", .role = lims::LimsRole::Supervisor, .company = lims::test::seededCompany(), .site = {}});
    // erin supervises the seeded site, which lets her found the second company, so the corrupt row is hers and no
    // lookup of alice's roles reads it.
    org.execute(lims::AssignRole{
        .principal = "erin", .role = lims::LimsRole::Supervisor, .company = {}, .site = lims::test::seededSite()});
    auto const osaka = [&] {
        ScopedPrincipal const asErin{"erin"};
        return org.execute(lims::CreateCompany{
            .name = "Osaka branch", .baseCurrency = lims::CurrencyCode::JPY, .fiscalYearStart = lims::Month::April});
    }();
    {
        Lightweight::DataMapper mapper;
        auto rows = mapper.Query<lims::db::RoleAssignmentRecord>()
                        .Where(Lightweight::FieldNameOf<&lims::db::RoleAssignmentRecord::scopeId>, "=", *osaka.id)
                        .All();
        REQUIRE(rows.size() == 1);
        rows.front().scopeKind = 99;
        mapper.Update(rows.front());
    }
    CHECK(org.execute(lims::RevokeRole{.assignmentId = dave.id}).assignmentId == dave.id);
}

// Mutations: look the scope up before the permission check (mallory gets NotFound); check permission only when the
// scope exists (the same); refuse a supervisor's unknown scope with Forbidden instead (alice gets Forbidden).
TEST_CASE("org roles: a caller who supervises nowhere cannot tell which scopes and assignments exist",
          "[lims][org][roles]") {
    DbFixture fixture;
    lims::OrgModel org;
    lims::RoleAssignmentView bob;
    {
        ScopedPrincipal const alice{"alice"};
        org.execute(lims::AssignRole{.principal = "alice",
                                     .role = lims::LimsRole::Supervisor,
                                     .company = lims::test::seededCompany(),
                                     .site = {}});
        bob = org.execute(lims::AssignRole{
            .principal = "bob", .role = lims::LimsRole::Analyst, .company = lims::test::seededCompany(), .site = {}});
        CHECK_THROWS_AS(::morph::model::executeChecked(org, lims::AssignRole{.principal = "bob",
                                                                             .role = lims::LimsRole::Analyst,
                                                                             .company = {},
                                                                             .site = lims::SiteId{999}}),
                        lims::Forbidden);
        CHECK_THROWS_AS(
            ::morph::model::executeChecked(org, lims::RevokeRole{.assignmentId = lims::RoleAssignmentId{999}}),
            lims::Forbidden);
    }
    ScopedPrincipal const mallory{"mallory"};
    CHECK_THROWS_AS(::morph::model::executeChecked(org, lims::AssignRole{.principal = "mallory",
                                                                         .role = lims::LimsRole::Analyst,
                                                                         .company = {},
                                                                         .site = lims::SiteId{999}}),
                    lims::Forbidden);
    CHECK_THROWS_AS(::morph::model::executeChecked(org, lims::AssignRole{.principal = "mallory",
                                                                         .role = lims::LimsRole::Analyst,
                                                                         .company = lims::CompanyId{999},
                                                                         .site = {}}),
                    lims::Forbidden);
    CHECK_THROWS_AS(::morph::model::executeChecked(org, lims::RevokeRole{.assignmentId = lims::RoleAssignmentId{999}}),
                    lims::Forbidden);
    CHECK_THROWS_AS(::morph::model::executeChecked(org, lims::RevokeRole{.assignmentId = bob.id}), lims::Forbidden);
}

// Mutations: bypass the dispatcher's journaling of a refused action (no Failed entry); drop AssignRole's
// loggability (neither entry appears).
TEST_CASE("org roles: the dispatcher journals a role assignment, the refused ones included", "[lims][org][roles]") {
    DbFixture fixture;
    auto const log = std::make_shared<morph::journal::InMemoryActionLog>(morph::ladder::testkit::storageOwner());
    auto holder = morph::model::detail::defaultRegistry().create("OrgModel");
    REQUIRE(holder != nullptr);
    holder->attachActionLog(log, std::string{});
    auto const dispatch = [&](std::string_view principal, lims::AssignRole const& action) {
        ScopedPrincipal const caller{std::string{principal}};
        morph::model::detail::defaultDispatcher().dispatch(
            "OrgModel", "AssignRole", *holder, morph::model::ActionTraits<lims::AssignRole>::toJson(action));
    };

    dispatch("alice", lims::AssignRole{.principal = "alice",
                                       .role = lims::LimsRole::Supervisor,
                                       .company = lims::test::seededCompany(),
                                       .site = {}});
    CHECK_THROWS_AS(dispatch("mallory", lims::AssignRole{.principal = "mallory",
                                                         .role = lims::LimsRole::Supervisor,
                                                         .company = lims::test::seededCompany(),
                                                         .site = {}}),
                    lims::Forbidden);

    auto const entries = log->entries();
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].modelType == "OrgModel");
    CHECK(entries[0].actionType == "AssignRole");
    CHECK(entries[0].outcome == morph::journal::Outcome::Succeeded);
    CHECK(entries[0].principal == "alice");
    CHECK(entries[1].actionType == "AssignRole");
    CHECK(entries[1].outcome == morph::journal::Outcome::Failed);
    CHECK(entries[1].principal == "mallory");
    CHECK_FALSE(entries[1].error.empty());
}

// Mutation: drop the id check from RevokeRole (an empty id reaches the lookup and revokes nothing, silently).
TEST_CASE("org: a revoke that names no assignment is refused", "[lims][org]") {
    DbFixture fixture;
    lims::test::grant("alice", lims::LimsRole::Supervisor);
    ScopedPrincipal const alice{"alice"};
    lims::OrgModel org;
    CHECK_THROWS_AS(org.execute(lims::RevokeRole{}), lims::ValidationError);
}

// Mutations: let a service principal log in; skip the username check; issue a token with no issuer installed.
TEST_CASE("auth: sign-in refuses a malformed or reserved principal and needs an issuer", "[lims][auth]") {
    lims::LimsAuthModel auth;
    auto const previous = lims::auth::tokenIssuer();
    lims::auth::setTokenIssuer(
        std::make_shared<morph::session::TokenIssuer>("auth-test-secret", morph::session::hmacSha256));

    auto const signedIn = auth.execute(lims::LimsLogin{.username = "alice"});
    CHECK(signedIn.principal == "alice");
    CHECK_FALSE(signedIn.token.empty());
    CHECK_THROWS_AS(auth.execute(lims::LimsLogin{.username = ""}), lims::ValidationError);
    CHECK_THROWS_AS(auth.execute(lims::LimsLogin{.username = "not a principal"}), lims::ValidationError);
    CHECK_THROWS_AS(auth.execute(lims::LimsLogin{.username = std::string(65, 'a')}), lims::ValidationError);
    CHECK_THROWS_AS(auth.execute(lims::LimsLogin{.username = "system:server"}), lims::ValidationError);

    lims::auth::setTokenIssuer(nullptr);
    CHECK_THROWS_AS(auth.execute(lims::LimsLogin{.username = "alice"}), lims::ValidationError);
    lims::auth::setTokenIssuer(previous);
}

// Mutation: answer UserView without its displayName in ListUsers (the check for "Dr. Anna Meyer" fails).
TEST_CASE("org: a supervisor names a person, and the users list carries the name", "[lims][org]") {
    DbFixture fixture;
    lims::test::grant("sam", lims::LimsRole::Supervisor);
    lims::test::grant("anna", lims::LimsRole::Analyst);
    ScopedPrincipal const sam{"sam"};
    lims::OrgModel org;
    CHECK(org.execute(lims::SetDisplayName{.principal = "anna", .displayName = "Dr. Anna Meyer"}).displayName ==
          "Dr. Anna Meyer");
    auto const users = org.execute(lims::ListUsers{}).users;
    auto const anna = std::ranges::find(users, std::string{"anna"}, &lims::UserView::id);
    REQUIRE(anna != users.end());
    CHECK(anna->displayName == "Dr. Anna Meyer");
    CHECK(lims::OrgModel::displayNameOf("anna") == "Dr. Anna Meyer");
    CHECK(lims::OrgModel::displayNameOf("nobody") == "nobody");
}

// Mutation: drop the self-or-supervisor check from SetDisplayName (rita renames anna).
TEST_CASE("org: a person names themselves; nobody else but a supervisor of their company names them", "[lims][org]") {
    DbFixture fixture;
    lims::test::grant("anna", lims::LimsRole::Analyst);
    lims::test::grant("rita", lims::LimsRole::Analyst);
    {
        ScopedPrincipal const anna{"anna"};
        CHECK(lims::OrgModel{}
                  .execute(lims::SetDisplayName{.principal = "anna", .displayName = "Anna Meyer"})
                  .displayName == "Anna Meyer");
        CHECK(lims::OrgModel{}.execute(lims::GetSignedInUser{}).displayName == "Anna Meyer");
    }
    ScopedPrincipal const rita{"rita"};
    CHECK_THROWS_AS(lims::OrgModel{}.execute(lims::SetDisplayName{.principal = "anna", .displayName = "Someone"}),
                    lims::Forbidden);
}

// Mutation: make SetDisplayName insert a second row for a person already named (the users list then carries the
// first name, or the unique index refuses); drop the length check (the 129-byte name reaches the table).
TEST_CASE("org: naming a person again replaces the name, and an empty or oversized name is refused", "[lims][org]") {
    DbFixture fixture;
    lims::test::grant("sam", lims::LimsRole::Supervisor);
    lims::test::grant("anna", lims::LimsRole::Analyst);
    ScopedPrincipal const sam{"sam"};
    lims::OrgModel org;
    static_cast<void>(org.execute(lims::SetDisplayName{.principal = "anna", .displayName = "Anna M."}));
    CHECK(org.execute(lims::SetDisplayName{.principal = "anna", .displayName = "Dr. Anna Meyer"}).displayName ==
          "Dr. Anna Meyer");
    CHECK(lims::OrgModel::displayNameOf("anna") == "Dr. Anna Meyer");
    CHECK_THROWS_AS(org.execute(lims::SetDisplayName{.principal = "anna", .displayName = ""}), lims::ValidationError);
    CHECK_THROWS_AS(org.execute(lims::SetDisplayName{.principal = "anna", .displayName = std::string(129, 'x')}),
                    lims::ValidationError);
    CHECK(org.execute(lims::SetDisplayName{.principal = "anna", .displayName = "Tobias Krüger"}).displayName ==
          "Tobias Krüger");
}

// Mutation: let a supervisor of any company name anyone (sam names a person who holds a role only elsewhere).
TEST_CASE("org: a supervisor names only people who hold a role in a company they supervise", "[lims][org]") {
    DbFixture fixture;
    lims::test::grant("sam", lims::LimsRole::Supervisor);
    lims::test::grant("zed", lims::LimsRole::Supervisor);
    {
        ScopedPrincipal const zed{"zed"};
        lims::OrgModel org;
        auto const other = org.execute(lims::CreateCompany{
            .name = "Elsewhere", .baseCurrency = lims::CurrencyCode::EUR, .fiscalYearStart = lims::Month::January});
        static_cast<void>(org.execute(
            lims::AssignRole{.principal = "bob", .role = lims::LimsRole::Analyst, .company = other.id, .site = {}}));
    }
    ScopedPrincipal const sam{"sam"};
    lims::OrgModel org;
    CHECK_THROWS_AS(org.execute(lims::SetDisplayName{.principal = "bob", .displayName = "Bob"}), lims::Forbidden);
    CHECK_THROWS_AS(org.execute(lims::SetDisplayName{.principal = "stranger", .displayName = "Stranger"}),
                    lims::Forbidden);
}
