// SPDX-License-Identifier: Apache-2.0

#include "AccountController.hpp"

#include <QVariantMap>

#include "../BankClient.hpp"
#include "Format.hpp"
#include "bank/core/types.hpp"
#include "bank/dto/account_dto.hpp"

namespace bankgui {

namespace {

QVariantMap toMap(const bank::dto::AccountInfo& account) {
    const bool closed = account.status == static_cast<int>(bank::AccountStatus::Closed);
    QVariantMap map;
    map[QStringLiteral("id")] = static_cast<qlonglong>(account.id);
    map[QStringLiteral("kind")] = fmt::accountKind(account.kind);
    map[QStringLiteral("number")] = fmt::last4(account.number);
    map[QStringLiteral("balanceText")] = fmt::money(account.balanceMinor, account.currency);
    map[QStringLiteral("statusText")] = closed ? QStringLiteral("Closed") : QStringLiteral("Open");
    map[QStringLiteral("statusKind")] = closed ? QStringLiteral("neutral") : QStringLiteral("good");
    map[QStringLiteral("closed")] = closed;
    map[QStringLiteral("hasOverdraft")] = account.overdraftMinor > 0;
    map[QStringLiteral("overdraftText")] =
        QStringLiteral("Overdraft ") + fmt::money(account.overdraftMinor, account.currency);
    return map;
}

}  // namespace

AccountController::AccountController(BankClient& client, QObject* parent)
    : BankController(client, parent), _model{client.bridge(), client.gui()} {}

void AccountController::refresh() {
    _model.execute(bank::dto::ListAccounts{})
        .then([this](bank::dto::AccountList list) {
            _accounts.clear();
            std::int64_t total = 0;
            int currency = list.accounts.empty() ? 0 : list.accounts.front().currency;
            bool sameCurrency = true;
            _openCount = 0;
            for (const auto& account : list.accounts) {
                _accounts.append(toMap(account));
                if (account.status != static_cast<int>(bank::AccountStatus::Closed)) {
                    ++_openCount;
                    total += account.balanceMinor;
                    if (account.currency != currency) {
                        sameCurrency = false;
                    }
                }
            }
            _totalBalance =
                (sameCurrency && _openCount > 0) ? fmt::money(total, currency) : QString::number(_openCount);
            emit accountsChanged();
        })
        .onError([this](const std::exception_ptr& err) { emit error(errorText(err)); });
}

void AccountController::openAccount(int kind, int currency, const QString& overdraft) {
    const auto minor = overdraft.trimmed().isEmpty() ? 0 : fmt::parseMinor(overdraft).value_or(0);
    _model.execute(bank::dto::OpenAccount{.kind = kind, .currency = currency, .overdraftMinor = minor})
        .then([this](bank::dto::AccountInfo) { refresh(); })
        .onError([this](const std::exception_ptr& err) { emit error(errorText(err)); });
}

}  // namespace bankgui
