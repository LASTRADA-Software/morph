// SPDX-License-Identifier: Apache-2.0
//
// Account-scoped RBAC and the industry per-field write-guard
// (README build order §5).

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <string>

#include "crm/core/errors.hpp"
#include "crm/models/account_model.hpp"
#include "crm_test_support.hpp"
#include "testkit/db_fixture.hpp"

using crm::test::ScopedPrincipal;
using morph::ladder::testkit::DbFixture;

namespace {
crm::AccountId createAcme(crm::AccountModel& model) {
    return model.execute(crm::CreateAccount{.name = "Acme Corp", .industry = "Manufacturing", .website = ""})
        .accountId;
}
}  // namespace

// ── Fail-open default: unaffected accounts behave exactly as before §5 ──────

TEST_CASE("An account with no declared roles is fail-open for any authenticated principal", "[crm][authz]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel model;

    const auto accountId = createAcme(model);
    // No SetAccountRole call at all — the same shape every step 1-4 test uses.
    const auto updated = model.execute(crm::UpdateAccount{
        .accountId = accountId, .name = "Acme Corp", .industry = "Industrials", .website = "", .expectedVersion = 1});
    CHECK(updated.account.industry == "Industrials");
}

// ── Per-entity RBAC ──────────────────────────────────────────────────────

TEST_CASE("SetAccountRole requires Manager on an account that already has roles", "[crm][authz]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel model;

    const auto accountId = createAcme(model);
    model.execute(crm::SetAccountRole{.accountId = accountId, .principal = "alice", .role = crm::Role::Manager});

    // Now that the account has at least one role row, enforcement engages:
    // bob (unlisted, implicit Viewer) may not grant himself Manager.
    const ScopedPrincipal bob{"bob"};
    CHECK_THROWS_AS(
        model.execute(crm::SetAccountRole{.accountId = accountId, .principal = "bob", .role = crm::Role::Manager}),
        crm::Forbidden);
}

TEST_CASE("A Member may UpdateAccount without touching industry", "[crm][authz]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel model;

    const auto accountId = createAcme(model);
    model.execute(crm::SetAccountRole{.accountId = accountId, .principal = "alice", .role = crm::Role::Manager});
    model.execute(crm::SetAccountRole{.accountId = accountId, .principal = "bob", .role = crm::Role::Member});

    const ScopedPrincipal bob{"bob"};
    const auto updated = model.execute(crm::UpdateAccount{.accountId = accountId,
                                                          .name = "Acme Corporation",
                                                          .industry = "Manufacturing",  // unchanged
                                                          .website = "",
                                                          .expectedVersion = 1});
    CHECK(updated.account.name == "Acme Corporation");
}

TEST_CASE("A Viewer may not UpdateAccount at all once roles are declared", "[crm][authz]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel model;

    const auto accountId = createAcme(model);
    model.execute(crm::SetAccountRole{.accountId = accountId, .principal = "alice", .role = crm::Role::Manager});
    model.execute(crm::SetAccountRole{.accountId = accountId, .principal = "carol", .role = crm::Role::Viewer});

    const ScopedPrincipal carol{"carol"};
    CHECK_THROWS_AS(model.execute(crm::UpdateAccount{.accountId = accountId,
                                                     .name = "Acme Corporation",
                                                     .industry = "Manufacturing",
                                                     .website = "",
                                                     .expectedVersion = 1}),
                    crm::Forbidden);
}

TEST_CASE("An unlisted principal on a role-declaring account is implicitly Viewer", "[crm][authz]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel model;

    const auto accountId = createAcme(model);
    model.execute(crm::SetAccountRole{.accountId = accountId, .principal = "alice", .role = crm::Role::Manager});

    const ScopedPrincipal dave{"dave"};  // never granted any role on this account
    CHECK_THROWS_AS(model.execute(crm::UpdateAccount{.accountId = accountId,
                                                     .name = "Acme Corporation",
                                                     .industry = "Manufacturing",
                                                     .website = "",
                                                     .expectedVersion = 1}),
                    crm::Forbidden);
}

// ── Per-field write-guard: industry is Manager-only to actually change ─────

TEST_CASE("A Member changing industry throws Forbidden even though UpdateAccount itself is allowed",
          "[crm][authz][per_field]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel model;

    const auto accountId = createAcme(model);
    model.execute(crm::SetAccountRole{.accountId = accountId, .principal = "alice", .role = crm::Role::Manager});
    model.execute(crm::SetAccountRole{.accountId = accountId, .principal = "bob", .role = crm::Role::Member});

    const ScopedPrincipal bob{"bob"};
    CHECK_THROWS_AS(model.execute(crm::UpdateAccount{.accountId = accountId,
                                                     .name = "Acme Corp",
                                                     .industry = "Retail",  // changed — Manager-only
                                                     .website = "",
                                                     .expectedVersion = 1}),
                    crm::Forbidden);
}

TEST_CASE("A Manager may change industry", "[crm][authz][per_field]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel model;

    const auto accountId = createAcme(model);
    model.execute(crm::SetAccountRole{.accountId = accountId, .principal = "alice", .role = crm::Role::Manager});

    const auto updated = model.execute(crm::UpdateAccount{
        .accountId = accountId, .name = "Acme Corp", .industry = "Retail", .website = "", .expectedVersion = 1});
    CHECK(updated.account.industry == "Retail");
}

TEST_CASE("GetAccountRoles lists every declared role", "[crm][authz]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel model;

    const auto accountId = createAcme(model);
    model.execute(crm::SetAccountRole{.accountId = accountId, .principal = "alice", .role = crm::Role::Manager});
    model.execute(crm::SetAccountRole{.accountId = accountId, .principal = "bob", .role = crm::Role::Member});

    const auto roles = model.execute(crm::GetAccountRoles{.accountId = accountId});
    REQUIRE(roles.roles.size() == 2);
}

TEST_CASE("SetAccountRole replaces (not duplicates) an existing principal's role", "[crm][authz]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel model;

    const auto accountId = createAcme(model);
    model.execute(crm::SetAccountRole{.accountId = accountId, .principal = "alice", .role = crm::Role::Manager});
    model.execute(crm::SetAccountRole{.accountId = accountId, .principal = "bob", .role = crm::Role::Viewer});
    model.execute(crm::SetAccountRole{.accountId = accountId, .principal = "bob", .role = crm::Role::Member});

    const auto roles = model.execute(crm::GetAccountRoles{.accountId = accountId});
    REQUIRE(roles.roles.size() == 2);  // alice, bob — not three rows
    const auto bobRole =
        std::find_if(roles.roles.begin(), roles.roles.end(), [](auto const& r) { return r.principal == "bob"; });
    REQUIRE(bobRole != roles.roles.end());
    CHECK(bobRole->role == crm::Role::Member);
}
