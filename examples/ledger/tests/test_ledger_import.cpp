// SPDX-License-Identifier: Apache-2.0
#include "ledger/core/errors.hpp"
#include "ledger/db/ledger_entity.hpp"
#include "ledger/models/ledger_model.hpp"
#include "testkit/db_fixture.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <catch2/catch_test_macros.hpp>
#include <morph/session/session.hpp>

#include <algorithm>

namespace {

/// @brief A `Context` carrying only @p principal -- same helper
///        `test_ledger_model.cpp` uses, kept local to this file rather than
///        shared, matching this codebase's existing per-test-file
///        duplication of this exact helper (`test_budget_model.cpp`,
///        `test_rule_model.cpp`).
[[nodiscard]] morph::session::Context contextFor(std::string principal) {
    morph::session::Context ctx;
    ctx.principal = std::move(principal);
    return ctx;
}

class ScopedPrincipal {
public:
    explicit ScopedPrincipal(std::string principal) : _ctx{contextFor(std::move(principal))}, _scope{_ctx} {}

private:
    morph::session::Context _ctx;
    morph::session::detail::ScopedContext _scope;
};

}  // namespace

TEST_CASE("Replaying the same opId is a safe no-op", "[ledger][import]") {
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Personal";
    mapper.Create(ledgerRow);
    const auto ledgerId = ledger::LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())};

    ledger::LedgerModel model;
    const ScopedPrincipal principal{"alice"};
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId,
                                      .name = "Checking",
                                      .kind = ledger::AccountKind::Asset,
                                      .currency = ledger::Currency::USD});
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId,
                                      .name = "Suspense",
                                      .kind = ledger::AccountKind::Asset,
                                      .currency = ledger::Currency::USD});
    auto ledgerState = model.execute(ledger::GetLedger{.ledgerId = ledgerId});
    auto checkingId = ledgerState.accounts[0].id;
    auto suspenseId = ledgerState.accounts[1].id;

    auto opId = ledger::ImportOpId::fromOptional(std::optional<std::string>{"chunk-1"});
    std::string csv =
        "date,description,account_id,amount\n2026-01-01T00:00:00Z,Coffee," + std::to_string(*checkingId) + ",-4.50\n";

    auto first = model.execute(ledger::ImportLedgerChunk{
        .ledgerId = ledgerId, .counterAccountId = suspenseId, .csvChunk = csv, .opId = opId});
    auto replay = model.execute(ledger::ImportLedgerChunk{
        .ledgerId = ledgerId, .counterAccountId = suspenseId, .csvChunk = csv, .opId = opId});
    // "Safe no-op" here means the replay never double-imports the row --
    // NOT that its own reported counts equal the first call's. The opId
    // ledger is populated but deliberately not read back for an early
    // return (this task's own scope-narrowing ruling, see
    // execute(ImportLedgerChunk)'s own comment), so the replay still runs
    // the full row loop and is caught by the content-hash check below,
    // which correctly reports it as a duplicate rather than a re-import.
    CHECK(first.imported == 1);
    CHECK(first.duplicates == 0);
    CHECK(replay.imported == 0);
    CHECK(replay.duplicates == 1);
}

