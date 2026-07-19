// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QVariantList>

#include <cstdint>

#include "BankController.hpp"

#ifndef Q_MOC_RUN
#include <morph/core/bridge.hpp>

#include "bank/models/account_model.hpp"
#include "bank/models/transaction_model.hpp"
#endif

namespace bankgui {

class BankClient;

/// @brief Deposit/withdraw/transfer + history for a selected account.
class TransactionController : public BankController {
    Q_OBJECT
    Q_PROPERTY(QVariantList accounts READ accounts NOTIFY accountsChanged)
    Q_PROPERTY(QVariantList history READ history NOTIFY historyChanged)
    Q_PROPERTY(qlonglong selectedAccount READ selectedAccount WRITE selectAccount NOTIFY selectedChanged)

public:
    explicit TransactionController(BankClient& client, QObject* parent = nullptr);

    [[nodiscard]] QVariantList accounts() const { return _accounts; }
    [[nodiscard]] QVariantList history() const { return _history; }
    [[nodiscard]] qlonglong selectedAccount() const { return _selected; }

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void selectAccount(qlonglong id);
    Q_INVOKABLE void deposit(const QString& amount);
    Q_INVOKABLE void withdraw(const QString& amount);
    Q_INVOKABLE void transfer(qlonglong toId, const QString& amount);

signals:
    void accountsChanged();
    void historyChanged();
    void selectedChanged();
    void posted();

private:
    void reloadHistory();
    [[nodiscard]] int selectedCurrency() const;

    morph::bridge::BridgeHandler<bank::AccountModel> _accountModel;
    morph::bridge::BridgeHandler<bank::TransactionModel> _txnModel;
    QVariantList _accounts;
    QVariantList _history;
    qlonglong _selected{0};
    int _selectedCurrency{0};
};

}  // namespace bankgui
