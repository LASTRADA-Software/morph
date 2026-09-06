// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <morph/util/datetime.hpp>
#include <morph/util/rational.hpp>
#include <optional>
#include <string>
#include <vector>

#include "ledger/core/import_op_id.hpp"
#include "ledger/core/time_util.hpp"
#include "ledger/core/types.hpp"
#include "ledger/core/units.hpp"

namespace ledger {

/// @brief One leg of a `StoreTransaction` (Task 8) or the multi-client
///        stress harness (Task 23). Declared ahead of `StoreTransaction`
///        itself, per this task's own scope.
struct TransactionLeg {
    AccountId accountId;
    morph::math::Rational amount;  // real currency comes from the account this leg names, per design spec §2
    std::optional<morph::math::Rational> foreignAmount;  // display/audit metadata only --
    std::optional<Currency> foreignCurrency;             // never enters a zero-sum check (design spec §1 step 3)
};

/// @brief Records a multi-leg transaction against `ledgerId`'s accounts,
///        enforcing design spec §1's per-currency zero-sum invariant: every
///        leg's amount is partitioned by the account it names' own
///        currency, and each partition's amounts must sum to canonical
///        zero (`LedgerModel::execute` throws `ZeroSumViolation` otherwise).
///
///        `opId` (Task 11b) is this action's exactly-once key: a disengaged
///        `opId` (the default -- Task 8/9's own existing call sites, which
///        predate this field) skips the applied-ops ledger entirely and
///        takes the ordinary insert-only path. An engaged `opId` that has
///        already been applied for this ledger returns the previously
///        stored `GetLedgerResult` verbatim instead of inserting a second
///        journal+legs row -- the mechanism `morph::journal::replay()`
///        (Task 12) relies on to re-dispatch this entry safely.
struct StoreTransaction {
    LedgerId ledgerId;
    std::string description;
    morph::time::Timestamp date;
    std::vector<TransactionLeg> legs;
    ImportOpId opId;

    [[nodiscard]] bool validate() const noexcept {
        return ledgerId.hasValue() && !description.empty() && legs.size() >= 2 &&
               std::ranges::all_of(legs, [](const auto& leg) { return leg.accountId.hasValue(); });
    }
};

/// @brief Links `accountId` to `categoryId` -- the same mutation
///        `BudgetModel::execute(LinkAccountToCategory)` (Task 10) performs,
///        reused here as the cascade's own action type so a `RuleTrigger::
///        DescriptionContains` match in `LedgerModel::execute(StoreTransaction)`
///        (Task 12, design spec §4/§5) has a loggable, replayable action to
///        record for the categorization it performs. Carries `ruleId` and
///        `ruleVersion` -- the firing rule's identity and the exact version
///        that fired -- so the recorded `LogEntry::payload` pins which rule
///        (and which edit of that rule) produced this cascade, per the
///        divergence test's own requirement: replaying an edited rule must
///        reproduce the original firing's outcome, never the edited rule's.
///
///        Registered via `BRIDGE_REGISTER_ACTION` and given a public
///        `execute()` overload purely so `morph::journal::replay()` can
///        route a recorded `"SetCategory"` entry back through the
///        dispatcher's registered-action table -- not because a client is
///        expected to dispatch it this way. The cascade path in
///        `execute(StoreTransaction)` never calls through that public
///        overload (it would double-log); it calls the shared
///        `setCategoryImpl` directly, then journals with a `causalParentId`
///        the public overload never sets. See kanban's own `ApplyTagMutation`
///        (`ladder-kanban-impl:examples/kanban/src/models/board_model.cpp`)
///        for the identical reasoning.
struct SetCategory {
    AccountId accountId;
    CategoryId categoryId;
    RuleId ruleId;
    std::int32_t ruleVersion;
};

/// @brief Empty result placeholder for `SetCategory`'s cascade logging --
///        this rung's own name for the same empty-result shape kanban's
///        `Ack` (`examples/kanban/include/kanban/dto/project_dto.hpp`) serves
///        there; not imported from kanban (a different rung's type), a fresh
///        local declaration with the same shape.
struct SetCategoryResult {};

/// @brief Undoes a previously-recorded `TransactionJournalRecord` (named by
///        `journalId`) as a compensating action -- a second, reversing
///        journal entry whose legs are the original legs' amounts negated
///        via `Rational::operator-() const` (the member unary negation),
///        never `morph::journal::undoLast()` (design spec §6): the ledger's
///        own journal is an audit trail, so "undo" must itself be a new,
///        visible entry, not an erasure of the original one.
///
///        `ledgerId` is redundant with `journalId` (the journal row already
///        names its own ledger via `TransactionJournalRecord::ledger`), but
///        is carried explicitly anyway so `ActionKeyTraits<UndoTransaction>::
///        key()` stays a trivial field read like every other keyed action in
///        this file, rather than requiring its own `Lightweight::DataMapper`
///        lookup before any key/dispatch/transaction context exists.
///        `LedgerModel::execute(const UndoTransaction&)` independently
///        verifies the looked-up journal's own `ledger` really matches
///        `ledgerId` (`throw NotFound{...}` on mismatch), so a wrong
///        `ledgerId` cannot be used to bypass anything or target the wrong
///        ledger's model instance.
struct UndoTransaction {
    LedgerId ledgerId;
    JournalId journalId;

