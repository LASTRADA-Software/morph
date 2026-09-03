// SPDX-License-Identifier: Apache-2.0
//
// Per-book authorization (morph#382).
//
// Until this file existed, `ledger` had no per-book ownership of any kind:
// any principal holding a valid token could read, write and post into any
// book, including one another principal had just created over the wire with
// `CreateLedger`. The signed-token check and the per-action empty-principal
// gate both held, and neither of them says *whose* book this is.
//
// The rule these cases pin: `CreateLedger` records its caller as the book's
// owner, and every action that reaches a book -- through its `ledgerId`, or
// through an account, budget, rule or report job that belongs to it -- refuses
// a different principal with `Forbidden`. Ownership is expressed through the
// relation (`examples/IMPLEMENTATION.md` rule 4, bank's `loadOwned`), not at
// the authorizer: `LedgerModel`'s instances are keyed by `ledgerId` and shared
// across every client that opens the same book, so `authorizeInstance` has no
// single owning caller to compare against.
//
// The last case pins the migration's backfill decision, which is the one place
// the old behaviour survives: a `ledgers` row written before the `owner` column
// existed has no owner, and stays readable and writable by everyone, exactly as
// it was. Nothing creates such a row any more.

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <morph/session/session.hpp>
#include <optional>
#include <string>

#include "ledger/core/errors.hpp"
#include "ledger/db/ledger_entity.hpp"
#include "ledger/models/budget_model.hpp"
#include "ledger/models/ledger_model.hpp"
#include "ledger/models/rule_model.hpp"
#include "testkit/db_fixture.hpp"

namespace {

/// @brief A `Context` carrying only @p principal -- see
///        `test_ledger_model.cpp`'s own identical `contextFor` for why this
///        is not a designated initializer.
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

/// @brief A book with no recorded owner -- the shape every `ledgers` row had
///        before morph#382's migration, written the only way one can still be
///        written now that `CreateLedger` always stamps its caller.
[[nodiscard]] ledger::LedgerId unownedBook(Lightweight::DataMapper& mapper, const std::string& name) {
    ledger::db::LedgerRecord row;
    row.name = Light::SqlAnsiString<128>{name};
    mapper.Create(row);
    return ledger::LedgerId{static_cast<std::int64_t>(row.id.Value())};
}

}  // namespace

TEST_CASE("A book records the principal that created it", "[ledger][ownership]") {
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;

    ledger::LedgerModel model;
    ledger::LedgerId book;
    {
        const ScopedPrincipal alice{"alice"};
        book = model.execute(ledger::CreateLedger{.name = "Alice's private book"}).id;
    }
    REQUIRE(book.hasValue());

    auto rows = mapper.Query<ledger::db::LedgerRecord>()
                    .Where(::Lightweight::FieldNameOf<&ledger::db::LedgerRecord::id>, "=", *book)
                    .All();
    REQUIRE(rows.size() == 1);
    REQUIRE(rows.front().owner.Value().has_value());
    CHECK(std::string{rows.front().owner.Value()->ToStringView()} == "alice");
}

