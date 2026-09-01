// SPDX-License-Identifier: Apache-2.0
#include <Lightweight/DataMapper/DataMapper.hpp>
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <glaze/glaze.hpp>
#include <morph/journal/action_log.hpp>
#include <morph/journal/journal.hpp>
#include <morph/session/session.hpp>

#include "ledger/core/errors.hpp"
#include "ledger/core/money.hpp"
#include "ledger/db/ledger_entity.hpp"
#include "ledger/models/budget_model.hpp"
#include "ledger/models/ledger_model.hpp"
#include "ledger/models/rule_model.hpp"
#include "testkit/db_fixture.hpp"

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
    auto created = model.execute(ledger::OpenAccount{.ledgerId = ledgerId,
                                                     .name = "Checking",
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
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId,
                                      .name = "Checking",
                                      .kind = ledger::AccountKind::Asset,
                                      .currency = ledger::Currency::USD});
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId,
                                      .name = "Groceries",
                                      .kind = ledger::AccountKind::Expense,
                                      .currency = ledger::Currency::USD});
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
        .legs = {ledger::TransactionLeg{
                     .accountId = checkingId,
                     .amount = morph::math::Rational{Numerator{-5000}, Denominator{1}, DecimalPlaces{2}}},
                 ledger::TransactionLeg{
                     .accountId = groceriesId,
                     .amount = morph::math::Rational{Numerator{5000}, Denominator{1}, DecimalPlaces{2}}}}});

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
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId,
                                      .name = "Checking",
                                      .kind = ledger::AccountKind::Asset,
                                      .currency = ledger::Currency::USD});
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId,
                                      .name = "Groceries",
                                      .kind = ledger::AccountKind::Expense,
                                      .currency = ledger::Currency::USD});
    auto ledgerState = model.execute(ledger::GetLedger{.ledgerId = ledgerId});

    using morph::math::DecimalPlaces;
    using morph::math::Denominator;
    using morph::math::Numerator;
    CHECK_THROWS_AS(
        model.execute(ledger::StoreTransaction{
            .ledgerId = ledgerId,
            .description = "Bad txn",
            .date = morph::time::Timestamp::now(),
            .legs = {ledger::TransactionLeg{
                         .accountId = ledgerState.accounts[0].id,
                         .amount = morph::math::Rational{Numerator{-5000}, Denominator{1}, DecimalPlaces{2}}},
                     ledger::TransactionLeg{
                         .accountId = ledgerState.accounts[1].id,
                         .amount = morph::math::Rational{Numerator{4000}, Denominator{1}, DecimalPlaces{2}}}}}),
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
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId,
                                      .name = "USD Checking",
                                      .kind = ledger::AccountKind::Asset,
                                      .currency = ledger::Currency::USD});
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId,
                                      .name = "USD Travel Expense",
                                      .kind = ledger::AccountKind::Expense,
                                      .currency = ledger::Currency::USD});
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId,
                                      .name = "EUR Wallet",
                                      .kind = ledger::AccountKind::Asset,
                                      .currency = ledger::Currency::EUR});
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId,
                                      .name = "EUR Merchant Payable",
                                      .kind = ledger::AccountKind::Liability,
                                      .currency = ledger::Currency::EUR});
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
        .legs = {
            ledger::TransactionLeg{
                .accountId = usdChecking,
                .amount = morph::math::Rational{Numerator{-5000}, Denominator{1}, DecimalPlaces{2}},
                .foreignAmount = morph::math::Rational{Numerator{4523}, Denominator{1}, DecimalPlaces{2}},
                .foreignCurrency = ledger::Currency::EUR},
            ledger::TransactionLeg{.accountId = usdExpense,
                                   .amount = morph::math::Rational{Numerator{5000}, Denominator{1}, DecimalPlaces{2}}},
            ledger::TransactionLeg{
                .accountId = eurWallet,
                .amount = morph::math::Rational{Numerator{-4523}, Denominator{1}, DecimalPlaces{2}}},
            ledger::TransactionLeg{
                .accountId = eurPayable,
                .amount = morph::math::Rational{Numerator{4523}, Denominator{1}, DecimalPlaces{2}}}}});

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
    CHECK_THROWS_AS(model.execute(ledger::StoreTransaction{.ledgerId = ledgerId,
                                                           .description = "Should be refused",
                                                           .date = morph::time::Timestamp::now(),
                                                           .legs = {}}),
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
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId,
                                      .name = "Checking",
                                      .kind = ledger::AccountKind::Asset,
                                      .currency = ledger::Currency::USD});

    // Attach a log, then repeat -- this call must be recorded.
    auto log = std::make_shared<morph::journal::InMemoryActionLog>();
    model.attachActionLog(log, std::to_string(*ledgerId));
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId,
                                      .name = "Savings",
                                      .kind = ledger::AccountKind::Asset,
                                      .currency = ledger::Currency::USD});

    auto entries = log->entries();
    REQUIRE(entries.size() == 1);  // only the second call was journaled -- the first ran before attachActionLog
    CHECK(entries[0].actionType == "OpenAccount");
    CHECK(entries[0].outcome == morph::journal::Outcome::Succeeded);
    CHECK(entries[0].entityKey == std::to_string(*ledgerId));
}

