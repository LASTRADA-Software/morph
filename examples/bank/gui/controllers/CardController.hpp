// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QVariantList>

#include <cstdint>

#include "BankController.hpp"

#ifndef Q_MOC_RUN
#include <morph/core/bridge.hpp>

#include "bank/models/account_model.hpp"
#include "bank/models/card_model.hpp"
#endif

namespace bankgui {

class BankClient;

/// @brief Card list + issue/freeze/unfreeze/cancel, exposed to QML as `cards`.
class CardController : public BankController {
    Q_OBJECT
    Q_PROPERTY(QVariantList cards READ cards NOTIFY cardsChanged)
    Q_PROPERTY(QVariantList accounts READ accounts NOTIFY accountsChanged)

public:
    explicit CardController(BankClient& client, QObject* parent = nullptr);

    [[nodiscard]] QVariantList cards() const { return _cards; }
    [[nodiscard]] QVariantList accounts() const { return _accounts; }

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void issue(qlonglong accountId, int kind, const QString& limit);
    Q_INVOKABLE void freeze(qlonglong id);
    Q_INVOKABLE void unfreeze(qlonglong id);
    Q_INVOKABLE void cancel(qlonglong id);

signals:
    void cardsChanged();
    void accountsChanged();

private:
    void reloadCards();
    void reloadAccounts();

    morph::bridge::BridgeHandler<bank::AccountModel> _accountModel;
    morph::bridge::BridgeHandler<bank::CardModel> _cardModel;
    QVariantList _cards;
    QVariantList _accounts;
};

}  // namespace bankgui
