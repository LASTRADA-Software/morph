// SPDX-License-Identifier: Apache-2.0
//
// GetAccountHistory (field-level audit, with per-field redaction) and
// UndoLastAccountChange (README build order §6).

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
crm::AccountId createAcme(crm::AccountModel& model) {
    return model.execute(crm::CreateAccount{.name = "Acme Corp", .industry = "Manufacturing", .website = ""})
        .accountId;
}
}  // namespace

// ── GetAccountHistory ────────────────────────────────────────────────────

TEST_CASE("GetAccountHistory lists every recorded change to an account", "[crm][history]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    auto log = std::make_shared<morph::journal::InMemoryActionLog>();
    crm::AccountModel model;
    model.attachActionLog(log, std::string{});

    const auto accountId = createAcme(model);
    model.execute(crm::UpdateAccount{.accountId = accountId,
                                     .name = "Acme Corporation",
                                     .industry = "Manufacturing",
                                     .website = "",
                                     .expectedVersion = 1});

    const auto history = model.execute(crm::GetAccountHistory{.accountId = accountId});
    REQUIRE(history.entries.size() == 2);
    CHECK(history.entries[0].actionType == "CreateAccount");
    CHECK(history.entries[1].actionType == "UpdateAccount");
    CHECK(history.entries[1].name == "Acme Corporation");
}

TEST_CASE("GetAccountHistory doesn't mix entries from a different account", "[crm][history]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    auto log = std::make_shared<morph::journal::InMemoryActionLog>();
    crm::AccountModel model;
    model.attachActionLog(log, std::string{});

    const auto acmeId = createAcme(model);
    const auto globexId =
        model.execute(crm::CreateAccount{.name = "Globex", .industry = "Retail", .website = ""}).accountId;
    model.execute(crm::UpdateAccount{
        .accountId = globexId, .name = "Globex Inc", .industry = "Retail", .website = "", .expectedVersion = 1});

    const auto acmeHistory = model.execute(crm::GetAccountHistory{.accountId = acmeId});
    REQUIRE(acmeHistory.entries.size() == 1);  // just its own CreateAccount

    const auto globexHistory = model.execute(crm::GetAccountHistory{.accountId = globexId});
    REQUIRE(globexHistory.entries.size() == 2);
}

TEST_CASE("A restricted principal's history redacts industry's historical value", "[crm][history][per_field]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    auto log = std::make_shared<morph::journal::InMemoryActionLog>();
    crm::AccountModel model;
    model.attachActionLog(log, std::string{});

    const auto accountId = createAcme(model);
    model.execute(crm::SetAccountRole{.accountId = accountId, .principal = "alice", .role = crm::Role::Manager});
    model.execute(crm::SetAccountRole{.accountId = accountId, .principal = "bob", .role = crm::Role::Member});
    model.execute(crm::UpdateAccount{
        .accountId = accountId, .name = "Acme Corp", .industry = "Retail", .website = "", .expectedVersion = 1});

    const ScopedPrincipal bob{"bob"};
    const auto history = model.execute(crm::GetAccountHistory{.accountId = accountId});
    for (const auto& entry : history.entries) {
        // A Member never sees industry's historical value at all — the
        // "test that a restricted principal leaks nothing through history"
        // requirement crm/README.md names.
        CHECK(entry.industry.empty());
    }
}

TEST_CASE("A Manager's history shows industry's historical value", "[crm][history][per_field]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    auto log = std::make_shared<morph::journal::InMemoryActionLog>();
    crm::AccountModel model;
    model.attachActionLog(log, std::string{});

    const auto accountId = createAcme(model);
    model.execute(crm::SetAccountRole{.accountId = accountId, .principal = "alice", .role = crm::Role::Manager});
    model.execute(crm::UpdateAccount{
        .accountId = accountId, .name = "Acme Corp", .industry = "Retail", .website = "", .expectedVersion = 1});

    const auto history = model.execute(crm::GetAccountHistory{.accountId = accountId});
    REQUIRE(history.entries.size() == 3);  // CreateAccount, SetAccountRole, UpdateAccount
    CHECK(history.entries[2].actionType == "UpdateAccount");
    CHECK(history.entries[2].industry == "Retail");
}

