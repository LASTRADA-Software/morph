// SPDX-License-Identifier: Apache-2.0
//
// An account, a budget and a category belong to one book (morph#373).
//
// Accounts, categories and budgets each carry their own `ledger`, and three
// client-facing actions join two of them by id alone: `SetCategory`,
// `LinkAccountToCategory` and `CreateBudget`. Until this file existed, none of
// them compared the two rows' books, so book two's account could be filed
// under book one's category and a book-one budget could report on a book-two
// category. Both rows are real, both ids are well-formed, and every id in this
// rung is a table-wide autoincrement -- so a lookup by id alone finds the
// foreign row and accepts it.
//
// morph#382's ownership gate reaches all three sites and refuses a link across
// an *ownership* boundary. What it deliberately does not refuse -- and what
// every case below exercises -- is two books the **same** principal owns, and
// two **unowned** books (the shape every `ledgers` row written before migration
// `20260819000015` has, including every fixture book the scenario corpus
// seeds).
//
// The rule these cases pin: an account and a category are the same book's, or
// the link is refused; a budget's category is its own book's, or the budget is
// refused. The refusal is `NotFound{"<Action>: category does not belong to
// this ledger"}` -- `accountInLedger`'s idiom from morph#380, deliberately
// distinct from `"<Action>: no such account or category"`, because a client
// that cannot tell them apart cannot tell a dead id from a mis-scoped one.
//
// The last case is the negative control: a same-book link and a same-book
// budget still succeed. Without it a guard that refused every link would pass
// every case above it and look correct.

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
#include "testkit/db_fixture.hpp"

namespace {

/// @brief A `Context` carrying only @p principal -- see
///        `test_ledger_book_ownership.cpp`'s own identical `contextFor` for
///        why this is not a designated initializer.
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
///        before morph#382's migration, and the shape the scenario corpus's
///        fixture books still have.
[[nodiscard]] ledger::LedgerId unownedBook(Lightweight::DataMapper& mapper, const std::string& name) {
    ledger::db::LedgerRecord row;
    row.name = Light::SqlAnsiString<128>{name};
    mapper.Create(row);
    return ledger::LedgerId{static_cast<std::int64_t>(row.id.Value())};
}

/// @brief The category an account is currently filed under, read straight off
///        the row.
///
///        Every refusal below is checked against this rather than against the
///        throw alone: a guard placed *after* the assignment would satisfy a
///        throw-only assertion while having already written the row it refused.
[[nodiscard]] std::optional<std::uint64_t> categoryOf(Lightweight::DataMapper& mapper,
                                                      const ledger::AccountId& accountId) {
    auto rows = mapper.Query<ledger::db::AccountRecord>()
                    .Where(::Lightweight::FieldNameOf<&ledger::db::AccountRecord::id>, "=", *accountId)
                    .All();
    REQUIRE(rows.size() == 1);
    return rows.front().category.Value();
}

/// @brief How many budgets exist, so `CreateBudget`'s refusal can be checked
///        for having written nothing.
[[nodiscard]] std::size_t budgetCount(Lightweight::DataMapper& mapper) {
    return mapper.Query<ledger::db::BudgetRecord>().All().size();
}

/// @brief Two books, an account and a category in each, plus a budget-shaped
///        pair of ids -- the fixture every case below starts from.
struct TwoBooks {
    ledger::LedgerId first;
    ledger::LedgerId second;
    ledger::CategoryId firstCategory;
    ledger::CategoryId secondCategory;
    ledger::AccountId secondSpend;
};

/// @brief Opens an expense account and a category in each of @p first and
///        @p second, and files @p secondSpend under its *own* book's category
///        so a refused cross-book write has a prior value to be checked
///        against.
[[nodiscard]] TwoBooks twoBooks(ledger::LedgerModel& ledgerModel, ledger::BudgetModel& budgetModel,
                                const ledger::LedgerId& first, const ledger::LedgerId& second) {
    TwoBooks books{.first = first,
                   .second = second,
                   .firstCategory = budgetModel.execute(ledger::CreateCategory{.ledgerId = first, .name = "Food"}),
                   .secondCategory = budgetModel.execute(ledger::CreateCategory{.ledgerId = second, .name = "Food"}),
                   .secondSpend = ledgerModel
                                      .execute(ledger::OpenAccount{.ledgerId = second,
                                                                   .name = "Isolated spend",
                                                                   .kind = ledger::AccountKind::Expense,
                                                                   .currency = ledger::Currency::USD})
                                      .id};
    budgetModel.execute(
        ledger::LinkAccountToCategory{.accountId = books.secondSpend, .categoryId = books.secondCategory});
    return books;
}

/// @brief Asserts that all three joins still succeed within one book @p book,
///        and that the link they wrote is readable back.
///
///        The negative control, factored out of its `TEST_CASE` so it can run
///        against both book shapes without a loop -- Catch2's assertion macros
///        each expand to a `try`/`catch`, so six of them nested inside a
///        `for` puts the case over `readability-function-cognitive-complexity`'s
///        threshold on its own.
void checkSameBookIsAccepted(ledger::LedgerModel& ledgerModel, ledger::BudgetModel& budgetModel,
                             Lightweight::DataMapper& mapper, const ledger::LedgerId& book) {
    const auto category = budgetModel.execute(ledger::CreateCategory{.ledgerId = book, .name = "Food"});
    const auto spend = ledgerModel
                           .execute(ledger::OpenAccount{.ledgerId = book,
                                                        .name = "Groceries",
                                                        .kind = ledger::AccountKind::Expense,
                                                        .currency = ledger::Currency::USD})
                           .id;

    CHECK_NOTHROW(budgetModel.execute(ledger::LinkAccountToCategory{.accountId = spend, .categoryId = category}));
    CHECK(categoryOf(mapper, spend) == static_cast<std::uint64_t>(*category));

    CHECK_NOTHROW(ledgerModel.execute(ledger::SetCategory{
        .accountId = spend, .categoryId = category, .ruleId = ledger::RuleId{}, .ruleVersion = 0}));
    CHECK(categoryOf(mapper, spend) == static_cast<std::uint64_t>(*category));

    CHECK_NOTHROW(budgetModel.execute(
        ledger::CreateBudget{.ledgerId = book, .name = "Monthly groceries", .categoryId = category}));
}

}  // namespace