TEST_CASE("StoreTransaction with a repeated opId is a safe no-op, not a second insert",
          "[ledger][model][exactly-once]") {
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Personal";
    mapper.Create(ledgerRow);
    const auto ledgerId = ledger::LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())};

    ScopedPrincipal principal{"alice"};
    ledger::LedgerModel model;
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId,
                                      .name = "Checking",
                                      .kind = ledger::AccountKind::Asset,
                                      .currency = ledger::Currency::USD});
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId,
                                      .name = "Groceries",
                                      .kind = ledger::AccountKind::Expense,
                                      .currency = ledger::Currency::USD});
    auto ledgerState = model.execute(ledger::GetLedger{.ledgerId = ledgerId});

    using morph::math::DecimalPlaces;
    using morph::math::Denominator;
    using morph::math::Numerator;
    const auto opId = ledger::ImportOpId::fromOptional(std::optional<std::string>{"txn-op-1"});
    const auto txn = ledger::StoreTransaction{
        .ledgerId = ledgerId,
        .description = "Groceries",
        .date = morph::time::Timestamp::now(),
        .legs = {ledger::TransactionLeg{
                     .accountId = ledgerState.accounts[0].id,
                     .amount = morph::math::Rational{Numerator{-3000}, Denominator{1}, DecimalPlaces{2}}},
                 ledger::TransactionLeg{
                     .accountId = ledgerState.accounts[1].id,
                     .amount = morph::math::Rational{Numerator{3000}, Denominator{1}, DecimalPlaces{2}}}},
        .opId = opId};

    auto first = model.execute(txn);
    auto second = model.execute(txn);  // identical opId -- must be a no-op replay, not a second insert

    auto findBalance = [&](ledger::AccountId id, const ledger::GetLedgerResult& r) {
        return std::ranges::find_if(r.accounts, [&](const auto& a) { return a.id == id; })->balance.numerator;
    };
    CHECK(findBalance(ledgerState.accounts[0].id, first) == -3000);
    CHECK(findBalance(ledgerState.accounts[0].id, second) == -3000);  // still -3000, not -6000
}

