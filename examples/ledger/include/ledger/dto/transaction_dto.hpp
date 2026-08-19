// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ledger/core/types.hpp"

#include <morph/util/datetime.hpp>
#include <morph/util/rational.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace ledger {

/// @brief One leg of a `StoreTransaction` (Task 8) or the multi-client
///        stress harness (Task 23). Declared ahead of `StoreTransaction`
///        itself, per this task's own scope.
struct TransactionLeg {
    AccountId accountId;
    morph::math::Rational amount;  // real currency comes from the account this leg names, per design spec §2
};

/// @brief Records a multi-leg transaction against `ledgerId`'s accounts,
///        enforcing design spec §1's per-currency zero-sum invariant: every
///        leg's amount is partitioned by the account it names' own
///        currency, and each partition's amounts must sum to canonical
///        zero (`LedgerModel::execute` throws `ZeroSumViolation` otherwise).
struct StoreTransaction {
    LedgerId ledgerId;
    std::string description;
    morph::time::Timestamp date;
    std::vector<TransactionLeg> legs;

    [[nodiscard]] bool validate() const noexcept {
        return ledgerId.hasValue() && !description.empty() && legs.size() >= 2 &&
               std::ranges::all_of(legs, [](const auto& leg) { return leg.accountId.hasValue(); });
    }
};

}  // namespace ledger
