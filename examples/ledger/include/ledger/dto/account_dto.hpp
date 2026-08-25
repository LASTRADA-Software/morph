// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/forms/forms.hpp>
#include <morph/util/rational.hpp>
#include <string>
#include <vector>

#include "ledger/core/types.hpp"
#include "ledger/core/units.hpp"

namespace ledger {

struct OpenAccount {
    LedgerId ledgerId;
    std::string name;
    AccountKind kind;
    Currency currency;

    [[nodiscard]] bool validate() const noexcept { return ledgerId.hasValue() && !name.empty(); }
};

struct GetLedger {
    LedgerId ledgerId;

    [[nodiscard]] bool validate() const noexcept { return morph::forms::allRequiredEngaged(*this); }
};

struct AccountInfo {
    AccountId id;
    std::string name;
    AccountKind kind;
    Currency currency;
    morph::math::Rational balance;  // plain Rational -- real currency is the sibling `currency` field above,
                                    // never a Quantity's compile-time unit parameter (design spec §2)
};

struct GetLedgerResult {
    std::vector<AccountInfo> accounts;
};

}  // namespace ledger
