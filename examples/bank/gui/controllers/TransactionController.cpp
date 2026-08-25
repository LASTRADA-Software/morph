// SPDX-License-Identifier: Apache-2.0

#include "TransactionController.hpp"

#include <QVariantMap>

#include "../BankClient.hpp"
#include "Format.hpp"
#include "bank/core/types.hpp"
#include "bank/dto/account_dto.hpp"
#include "bank/dto/transaction_dto.hpp"

namespace bankgui {

TransactionController::TransactionController(BankClient& client, QObject* parent)
    : BankController(client, parent),
      _accountModel{client.bridge(), client.gui()},
      _txnModel{client.bridge(), client.gui()} {}

void TransactionController::refresh() {
    _accountModel.execute(bank::dto::ListAccounts{})
        .then([this](bank::dto::AccountList list) {
            _accounts.clear();
            bool stillPresent = false;
            for (const auto& account : list.accounts) {
                if (account.status == static_cast<int>(bank::AccountStatus::Closed)) {
                    continue;
                }
                QVariantMap map;
                map[QStringLiteral("id")] = static_cast<qlonglong>(account.id);
                map[QStringLiteral("label")] = fmt::last4(account.number) + QStringLiteral("  ·  ") +
                                               fmt::money(account.balanceMinor, account.currency);
                map[QStringLiteral("currency")] = account.currency;
                _accounts.append(map);
                if (account.id == _selected) {
                    stillPresent = true;
                    _selectedCurrency = account.currency;
                }
            }
            if (!stillPresent && !_accounts.isEmpty()) {
                _selected = _accounts.front().toMap().value(QStringLiteral("id")).toLongLong();
                _selectedCurrency = _accounts.front().toMap().value(QStringLiteral("currency")).toInt();
            }
            emit accountsChanged();
            emit selectedChanged();
            reloadHistory();
        })
        .onError([this](const std::exception_ptr& err) { emit error(errorText(err)); });
}

void TransactionController::selectAccount(qlonglong id) {
    if (_selected == id) {
        return;
    }
    _selected = id;
    for (const auto& entry : std::as_const(_accounts)) {
        const auto map = entry.toMap();
        if (map.value(QStringLiteral("id")).toLongLong() == id) {
            _selectedCurrency = map.value(QStringLiteral("currency")).toInt();
        }
    }
    emit selectedChanged();
    reloadHistory();
}

int TransactionController::selectedCurrency() const { return _selectedCurrency; }

void TransactionController::reloadHistory() {
    _history.clear();
    if (_selected == 0) {
        emit historyChanged();
        return;
    }
    _txnModel.execute(bank::dto::History{.accountId = _selected, .limit = 50})
        .then([this](bank::dto::HistoryPage page) {
            _history.clear();
            for (const auto& entry : page.entries) {
                const bool credit = entry.direction == static_cast<int>(bank::TxnDirection::Credit);
                QVariantMap map;
                map[QStringLiteral("kind")] = fmt::txnKind(entry.kind);
                map[QStringLiteral("amountText")] = (credit ? QStringLiteral("+") : QStringLiteral("−")) +
                                                    fmt::money(entry.amountMinor, entry.currency);
                map[QStringLiteral("isCredit")] = credit;
                map[QStringLiteral("balanceText")] = fmt::money(entry.balanceAfterMinor, entry.currency);
                _history.append(map);
            }
            emit historyChanged();
        })
        .onError([this](const std::exception_ptr& err) { emit error(errorText(err)); });
}

void TransactionController::deposit(const QString& amount) {
    const auto minor = fmt::parseMinor(amount, bank::currencyDecimals(static_cast<bank::Currency>(_selectedCurrency)));
    if (!minor || _selected == 0) {
        emit error(QStringLiteral("Enter a valid amount."));
        return;
    }
    _txnModel.execute(bank::dto::Deposit{.accountId = _selected, .amountMinor = *minor})
        .then([this](bank::dto::TxnInfo) {
            emit posted();
            refresh();
        })
        .onError([this](const std::exception_ptr& err) { emit error(errorText(err)); });
}

void TransactionController::withdraw(const QString& amount) {
    const auto minor = fmt::parseMinor(amount, bank::currencyDecimals(static_cast<bank::Currency>(_selectedCurrency)));
    if (!minor || _selected == 0) {
        emit error(QStringLiteral("Enter a valid amount."));
        return;
    }
    _txnModel.execute(bank::dto::Withdraw{.accountId = _selected, .amountMinor = *minor})
        .then([this](bank::dto::TxnInfo) {
            emit posted();
            refresh();
        })
        .onError([this](const std::exception_ptr& err) { emit error(errorText(err)); });
}

void TransactionController::transfer(qlonglong toId, const QString& amount) {
    const auto minor = fmt::parseMinor(amount, bank::currencyDecimals(static_cast<bank::Currency>(_selectedCurrency)));
    if (!minor || _selected == 0 || toId == 0) {
        emit error(QStringLiteral("Pick a target account and amount."));
        return;
    }
    _txnModel.execute(bank::dto::Transfer{.fromAccountId = _selected, .toAccountId = toId, .amountMinor = *minor})
        .then([this](bank::dto::TransferResult) {
            emit posted();
            refresh();
        })
        .onError([this](const std::exception_ptr& err) { emit error(errorText(err)); });
}

}  // namespace bankgui
