// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/bridge.hpp>

#include <vector>

#include "../BankClient.hpp"
#include "../Page.hpp"
#include "bank/dto/card_dto.hpp"
#include "bank/models/account_model.hpp"
#include "bank/models/card_model.hpp"

class QComboBox;
class QLineEdit;
class QVBoxLayout;

namespace bankgui {

/// @brief Issue cards against an account and freeze / unfreeze / cancel them.
class CardsView : public Page {
public:
    explicit CardsView(BankClient& client, QWidget* parent = nullptr);
    void refresh() override;

private:
    void issueCard();
    void rebuild(const std::vector<bank::dto::CardInfo>& cards);

    BankClient& _client;
    morph::bridge::BridgeHandler<bank::AccountModel> _accounts;
    morph::bridge::BridgeHandler<bank::CardModel> _cards;
    QComboBox* _account{};
    QComboBox* _kind{};
    QLineEdit* _limit{};
    QVBoxLayout* _list{};
};

}  // namespace bankgui
