// SPDX-License-Identifier: Apache-2.0
//
// AccountModel CRUD (README build order §1).

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/journal/action_log.hpp>
#include <morph/session/session.hpp>
#include <string>

#include "crm/core/errors.hpp"
#include "crm/models/account_model.hpp"
#include "crm_test_support.hpp"
#include "testkit/db_fixture.hpp"

using crm::test::ScopedPrincipal;
using morph::ladder::testkit::DbFixture;

namespace {
crm::CreateAccount acme() {
    return crm::CreateAccount{.name = "Acme Corp", .industry = "Manufacturing", .website = "acme.example"};
}
}  // namespace

TEST_CASE("CreateAccount creates the account", "[crm][account]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel model;

    const auto created = model.execute(acme());
    REQUIRE(created.accountId.hasValue());

    const auto view = model.execute(crm::GetAccount{.accountId = created.accountId});
    CHECK(view.name == "Acme Corp");
    CHECK(view.industry == "Manufacturing");
    CHECK(view.website == "acme.example");
    CHECK(view.version == 1);
}

TEST_CASE("CreateAccount with an empty name is rejected", "[crm][account]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel model;

    auto bad = acme();
    bad.name.clear();
    CHECK_THROWS_AS(model.execute(bad), crm::ValidationError);
}

TEST_CASE("Creating an account with no principal is refused", "[crm][account][audit]") {
    DbFixture fixture;
    crm::AccountModel model;  // no ScopedPrincipal
    CHECK_THROWS_AS(model.execute(acme()), crm::EmptyPrincipalError);
}

TEST_CASE("UpdateAccount replaces the editable fields and bumps the version", "[crm][account]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel model;

    const auto created = model.execute(acme());
    const auto updated = model.execute(crm::UpdateAccount{
        .accountId = created.accountId,
        .name = "Acme Corporation",
        .industry = "Industrials",
        .website = "acme.example",
        .expectedVersion = 1,
    });
    CHECK(updated.account.name == "Acme Corporation");
    CHECK(updated.account.industry == "Industrials");
    CHECK(updated.account.version == 2);
}

TEST_CASE("UpdateAccount with a stale version is rejected as a Conflict", "[crm][account]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel model;

    const auto created = model.execute(acme());
    CHECK_THROWS_AS(model.execute(crm::UpdateAccount{
                        .accountId = created.accountId,
                        .name = "Acme Corporation",
                        .industry = "Industrials",
                        .website = "acme.example",
                        .expectedVersion = 0,  // stale
                    }),
                    crm::Conflict);
}

TEST_CASE("UpdateAccount naming a nonexistent account is NotFound", "[crm][account]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel model;

    CHECK_THROWS_AS(model.execute(crm::UpdateAccount{
                        .accountId = crm::AccountId{999},
                        .name = "Nobody",
                        .industry = "",
                        .website = "",
                        .expectedVersion = 1,
                    }),
                    crm::NotFound);
}

TEST_CASE("GetAccount naming a nonexistent account is NotFound", "[crm][account]") {
    DbFixture fixture;
    crm::AccountModel model;
    CHECK_THROWS_AS(model.execute(crm::GetAccount{.accountId = crm::AccountId{999}}), crm::NotFound);
}

TEST_CASE("ListAccounts lists every account", "[crm][account]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel model;

    model.execute(acme());
    model.execute(crm::CreateAccount{.name = "Globex", .industry = "Retail", .website = "globex.example"});

    const auto listed = model.execute(crm::ListAccounts{});
    REQUIRE(listed.accounts.size() == 2);
}

TEST_CASE("ListAccountOptions serves {id, name} rows for the Choice combo", "[crm][account]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    crm::AccountModel model;

    const auto created = model.execute(acme());
    const auto options = model.execute(crm::ListAccountOptions{});
    REQUIRE(options.accounts.size() == 1);
    CHECK(options.accounts.front().id == std::to_string(*created.accountId));
    CHECK(options.accounts.front().name == "Acme Corp");
}

TEST_CASE("AccountModel journals its edits against the attached identity", "[crm][account][audit]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    auto log = std::make_shared<morph::journal::InMemoryActionLog>();
    crm::AccountModel model;
    model.attachActionLog(log, std::string{"accounts"});

    const auto created = model.execute(acme());
    model.execute(crm::UpdateAccount{
        .accountId = created.accountId,
        .name = "Acme Corporation",
        .industry = "Industrials",
        .website = "acme.example",
        .expectedVersion = 1,
    });

    const auto entries = log->entries("accounts");
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].actionType == "CreateAccount");
    CHECK(entries[1].actionType == "UpdateAccount");
    for (const auto& entry : entries) {
        CHECK(entry.modelType == "AccountModel");
        CHECK(entry.principal == "alice");
        CHECK(entry.outcome == morph::journal::Outcome::Succeeded);
    }
}
