// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QString>
#include <cmath>
#include <cstdint>
#include <optional>

#include "bank/core/money.hpp"
#include "bank/core/types.hpp"

/// @file
/// Display/format helpers shared by the QML controllers. Controllers hand QML
/// display-ready strings (money already formatted), so the QML layer never does
/// currency math.

namespace bankgui::fmt {

inline QString money(std::int64_t minor, int currency) {
    return QString::fromStdString(
        bank::format(bank::Money{.minor = minor, .currency = static_cast<bank::Currency>(currency)}));
}

inline QString accountKind(int kind) {
    switch (static_cast<bank::AccountKind>(kind)) {
        case bank::AccountKind::Checking:
            return QStringLiteral("Checking");
        case bank::AccountKind::Savings:
            return QStringLiteral("Savings");
        case bank::AccountKind::Credit:
            return QStringLiteral("Credit");
    }
    return QStringLiteral("Account");
}

inline QString txnKind(int kind) {
    switch (static_cast<bank::TxnKind>(kind)) {
        case bank::TxnKind::Deposit:
            return QStringLiteral("Deposit");
        case bank::TxnKind::Withdrawal:
            return QStringLiteral("Withdrawal");
        case bank::TxnKind::TransferIn:
            return QStringLiteral("Transfer in");
        case bank::TxnKind::TransferOut:
            return QStringLiteral("Transfer out");
        case bank::TxnKind::Payment:
            return QStringLiteral("Payment");
        case bank::TxnKind::Fee:
            return QStringLiteral("Fee");
        case bank::TxnKind::Interest:
            return QStringLiteral("Interest");
        case bank::TxnKind::LoanDisbursement:
            return QStringLiteral("Loan in");
        case bank::TxnKind::LoanRepayment:
            return QStringLiteral("Loan repay");
        case bank::TxnKind::CardPurchase:
            return QStringLiteral("Card");
        case bank::TxnKind::Exchange:
            return QStringLiteral("Exchange");
    }
    return QStringLiteral("Entry");
}

inline QString last4(const std::string& number) {
    return QStringLiteral("•••• ") + QString::fromStdString(number).right(4);
}

/// @brief The first `double` value that no longer fits in a `std::int64_t`, i.e. 2^63.
///
/// `std::numeric_limits<std::int64_t>::max()` is 2^63-1, which is *not*
/// representable as a `double` -- converting it rounds **up**, to 2^63. So a
/// bound written as `static_cast<double>(max())` is off by one in the unsafe
/// direction, and one written as `max() / scale` is off by the rounding of a
/// division on top of that. 2^63 is exactly representable, so this literal is
/// the one form of the bound that is exact.
inline constexpr double kMinorUnitsBound = 0x1p63;

/// @brief Parses a user-entered major-unit amount into minor units (assumes @p decimals).
///
/// Returns `std::nullopt` for anything that is not a non-negative amount that
/// fits in `std::int64_t` minor units -- unparseable text, a negative value,
/// `inf`/`nan` (both of which `QString::toDouble` accepts), and any magnitude
/// whose scaled value would not fit. Every caller already treats `nullopt` as
/// "reject this input", so the out-of-range cases join the ones that were
/// already rejected rather than needing new handling.
///
/// The range check is what makes the rounding below defined: `std::llround`
/// on a value whose result is outside `long long` raises a domain error and
/// returns an unspecified value, exactly as converting one directly was UB
/// ([conv.fpint]) before it, and `QString::toDouble` happily accepts `1e30`
/// from a QML text field with no validator. The check is on the
/// *scaled* value rather than on @p text's value, because only the scaled
/// value is what gets rounded -- `double` arithmetic itself cannot trap here,
/// so computing it first costs nothing and removes the need to reason about
/// how dividing the bound by @p scale rounds.
///
/// Rounding is to nearest, halves away from zero. It is `std::llround` rather
/// than a `+ 0.5` and a truncation, which is not the same function: the two
/// disagree on the double immediately below one half.
///
/// @param text     the user-entered amount, in major units
/// @param decimals the number of minor-unit digits of the target currency
/// @return the amount in minor units, or `std::nullopt` if @p text is not a
///         representable non-negative amount
inline std::optional<std::int64_t> parseMinor(const QString& text, int decimals = 2) {
    bool ok = false;
    const double major = text.trimmed().toDouble(&ok);
    if (!ok || major < 0.0) {
        return std::nullopt;
    }
    // Reuse the core scale primitive so parse and format share one source.
    const auto scale = static_cast<double>(bank::pow10i(decimals));
    const double scaled = major * scale;
    // Negated rather than written as `scaled >= kMinorUnitsBound`, so that a
    // NaN -- which compares false against everything, and which reaches here
    // because `nan < 0.0` is false -- is rejected rather than let through.
    //
    // The guard is still what makes the line below defined, and it still runs
    // first. It bounds the *unrounded* value, which is the
    // stronger of the two: every `double` strictly below 2^63 is at most
    // 2^63-1024, so its rounding is inside `std::int64_t` with room to spare,
    // and the bound stays the one form that is exact.
    if (!(scaled < kMinorUnitsBound)) {
        return std::nullopt;
    }
    // `std::llround`, not `(major * scale) + 0.5` truncated: the two disagree
    // on the double immediately below one half. 0.49999999999999994 + 0.5 is
    // exactly 1.0 in IEEE-754 -- the sum is not representable and rounds up --
    // so truncating it charged a whole minor unit for an amount below half of
    // one. `llround` rounds to nearest with halves away from zero,
    // which is what the `+ 0.5` was reaching for.
    return static_cast<std::int64_t>(std::llround(scaled));
}

}  // namespace bankgui::fmt
