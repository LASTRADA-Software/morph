// SPDX-License-Identifier: Apache-2.0
//
// `ListTransactions` -- the read that makes a journal id nameable (morph#428).
//
// Before this action, `JournalId` appeared in exactly one DTO field in the
// whole rung, and that field was `UndoTransaction`'s own *input*.
// `StoreTransaction` and `UndoTransaction` both answer with `GetLedgerResult`
// (accounts and balances), `GetLedger` the same, `ImportLedgerChunk` with
// counts; there was no `GetJournal` and no listing. So the reversal this rung
// ships -- and the Undo button `gui/qml/LedgerView.qml` ships with it -- could
// only ever be handed a number someone guessed. The rung's own tests reached
// around that by querying the row through a `DataMapper`, which is exactly
// what a socket client and the QML bridge do not have.
//
// The cases below are written so that none of them would pass against a stub
// that returned "something":
//
//   * the cross-book case fails against an unfiltered query -- it asserts an
//     *absence*, and a listing that ignored `WHERE ledger` would return the
//     other book's entry;
//   * the round-trip case feeds a returned id straight back into
//     `UndoTransaction` and requires the balances to return to their exact
//     pre-transaction values, so an id that is merely well-formed is not
//     enough -- it has to be the id of that entry;
//   * the month case asserts an absence too, on both sides of the half-open
//     `[start, end)` boundary.
//
// In-process against `DbFixture`, the same shape as
// `test_ledger_book_ownership.cpp`: these are model cases, and the wire path
// is covered end to end by
// `scripts/scenario/scenarios/ledger/store-list-and-undo-an-entry.scenario`,
// which drives a real `ladder_ledger_server` and takes its journal id from a
// reply rather than from a database.

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <morph/session/session.hpp>
#include <morph/util/datetime.hpp>
#include <morph/util/rational.hpp>
#include <string>

#include "ledger/core/errors.hpp"
#include "ledger/db/ledger_entity.hpp"
#include "ledger/models/ledger_model.hpp"
#include "testkit/db_fixture.hpp"