TEST_CASE("A matching rule cascades SetCategory with a causalParentId, not LogEntry::seq", "[ledger][rule][journal]") {
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Personal";
    mapper.Create(ledgerRow);
    const auto ledgerId = ledger::LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())};

    ScopedPrincipal principal{"alice"};
    ledger::RuleModel ruleModel;
    ruleModel.execute(ledger::CreateRule{.ledgerId = ledgerId,
                                         .trigger = ledger::RuleTrigger::DescriptionContains,
                                         .matchText = "Coffee",
                                         .action = ledger::RuleAction::SetCategory,
                                         .actionValue = "Dining"});

    ledger::BudgetModel budgetModel;
    budgetModel.execute(ledger::CreateCategory{.ledgerId = ledgerId, .name = "Dining"});

    ledger::LedgerModel ledgerModel;
    ledgerModel.execute(ledger::OpenAccount{.ledgerId = ledgerId,
                                            .name = "Checking",
                                            .kind = ledger::AccountKind::Asset,
                                            .currency = ledger::Currency::USD});
    ledgerModel.execute(ledger::OpenAccount{.ledgerId = ledgerId,
                                            .name = "Dining",
                                            .kind = ledger::AccountKind::Expense,
                                            .currency = ledger::Currency::USD});
    auto ledgerState = ledgerModel.execute(ledger::GetLedger{.ledgerId = ledgerId});

    auto log = std::make_shared<morph::journal::InMemoryActionLog>();
    ledgerModel.attachActionLog(log, std::to_string(*ledgerId));

    using morph::math::DecimalPlaces;
    using morph::math::Denominator;
    using morph::math::Numerator;
    ledgerModel.execute(ledger::StoreTransaction{
        .ledgerId = ledgerId,
        .description = "Coffee at the cafe",
        .date = morph::time::Timestamp::now(),
        .legs = {
            ledger::TransactionLeg{.accountId = ledgerState.accounts[0].id,
                                   .amount = morph::math::Rational{Numerator{-450}, Denominator{1}, DecimalPlaces{2}}},
            ledger::TransactionLeg{
                .accountId = ledgerState.accounts[1].id,
                .amount = morph::math::Rational{Numerator{450}, Denominator{1}, DecimalPlaces{2}}}}});

    auto entries = log->entries();
    REQUIRE(entries.size() == 2);  // trigger + cascade
    CHECK(entries[0].actionType == "StoreTransaction");
    CHECK(entries[0].causalParentId.empty());  // the trigger itself has no parent
    CHECK(entries[1].actionType == "SetCategory");
    CHECK_FALSE(entries[1].causalParentId.empty());
    CHECK(entries[1].causalParentId != std::to_string(entries[0].seq));  // never LogEntry::seq
    CHECK(entries[1].payload.find("ruleId") != std::string::npos);
    CHECK(entries[1].payload.find("ruleVersion") != std::string::npos);
}

TEST_CASE("Replay after editing a rule reproduces the v1 cascade, never the v2 outcome",
          "[ledger][rule][journal][divergence]") {
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Personal";
    mapper.Create(ledgerRow);
    const auto ledgerId = ledger::LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())};

    ScopedPrincipal principal{"alice"};
    ledger::BudgetModel budgetModel;
    auto categoryA = budgetModel.execute(ledger::CreateCategory{.ledgerId = ledgerId, .name = "Dining"});
    auto categoryB = budgetModel.execute(ledger::CreateCategory{.ledgerId = ledgerId, .name = "Groceries"});

    ledger::RuleModel ruleModel;
    auto ruleId = ruleModel.execute(ledger::CreateRule{.ledgerId = ledgerId,
                                                       .trigger = ledger::RuleTrigger::DescriptionContains,
                                                       .matchText = "Coffee",
                                                       .action = ledger::RuleAction::SetCategory,
                                                       .actionValue = "Dining"});  // v1: sets Dining

    ledger::LedgerModel ledgerModel;
    ledgerModel.execute(ledger::OpenAccount{.ledgerId = ledgerId,
                                            .name = "Checking",
                                            .kind = ledger::AccountKind::Asset,
                                            .currency = ledger::Currency::USD});
    ledgerModel.execute(ledger::OpenAccount{.ledgerId = ledgerId,
                                            .name = "Dining Out",
                                            .kind = ledger::AccountKind::Expense,
                                            .currency = ledger::Currency::USD});
    auto ledgerState = ledgerModel.execute(ledger::GetLedger{.ledgerId = ledgerId});
    const auto expenseAccountId = ledgerState.accounts[1].id;

    auto log = std::make_shared<morph::journal::InMemoryActionLog>();
    ledgerModel.attachActionLog(log, std::to_string(*ledgerId));

    using morph::math::DecimalPlaces;
    using morph::math::Denominator;
    using morph::math::Numerator;
    ledgerModel.execute(ledger::StoreTransaction{
        .ledgerId = ledgerId,
        .description = "Coffee run",
        .date = morph::time::Timestamp::now(),
        .legs = {
            ledger::TransactionLeg{.accountId = ledgerState.accounts[0].id,
                                   .amount = morph::math::Rational{Numerator{-450}, Denominator{1}, DecimalPlaces{2}}},
            ledger::TransactionLeg{
                .accountId = expenseAccountId,
                .amount = morph::math::Rational{Numerator{450}, Denominator{1}, DecimalPlaces{2}}}}});

    // The expense account is now linked to category A (Dining) -- confirm
    // this directly before editing the rule, so the assertion after replay
    // is a genuine "still A, never B" check, not a vacuous one.
    auto accountRowsBefore =
        mapper.Query<ledger::db::AccountRecord>()
            .Where(::Lightweight::FieldNameOf<&ledger::db::AccountRecord::id>, "=", *expenseAccountId)
            .All();
    REQUIRE(accountRowsBefore.front().category.Value().has_value());
    CHECK(static_cast<std::int64_t>(accountRowsBefore.front().category.Value().value()) == *categoryA);

    // Edit RuleX to v2: now sets Groceries instead of Dining.
    ruleModel.execute(ledger::UpdateRule{.ruleId = ruleId, .matchText = "Coffee", .actionValue = "Groceries"});

    // Replay the captured log against a fresh model instance. LedgerModel's
    // own mutations are persisted to the real SQLite database, not held in
    // memory -- replay() re-dispatches every recorded entry (including the
    // cascade's own recorded SetCategory entry) against those same rows, so
    // re-querying the database directly afterward (as this test already
    // does via mapper.Query<AccountRecord>() below) is sufficient on its
    // own; the returned IModelHolder is not needed to observe the effect.
    auto replayedEntries = log->entries();
    morph::journal::replay("LedgerModel", replayedEntries);

    // The replayed database state must still show category A, never B --
    // the cascade's own recorded entry (payload includes ruleVersion=1)
    // pins the v1 outcome; replay's isReplaying()-gated rule suppression
    // means the trigger entry never re-evaluates against the now-v2 rule.
    auto accountRowsAfter =
        mapper.Query<ledger::db::AccountRecord>()
            .Where(::Lightweight::FieldNameOf<&ledger::db::AccountRecord::id>, "=", *expenseAccountId)
            .All();
    REQUIRE(accountRowsAfter.front().category.Value().has_value());
    CHECK(static_cast<std::int64_t>(accountRowsAfter.front().category.Value().value()) == *categoryA);
    CHECK(static_cast<std::int64_t>(accountRowsAfter.front().category.Value().value()) != *categoryB);
}

