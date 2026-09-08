// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/DataMapper/DataMapper.hpp>
#include <cstdint>
#include <morph/session/session.hpp>
#include <string>
#include <string_view>

#include "ledger/core/errors.hpp"
#include "ledger/core/types.hpp"
#include "ledger/db/ledger_entity.hpp"

/// @file
/// The single home for this rung's "whose book is this?" rule (morph#382), and
/// for the "*which* book is this?" rule that sits beside it
/// (`requireCategoryInBook`, morph#373).
///
/// **Where the rule lives, and why not at the authorizer.**
/// `examples/IMPLEMENTATION.md` rule 4 puts ownership authorization *through
/// the relation*, the way `bank::db::loadOwned`
/// (`examples/bank/include/bank/db/ledger_ops.hpp`) does for the rung with the
/// same subject matter. `LedgerAuthorizer`'s `authorizeInstance` hook cannot
/// express it: that hook compares one recorded register-time owner against the
/// caller, and `LedgerModel`'s instances are keyed by `ledgerId` and shared
/// across every client that opens the same book, so there is no single owning
/// caller for it to compare against. Rule 1 says the same thing from the other
/// side -- models re-check their own authorization.
///
/// So every action that reaches a book calls into this header, either directly
/// on its own `ledgerId` (`requireOwnedBook`) or, when it names a child row
/// instead, on the ledger that row belongs to (`requireOwnedParentBook`).
///
/// **What a NULL owner means.** A `ledgers` row written before the `owner`
/// column existed (schema migration 20260819000015) carries no owner, and this
/// header lets every authenticated principal through for it -- exactly the
/// behaviour that book had before the column was added. `CreateLedger` stamps
/// every book written from here on, so nothing produces a new unowned book.
/// The scenario corpus's fixture books are seeded by raw `INSERT` and are
/// unowned for this reason.
///
/// **What it does not hide.** A principal that does not own book 7 learns that
/// book 7 exists, because the refusals are distinguishable (`NotFound` versus
/// `Forbidden`). That is `bank::db::loadOwned`'s own ordering and it is the
/// deliberate choice here too: book ids are dense and sequential, so existence
/// is not a secret this rung could keep, and collapsing the two refusals would
/// make a real "no such ledger" indistinguishable from a permissions problem
/// for the owner debugging it.

