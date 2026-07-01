// SPDX-License-Identifier: Apache-2.0

#include "LoanController.hpp"

#include <QVariantMap>

#include "../BankClient.hpp"
#include "Format.hpp"
#include "bank/core/types.hpp"
#include "bank/dto/account_dto.hpp"

namespace bankgui {

LoanController::LoanController(BankClient& client, QObject* parent)
    : BankController(client, parent),
      _loanModel{client.bridge(), client.gui()},
      _accountModel{client.bridge(), client.gui()} {}

void LoanController::refresh() {
    reloadAccounts();
    reloadLoans();
}

void LoanController::reloadAccounts() {
    _accountModel.execute(bank::dto::ListAccounts{})
        .then([this](bank::dto::AccountList list) {
            _accounts.clear();
            for (const auto& account : list.accounts) {
                if (account.status == static_cast<int>(bank::AccountStatus::Closed)) {
                    continue;
                }
                QVariantMap map;
                map[QStringLiteral("id")] = static_cast<qlonglong>(account.id);
                map[QStringLiteral("label")] = fmt::last4(account.number);
                _accounts.append(map);
            }
            emit accountsChanged();
        })
        .onError([this](const std::exception_ptr& err) { emit error(errorText(err)); });
}

void LoanController::reloadLoans() {
    _loanModel.execute(bank::dto::ListLoans{})
        .then([this](bank::dto::LoanList list) {
            _loans.clear();
            for (const auto& loan : list.loans) {
                const auto status = static_cast<bank::LoanStatus>(loan.status);
                const bool paid = status == bank::LoanStatus::PaidOff;
                QVariantMap map;
                map[QStringLiteral("id")] = static_cast<qlonglong>(loan.id);
                map[QStringLiteral("accountId")] = static_cast<qlonglong>(loan.accountId);
                map[QStringLiteral("title")] = QStringLiteral("Loan #%1").arg(loan.id);
                map[QStringLiteral("detail")] =
                    QStringLiteral("Outstanding %1  ·  %2 bps  ·  %3 mo")
                        .arg(fmt::money(loan.outstandingMinor, loan.currency))
                        .arg(loan.rateBps)
                        .arg(loan.termMonths);
                map[QStringLiteral("outstanding")] = static_cast<qlonglong>(loan.outstandingMinor);
                map[QStringLiteral("statusText")] = paid ? QStringLiteral("Paid off")
                                                         : QStringLiteral("Active");
                map[QStringLiteral("statusKind")] = paid ? QStringLiteral("good")
                                                         : QStringLiteral("neutral");
                map[QStringLiteral("active")] = status == bank::LoanStatus::Active;
                _loans.append(map);
            }
            emit loansChanged();
        })
        .onError([this](const std::exception_ptr& err) { emit error(errorText(err)); });
}

void LoanController::apply(qlonglong accountId, const QString& principal, int rateBps, int termMonths) {
    const auto minor = fmt::parseMinor(principal);
    // rateBps < 0 means the field was left blank (see LoansPage.qml); a rate of 0
    // is a valid interest-free loan, which the model accepts.
    if (!minor || accountId == 0 || rateBps < 0 || termMonths <= 0) {
        emit error(QStringLiteral("Enter account, principal, rate (bps), and term."));
        return;
    }
    _loanModel
        .execute(bank::dto::ApplyLoan{.accountId = accountId, .principalMinor = *minor,
                                      .rateBps = rateBps, .termMonths = termMonths})
        .then([this](bank::dto::LoanInfo) { reloadLoans(); })
        .onError([this](const std::exception_ptr& err) { emit error(errorText(err)); });
}

void LoanController::repay(qlonglong loanId, qlonglong accountId, const QString& amount) {
    const auto minor = fmt::parseMinor(amount);
    if (!minor) {
        emit error(QStringLiteral("Enter a valid amount."));
        return;
    }
    _loanModel
        .execute(bank::dto::RepayLoan{.loanId = loanId, .fromAccountId = accountId, .amountMinor = *minor})
        .then([this](bank::dto::LoanInfo) { reloadLoans(); })
        .onError([this](const std::exception_ptr& err) { emit error(errorText(err)); });
}

void LoanController::showSchedule(qlonglong loanId) {
    _loanModel.execute(bank::dto::LoanScheduleRequest{.loanId = loanId})
        .then([this](bank::dto::LoanScheduleResult result) {
            _schedule.clear();
            for (const auto& inst : result.installments) {
                QVariantMap map;
                map[QStringLiteral("month")] = inst.month;
                map[QStringLiteral("principalText")] = fmt::money(inst.principalMinor, 0);
                map[QStringLiteral("interestText")] = fmt::money(inst.interestMinor, 0);
                map[QStringLiteral("remainingText")] = fmt::money(inst.remainingMinor, 0);
                _schedule.append(map);
            }
            emit scheduleChanged();
        })
        .onError([this](const std::exception_ptr& err) { emit error(errorText(err)); });
}

}  // namespace bankgui