TEST_CASE("A clamped Rational leg is caught incidentally by the zero-sum check, not by validate()",
          "[ledger][rational][security]") {
    // Decode the raw wire JSON {"num":5,"den":0,"dp":2} through glaze's
    // real JSON codec (glz::read_json), which reaches Rational::setWire
    // (the glz::meta<Rational> specialization in
    // include/morph/util/rational.hpp) rather than the in-process 3-arg
    // constructor. setWire clamps the zero denominator to 1 instead of
    // rejecting the decode, so the decode itself succeeds with a
    // silently-clamped 5/1 value. Use that decoded Rational as a leg's
    // amount and assert the resulting legs fail the zero-sum check
    // (ZeroSumViolation thrown) rather than silently committing --
    // proving it's the ledger's own zero-sum invariant that catches this,
    // incidentally, not validate(), per design spec §7.
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
                                      .name = "Expenses",
                                      .kind = ledger::AccountKind::Expense,
                                      .currency = ledger::Currency::USD});
    auto ledgerState = model.execute(ledger::GetLedger{.ledgerId = ledgerId});
    auto checkingId = ledgerState.accounts[0].id;
    auto expensesId = ledgerState.accounts[1].id;

    using morph::math::DecimalPlaces;
    using morph::math::Denominator;
    using morph::math::Numerator;
    using morph::math::Rational;

    // One leg is normal, one is decoded from wire JSON with den=0 and
    // comes back clamped (den=0 becomes den=1) by Rational::setWire.
    // Together they won't sum to zero because the clamped leg changed value.
    // This should be caught by the zero-sum check, throwing ZeroSumViolation.
    Rational normalLeg{Numerator{-500}, Denominator{1}, DecimalPlaces{2}};  // -5.00

    Rational clampedLeg;
    auto err = glz::read_json(clampedLeg, std::string{R"({"num":5,"den":0,"dp":2})"});
    REQUIRE_FALSE(err);  // decode succeeds -- clamping is silent, not a decode failure

    CHECK_THROWS_AS(model.execute(ledger::StoreTransaction{
                        .ledgerId = ledgerId,
                        .description = "Unbalanced with clamped leg",
                        .date = morph::time::Timestamp::now(),
                        .legs = {ledger::TransactionLeg{.accountId = checkingId, .amount = normalLeg},
                                 ledger::TransactionLeg{.accountId = expensesId, .amount = clampedLeg}}}),
                    ledger::ZeroSumViolation);
}

