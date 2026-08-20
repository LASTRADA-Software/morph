// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ledger/core/import_op_id.hpp"
#include "ledger/core/types.hpp"
#include "ledger/core/units.hpp"

#include <morph/util/datetime.hpp>
#include <morph/util/rational.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace ledger {

/// @brief One leg of a `StoreTransaction` (Task 8) or the multi-client
///        stress harness (Task 23). Declared ahead of `StoreTransaction`
///        itself, per this task's own scope.
struct TransactionLeg {
    AccountId accountId;
    morph::math::Rational amount;  // real currency comes from the account this leg names, per design spec §2
    std::optional<morph::math::Rational> foreignAmount;    // display/audit metadata only --
    std::optional<Currency> foreignCurrency;                // never enters a zero-sum check (design spec §1 step 3)
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

}  // namespace ledger