TEST_CASE("Re-importing the same statement under a different opId is caught by content-hash dedup",
          "[ledger][import]") {
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Personal";
    mapper.Create(ledgerRow);
    const auto ledgerId = ledger::LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())};

    ledger::LedgerModel model;
    const ScopedPrincipal principal{"alice"};
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId,
                                      .name = "Checking",
                                      .kind = ledger::AccountKind::Asset,
                                      .currency = ledger::Currency::USD});
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId,
                                      .name = "Suspense",
                                      .kind = ledger::AccountKind::Asset,
                                      .currency = ledger::Currency::USD});
    auto ledgerState = model.execute(ledger::GetLedger{.ledgerId = ledgerId});
    auto checkingId = ledgerState.accounts[0].id;
    auto suspenseId = ledgerState.accounts[1].id;

    std::string csv =
        "date,description,account_id,amount\n2026-01-01T00:00:00Z,Coffee," + std::to_string(*checkingId) + ",-4.50\n";

    auto first = model.execute(
        ledger::ImportLedgerChunk{.ledgerId = ledgerId,
                                  .counterAccountId = suspenseId,
                                  .csvChunk = csv,
                                  .opId = ledger::ImportOpId::fromOptional(std::optional<std::string>{"chunk-A"})});
    auto second = model.execute(
        ledger::ImportLedgerChunk{.ledgerId = ledgerId,
                                  .counterAccountId = suspenseId,
                                  .csvChunk = csv,
                                  .opId = ledger::ImportOpId::fromOptional(std::optional<std::string>{"chunk-B"})});
    CHECK(first.imported == 1);
    CHECK(second.imported == 0);
    CHECK(second.duplicates == 1);
}

TEST_CASE("ImportLedgerChunk posts a balanced two-leg entry against the row's account and the counter account",
          "[ledger][import]") {
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Personal";
    mapper.Create(ledgerRow);
    const auto ledgerId = ledger::LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())};

    ledger::LedgerModel model;
    const ScopedPrincipal principal{"alice"};
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId,
                                      .name = "Checking",
                                      .kind = ledger::AccountKind::Asset,
                                      .currency = ledger::Currency::USD});
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId,
                                      .name = "Suspense",
                                      .kind = ledger::AccountKind::Asset,
                                      .currency = ledger::Currency::USD});
    auto ledgerState = model.execute(ledger::GetLedger{.ledgerId = ledgerId});
    auto checkingId = ledgerState.accounts[0].id;
    auto suspenseId = ledgerState.accounts[1].id;

    std::string csv =
        "date,description,account_id,amount\n2026-01-01T00:00:00Z,Coffee," + std::to_string(*checkingId) + ",-4.50\n";

    auto result = model.execute(ledger::ImportLedgerChunk{
        .ledgerId = ledgerId,
        .counterAccountId = suspenseId,
        .csvChunk = csv,
        .opId = ledger::ImportOpId::fromOptional(std::optional<std::string>{"chunk-single"})});
    CHECK(result.imported == 1);
    CHECK(result.duplicates == 0);

    auto finalState = model.execute(ledger::GetLedger{.ledgerId = ledgerId});
    auto checking = std::ranges::find_if(finalState.accounts, [&](const auto& a) { return a.id == checkingId; });
    auto suspense = std::ranges::find_if(finalState.accounts, [&](const auto& a) { return a.id == suspenseId; });
    REQUIRE(checking != finalState.accounts.end());
    REQUIRE(suspense != finalState.accounts.end());
    CHECK(checking->balance.numerator == -450);
    CHECK(suspense->balance.numerator == 450);
}

TEST_CASE("ImportLedgerChunk rejects a malformed CSV row", "[ledger][import]") {
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Personal";
    mapper.Create(ledgerRow);
    const auto ledgerId = ledger::LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())};

    ledger::LedgerModel model;
    const ScopedPrincipal principal{"alice"};
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId,
                                      .name = "Checking",
                                      .kind = ledger::AccountKind::Asset,
                                      .currency = ledger::Currency::USD});
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId,
                                      .name = "Suspense",
                                      .kind = ledger::AccountKind::Asset,
                                      .currency = ledger::Currency::USD});
    auto ledgerState = model.execute(ledger::GetLedger{.ledgerId = ledgerId});
    auto suspenseId = ledgerState.accounts[1].id;

    std::string csv = "date,description,account_id,amount\n2026-01-01T00:00:00Z,Coffee,-4.50\n";  // only 3 fields

    CHECK_THROWS_AS(model.execute(ledger::ImportLedgerChunk{
                        .ledgerId = ledgerId,
                        .counterAccountId = suspenseId,
                        .csvChunk = csv,
                        .opId = ledger::ImportOpId::fromOptional(std::optional<std::string>{"chunk-bad"})}),
                    ledger::ValidationError);
}
