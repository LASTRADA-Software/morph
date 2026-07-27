// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QString>
#include <QVariantList>

#include "BankController.hpp"

#ifndef Q_MOC_RUN
#include <morph/core/bridge.hpp>

#include "bank/models/customer_model.hpp"
#endif

namespace bankgui {

class BankClient;

/// @brief Account list + open/close, exposed to QML as `accounts`.
class AccountController : public BankController {
    Q_OBJECT
    Q_PROPERTY(QVariantList accounts READ accounts NOTIFY accountsChanged)
    Q_PROPERTY(QString totalBalance READ totalBalance NOTIFY accountsChanged)
    Q_PROPERTY(int openCount READ openCount NOTIFY accountsChanged)

public:
    explicit AccountController(BankClient& client, QObject* parent = nullptr);

    [[nodiscard]] QVariantList accounts() const { return _accounts; }
    [[nodiscard]] QString totalBalance() const { return _totalBalance; }
    [[nodiscard]] int openCount() const { return _openCount; }

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void openAccount(int kind, int currency, const QString& overdraft);

signals:
    void accountsChanged();

private:
    morph::bridge::BridgeHandler<bank::CustomerModel> _model;
    QVariantList _accounts;
    QString _totalBalance{QStringLiteral("—")};
    int _openCount{0};
};

}  // namespace bankgui