TEST_CASE("A second principal can neither read nor write a book it does not own", "[ledger][ownership]") {
    morph::ladder::testkit::DbFixture fixture;

    ledger::LedgerModel model;
    ledger::LedgerId book;
    ledger::AccountId cash;
    ledger::AccountId spend;
    {
        const ScopedPrincipal alice{"alice"};
        book = model.execute(ledger::CreateLedger{.name = "Alice's private book"}).id;
        cash = model
                   .execute(ledger::OpenAccount{.ledgerId = book,
                                                .name = "Alice Cash",
                                                .kind = ledger::AccountKind::Asset,
                                                .currency = ledger::Currency::USD})
                   .id;
        spend = model
                    .execute(ledger::OpenAccount{.ledgerId = book,
                                                 .name = "Alice Spend",
                                                 .kind = ledger::AccountKind::Expense,
                                                 .currency = ledger::Currency::USD})
                    .id;
    }

    const ScopedPrincipal bob{"bob"};

    // The read half. `GetLedger` had no principal check at all, so this is
    // where a second client learned the whole book.
    CHECK_THROWS_AS(model.execute(ledger::GetLedger{.ledgerId = book}), ledger::Forbidden);

    // The write half, in the two shapes the reproduction on morph#382 used.
    CHECK_THROWS_AS(model.execute(ledger::OpenAccount{.ledgerId = book,
                                                      .name = "Bob's account in Alice's book",
                                                      .kind = ledger::AccountKind::Liability,
                                                      .currency = ledger::Currency::USD}),
                    ledger::Forbidden);
    CHECK_THROWS_AS(
        model.execute(ledger::StoreTransaction{
            .ledgerId = book,
            .description = "Bob posts into Alice's book",
            .date = {},
            .legs = {{.accountId = cash,
                      .amount = morph::math::Rational{morph::math::Numerator{-100}, morph::math::Denominator{1},
                                                      morph::math::DecimalPlaces{2}}},
                     {.accountId = spend,
                      .amount = morph::math::Rational{morph::math::Numerator{100}, morph::math::Denominator{1},
                                                      morph::math::DecimalPlaces{2}}}},
            .opId = {}}),
        ledger::Forbidden);
    CHECK_THROWS_AS(model.execute(ledger::ImportLedgerChunk{.ledgerId = book,
                                                            .counterAccountId = spend,
                                                            .csvChunk = "date,description,account_id,amount\n",
                                                            .opId = ledger::ImportOpId{"bob-import"}}),
                    ledger::Forbidden);
    CHECK_THROWS_AS(model.execute(ledger::SubmitReport{
                        .ledgerId = book, .kind = ledger::ReportKind::MonthlyStatement, .params = "{}"}),
                    ledger::Forbidden);
}

TEST_CASE("The owner is still free to work in its own book", "[ledger][ownership]") {
    morph::ladder::testkit::DbFixture fixture;

    ledger::LedgerModel model;
    const ScopedPrincipal alice{"alice"};

    const auto book = model.execute(ledger::CreateLedger{.name = "Alice's private book"}).id;
    const auto cash = model
                          .execute(ledger::OpenAccount{.ledgerId = book,
                                                       .name = "Alice Cash",
                                                       .kind = ledger::AccountKind::Asset,
                                                       .currency = ledger::Currency::USD})
                          .id;
    const auto spend = model
                           .execute(ledger::OpenAccount{.ledgerId = book,
                                                        .name = "Alice Spend",
                                                        .kind = ledger::AccountKind::Expense,
                                                        .currency = ledger::Currency::USD})
                           .id;

    CHECK_NOTHROW(model.execute(ledger::GetLedger{.ledgerId = book}));
    CHECK_NOTHROW(model.execute(ledger::StoreTransaction{
        .ledgerId = book,
        .description = "groceries",
        .date = {},
        .legs = {{.accountId = cash,
                  .amount = morph::math::Rational{morph::math::Numerator{-100}, morph::math::Denominator{1},
                                                  morph::math::DecimalPlaces{2}}},
                 {.accountId = spend,
                  .amount = morph::math::Rational{morph::math::Numerator{100}, morph::math::Denominator{1},
                                                  morph::math::DecimalPlaces{2}}}},
        .opId = {}}));
}

