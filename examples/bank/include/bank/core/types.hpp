// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string_view>

/// @file
/// Core enumerations shared by the wire DTOs and the database entities.
///
/// These enums are stored in the database as their integer value and travel
/// over the morph wire as integers too, so the numeric values are part of the
/// (example) protocol — append, never renumber.

namespace bank {

/// @brief ISO-4217-ish currency tags supported by the demo bank.
enum class Currency : std::uint8_t {
    USD = 0,
    EUR = 1,
    GBP = 2,
    CHF = 3,
    JPY = 4,
};

/// @brief Number of fractional digits a currency uses (minor units per major unit exponent).
/// JPY has no minor unit; everything else here uses 2 decimal places.
[[nodiscard]] constexpr int currencyDecimals(Currency c) noexcept { return c == Currency::JPY ? 0 : 2; }

/// @brief Integer 10^exponent. Single source of truth for the parse/format
/// scale, so the hand-rolled `10^decimals` loops don't drift apart across the
/// core and the GUI.
[[nodiscard]] constexpr std::int64_t pow10i(int exponent) noexcept {
    std::int64_t value = 1;
    for (int idx = 0; idx < exponent; ++idx) {
        value *= 10;
    }
    return value;
}

/// @brief Number of minor units in one major unit of @p c, i.e. 10^decimals.
[[nodiscard]] constexpr std::int64_t currencyScale(Currency c) noexcept { return pow10i(currencyDecimals(c)); }

/// @brief Three-letter code for a currency (for display / statements).
[[nodiscard]] constexpr std::string_view currencyCode(Currency c) noexcept {
    switch (c) {
        case Currency::USD:
            return "USD";
        case Currency::EUR:
            return "EUR";
        case Currency::GBP:
            return "GBP";
        case Currency::CHF:
            return "CHF";
        case Currency::JPY:
            return "JPY";
    }
    return "???";
}

/// @brief Kind of account a customer can hold.
enum class AccountKind : std::uint8_t {
    Checking = 0,
    Savings = 1,
    Credit = 2,
};

/// @brief Lifecycle state of an account.
enum class AccountStatus : std::uint8_t {
    Open = 0,
    Frozen = 1,
    Closed = 2,
};

/// @brief Whether a transaction moved money in or out of an account.
enum class TxnDirection : std::uint8_t {
    Credit = 0,  ///< money in
    Debit = 1,   ///< money out
};

/// @brief How a payment recurs.
enum class PaymentSchedule : std::uint8_t {
    OneOff = 0,     ///< paid immediately, once
    Scheduled = 1,  ///< due once at a future time
    Standing = 2,   ///< recurring every N days
};

/// @brief Lifecycle of a payment instruction.
enum class PaymentStatus : std::uint8_t {
    Pending = 0,
    Completed = 1,
    Cancelled = 2,
    Failed = 3,
};

/// @brief Lifecycle/state of a payment card.
enum class CardStatus : std::uint8_t {
    Active = 0,
    Frozen = 1,
    Cancelled = 2,
};

/// @brief Kind of payment card.
enum class CardKind : std::uint8_t {
    Debit = 0,
    Credit = 1,
};

/// @brief Lifecycle of a loan.
enum class LoanStatus : std::uint8_t {
    Active = 0,
    PaidOff = 1,
    Defaulted = 2,
};

/// @brief Category of a ledger entry (drives history rendering and analytics).
enum class TxnKind : std::uint8_t {
    Deposit = 0,
    Withdrawal = 1,
    TransferIn = 2,
    TransferOut = 3,
    Payment = 4,
    Fee = 5,
    Interest = 6,
    LoanDisbursement = 7,
    LoanRepayment = 8,
    CardPurchase = 9,
    Exchange = 10,
};

}  // namespace bank
