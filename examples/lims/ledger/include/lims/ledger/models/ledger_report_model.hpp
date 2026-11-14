// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/core/bridge.hpp>
#include <morph/core/effects.hpp>
#include <morph/core/registry.hpp>

#include "lims/ledger/dto/report_dto.hpp"

/// @file
/// `LedgerReportModel`: the trial balance, an account's balance and an account's ledger.

namespace lims {

/// @brief The ledger's reports, over posted entries only and in company currency.
///
/// Unkeyed, holding no connection: each `execute()` opens a `Lightweight::DataMapper` for its own duration. Every
/// total is an `ExactSum` (a total that does not fit is refused, never clamped). A closed period that lies wholly
/// inside a report's range contributes its frozen per-account totals instead of its lines.
class LedgerReportModel {
public:
    /// @brief Every account's debit and credit totals up to an instant, and the company's totals.
    /// @param action The company and the last instant counted.
    /// @return The accounts with any posted line, by code, and the totals; empty for a company the caller cannot see.
    /// @throws ValidationError if the query is malformed, or a row holds a value outside its enumeration.
    /// @throws OutOfRange if a total does not fit.
    TrialBalanceView execute(const TrialBalance& action);

    /// @brief One account's debit and credit totals up to an instant, and its balance (debit less credit).
    /// @param action The company, the account's code and the last instant counted.
    /// @return The totals; unset amounts for a company the caller cannot see.
    /// @throws ValidationError if the query is malformed, or a row holds a value outside its enumeration.
    /// @throws NotFound if the company has no such account.
    /// @throws OutOfRange if a total or the balance does not fit.
    AccountBalanceView execute(const AccountBalance& action);

    /// @brief One account's posted lines from one instant to another, each with the running balance.
    /// @param action The company, the account's code and the range, both ends included.
    /// @return The lines by date then entry; none for a company the caller cannot see. The first line's balance
    ///         starts from everything posted before the range.
    /// @throws ValidationError if the query is malformed, the range ends before it starts, or a row holds a value
    ///         outside its enumeration.
    /// @throws NotFound if the company has no such account.
    /// @throws OutOfRange if a total or a running balance does not fit.
    AccountLedgerView execute(const AccountLedger& action);
};

}  // namespace lims

// The registration macros declare a non-const status object each.
// NOLINTBEGIN(misc-const-correctness)
BRIDGE_REGISTER_MODEL(lims::LedgerReportModel, "LedgerReportModel")
BRIDGE_REGISTER_ACTION(lims::LedgerReportModel, lims::TrialBalance, "TrialBalance", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::LedgerReportModel, lims::AccountBalance, "AccountBalance", ::morph::model::Loggable::No)
BRIDGE_REGISTER_ACTION(lims::LedgerReportModel, lims::AccountLedger, "AccountLedger", ::morph::model::Loggable::No)
// NOLINTEND(misc-const-correctness)