TEST_CASE("UndoTransaction produces an exact negation that re-passes zero-sum and restores balances",
          "[ledger][undo]") {
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
                                      .name = "Groceries",
                                      .kind = ledger::AccountKind::Expense,
                                      .currency = ledger::Currency::USD});
    auto ledgerState = model.execute(ledger::GetLedger{.ledgerId = ledgerId});
    auto checkingId = ledgerState.accounts[0].id;
    auto groceriesId = ledgerState.accounts[1].id;

    using morph::math::DecimalPlaces;
    using morph::math::Denominator;
    using morph::math::Numerator;
    // -50.00 from Checking, +50.00 to Groceries -- same shape as this
    // file's own "StoreTransaction with two balanced USD legs commits" test.
    model.execute(ledger::StoreTransaction{
        .ledgerId = ledgerId,
        .description = "Weekly shop",
        .date = morph::time::Timestamp::now(),
        .legs = {ledger::TransactionLeg{
                     .accountId = checkingId,
                     .amount = morph::math::Rational{Numerator{-5000}, Denominator{1}, DecimalPlaces{2}}},
                 ledger::TransactionLeg{
                     .accountId = groceriesId,
                     .amount = morph::math::Rational{Numerator{5000}, Denominator{1}, DecimalPlaces{2}}}}});

    // GetLedgerResult/StoreTransaction's own return value never exposes a
    // journal id (design spec's own account_dto.hpp shape) -- the only way
    // to name the journal to undo is to query the row directly, same as
    // any other test in this file that needs a DB-assigned id its DTOs
    // don't surface.
    auto journalRows = mapper.Query<ledger::db::TransactionJournalRecord>()
                           .Where(::Lightweight::FieldNameOf<&ledger::db::TransactionJournalRecord::description>, "=",
                                  Lightweight::SqlAnsiString<256>{"Weekly shop"})
                           .All();
    REQUIRE(journalRows.size() == 1);
    const auto journalId = ledger::JournalId{static_cast<std::int64_t>(journalRows.front().id.Value())};

    auto undoResult = model.execute(ledger::UndoTransaction{.ledgerId = ledgerId, .journalId = journalId});

    // Post-undo balances match pre-transaction values exactly (both
    // accounts back to their opening zero balance) -- Rational equality,
    // not floating-point tolerance.
    REQUIRE(undoResult.accounts.size() == 2);
    auto checking = std::ranges::find_if(undoResult.accounts, [&](const auto& a) { return a.id == checkingId; });
    auto groceries = std::ranges::find_if(undoResult.accounts, [&](const auto& a) { return a.id == groceriesId; });
    REQUIRE(checking != undoResult.accounts.end());
    REQUIRE(groceries != undoResult.accounts.end());
    CHECK(checking->balance.numerator == 0);
    CHECK(groceries->balance.numerator == 0);

    // The reversal's own legs are the exact negation of the original's.
    auto reversalJournalRows =
        mapper.Query<ledger::db::TransactionJournalRecord>()
            .Where(::Lightweight::FieldNameOf<&ledger::db::TransactionJournalRecord::id>, "!=", *journalId)
            .All();
    REQUIRE(reversalJournalRows.size() == 1);
    auto reversalLegRows = mapper.Query<ledger::db::TransactionLegRecord>()
                               .Where(::Lightweight::FieldNameOf<&ledger::db::TransactionLegRecord::journal>, "=",
                                      reversalJournalRows.front().id.Value())
                               .All();
    REQUIRE(reversalLegRows.size() == 2);
    for (const auto& legRow : reversalLegRows) {
        if (legRow.account.Value() == static_cast<std::uint64_t>(*checkingId)) {
            CHECK(legRow.amountNum.Value() == 5000);  // negation of the original -5000
        } else {
            CHECK(legRow.amountNum.Value() == -5000);  // negation of the original 5000
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────
// Per-currency scale: a leg's `decimalPlaces` is the scale its numerator is
// expressed in, so two legs at different scales are not comparable until
// both are restated at the account currency's own scale. These two cases
// are the ones morph#304 §A1 predicted; both are checked through the real
// `StoreTransaction` path, not against the partitioning helper directly.
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("StoreTransaction rejects legs that balance only because their scales differ", "[ledger][model]") {
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
                                      .name = "Groceries",
                                      .kind = ledger::AccountKind::Expense,
                                      .currency = ledger::Currency::USD});
    auto ledgerState = model.execute(ledger::GetLedger{.ledgerId = ledgerId});

    using morph::math::DecimalPlaces;
    using morph::math::Denominator;
    using morph::math::Numerator;
    // "4.50" is 450 at a scale of 2; "-45.0" is -450 at a scale of 1. They
    // are $4.50 and -$45.00 -- forty dollars fifty apart -- and the pair
    // must be refused. Summing the two numerators without restating either
    // at USD's own scale reads them as 450 and -450 and calls it balanced.
    CHECK_THROWS_AS(
        model.execute(ledger::StoreTransaction{
            .ledgerId = ledgerId,
            .description = "Scale-mismatched pair",
            .date = morph::time::Timestamp::now(),
            .legs = {ledger::TransactionLeg{
                         .accountId = ledgerState.accounts[0].id,
                         .amount = morph::math::Rational{Numerator{450}, Denominator{1}, DecimalPlaces{2}}},
                     ledger::TransactionLeg{
                         .accountId = ledgerState.accounts[1].id,
                         .amount = morph::math::Rational{Numerator{-450}, Denominator{1}, DecimalPlaces{1}}}}}),
        ledger::ZeroSumViolation);
}

