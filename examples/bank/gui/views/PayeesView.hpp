// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/bridge.hpp>

#include <vector>

#include "../BankClient.hpp"
#include "../Page.hpp"
#include "bank/dto/payee_dto.hpp"
#include "bank/models/account_model.hpp"
#include "bank/models/payee_model.hpp"
#include "bank/models/payment_model.hpp"

class QComboBox;
class QLineEdit;
class QLabel;
class QVBoxLayout;

namespace bankgui {

/// @brief Manage beneficiaries and pay bills.
class PayeesView : public Page {
public:
    explicit PayeesView(BankClient& client, QWidget* parent = nullptr);
    void refresh() override;

private:
    void addPayee();
    void payBill();
    void rebuild(const std::vector<bank::dto::PayeeInfo>& payees);
    void setStatus(const QString& message, bool error);

    BankClient& _client;
    morph::bridge::BridgeHandler<bank::PayeeModel> _payees;
    morph::bridge::BridgeHandler<bank::AccountModel> _accounts;
    morph::bridge::BridgeHandler<bank::PaymentModel> _payments;
    QLineEdit* _name{};
    QLineEdit* _iban{};
    QLineEdit* _bank{};
    QComboBox* _payAccount{};
    QComboBox* _payPayee{};
    QLineEdit* _payAmount{};
    QLabel* _status{};
    QVBoxLayout* _list{};
};

}  // namespace bankgui
