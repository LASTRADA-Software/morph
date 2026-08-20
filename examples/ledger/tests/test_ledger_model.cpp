// SPDX-License-Identifier: Apache-2.0
#include "ledger/core/errors.hpp"
#include "ledger/db/ledger_entity.hpp"
#include "ledger/models/ledger_model.hpp"
#include "testkit/db_fixture.hpp"

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <catch2/catch_test_macros.hpp>
#include <morph/journal/action_log.hpp>
#include <morph/session/session.hpp>

#include <algorithm>

namespace {

/// @brief A `Context` carrying only @p principal. See
///        `bookmarks::tests::test_bookmark_model.cpp`'s own `contextFor` for
///        why this is not a designated initializer (`-Wmissing-designated-
///        field-initializers` under `-Weverything`).
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

TEST_CASE("OpenAccount creates an account visible in GetLedger", "[ledger][model]") {
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    // This rung has no CreateLedger action in scope -- see the task brief's
    // own note -- so the test seeds the ledgers row directly, mirroring
    // Task 5's own schema test.
    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Personal";
    mapper.Create(ledgerRow);
    const auto ledgerId = ledger::LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())};

    ledger::LedgerModel model;
    const ScopedPrincipal principal{"alice"};
    auto created = model.execute(ledger::OpenAccount{.ledgerId = ledgerId, .name = "Checking",
                                                       .kind = ledger::AccountKind::Asset,
                                                       .currency = ledger::Currency::USD});
    CHECK(created.id.hasValue());

    auto result = model.execute(ledger::GetLedger{.ledgerId = ledgerId});
    REQUIRE(result.accounts.size() == 1);
    CHECK(result.accounts[0].name == "Checking");
}

TEST_CASE("StoreTransaction with two balanced USD legs commits", "[ledger][model]") {
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Personal";
    mapper.Create(ledgerRow);
    const auto ledgerId = ledger::LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())};

    ledger::LedgerModel model;
    const ScopedPrincipal principal{"alice"};
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId, .name = "Checking",
                                       .kind = ledger::AccountKind::Asset, .currency = ledger::Currency::USD});
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId, .name = "Groceries",
                                       .kind = ledger::AccountKind::Expense, .currency = ledger::Currency::USD});
    auto ledgerState = model.execute(ledger::GetLedger{.ledgerId = ledgerId});
    auto checkingId = ledgerState.accounts[0].id;
    auto groceriesId = ledgerState.accounts[1].id;

    using morph::math::DecimalPlaces;
    using morph::math::Denominator;
    using morph::math::Numerator;
    // -50.00 from Checking, +50.00 to Groceries -- exact Rational legs, sums to zero.
    auto result = model.execute(ledger::StoreTransaction{
        .ledgerId = ledgerId,
        .description = "Weekly shop",
        .date = morph::time::Timestamp::now(),
        .legs = {ledger::TransactionLeg{.accountId = checkingId,
                                         .amount = morph::math::Rational{Numerator{-5000}, Denominator{1},
                                                                          DecimalPlaces{2}}},
                 ledger::TransactionLeg{.accountId = groceriesId,
                                        .amount = morph::math::Rational{Numerator{5000}, Denominator{1},
                                                                         DecimalPlaces{2}}}}});

    REQUIRE(result.accounts.size() == 2);
    auto checking = std::ranges::find_if(result.accounts, [&](const auto& a) { return a.id == checkingId; });
    auto groceries = std::ranges::find_if(result.accounts, [&](const auto& a) { return a.id == groceriesId; });
    REQUIRE(checking != result.accounts.end());
    REQUIRE(groceries != result.accounts.end());
    CHECK(checking->balance.numerator == -5000);
    CHECK(groceries->balance.numerator == 5000);
}