namespace {

using morph::math::DecimalPlaces;
using morph::math::Denominator;
using morph::math::Numerator;

/// @brief A `Context` carrying only @p principal -- see
///        `test_ledger_model.cpp`'s own identical `contextFor` for why this
///        is not a designated initializer.
/// @param principal The principal to carry.
/// @return The context.
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
/// @param mapper The mapper to write through.
/// @param name   The book's name.
/// @return The new book's id.
[[nodiscard]] ledger::LedgerId unownedBook(Lightweight::DataMapper& mapper, const std::string& name) {
    ledger::db::LedgerRecord row;
    row.name = Light::SqlAnsiString<128>{name};
    mapper.Create(row);
    return ledger::LedgerId{static_cast<std::int64_t>(row.id.Value())};
}

/// @brief An exact USD amount in cents, never a float (design spec §7).
/// @param cents The amount in minor units.
/// @return The amount as a `Rational` at two decimal places.
[[nodiscard]] morph::math::Rational usd(std::int64_t cents) {
    return morph::math::Rational{Numerator{cents}, Denominator{1}, DecimalPlaces{2}};
}

/// @brief A UTC midnight instant.
/// @param year  Calendar year.
/// @param month Calendar month, 1-12.
/// @param day   Day of month.
/// @return The instant as a `Timestamp`.
[[nodiscard]] morph::time::Timestamp utcMidnight(int year, unsigned month, unsigned day) {
    return morph::time::Timestamp{morph::time::DateTime{std::chrono::year{year}, std::chrono::month{month},
                                                        std::chrono::day{day}, std::chrono::hours{0},
                                                        std::chrono::minutes{0}, std::chrono::seconds{0}}};
}

/// @brief One book with a cash and a spend account, ready to post into.
struct Book {
    ledger::LedgerId id;
    ledger::AccountId cash;
    ledger::AccountId spend;
};

/// @brief Creates a book owned by the current principal, with two accounts.
/// @param model The model to create through.
/// @param name  The book's name.
/// @return The book and its two accounts.
[[nodiscard]] Book makeBook(ledger::LedgerModel& model, const std::string& name) {
    Book book;
    book.id = model.execute(ledger::CreateLedger{.name = name}).id;
    book.cash = model
                    .execute(ledger::OpenAccount{.ledgerId = book.id,
                                                 .name = name + " cash",
                                                 .kind = ledger::AccountKind::Asset,
                                                 .currency = ledger::Currency::USD})
                    .id;
    book.spend = model
                     .execute(ledger::OpenAccount{.ledgerId = book.id,
                                                  .name = name + " spend",
                                                  .kind = ledger::AccountKind::Expense,
                                                  .currency = ledger::Currency::USD})
                     .id;
    return book;
}

/// @brief Posts a balanced two-leg entry moving @p cents from cash to spend.
/// @param model       The model to post through.
/// @param book        The book and accounts to post against.
/// @param description The entry's description.
/// @param date        The entry's instant.
/// @param cents       The amount in minor units.
void post(ledger::LedgerModel& model, const Book& book, const std::string& description,
          const morph::time::Timestamp& date, std::int64_t cents) {
    model.execute(ledger::StoreTransaction{
        .ledgerId = book.id,
        .description = description,
        .date = date,
        .legs = {{.accountId = book.cash, .amount = usd(-cents)}, {.accountId = book.spend, .amount = usd(cents)}},
        .opId = {}});
}

/// @brief The balance of @p accountId in @p result.
/// @param result    A `GetLedgerResult` to read.
/// @param accountId The account to find.
/// @return That account's balance.
[[nodiscard]] morph::math::Rational balanceOf(const ledger::GetLedgerResult& result,
                                              const ledger::AccountId& accountId) {
    for (const auto& account : result.accounts) {
        if (account.id == accountId) {
            return account.balance;
        }
    }
    FAIL("no such account in the ledger state");
    return {};
}

}  // namespace

TEST_CASE("ListTransactions returns only the named book's entries", "[ledger][list-transactions]") {
    // The case that fails against an unfiltered query. Two books, both owned
    // by the same principal so ownership cannot be what hides the second, both
    // with an entry in the same month: the listing must name one of them.
    morph::ladder::testkit::DbFixture fixture;
    ledger::LedgerModel model;
    const ScopedPrincipal alice{"alice"};

    const auto mine = makeBook(model, "mine");
    const auto other = makeBook(model, "other");
    post(model, mine, "mine: coffee", utcMidnight(2026, 8, 3), 350);
    post(model, other, "other: coffee", utcMidnight(2026, 8, 3), 450);

    const auto listed = model.execute(ledger::ListTransactions{.ledgerId = mine.id, .month = "2026-08"});

    REQUIRE(listed.entries.size() == 1);
    CHECK(listed.entries.front().description == "mine: coffee");
    for (const auto& entry : listed.entries) {
        CHECK(entry.description != "other: coffee");
    }
}

TEST_CASE("ListTransactions bounds its answer to the month it was asked for", "[ledger][list-transactions]") {
    // Both sides of the half-open [start, end) range, so a bound written with
    // the wrong comparison on either end fails here rather than silently
    // widening or narrowing by a day.
    morph::ladder::testkit::DbFixture fixture;
    ledger::LedgerModel model;
    const ScopedPrincipal alice{"alice"};

    const auto book = makeBook(model, "bounded");
    post(model, book, "july, the last instant of it", utcMidnight(2026, 7, 31), 100);
    post(model, book, "august, the first instant of it", utcMidnight(2026, 8, 1), 200);
    post(model, book, "august, the last day", utcMidnight(2026, 8, 31), 300);
    post(model, book, "september, the first day", utcMidnight(2026, 9, 1), 400);

    const auto august = model.execute(ledger::ListTransactions{.ledgerId = book.id, .month = "2026-08"});

    REQUIRE(august.entries.size() == 2);
    CHECK(august.entries[0].description == "august, the first instant of it");
    CHECK(august.entries[1].description == "august, the last day");
}