// ── UndoLastAccountChange ─────────────────────────────────────────────────

TEST_CASE("UndoLastAccountChange restores the account's fields to their prior value", "[crm][history][undo]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    auto log = std::make_shared<morph::journal::InMemoryActionLog>();
    crm::AccountModel model;
    model.attachActionLog(log, std::string{});

    const auto accountId = createAcme(model);
    model.execute(crm::UpdateAccount{.accountId = accountId,
                                     .name = "Acme Corporation",
                                     .industry = "Manufacturing",
                                     .website = "",
                                     .expectedVersion = 1});

    const auto undone = model.execute(crm::UndoLastAccountChange{.accountId = accountId});
    CHECK(undone.account.name == "Acme Corp");  // the CreateAccount-era name, restored

    const auto view = model.execute(crm::GetAccount{.accountId = accountId});
    CHECK(view.name == "Acme Corp");
}

TEST_CASE("UndoLastAccountChange records its own new journal entry, not a rewrite", "[crm][history][undo][audit]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    auto log = std::make_shared<morph::journal::InMemoryActionLog>();
    crm::AccountModel model;
    model.attachActionLog(log, std::string{});

    const auto accountId = createAcme(model);
    model.execute(crm::UpdateAccount{.accountId = accountId,
                                     .name = "Acme Corporation",
                                     .industry = "Manufacturing",
                                     .website = "",
                                     .expectedVersion = 1});
    model.execute(crm::UndoLastAccountChange{.accountId = accountId});

    const auto history = model.execute(crm::GetAccountHistory{.accountId = accountId});
    REQUIRE(history.entries.size() == 3);  // Create, Update, UndoLastAccountChange
    CHECK(history.entries[2].actionType == "UndoLastAccountChange");
}

TEST_CASE("UndoLastAccountChange with no prior change to undo to is NotFound", "[crm][history][undo]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    auto log = std::make_shared<morph::journal::InMemoryActionLog>();
    crm::AccountModel model;
    model.attachActionLog(log, std::string{});

    const auto accountId = createAcme(model);  // only CreateAccount recorded — nothing to undo to
    CHECK_THROWS_AS(model.execute(crm::UndoLastAccountChange{.accountId = accountId}), crm::NotFound);
}

TEST_CASE("A Member undoing a change that would restore industry to a Manager-set value is Forbidden",
          "[crm][history][undo][per_field]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    auto log = std::make_shared<morph::journal::InMemoryActionLog>();
    crm::AccountModel model;
    model.attachActionLog(log, std::string{});

    const auto accountId = createAcme(model);  // industry = "Manufacturing"
    model.execute(crm::SetAccountRole{.accountId = accountId, .principal = "alice", .role = crm::Role::Manager});
    model.execute(crm::SetAccountRole{.accountId = accountId, .principal = "bob", .role = crm::Role::Member});
    model.execute(crm::UpdateAccount{
        .accountId = accountId, .name = "Acme Corp", .industry = "Retail", .website = "", .expectedVersion = 1});

    const ScopedPrincipal bob{"bob"};
    // Undoing would restore industry from "Retail" back to "Manufacturing" —
    // a change to a Manager-only field, so a Member may not perform it,
    // exactly as they could not have made that change directly.
    CHECK_THROWS_AS(model.execute(crm::UndoLastAccountChange{.accountId = accountId}), crm::Forbidden);
}

TEST_CASE("Undoing with no principal is refused", "[crm][history][undo][audit]") {
    DbFixture fixture;
    auto log = std::make_shared<morph::journal::InMemoryActionLog>();
    crm::AccountModel model;
    model.attachActionLog(log, std::string{});
    crm::AccountId accountId;
    {
        const ScopedPrincipal alice{"alice"};
        accountId = createAcme(model);
        model.execute(crm::UpdateAccount{.accountId = accountId,
                                         .name = "Acme Corporation",
                                         .industry = "Manufacturing",
                                         .website = "",
                                         .expectedVersion = 1});
    }
    CHECK_THROWS_AS(model.execute(crm::UndoLastAccountChange{.accountId = accountId}), crm::EmptyPrincipalError);
}