TEST_CASE("StoreTransaction accepts a balanced pair written at different scales", "[ledger][model]") {
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
                                      .name = "Groceries",
                                      .kind = ledger::AccountKind::Expense,
                                      .currency = ledger::Currency::USD});
    auto ledgerState = model.execute(ledger::GetLedger{.ledgerId = ledgerId});
    const auto checkingId = ledgerState.accounts[0].id;
    const auto groceriesId = ledgerState.accounts[1].id;

    using morph::math::DecimalPlaces;
    using morph::math::Denominator;
    using morph::math::Numerator;
    // "4.5" is 45 at a scale of 1; "-4.50" is -450 at a scale of 2. Both are
    // $4.50, so the pair balances and must be accepted -- and both legs must
    // land on USD's own scale of 2, whatever scale they arrived on.
    auto result = model.execute(ledger::StoreTransaction{
        .ledgerId = ledgerId,
        .description = "Balanced pair at mixed scales",
        .date = morph::time::Timestamp::now(),
        .legs = {
            ledger::TransactionLeg{.accountId = checkingId,
                                   .amount = morph::math::Rational{Numerator{45}, Denominator{1}, DecimalPlaces{1}}},
            ledger::TransactionLeg{
                .accountId = groceriesId,
                .amount = morph::math::Rational{Numerator{-450}, Denominator{1}, DecimalPlaces{2}}}}});

    REQUIRE(result.accounts.size() == 2);
    auto checking = std::ranges::find_if(result.accounts, [&](const auto& a) { return a.id == checkingId; });
    auto groceries = std::ranges::find_if(result.accounts, [&](const auto& a) { return a.id == groceriesId; });
    REQUIRE(checking != result.accounts.end());
    REQUIRE(groceries != result.accounts.end());
    CHECK(checking->balance.numerator == 450);
    CHECK(checking->balance.decimalPlaces == DecimalPlaces{2});
    CHECK(groceries->balance.numerator == -450);
    CHECK(groceries->balance.decimalPlaces == DecimalPlaces{2});
}