TEST_CASE("SetCategory refuses a category from the caller's other book", "[ledger][scope]") {
    const morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;

    ledger::LedgerModel ledgerModel;
    ledger::BudgetModel budgetModel;
    const ScopedPrincipal alice{"alice"};

    const auto books =
        twoBooks(ledgerModel, budgetModel, ledgerModel.execute(ledger::CreateLedger{.name = "Alice book one"}).id,
                 ledgerModel.execute(ledger::CreateLedger{.name = "Alice book two"}).id);

    // Both books are Alice's, so morph#382's ownership gate lets this through
    // and only the book-scope check can refuse it.
    try {
        ledgerModel.execute(ledger::SetCategory{.accountId = books.secondSpend,
                                                .categoryId = books.firstCategory,
                                                .ruleId = ledger::RuleId{},
                                                .ruleVersion = 0});
        FAIL("SetCategory accepted a category from another book");
    } catch (const ledger::NotFound& error) {
        CHECK(std::string{error.what()} == "SetCategory: category does not belong to this ledger");
    }

    // Refused means unwritten: the account is still on its own book's
    // category, not the foreign one and not nothing.
    CHECK(categoryOf(mapper, books.secondSpend) == static_cast<std::uint64_t>(*books.secondCategory));
}

TEST_CASE("LinkAccountToCategory refuses a category from the caller's other book", "[ledger][scope]") {
    const morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;

    ledger::LedgerModel ledgerModel;
    ledger::BudgetModel budgetModel;
    const ScopedPrincipal alice{"alice"};

    const auto books =
        twoBooks(ledgerModel, budgetModel, ledgerModel.execute(ledger::CreateLedger{.name = "Alice book one"}).id,
                 ledgerModel.execute(ledger::CreateLedger{.name = "Alice book two"}).id);

    try {
        budgetModel.execute(
            ledger::LinkAccountToCategory{.accountId = books.secondSpend, .categoryId = books.firstCategory});
        FAIL("LinkAccountToCategory accepted a category from another book");
    } catch (const ledger::NotFound& error) {
        CHECK(std::string{error.what()} == "LinkAccountToCategory: category does not belong to this ledger");
    }

    CHECK(categoryOf(mapper, books.secondSpend) == static_cast<std::uint64_t>(*books.secondCategory));
}

