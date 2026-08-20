// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "ledger/core/import_op_id.hpp"
#include "ledger/core/types.hpp"

#include <cstdint>
#include <string>

namespace ledger {

/// @brief One chunk of a CSV/OFX statement upload (design spec §8):
///        `date,description,account_id,amount` rows, one header line
///        skipped. Every parsed row posts a two-leg entry against the
///        row's own `account_id` and this chunk's shared
///        `counterAccountId` (a CSV row alone names no offsetting
///        account -- a real statement import is always "for" one
///        account, with every transaction offsetting against some
///        counter/suspense account, per this task's own plan
///        self-review ruling).
struct ImportLedgerChunk {
    LedgerId ledgerId;
    AccountId counterAccountId;
    std::string csvChunk;
    ImportOpId opId;
};

/// @brief `imported` counts rows newly committed this call; `duplicates`
///        counts rows skipped by the content-hash check (design spec
///        §8's own "skip, don't throw" rule) -- NOT rows skipped by the
///        opId chunk-retry check, which returns the ORIGINAL call's
///        stored counts verbatim (see execute(ImportLedgerChunk)'s own
///        comment on why a replay hit short-circuits before either
///        counter is touched).
struct ImportResult {
    std::int64_t imported{0};
    std::int64_t duplicates{0};
};

}  // namespace ledger