TEST_CASE("StoreTransaction rejects a leg carrying more precision than its currency has", "[ledger][model]") {
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
                                      .name = "Groceries",
                                      .kind = ledger::AccountKind::Expense,
                                      .currency = ledger::Currency::USD});
    auto ledgerState = model.execute(ledger::GetLedger{.ledgerId = ledgerId});

    using morph::math::DecimalPlaces;
    using morph::math::Denominator;
    using morph::math::Numerator;
    // $4.505 in a USD account. Restating dp 3 onto USD's dp 2 would have to
    // drop a non-zero digit, and the model never rounds money -- so the pair
    // is rejected outright even though it is perfectly self-balancing.
    CHECK_THROWS_AS(
        model.execute(ledger::StoreTransaction{
            .ledgerId = ledgerId,
            .description = "Sub-cent leg",
            .date = morph::time::Timestamp::now(),
            .legs = {ledger::TransactionLeg{
                         .accountId = ledgerState.accounts[0].id,
                         .amount = morph::math::Rational{Numerator{-4505}, Denominator{1}, DecimalPlaces{3}}},
                     ledger::TransactionLeg{
                         .accountId = ledgerState.accounts[1].id,
                         .amount = morph::math::Rational{Numerator{4505}, Denominator{1}, DecimalPlaces{3}}}}}),
        ledger::ValidationError);

    // A wider scale is fine when it carries no digit below a cent: $4.50
    // written at dp 4 restates to {450, dp 2}.
    auto result = model.execute(ledger::StoreTransaction{
        .ledgerId = ledgerId,
        .description = "Wide but exact",
        .date = morph::time::Timestamp::now(),
        .legs = {ledger::TransactionLeg{
                     .accountId = ledgerState.accounts[0].id,
                     .amount = morph::math::Rational{Numerator{-45000}, Denominator{1}, DecimalPlaces{4}}},
                 ledger::TransactionLeg{
                     .accountId = ledgerState.accounts[1].id,
                     .amount = morph::math::Rational{Numerator{45000}, Denominator{1}, DecimalPlaces{4}}}}});
    REQUIRE(result.accounts.size() == 2);
    CHECK(result.accounts[0].balance.numerator == -450);
    CHECK(result.accounts[0].balance.decimalPlaces == DecimalPlaces{2});
}

TEST_CASE("StoreTransaction rejects a leg that is not a whole number of minor units", "[ledger][model]") {
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
                                      .name = "Groceries",
                                      .kind = ledger::AccountKind::Expense,
                                      .currency = ledger::Currency::USD});
    auto ledgerState = model.execute(ledger::GetLedger{.ledgerId = ledgerId});

    using morph::math::DecimalPlaces;
    using morph::math::Denominator;
    using morph::math::Numerator;
    // `{"num":9,"den":2}` off the wire: nine halves of a cent is not a
    // quantity of money this rung can store, and `Rational`'s codec clamps
    // rather than rejects, so the model is the only thing that can refuse it.
    CHECK_THROWS_AS(model.execute(ledger::StoreTransaction{
                        .ledgerId = ledgerId,
                        .description = "Fractional minor units",
                        .date = morph::time::Timestamp::now(),
                        .legs = {ledger::TransactionLeg{
                                     .accountId = ledgerState.accounts[0].id,
                                     .amount = morph::math::Rational{Numerator{-9}, Denominator{2}, DecimalPlaces{2}}},
                                 ledger::TransactionLeg{.accountId = ledgerState.accounts[1].id,
                                                        .amount = morph::math::Rational{Numerator{9}, Denominator{2},
                                                                                        DecimalPlaces{2}}}}}),
                    ledger::ValidationError);
}

TEST_CASE("A JPY leg stores and renders as a true integer", "[ledger][model]") {
    // The README's own named test for a zero-decimal currency. JPY declares
    // dp 0, so a leg is a whole number of yen: a client that sends it at
    // dp 2 (the majority default) has it restated onto JPY's scale, and the
    // stored row and rendered text both come back as whole yen -- no
    // `x-rules` gate, no app-side workaround.
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord ledgerRow;
    ledgerRow.name = "Tokyo trip";
    mapper.Create(ledgerRow);
    const auto ledgerId = ledger::LedgerId{static_cast<std::int64_t>(ledgerRow.id.Value())};

    ledger::LedgerModel model;
    const ScopedPrincipal principal{"alice"};
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId,
                                      .name = "Yen wallet",
                                      .kind = ledger::AccountKind::Asset,
                                      .currency = ledger::Currency::JPY});
    model.execute(ledger::OpenAccount{.ledgerId = ledgerId,
                                      .name = "Ramen",
                                      .kind = ledger::AccountKind::Expense,
                                      .currency = ledger::Currency::JPY});
    auto ledgerState = model.execute(ledger::GetLedger{.ledgerId = ledgerId});

    using morph::math::DecimalPlaces;
    using morph::math::Denominator;
    using morph::math::Numerator;
    auto result = model.execute(ledger::StoreTransaction{
        .ledgerId = ledgerId,
        .description = "Ramen",
        .date = morph::time::Timestamp::now(),
        .legs = {ledger::TransactionLeg{
                     .accountId = ledgerState.accounts[0].id,
                     .amount = morph::math::Rational{Numerator{-1500}, Denominator{1}, DecimalPlaces{0}}},
                 // The same ¥1500, sent at the dp-2 default a generic client
                 // would use.
                 ledger::TransactionLeg{
                     .accountId = ledgerState.accounts[1].id,
                     .amount = morph::math::Rational{Numerator{150000}, Denominator{1}, DecimalPlaces{2}}}}});

    REQUIRE(result.accounts.size() == 2);
    CHECK(result.accounts[0].balance.numerator == -1500);
    CHECK(result.accounts[0].balance.decimalPlaces == DecimalPlaces{0});
    CHECK(result.accounts[1].balance.numerator == 1500);
    CHECK(result.accounts[1].balance.decimalPlaces == DecimalPlaces{0});
    CHECK(ledger::formatMoney(ledger::Currency::JPY, result.accounts[1].balance) == "1500");
}

