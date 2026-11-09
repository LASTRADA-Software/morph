// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <glaze/glaze.hpp>

#include "lims/org/core/strong_id.hpp"

/// @file
/// The ledger's identities and closed sets.

namespace lims {

LIMS_DEFINE_STRONG_ID(AccountId);
LIMS_DEFINE_STRONG_ID(PeriodId);
LIMS_DEFINE_STRONG_ID(ExchangeRateId);

/// @brief What an account records.
enum class AccountType : std::uint8_t {
    Asset,      ///< What the company owns or is owed.
    Liability,  ///< What it owes.
    Equity,     ///< What its owners hold.
    Revenue,    ///< What it earns.
    Expense,    ///< What it spends.
};

/// @brief Where a month of the books stands.
enum class PeriodState : std::uint8_t {
    Open,     ///< Entries are drafted and posted.
    Closing,  ///< No new drafts; postings and the drafts already there may still be posted.
    Closed,   ///< Frozen: nothing more is recorded in it.
};

/// @brief Where a journal entry stands.
enum class EntryState : std::uint8_t {
    Draft,   ///< Made and not yet in the balances; it is only ever posted.
    Posted,  ///< In the balances and immutable; a correction is a reversal.
};

}  // namespace lims

LIMS_DEFINE_STRONG_ID_WIRE(AccountId);
LIMS_DEFINE_STRONG_ID_WIRE(PeriodId);
LIMS_DEFINE_STRONG_ID_WIRE(ExchangeRateId);
LIMS_DEFINE_STRONG_ID_SHAPE_TAG(AccountId, "lims.accountId");
LIMS_DEFINE_STRONG_ID_SHAPE_TAG(PeriodId, "lims.periodId");
LIMS_DEFINE_STRONG_ID_SHAPE_TAG(ExchangeRateId, "lims.exchangeRateId");

/// @brief Reflects `AccountType` as its enumerator name.
template <>
struct glz::meta<lims::AccountType> {
    using enum lims::AccountType;

    /// @brief The enumerator ⇄ name mapping glaze reads and writes.
    static constexpr auto value = glz::enumerate(Asset, Liability, Equity, Revenue, Expense);
};

/// @brief Reflects `PeriodState` as its enumerator name.
template <>
struct glz::meta<lims::PeriodState> {
    using enum lims::PeriodState;

    /// @brief The enumerator ⇄ name mapping glaze reads and writes.
    static constexpr auto value = glz::enumerate(Open, Closing, Closed);
};

/// @brief Reflects `EntryState` as its enumerator name.
template <>
struct glz::meta<lims::EntryState> {
    using enum lims::EntryState;

    /// @brief The enumerator ⇄ name mapping glaze reads and writes.
    static constexpr auto value = glz::enumerate(Draft, Posted);
};