TEST_CASE("Replaying another principal's opId does not hand back its book", "[ledger][ownership]") {
    // `StoreTransaction`'s exactly-once path answers a repeated `opId` with
    // the stored `GetLedgerResult` -- every account and balance in the book.
    // That return sits early in the action, so a book gate placed after it
    // would leak exactly what the `GetLedger` gate withholds: guess the opId
    // and read the whole book. The account lookups just below it are the same
    // hazard in weaker form, a "does account N belong to book B" oracle.
    morph::ladder::testkit::DbFixture fixture;

    ledger::LedgerModel model;
    ledger::LedgerId book;
    ledger::AccountId cash;
    ledger::AccountId spend;
    const ledger::ImportOpId opId{"tx-1"};
    {
        const ScopedPrincipal alice{"alice"};
        book = model.execute(ledger::CreateLedger{.name = "Alice's private book"}).id;
        cash = model
                   .execute(ledger::OpenAccount{.ledgerId = book,
                                                .name = "Alice Cash",
                                                .kind = ledger::AccountKind::Asset,
                                                .currency = ledger::Currency::USD})
                   .id;
        spend = model
                    .execute(ledger::OpenAccount{.ledgerId = book,
                                                 .name = "Alice Spend",
                                                 .kind = ledger::AccountKind::Expense,
                                                 .currency = ledger::Currency::USD})
                    .id;
        model.execute(ledger::StoreTransaction{
            .ledgerId = book,
            .description = "groceries",
            .date = {},
            .legs = {{.accountId = cash,
                      .amount = morph::math::Rational{morph::math::Numerator{-100}, morph::math::Denominator{1},
                                                      morph::math::DecimalPlaces{2}}},
                     {.accountId = spend,
                      .amount = morph::math::Rational{morph::math::Numerator{100}, morph::math::Denominator{1},
                                                      morph::math::DecimalPlaces{2}}}},
            .opId = opId});
    }

    const ScopedPrincipal bob{"bob"};
    CHECK_THROWS_AS(
        model.execute(ledger::StoreTransaction{
            .ledgerId = book,
            .description = "groceries",
            .date = {},
            .legs = {{.accountId = cash,
                      .amount = morph::math::Rational{morph::math::Numerator{-100}, morph::math::Denominator{1},
                                                      morph::math::DecimalPlaces{2}}},
                     {.accountId = spend,
                      .amount = morph::math::Rational{morph::math::Numerator{100}, morph::math::Denominator{1},
                                                      morph::math::DecimalPlaces{2}}}},
            .opId = opId}),
        ledger::Forbidden);
}

TEST_CASE("SetCategory refuses a category in another principal's book", "[ledger][ownership]") {
    // The action joins two rows, and gating only the account would let Bob
    // file his own account under Alice's category. `GetBudgetReport` selects
    // legs by exactly that link, so every entry Bob posts would then be summed
    // into Alice's budget report.
    morph::ladder::testkit::DbFixture fixture;

    ledger::LedgerModel ledgerModel;
    ledger::BudgetModel budgetModel;

    ledger::CategoryId aliceCategory;
    {
        const ScopedPrincipal alice{"alice"};
        const auto aliceBook = ledgerModel.execute(ledger::CreateLedger{.name = "Alice's private book"}).id;
        aliceCategory = budgetModel.execute(ledger::CreateCategory{.ledgerId = aliceBook, .name = "Alice food"});
    }

    const ScopedPrincipal bob{"bob"};
    const auto bobBook = ledgerModel.execute(ledger::CreateLedger{.name = "Bob's own book"}).id;
    const auto bobAccount = ledgerModel
                                .execute(ledger::OpenAccount{.ledgerId = bobBook,
                                                             .name = "Bob Spend",
                                                             .kind = ledger::AccountKind::Expense,
                                                             .currency = ledger::Currency::USD})
                                .id;

    CHECK_THROWS_AS(
        ledgerModel.execute(ledger::SetCategory{
            .accountId = bobAccount, .categoryId = aliceCategory, .ruleId = ledger::RuleId{}, .ruleVersion = 0}),
        ledger::Forbidden);
    CHECK_THROWS_AS(
        budgetModel.execute(ledger::LinkAccountToCategory{.accountId = bobAccount, .categoryId = aliceCategory}),
        ledger::Forbidden);
}

