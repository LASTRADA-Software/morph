// SPDX-License-Identifier: Apache-2.0

#include "CardController.hpp"

#include <QVariantMap>

#include "../BankClient.hpp"
#include "Format.hpp"
#include "bank/core/types.hpp"
#include "bank/dto/account_dto.hpp"
#include "bank/dto/card_dto.hpp"

namespace bankgui {

CardController::CardController(BankClient& client, QObject* parent)
    : BankController(client, parent),
      _accountModel{client.bridge(), client.gui()},
      _cardModel{client.bridge(), client.gui()} {}

void CardController::refresh() {
    reloadAccounts();
    reloadCards();
}

void CardController::reloadAccounts() {
    _accountModel.execute(bank::dto::ListAccounts{})
        .then([this](const bank::dto::AccountList& list) {
            _accounts.clear();
            for (const auto& account : list.accounts) {
                if (account.status == static_cast<int>(bank::AccountStatus::Closed)) {
                    continue;
                }
                QVariantMap map;
                map.insert(QStringLiteral("id"), static_cast<qlonglong>(account.id));
                map.insert(QStringLiteral("label"), fmt::last4(account.number));
                _accounts.append(map);
            }
            emit accountsChanged();
        })
        .onError([this](const std::exception_ptr& err) { emit error(errorText(err)); });
}

void CardController::reloadCards() {
    _cardModel.execute(bank::dto::ListCards{})
        .then([this](const bank::dto::CardList& list) {
            _cards.clear();
            for (const auto& card : list.cards) {
                const auto status = static_cast<bank::CardStatus>(card.status);
                const QString kind = card.kind == static_cast<int>(bank::CardKind::Credit) ? QStringLiteral("Credit")
                                                                                           : QStringLiteral("Debit");
                // Cancelled is the fall-through arm rather than a third branch: the
                // two `statusText`/`statusKind` chains this replaces were nested
                // conditional operators, which is what QML reads as the pill's label
                // and colour.
                QString statusText = QStringLiteral("Cancelled");
                QString statusKind = QStringLiteral("bad");
                if (status == bank::CardStatus::Active) {
                    statusText = QStringLiteral("Active");
                    statusKind = QStringLiteral("good");
                } else if (status == bank::CardStatus::Frozen) {
                    statusText = QStringLiteral("Frozen");
                    statusKind = QStringLiteral("warn");
                }
                QVariantMap map;
                map.insert(QStringLiteral("id"), static_cast<qlonglong>(card.id));
                map.insert(QStringLiteral("title"),
                           kind + QStringLiteral(" card  ••••") + QString::fromStdString(card.panLast4));
                map.insert(QStringLiteral("limitText"),
                           QStringLiteral("Daily limit ") + fmt::money(card.dailyLimitMinor, 0));
                map.insert(QStringLiteral("statusText"), statusText);
                map.insert(QStringLiteral("statusKind"), statusKind);
                map.insert(QStringLiteral("active"), status == bank::CardStatus::Active);
                map.insert(QStringLiteral("cancelled"), status == bank::CardStatus::Cancelled);
                _cards.append(map);
            }
            emit cardsChanged();
        })
        .onError([this](const std::exception_ptr& err) { emit error(errorText(err)); });
}

void CardController::issue(qlonglong accountId, int kind, const QString& limit) {
    if (accountId == 0) {
        emit error(QStringLiteral("Pick an account."));
        return;
    }
    const auto minor = limit.trimmed().isEmpty() ? 0 : fmt::parseMinor(limit).value_or(0);
    _cardModel.execute(bank::dto::IssueCard{.accountId = accountId, .kind = kind, .dailyLimitMinor = minor})
        .then([this](const bank::dto::CardInfo&) { reloadCards(); })
        .onError([this](const std::exception_ptr& err) { emit error(errorText(err)); });
}

void CardController::freeze(qlonglong cardId) {
    _cardModel.execute(bank::dto::FreezeCard{.id = cardId})
        .then([this](const bank::dto::CommandResult&) { reloadCards(); })
        .onError([this](const std::exception_ptr& err) { emit error(errorText(err)); });
}

void CardController::unfreeze(qlonglong cardId) {
    _cardModel.execute(bank::dto::UnfreezeCard{.id = cardId})
        .then([this](const bank::dto::CommandResult&) { reloadCards(); })
        .onError([this](const std::exception_ptr& err) { emit error(errorText(err)); });
}

void CardController::cancel(qlonglong cardId) {
    _cardModel.execute(bank::dto::CancelCard{.id = cardId})
        .then([this](const bank::dto::CommandResult&) { reloadCards(); })
        .onError([this](const std::exception_ptr& err) { emit error(errorText(err)); });
}

}  // namespace bankgui