TEST_CASE("A month with no entries lists empty rather than refusing", "[ledger][list-transactions]") {
    morph::ladder::testkit::DbFixture fixture;
    ledger::LedgerModel model;
    const ScopedPrincipal alice{"alice"};

    const auto book = makeBook(model, "quiet");
    post(model, book, "august", utcMidnight(2026, 8, 3), 350);

    ledger::ListTransactionsResult listed;
    CHECK_NOTHROW(listed = model.execute(ledger::ListTransactions{.ledgerId = book.id, .month = "2026-09"}));
    CHECK(listed.entries.empty());
}

TEST_CASE("A listed id drives UndoTransaction and restores the exact balances", "[ledger][list-transactions]") {
    // The round trip the whole action exists for, and the reason a stub does
    // not pass: the id has to be *that entry's* id, or the reversal reverses
    // the wrong entry (or nothing) and the balances do not come back.
    morph::ladder::testkit::DbFixture fixture;
    ledger::LedgerModel model;
    const ScopedPrincipal alice{"alice"};

    const auto book = makeBook(model, "round-trip");
    post(model, book, "kept", utcMidnight(2026, 8, 1), 700);

    const auto before = model.execute(ledger::GetLedger{.ledgerId = book.id});
    const auto cashBefore = balanceOf(before, book.cash);
    const auto spendBefore = balanceOf(before, book.spend);

    post(model, book, "to be undone", utcMidnight(2026, 8, 2), 300);

    const auto listed = model.execute(ledger::ListTransactions{.ledgerId = book.id, .month = "2026-08"});
    REQUIRE(listed.entries.size() == 2);
    const auto& undoTarget = listed.entries[1];
    REQUIRE(undoTarget.description == "to be undone");
    REQUIRE(undoTarget.id.hasValue());

    // The legs come back exactly as they were stored -- the same Rational
    // triple, never a float that has been through a double (design spec §7).
    REQUIRE(undoTarget.legs.size() == 2);
    CHECK(undoTarget.legs[0].accountId == book.cash);
    CHECK(undoTarget.legs[0].amount == usd(-300));
    CHECK(undoTarget.legs[1].accountId == book.spend);
    CHECK(undoTarget.legs[1].amount == usd(300));
    CHECK_FALSE(undoTarget.legs[0].foreignAmount.has_value());
    CHECK_FALSE(undoTarget.legs[0].foreignCurrency.has_value());

    const auto after = model.execute(ledger::UndoTransaction{.ledgerId = book.id, .journalId = undoTarget.id});

    CHECK(balanceOf(after, book.cash) == cashBefore);
    CHECK(balanceOf(after, book.spend) == spendBefore);

    // Undo is a compensating action, not an erasure (design spec §6): the
    // reversed entry is still there, still listed, still carrying the same id.
    // The reversal itself is dated when the undo happened rather than when the
    // original was posted, so it is deliberately not asserted to be in this
    // month -- "now" is not August. That it exists is pinned by the second
    // reversal being refused.
    const auto relisted = model.execute(ledger::ListTransactions{.ledgerId = book.id, .month = "2026-08"});
    REQUIRE(relisted.entries.size() == 2);
    CHECK(relisted.entries[1].id == undoTarget.id);
    CHECK(relisted.entries[1].description == "to be undone");
    CHECK_THROWS_AS(model.execute(ledger::UndoTransaction{.ledgerId = book.id, .journalId = undoTarget.id}),
                    ledger::AlreadyReversed);
}

