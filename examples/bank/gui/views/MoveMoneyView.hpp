// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/bridge.hpp>

#include <vector>

#include "../BankClient.hpp"
#include "../Page.hpp"
#include "bank/dto/account_dto.hpp"
#include "bank/dto/transaction_dto.hpp"
#include "bank/models/account_model.hpp"
#include "bank/models/transaction_model.hpp"

class QComboBox;
class QLineEdit;
class QLabel;
class QTableWidget;

namespace bankgui {

/// @brief Deposit / withdraw / transfer plus the selected account's history.
class MoveMoneyView : public Page {
public:
    explicit MoveMoneyView(BankClient& client, QWidget* parent = nullptr);
    void refresh() override;

private:
    [[nodiscard]] std::int64_t selectedAccountId() const;
    [[nodiscard]] int selectedCurrency() const;
    void reloadHistory();
    void setStatus(const QString& message, bool error);

    BankClient& _client;
    morph::bridge::BridgeHandler<bank::AccountModel> _accounts;
    morph::bridge::BridgeHandler<bank::TransactionModel> _txns;
    std::vector<bank::dto::AccountInfo> _cache;
    QComboBox* _account{};
    QComboBox* _target{};
    QLineEdit* _amount{};
    QLineEdit* _transferAmount{};
    QLabel* _status{};
    QTableWidget* _history{};
};

}  // namespace bankgui
