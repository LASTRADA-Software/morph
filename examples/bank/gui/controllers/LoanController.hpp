// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QVariantList>

#include <cstdint>

#include "BankController.hpp"

#ifndef Q_MOC_RUN
#include <morph/bridge.hpp>

#include "bank/models/account_model.hpp"
#include "bank/models/loan_model.hpp"
#endif

namespace bankgui {

class BankClient;

/// @brief Loans: apply, repay, and amortization schedule, exposed as `loans`.
class LoanController : public BankController {
    Q_OBJECT
    Q_PROPERTY(QVariantList loans READ loans NOTIFY loansChanged)
    Q_PROPERTY(QVariantList accounts READ accounts NOTIFY accountsChanged)
    Q_PROPERTY(QVariantList schedule READ schedule NOTIFY scheduleChanged)

public:
    explicit LoanController(BankClient& client, QObject* parent = nullptr);

    [[nodiscard]] QVariantList loans() const { return _loans; }
    [[nodiscard]] QVariantList accounts() const { return _accounts; }
    [[nodiscard]] QVariantList schedule() const { return _schedule; }

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void apply(qlonglong accountId, const QString& principal, int rateBps, int termMonths);
    Q_INVOKABLE void repay(qlonglong loanId, qlonglong accountId, const QString& amount);
    Q_INVOKABLE void showSchedule(qlonglong loanId);

signals:
    void loansChanged();
    void accountsChanged();
    void scheduleChanged();

private:
    void reloadLoans();
    void reloadAccounts();

    morph::bridge::BridgeHandler<bank::LoanModel> _loanModel;
    morph::bridge::BridgeHandler<bank::AccountModel> _accountModel;
    QVariantList _loans;
    QVariantList _accounts;
    QVariantList _schedule;
};

}  // namespace bankgui