TEST_CASE("StoreTransaction with unbalanced USD legs throws ZeroSumViolation", "[ledger][model]") {
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Personal";
    mapper.Create(ledgerRow);
    const auto ledgerId = ledger::LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())};

    ledger::LedgerModel model;
    const ScopedPrincipal principal{"alice"};
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId, .name = "Checking",
                                       .kind = ledger::AccountKind::Asset, .currency = ledger::Currency::USD});
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId, .name = "Groceries",
                                       .kind = ledger::AccountKind::Expense, .currency = ledger::Currency::USD});
    auto ledgerState = model.execute(ledger::GetLedger{.ledgerId = ledgerId});

    using morph::math::DecimalPlaces;
    using morph::math::Denominator;
    using morph::math::Numerator;
    CHECK_THROWS_AS(
        model.execute(ledger::StoreTransaction{
            .ledgerId = ledgerId,
            .description = "Bad txn",
            .date = morph::time::Timestamp::now(),
            .legs = {ledger::TransactionLeg{.accountId = ledgerState.accounts[0].id,
                                             .amount = morph::math::Rational{Numerator{-5000}, Denominator{1},
                                                                              DecimalPlaces{2}}},
                     ledger::TransactionLeg{.accountId = ledgerState.accounts[1].id,
                                            .amount = morph::math::Rational{Numerator{4000}, Denominator{1},
                                                                             DecimalPlaces{2}}}}}),
        ledger::ZeroSumViolation);
}

TEST_CASE("A foreign-amount pair balances USD and EUR partitions independently", "[ledger][model]") {
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Personal";
    mapper.Create(ledgerRow);
    const auto ledgerId = ledger::LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())};

    ledger::LedgerModel model;
    const ScopedPrincipal principal{"alice"};
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId, .name = "USD Checking",
                                       .kind = ledger::AccountKind::Asset, .currency = ledger::Currency::USD});
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId, .name = "USD Travel Expense",
                                       .kind = ledger::AccountKind::Expense, .currency = ledger::Currency::USD});
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId, .name = "EUR Wallet",
                                       .kind = ledger::AccountKind::Asset, .currency = ledger::Currency::EUR});
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId, .name = "EUR Merchant Payable",
                                       .kind = ledger::AccountKind::Liability, .currency = ledger::Currency::EUR});
    auto ledgerState = model.execute(ledger::GetLedger{.ledgerId = ledgerId});
    auto usdChecking = ledgerState.accounts[0].id;
    auto usdExpense = ledgerState.accounts[1].id;
    auto eurWallet = ledgerState.accounts[2].id;
    auto eurPayable = ledgerState.accounts[3].id;

    using morph::math::DecimalPlaces;
    using morph::math::Denominator;
    using morph::math::Numerator;
    // A real 4-leg transaction: USD partition legs sum to zero on their
    // own (a -50.00/+50.00 pair), EUR partition legs sum to zero on their
    // own (a -45.23/+45.23 pair) -- the foreign-amount annotation on the
    // USD leg is display metadata only, never entering either check
    // (design spec §1 step 3).
    auto result = model.execute(ledger::StoreTransaction{
        .ledgerId = ledgerId,
        .description = "Travel expense with EUR receipt",
        .date = morph::time::Timestamp::now(),
        .legs = {ledger::TransactionLeg{.accountId = usdChecking,
                                         .amount = morph::math::Rational{Numerator{-5000}, Denominator{1},
                                                                          DecimalPlaces{2}},
                                         .foreignAmount = morph::math::Rational{Numerator{4523}, Denominator{1},
                                                                                 DecimalPlaces{2}},
                                         .foreignCurrency = ledger::Currency::EUR},
                 ledger::TransactionLeg{.accountId = usdExpense,
                                        .amount = morph::math::Rational{Numerator{5000}, Denominator{1},
                                                                         DecimalPlaces{2}}},
                 ledger::TransactionLeg{.accountId = eurWallet,
                                        .amount = morph::math::Rational{Numerator{-4523}, Denominator{1},
                                                                         DecimalPlaces{2}}},
                 ledger::TransactionLeg{.accountId = eurPayable,
                                        .amount = morph::math::Rational{Numerator{4523}, Denominator{1},
                                                                         DecimalPlaces{2}}}}});

    // No ZeroSumViolation thrown (implicit -- the call above would have
    // thrown otherwise); assert both currencies' balances landed correctly.
    auto findBalance = [&](ledger::AccountId id) {
        return std::ranges::find_if(result.accounts, [&](const auto& a) { return a.id == id; })->balance.numerator;
    };
    CHECK(findBalance(usdChecking) == -5000);
    CHECK(findBalance(usdExpense) == 5000);
    CHECK(findBalance(eurWallet) == -4523);
    CHECK(findBalance(eurPayable) == 4523);
}

