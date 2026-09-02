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
/// The single home for this rung's "whose book is this?" rule (morph#382).
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
