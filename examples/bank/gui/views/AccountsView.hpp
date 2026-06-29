// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/bridge.hpp>

#include <vector>

#include "../BankClient.hpp"
#include "../Page.hpp"
#include "bank/dto/account_dto.hpp"
#include "bank/models/account_model.hpp"

class QComboBox;
class QLineEdit;
class QLabel;
class QGridLayout;

namespace bankgui {

/// @brief Dashboard: a summary stat, an inline "open account" form, and a grid
///        of account cards.
class AccountsView : public Page {
public:
    explicit AccountsView(BankClient& client, QWidget* parent = nullptr);
    void refresh() override;

private:
    void openAccount();
    void rebuild(const std::vector<bank::dto::AccountInfo>& accounts);

    BankClient& _client;
    morph::bridge::BridgeHandler<bank::AccountModel> _accounts;
    QComboBox* _kind{};
    QComboBox* _currency{};
    QLineEdit* _overdraft{};
    QLabel* _statValue{};
    QLabel* _statLabel{};
    QGridLayout* _grid{};
};

}  // namespace bankgui
