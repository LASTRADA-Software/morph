// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/bridge.hpp>

#include <vector>

#include "../BankClient.hpp"
#include "../Page.hpp"
#include "bank/dto/loan_dto.hpp"
#include "bank/models/account_model.hpp"
#include "bank/models/loan_model.hpp"

class QComboBox;
class QLineEdit;
class QLabel;
class QVBoxLayout;
class QTableWidget;

namespace bankgui {

/// @brief Apply for loans, view amortization schedules, and make repayments.
class LoansView : public Page {
public:
    explicit LoansView(BankClient& client, QWidget* parent = nullptr);
    void refresh() override;

private:
    void applyLoan();
    void rebuild(const std::vector<bank::dto::LoanInfo>& loans);
    void showSchedule(std::int64_t loanId);
    void setStatus(const QString& message, bool error);

    BankClient& _client;
    morph::bridge::BridgeHandler<bank::LoanModel> _loans;
    morph::bridge::BridgeHandler<bank::AccountModel> _accounts;
    QComboBox* _account{};
    QLineEdit* _principal{};
    QLineEdit* _rate{};
    QLineEdit* _term{};
    QLabel* _status{};
    QVBoxLayout* _list{};
    QTableWidget* _schedule{};
};

}  // namespace bankgui