TEST_CASE("CreateBudget refuses a category from another book", "[ledger][scope]") {
    // The site the original report missed, and the one with report
    // consequences: `budgetRow.ledger` and `budgetRow.category` were resolved
    // independently, so a book-one budget could name a book-two category --
    // and that category is what `GetBudgetReport` fans its account lookup out
    // over.
    const morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;

    ledger::LedgerModel ledgerModel;
    ledger::BudgetModel budgetModel;
    const ScopedPrincipal alice{"alice"};

    const auto books =
        twoBooks(ledgerModel, budgetModel, ledgerModel.execute(ledger::CreateLedger{.name = "Alice book one"}).id,
                 ledgerModel.execute(ledger::CreateLedger{.name = "Alice book two"}).id);
    REQUIRE(budgetCount(mapper) == 0);

    try {
        budgetModel.execute(ledger::CreateBudget{
            .ledgerId = books.first, .name = "Book one budget", .categoryId = books.secondCategory});
        FAIL("CreateBudget accepted a category from another book");
    } catch (const ledger::NotFound& error) {
        CHECK(std::string{error.what()} == "CreateBudget: category does not belong to this ledger");
    }

    // Refused means unwritten: no budget row was left behind by a guard that
    // fired after the `Create`.
    CHECK(budgetCount(mapper) == 0);
}

TEST_CASE("Two unowned books may not be cross-linked either", "[ledger][scope]") {
    // The case morph#382's ownership gate deliberately admits: a NULL owner
    // means "created before ownership existed", so every principal passes the
    // ownership check on both books and the scope check is the only refusal
    // left. This is also the shape the scenario corpus seeds, so it is the
    // shape that has to hold over the wire.
    const morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    const auto first = unownedBook(mapper, "Fixture book one");
    const auto second = unownedBook(mapper, "Fixture book two");

    ledger::LedgerModel ledgerModel;
    ledger::BudgetModel budgetModel;
    const ScopedPrincipal alice{"alice"};

    const auto books = twoBooks(ledgerModel, budgetModel, first, second);
    REQUIRE(budgetCount(mapper) == 0);

    CHECK_THROWS_AS(ledgerModel.execute(ledger::SetCategory{.accountId = books.secondSpend,
                                                            .categoryId = books.firstCategory,
                                                            .ruleId = ledger::RuleId{},
                                                            .ruleVersion = 0}),
                    ledger::NotFound);
    CHECK_THROWS_AS(budgetModel.execute(ledger::LinkAccountToCategory{.accountId = books.secondSpend,
                                                                      .categoryId = books.firstCategory}),
                    ledger::NotFound);
    CHECK_THROWS_AS(budgetModel.execute(ledger::CreateBudget{
                        .ledgerId = books.first, .name = "Book one budget", .categoryId = books.secondCategory}),
                    ledger::NotFound);

    CHECK(categoryOf(mapper, books.secondSpend) == static_cast<std::uint64_t>(*books.secondCategory));
    CHECK(budgetCount(mapper) == 0);
}

TEST_CASE("A same-book link and a same-book budget are still accepted", "[ledger][scope]") {
    // The negative control. Every case above asserts a refusal, and a guard
    // that refused *every* link would satisfy all of them -- so the ordinary
    // path is pinned here, on both an owned and an unowned book, and the link
    // is read back rather than merely not-thrown.
    const morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;

    ledger::LedgerModel ledgerModel;
    ledger::BudgetModel budgetModel;
    const ScopedPrincipal alice{"alice"};

    // Once on a book Alice owns, and once on an unowned one, because those are
    // the two book shapes the refusing cases above use and a guard that
    // refused on either would be caught here.
    checkSameBookIsAccepted(ledgerModel, budgetModel, mapper,
                            ledgerModel.execute(ledger::CreateLedger{.name = "Alice's own book"}).id);
    checkSameBookIsAccepted(ledgerModel, budgetModel, mapper, unownedBook(mapper, "Fixture book"));

    CHECK(budgetCount(mapper) == 2);
}