TEST_CASE("BudgetModel and RuleModel refuse a book they do not own", "[ledger][ownership]") {
    morph::ladder::testkit::DbFixture fixture;

    ledger::LedgerModel ledgerModel;
    ledger::BudgetModel budgetModel;
    ledger::RuleModel ruleModel;

    ledger::LedgerId book;
    ledger::CategoryId category;
    ledger::BudgetId budget;
    ledger::RuleId rule;
    {
        const ScopedPrincipal alice{"alice"};
        book = ledgerModel.execute(ledger::CreateLedger{.name = "Alice's private book"}).id;
        category = budgetModel.execute(ledger::CreateCategory{.ledgerId = book, .name = "Food"});
        budget = budgetModel.execute(
            ledger::CreateBudget{.ledgerId = book, .name = "Monthly food", .categoryId = category});
        rule = ruleModel.execute(ledger::CreateRule{.ledgerId = book,
                                                    .trigger = ledger::RuleTrigger::DescriptionContains,
                                                    .matchText = "market",
                                                    .action = ledger::RuleAction::SetCategory,
                                                    .actionValue = std::to_string(*category)});
    }

    const ScopedPrincipal bob{"bob"};

    CHECK_THROWS_AS(budgetModel.execute(ledger::CreateCategory{.ledgerId = book, .name = "Bob's category"}),
                    ledger::Forbidden);
    CHECK_THROWS_AS(
        budgetModel.execute(ledger::CreateBudget{.ledgerId = book, .name = "Bob's budget", .categoryId = category}),
        ledger::Forbidden);
    CHECK_THROWS_AS(budgetModel.execute(ledger::SetBudgetLimit{
                        .budgetId = budget,
                        .month = "2026-08",
                        .limit = morph::math::Rational{morph::math::Numerator{10000}, morph::math::Denominator{1},
                                                       morph::math::DecimalPlaces{2}},
                        .currency = ledger::Currency::USD}),
                    ledger::Forbidden);
    CHECK_THROWS_AS(budgetModel.execute(ledger::GetBudgetReport{.budgetId = budget, .month = "2026-08"}),
                    ledger::Forbidden);
    CHECK_THROWS_AS(ruleModel.execute(ledger::CreateRule{.ledgerId = book,
                                                         .trigger = ledger::RuleTrigger::DescriptionContains,
                                                         .matchText = "bob",
                                                         .action = ledger::RuleAction::SetCategory,
                                                         .actionValue = std::to_string(*category)}),
                    ledger::Forbidden);
    CHECK_THROWS_AS(ruleModel.execute(ledger::UpdateRule{.ruleId = rule,
                                                         .matchText = "bob",
                                                         .actionValue = std::to_string(*category),
                                                         .expectedVersion = std::nullopt}),
                    ledger::Forbidden);
}

TEST_CASE("A book written before the owner column stays open to everyone", "[ledger][ownership]") {
    // The migration's backfill decision, pinned. `AddNotRequiredColumn` cannot
    // populate rows that already exist and there is no principal to attribute
    // them to, so a NULL owner means "created before ownership existed" -- the
    // same reading `params_json` already has on the report-job row -- and such
    // a book behaves exactly as every book did before morph#382. The scenario
    // corpus's fixture books are written this way.
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    const auto book = unownedBook(mapper, "Scenario book");

    ledger::LedgerModel model;
    ledger::AccountId cash;
    {
        const ScopedPrincipal alice{"alice"};
        cash = model
                   .execute(ledger::OpenAccount{.ledgerId = book,
                                                .name = "Shared cash",
                                                .kind = ledger::AccountKind::Asset,
                                                .currency = ledger::Currency::USD})
                   .id;
    }

    const ScopedPrincipal bob{"bob"};
    CHECK_NOTHROW(model.execute(ledger::GetLedger{.ledgerId = book}));
    CHECK_NOTHROW(model.execute(ledger::OpenAccount{.ledgerId = book,
                                                    .name = "Also shared",
                                                    .kind = ledger::AccountKind::Expense,
                                                    .currency = ledger::Currency::USD}));
    CHECK(cash.hasValue());
}