namespace ledger::db {

/// @brief Whether @p book may be reached by @p principal.
///
/// True when the book records no owner (see this file's comment on NULL) or
/// records exactly @p principal. Byte comparison, not a case- or
/// whitespace-folding one: `LedgerAuthorizer::isValidPrincipal` already
/// restricts a principal to `[A-Za-z0-9._:-]`, and `RemoteServer` overwrites
/// `Context::principal` with the identity the token verified, so the two sides
/// of this comparison are the same bytes or they are different identities.
/// @param book      The loaded `ledgers` row.
/// @param principal The caller's authenticated principal.
/// @return `true` if the caller may read and write this book.
[[nodiscard]] inline bool bookIsReachableBy(const LedgerRecord& book, std::string_view principal) noexcept {
    const auto& owner = book.owner.Value();
    if (!owner.has_value()) {
        return true;
    }
    return owner->ToStringView() == principal;
}

/// @brief Loads the book @p ledgerId names, requiring it to exist and to be
///        reachable by @p principal.
///
/// The guard every action carrying a `ledgerId` runs. @p action is woven into
/// both refusals so a client is told which action refused, matching the
/// `"<Action>: no such ledger"` messages these call sites already threw.
/// @param mapper    The data mapper to query through.
/// @param ledgerId  The book's row id.
/// @param principal The caller's authenticated principal.
/// @param action    The calling action's name, prefixed onto both refusals.
/// @return The book's row, so a caller needing it for a `BelongsTo`
///         assignment does not query twice.
/// @throws NotFound if no book has that id.
/// @throws Forbidden if the book belongs to a different principal.
[[nodiscard]] inline LedgerRecord requireOwnedBookById(Lightweight::DataMapper& mapper, std::int64_t ledgerId,
                                                       std::string_view principal, std::string_view action) {
    auto rows = mapper.Query<LedgerRecord>().Where(::Lightweight::FieldNameOf<&LedgerRecord::id>, "=", ledgerId).All();
    if (rows.empty()) {
        throw NotFound{std::string{action} + ": no such ledger"};
    }
    if (!bookIsReachableBy(rows.front(), principal)) {
        throw Forbidden{std::string{action} + ": this book belongs to another principal"};
    }
    return rows.front();
}

/// @brief `requireOwnedBookById` for a strong `LedgerId`.
/// @param mapper    The data mapper to query through.
/// @param ledgerId  The book's id, which must be engaged.
/// @param principal The caller's authenticated principal.
/// @param action    The calling action's name, prefixed onto both refusals.
/// @return The book's row.
/// @throws NotFound if no book has that id.
/// @throws Forbidden if the book belongs to a different principal.
[[nodiscard]] inline LedgerRecord requireOwnedBook(Lightweight::DataMapper& mapper, const LedgerId& ledgerId,
                                                   std::string_view principal, std::string_view action) {
    return requireOwnedBookById(mapper, *ledgerId, principal, action);
}

/// @brief The same guard for an action that named a *child* row -- an account,
///        a budget, a rule, a report job -- rather than a book.
///
/// Such an action has already loaded its own row and refused a missing one
/// with its own message; what is left is the book that row belongs to, whose
/// id comes off the child's `BelongsTo`.
///
/// A child row whose book has vanished is *admitted*, not refused. This guard
/// answers "is this book someone else's?", and a book that does not exist is
/// nobody's -- there is no owner left to wrong, and refusing would turn an
/// orphaned row into a permissions error for the one caller who needs to see
/// it. `test_app.cpp`'s "A job whose ledger no longer exists settles Failed"
/// is exactly that case: the job settles terminally and its status must stay
/// readable, or a poller spins on it forever.
/// @param mapper    The data mapper to query through.
/// @param ledgerId  The child row's `ledger.Value()`.
/// @param principal The caller's authenticated principal.
/// @param action    The calling action's name, prefixed onto the refusal.
/// @throws Forbidden if the book exists and belongs to a different principal.
inline void requireOwnedParentBook(Lightweight::DataMapper& mapper, std::uint64_t ledgerId, std::string_view principal,
                                   std::string_view action) {
    auto rows = mapper.Query<LedgerRecord>()
                    .Where(::Lightweight::FieldNameOf<&LedgerRecord::id>, "=", static_cast<std::int64_t>(ledgerId))
                    .All();
    if (!rows.empty() && !bookIsReachableBy(rows.front(), principal)) {
        throw Forbidden{std::string{action} + ": this book belongs to another principal"};
    }
}

/// @brief Refuses a category that belongs to a book other than @p bookLedgerId
///        -- *which* book, where everything above answers *whose* (morph#373).
///
/// Three actions join a category to something else by id alone:
/// `SetCategory` and `LinkAccountToCategory` join it to an account,
/// `CreateBudget` joins it to a book. Category and account ids are table-wide
/// autoincrements, so another book's id is a perfectly well-formed number
/// naming a real row: a lookup by id alone finds it and accepts it, and the
/// cross-book link is written. `requireOwnedParentBook` above refuses that
/// only when the two books have *different owners*; two books the same
/// principal owns, and two unowned ones, passed straight through.
///
/// `examples/IMPLEMENTATION.md` rule 1 is what makes that a defect rather than
/// a documented liberty: a model re-checks its own preconditions, and "an
/// account and a category are the same book's" is a precondition this rung
/// documented and did not check. What made the mis-scoped row survivable was
/// an invariant nothing states -- `GetBudgetReport` filters legs by the
/// budget's own ledger's journals, so a foreign account's legs never reach the
/// sum -- and a guarantee that rests on every future report keeping a filter
/// nobody wrote down is the half-a-scheme shape morph#384 rejected.
///
/// Neither `SetCategory` nor `LinkAccountToCategory` carries a `ledgerId`, so
/// this cannot be a `Where` folded into the lookup the way morph#380's
/// `accountInLedger` scopes a leg's account against the ledger its action
/// names. It is a comparison of the two loaded rows' own `ledger` values
/// instead -- which is also why the refusal is raised after the not-found and
/// ownership ones, leaving their wording and ordering untouched.
///
/// `NotFound`, and a message of its own: `accountInLedger`'s exact idiom for
/// the identical question about an account, for the reason morph#380 gave --
/// a client that cannot tell "that id names nothing" from "that id is in your
/// other book" cannot tell a dead id from a mis-scoped one. Not
/// `ValidationError`: the request is well-formed, and every other "wrong book"
/// answer in this rung (`UndoTransaction`'s journal, `RunReportJob`'s job,
/// `accountInLedger`'s account) is a `NotFound`.
///
/// **Write-side only.** A row already holding a cross-book link stays as it
/// is; no migration rewrites one, and no read refuses one. See
/// `examples/ledger/README.md`'s "Which book it is" for why.
/// @param categoryLedgerId The category row's `ledger.Value()`.
/// @param bookLedgerId     The book the action is scoped to -- the account's
///                         own ledger for a link, the named `ledgerId` for a
///                         budget.
/// @param action           The calling action's name, prefixed onto the refusal.
/// @throws NotFound if the category belongs to a different book.
inline void requireCategoryInBook(std::uint64_t categoryLedgerId, std::uint64_t bookLedgerId,
                                  std::string_view action) {
    if (categoryLedgerId != bookLedgerId) {
        throw NotFound{std::string{action} + ": category does not belong to this ledger"};
    }
}

/// @brief The caller's authenticated principal, or an empty view when no
///        session is in scope.
///
/// An empty principal never matches an owner, so a read that carries no
/// principal at all is refused on an owned book and admitted on an unowned
/// one -- which is what the mutating actions' own `EmptyPrincipalError` gate
/// already achieves for writes, without this header needing to duplicate it.
/// @return The current principal, or `{}`.
[[nodiscard]] inline std::string_view currentPrincipal() noexcept {
    const auto* ctx = ::morph::session::current();
    return ctx != nullptr ? std::string_view{ctx->principal} : std::string_view{};
}

}  // namespace ledger::db