TEST_CASE("StoreTransaction refuses an empty principal", "[ledger][model][security]") {
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Personal";
    mapper.Create(ledgerRow);
    const auto ledgerId = ledger::LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())};

    ledger::LedgerModel model;
    ScopedPrincipal empty{""};  // installs a Context with an empty principal for this scope
    CHECK_THROWS_AS(
        model.execute(ledger::StoreTransaction{.ledgerId = ledgerId, .description = "Should be refused",
                                                 .date = morph::time::Timestamp::now(), .legs = {}}),
        ledger::EmptyPrincipalError);
}

TEST_CASE("OpenAccount records a LogEntry once a log is attached, and is a no-op without one",
          "[ledger][model][journal]") {
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Personal";
    mapper.Create(ledgerRow);
    const auto ledgerId = ledger::LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())};

    ledger::LedgerModel model;
    const ScopedPrincipal principal{"alice"};

    // No log attached: succeeds, no crash, nothing recorded anywhere to
    // check against -- this half of the test exists to prove the no-op
    // path doesn't throw or misbehave when _log is null.
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId, .name = "Checking",
                                       .kind = ledger::AccountKind::Asset, .currency = ledger::Currency::USD});

    // Attach a log, then repeat -- this call must be recorded.
    auto log = std::make_shared<morph::journal::InMemoryActionLog>();
    model.attachActionLog(log, std::to_string(*ledgerId));
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId, .name = "Savings",
                                       .kind = ledger::AccountKind::Asset, .currency = ledger::Currency::USD});

    auto entries = log->entries();
    REQUIRE(entries.size() == 1);  // only the second call was journaled -- the first ran before attachActionLog
    CHECK(entries[0].actionType == "OpenAccount");
    CHECK(entries[0].outcome == morph::journal::Outcome::Succeeded);
    CHECK(entries[0].entityKey == std::to_string(*ledgerId));
}

TEST_CASE("StoreTransaction with a repeated opId is a safe no-op, not a second insert", "[ledger][model][exactly-once]") {
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Personal";
    mapper.Create(ledgerRow);
    const auto ledgerId = ledger::LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())};

    ScopedPrincipal principal{"alice"};
    ledger::LedgerModel model;
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId, .name = "Checking",
                                       .kind = ledger::AccountKind::Asset, .currency = ledger::Currency::USD});
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId, .name = "Groceries",
                                       .kind = ledger::AccountKind::Expense, .currency = ledger::Currency::USD});
    auto ledgerState = model.execute(ledger::GetLedger{.ledgerId = ledgerId});

    using morph::math::DecimalPlaces;
    using morph::math::Denominator;
    using morph::math::Numerator;
    const auto opId = ledger::ImportOpId::fromOptional(std::optional<std::string>{"txn-op-1"});
    const auto txn = ledger::StoreTransaction{
        .ledgerId = ledgerId, .description = "Groceries", .date = morph::time::Timestamp::now(),
        .legs = {ledger::TransactionLeg{.accountId = ledgerState.accounts[0].id,
                                         .amount = morph::math::Rational{Numerator{-3000}, Denominator{1},
                                                                          DecimalPlaces{2}}},
                 ledger::TransactionLeg{.accountId = ledgerState.accounts[1].id,
                                        .amount = morph::math::Rational{Numerator{3000}, Denominator{1},
                                                                         DecimalPlaces{2}}}},
        .opId = opId};

    auto first = model.execute(txn);
    auto second = model.execute(txn);  // identical opId -- must be a no-op replay, not a second insert

    auto findBalance = [&](ledger::AccountId id, const ledger::GetLedgerResult& r) {
        return std::ranges::find_if(r.accounts, [&](const auto& a) { return a.id == id; })->balance.numerator;
    };
    CHECK(findBalance(ledgerState.accounts[0].id, first) == -3000);
    CHECK(findBalance(ledgerState.accounts[0].id, second) == -3000);  // still -3000, not -6000
}