    [[nodiscard]] bool validate() const noexcept { return ledgerId.hasValue() && journalId.hasValue(); }
};

/// @brief Lists the journal entries `ledgerId` recorded during `month`, so a
///        client can name one -- to `UndoTransaction`, or to anything else
///        that takes a `JournalId`.
///
///        This action exists because nothing else in the rung's wire surface
///        ever hands a `journalId` back (morph#428). `StoreTransaction` and
///        `UndoTransaction` both answer with `GetLedgerResult` -- the
///        accounts and their balances -- `GetLedger` the same, and
///        `ImportLedgerChunk` with counts. `JournalId` appeared in exactly
///        one DTO field in the whole rung, and that field was
///        `UndoTransaction`'s own *input*. The rung's own tests reached
///        around that by querying the row through a `DataMapper`, which is
///        precisely what a WebSocket client and the QML bridge do not have,
///        so the shipped Undo control had no source for the one number it
///        asks for.
///
///        Month-bounded, in the exact `"YYYY-MM"` shape
///        `GetBudgetReport::month` uses and validated by the same
///        `detail::isValidYearMonth`, so the result cannot grow without
///        bound as a book ages. That bound is the whole bounding mechanism --
///        deliberately no cursor and no page size (morph#428's own scope):
///        one month of one book is a quantity a client can hold, and a
///        pagination protocol is a wire contract worth designing on its own
///        evidence rather than inventing here.
struct ListTransactions {
    LedgerId ledgerId;
    std::string month;  // "YYYY-MM", the exact shape GetBudgetReport::month uses

    [[nodiscard]] bool validate() const noexcept { return ledgerId.hasValue() && detail::isValidYearMonth(month); }
};

/// @brief One journal entry as a client sees it: the id it can name, plus
///        enough of the entry to recognise which one it is.
///
///        `legs` are the rung's existing `TransactionLeg` aggregate, so an
///        entry's own amounts travel in the same exact `Rational` form
///        `StoreTransaction` sent them in -- never a float, per design spec
///        §7. `foreignAmount`/`foreignCurrency` come back disengaged for a
///        leg that carried none, exactly as they were stored.
///
///        No balance and no account names: this is a listing of *entries*,
///        and `GetLedger` already answers the account question. Keeping the
///        two apart is what lets this one stay a cheap month-scoped read.
struct TransactionEntryInfo {
    JournalId id;
    std::string description;
    morph::time::Timestamp date;
    std::vector<TransactionLeg> legs;
};

/// @brief `ListTransactions`' answer: the month's entries, oldest first.
///
///        Ordered by the journal's own row id rather than by `date`, which is
///        client-supplied and therefore not required to be monotonic: two
///        entries can carry the same instant, and an import can post an older
///        date after a newer one. Row id is the order the book recorded them
///        in, which is the order an audit trail is read in and the only one
///        this rung can guarantee is total.
///
///        An empty vector for a month with no entries -- that is an answer,
///        not a refusal.
struct ListTransactionsResult {
    std::vector<TransactionEntryInfo> entries;
};

}  // namespace ledger
