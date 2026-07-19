// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QVariantList>

#include <cstdint>

#include "BankController.hpp"

#ifndef Q_MOC_RUN
#include <morph/core/bridge.hpp>

#include "bank/models/account_model.hpp"
#include "bank/models/payee_model.hpp"
#include "bank/models/payment_model.hpp"
#endif

namespace bankgui {

class BankClient;

/// @brief Payees + bill payment, exposed to QML as `payees`.
class PayeeController : public BankController {
    Q_OBJECT
    Q_PROPERTY(QVariantList payees READ payees NOTIFY payeesChanged)
    Q_PROPERTY(QVariantList accounts READ accounts NOTIFY accountsChanged)

public:
    explicit PayeeController(BankClient& client, QObject* parent = nullptr);

    [[nodiscard]] QVariantList payees() const { return _payees; }
    [[nodiscard]] QVariantList accounts() const { return _accounts; }

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void addPayee(const QString& name, const QString& iban, const QString& bank);
    Q_INVOKABLE void removePayee(qlonglong id);
    Q_INVOKABLE void payBill(qlonglong accountId, qlonglong payeeId, const QString& amount);

signals:
    void payeesChanged();
    void accountsChanged();
    void paid();

private:
    void reloadPayees();
    void reloadAccounts();

    morph::bridge::BridgeHandler<bank::PayeeModel> _payeeModel;
    morph::bridge::BridgeHandler<bank::AccountModel> _accountModel;
    morph::bridge::BridgeHandler<bank::PaymentModel> _paymentModel;
    QVariantList _payees;
    QVariantList _accounts;
};

}  // namespace bankgui