TEST_CASE("ListTransactions refuses a principal that does not own the book",
          "[ledger][list-transactions][ownership]") {
    // The gate every other book-reaching read carries (morph#382). A listing
    // of a book's entries is precisely the read it exists for: without it, a
    // second authenticated client learns every description and amount in a
    // book it has nothing to do with.
    morph::ladder::testkit::DbFixture fixture;
    ledger::LedgerModel model;

    ledger::LedgerId book;
    {
        const ScopedPrincipal alice{"alice"};
        const auto owned = makeBook(model, "alice's");
        post(model, owned, "alice's coffee", utcMidnight(2026, 8, 3), 350);
        book = owned.id;
    }

    const ScopedPrincipal bob{"bob"};
    CHECK_THROWS_AS(model.execute(ledger::ListTransactions{.ledgerId = book, .month = "2026-08"}), ledger::Forbidden);
    try {
        static_cast<void>(model.execute(ledger::ListTransactions{.ledgerId = book, .month = "2026-08"}));
        FAIL("expected Forbidden");
    } catch (const ledger::Forbidden& error) {
        CHECK(std::string{error.what()} == "ListTransactions: this book belongs to another principal");
    }
}

TEST_CASE("An unowned book is still listable by anyone", "[ledger][list-transactions][ownership]") {
    // The migration's backfill decision, pinned here as it is for every other
    // book-reaching action: a `ledgers` row written before the `owner` column
    // existed has no owner and stays readable, exactly as it was.
    morph::ladder::testkit::DbFixture fixture;
    Lightweight::DataMapper mapper;
    ledger::LedgerModel model;

    ledger::LedgerId book;
    ledger::AccountId cash;
    ledger::AccountId spend;
    {
        const ScopedPrincipal alice{"alice"};
        book = unownedBook(mapper, "nobody's book");
        cash = model
                   .execute(ledger::OpenAccount{.ledgerId = book,
                                                .name = "cash",
                                                .kind = ledger::AccountKind::Asset,
                                                .currency = ledger::Currency::USD})
                   .id;
        spend = model
                    .execute(ledger::OpenAccount{.ledgerId = book,
                                                 .name = "spend",
                                                 .kind = ledger::AccountKind::Expense,
                                                 .currency = ledger::Currency::USD})
                    .id;
        model.execute(ledger::StoreTransaction{
            .ledgerId = book,
            .description = "anyone's entry",
            .date = utcMidnight(2026, 8, 3),
            .legs = {{.accountId = cash, .amount = usd(-350)}, {.accountId = spend, .amount = usd(350)}},
            .opId = {}});
    }

    const ScopedPrincipal bob{"bob"};
    const auto listed = model.execute(ledger::ListTransactions{.ledgerId = book, .month = "2026-08"});
    REQUIRE(listed.entries.size() == 1);
    CHECK(listed.entries.front().description == "anyone's entry");
}

TEST_CASE("ListTransactions refuses a malformed month and a missing book", "[ledger][list-transactions]") {
    morph::ladder::testkit::DbFixture fixture;
    ledger::LedgerModel model;
    const ScopedPrincipal alice{"alice"};

    const auto book = makeBook(model, "refusals");

    // The month is validated at the DTO boundary, before any date arithmetic
    // -- the same `detail::isValidYearMonth` `GetBudgetReport` uses.
    CHECK_THROWS_AS(model.execute(ledger::ListTransactions{.ledgerId = book.id, .month = "2026-13"}),
                    ledger::ValidationError);
    CHECK_THROWS_AS(model.execute(ledger::ListTransactions{.ledgerId = book.id, .month = "august"}),
                    ledger::ValidationError);
    CHECK_THROWS_AS(model.execute(ledger::ListTransactions{.ledgerId = {}, .month = "2026-08"}),
                    ledger::ValidationError);
    CHECK_THROWS_AS(model.execute(ledger::ListTransactions{.ledgerId = ledger::LedgerId{999999}, .month = "2026-08"}),
                    ledger::NotFound);
}
