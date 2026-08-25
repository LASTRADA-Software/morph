// SPDX-License-Identifier: Apache-2.0

#include "PayeeController.hpp"

#include <QVariantMap>

#include "../BankClient.hpp"
#include "Format.hpp"
#include "bank/core/types.hpp"
#include "bank/dto/account_dto.hpp"
#include "bank/dto/payment_dto.hpp"

namespace bankgui {

PayeeController::PayeeController(BankClient& client, QObject* parent)
    : BankController(client, parent),
      _payeeModel{client.bridge(), client.gui()},
      _accountModel{client.bridge(), client.gui()},
      _paymentModel{client.bridge(), client.gui()} {}

void PayeeController::refresh() {
    reloadAccounts();
    reloadPayees();
}

void PayeeController::reloadAccounts() {
    _accountModel.execute(bank::dto::ListAccounts{})
        .then([this](bank::dto::AccountList list) {
            _accounts.clear();
            for (const auto& account : list.accounts) {
                if (account.status == static_cast<int>(bank::AccountStatus::Closed)) {
                    continue;
                }
                QVariantMap map;
                map[QStringLiteral("id")] = static_cast<qlonglong>(account.id);
                map[QStringLiteral("label")] = fmt::last4(account.number);
                _accounts.append(map);
            }
            emit accountsChanged();
        })
        .onError([this](const std::exception_ptr& err) { emit error(errorText(err)); });
}

void PayeeController::reloadPayees() {
    _payeeModel.execute(bank::dto::ListPayees{})
        .then([this](bank::dto::PayeeList list) {
            _payees.clear();
            for (const auto& payee : list.payees) {
                QVariantMap map;
                map[QStringLiteral("id")] = static_cast<qlonglong>(payee.id);
                map[QStringLiteral("name")] = QString::fromStdString(payee.name);
                map[QStringLiteral("iban")] = QString::fromStdString(payee.iban);
                _payees.append(map);
            }
            emit payeesChanged();
        })
        .onError([this](const std::exception_ptr& err) { emit error(errorText(err)); });
}

void PayeeController::addPayee(const QString& name, const QString& iban, const QString& bank) {
    _payeeModel
        .execute(bank::dto::AddPayee{
            .name = name.toStdString(), .iban = iban.trimmed().toStdString(), .bankName = bank.toStdString()})
        .then([this](bank::dto::PayeeInfo) { reloadPayees(); })
        .onError([this](const std::exception_ptr& err) { emit error(errorText(err)); });
}

void PayeeController::removePayee(qlonglong id) {
    _payeeModel.execute(bank::dto::RemovePayee{.id = id})
        .then([this](bank::dto::CommandResult) { reloadPayees(); })
        .onError([this](const std::exception_ptr& err) { emit error(errorText(err)); });
}

void PayeeController::payBill(qlonglong accountId, qlonglong payeeId, const QString& amount) {
    const auto minor = fmt::parseMinor(amount);
    if (!minor || accountId == 0 || payeeId == 0) {
        emit error(QStringLiteral("Pick an account, payee, and amount."));
        return;
    }
    _paymentModel.execute(bank::dto::PayBill{.fromAccountId = accountId, .payeeId = payeeId, .amountMinor = *minor})
        .then([this](bank::dto::PaymentInfo) { emit paid(); })
        .onError([this](const std::exception_ptr& err) { emit error(errorText(err)); });
}

}  // namespace bankgui
