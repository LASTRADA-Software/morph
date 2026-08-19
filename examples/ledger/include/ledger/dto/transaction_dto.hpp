// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ledger/core/types.hpp"

#include <morph/util/rational.hpp>

#include <vector>

namespace ledger {

/// @brief One leg of a `StoreTransaction` (Task 8) or the multi-client
///        stress harness (Task 23). Declared ahead of `StoreTransaction`
///        itself, per this task's own scope.
struct TransactionLeg {
    AccountId accountId;
    morph::math::Rational amount;  // real currency comes from the account this leg names, per design spec §2
};

}  // namespace ledger
