// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QString>
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

/// Parses a user-entered major-unit amount into minor units (assumes @p decimals).
inline std::optional<std::int64_t> parseMinor(const QString& text, int decimals = 2) {
    bool ok = false;
    const double major = text.trimmed().toDouble(&ok);
    if (!ok || major < 0.0) {
        return std::nullopt;
    }
    // Reuse the core scale primitive so parse and format share one source.
    const auto scale = static_cast<double>(bank::pow10i(decimals));
    return static_cast<std::int64_t>(major * scale + 0.5);
}

}  // namespace bankgui::fmt