TEST_CASE("StoreTransaction refuses a leg on another book's account", "[ledger][model][security]") {
    // Two books in one database -- every other test in this file uses one,
    // which is exactly why this hole survived out-of-process for so long.
    // Account ids are a table-wide autoincrement, so book two's account id is
    // a well-formed number naming a real row and a lookup by id alone finds
    // it. Without the ledger filter the entry is accepted, the journal is
    // filed under book one, and book two's balance moves with no journal of
    // its own to explain it (morph#367).
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::db::LedgerRecord firstBookRow;
    firstBookRow.name = "Book one";
    mapper.Create(firstBookRow);
    ledger::db::LedgerRecord secondBookRow;
    secondBookRow.name = "Book two";
    mapper.Create(secondBookRow);
    const auto firstBook = ledger::LedgerId{static_cast<std::int64_t>(firstBookRow.id.Value())};
    const auto secondBook = ledger::LedgerId{static_cast<std::int64_t>(secondBookRow.id.Value())};

    ledger::LedgerModel model;
    const ScopedPrincipal principal{"alice"};
    model.execute(ledger::OpenAccount{.ledgerId = firstBook,
                                      .name = "Checking",
                                      .kind = ledger::AccountKind::Asset,
                                      .currency = ledger::Currency::USD});
    model.execute(ledger::OpenAccount{.ledgerId = secondBook,
                                      .name = "Groceries",
                                      .kind = ledger::AccountKind::Expense,
                                      .currency = ledger::Currency::USD});
    const auto ours = model.execute(ledger::GetLedger{.ledgerId = firstBook}).accounts.at(0).id;
    const auto theirs = model.execute(ledger::GetLedger{.ledgerId = secondBook}).accounts.at(0).id;

    using morph::math::DecimalPlaces;
    using morph::math::Denominator;
    using morph::math::Numerator;
    // The message must be the *scope* one, not the not-found one: a leg
    // naming an id no book holds is a different refusal, and a client that
    // cannot tell the two apart cannot tell "you typed a dead id" from "that
    // account is in your other book".
    try {
        model.execute(ledger::StoreTransaction{
            .ledgerId = firstBook,
            .description = "A leg from the other book",
            .date = morph::time::Timestamp::now(),
            .legs = {ledger::TransactionLeg{
                         .accountId = ours,
                         .amount = morph::math::Rational{Numerator{-100}, Denominator{1}, DecimalPlaces{2}}},
                     ledger::TransactionLeg{
                         .accountId = theirs,
                         .amount = morph::math::Rational{Numerator{100}, Denominator{1}, DecimalPlaces{2}}}}});
        FAIL("StoreTransaction accepted a leg on another book's account");
    } catch (const ledger::NotFound& error) {
        CHECK(std::string{error.what()} == "StoreTransaction: account does not belong to this ledger");
    }

    // Refused means nothing moved -- in either book.
    CHECK(model.execute(ledger::GetLedger{.ledgerId = firstBook}).accounts.at(0).balance.numerator == 0);
    CHECK(model.execute(ledger::GetLedger{.ledgerId = secondBook}).accounts.at(0).balance.numerator == 0);
}
