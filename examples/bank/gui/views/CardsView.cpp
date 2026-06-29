// SPDX-License-Identifier: Apache-2.0

#include "CardsView.hpp"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QScrollArea>
#include <QVBoxLayout>

#include <optional>

#include "../Ui.hpp"
#include "bank/core/types.hpp"
#include "bank/dto/account_dto.hpp"

namespace bankgui {

namespace {

QString statusName(int status) {
    switch (static_cast<bank::CardStatus>(status)) {
        case bank::CardStatus::Active: return QStringLiteral("Active");
        case bank::CardStatus::Frozen: return QStringLiteral("Frozen");
        case bank::CardStatus::Cancelled: return QStringLiteral("Cancelled");
    }
    return QStringLiteral("—");
}

QString statusPill(int status) {
    switch (static_cast<bank::CardStatus>(status)) {
        case bank::CardStatus::Active: return QStringLiteral("good");
        case bank::CardStatus::Frozen: return QStringLiteral("warn");
        case bank::CardStatus::Cancelled: return QStringLiteral("bad");
    }
    return QStringLiteral("neutral");
}

}  // namespace

CardsView::CardsView(BankClient& client, QWidget* parent)
    : Page(parent),
      _client{client},
      _accounts{client.bridge(), client.gui()},
      _cards{client.bridge(), client.gui()} {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(18);

    auto* formCard = ui::card();
    auto* form = new QHBoxLayout(formCard);
    form->setContentsMargins(16, 14, 16, 14);
    form->setSpacing(10);
    form->addWidget(ui::label(QStringLiteral("Issue card"), QStringLiteral("H2")));
    form->addStretch();
    _account = new QComboBox;
    _account->setMinimumWidth(220);
    _kind = new QComboBox;
    _kind->addItems({QStringLiteral("Debit"), QStringLiteral("Credit")});
    _limit = new QLineEdit;
    _limit->setPlaceholderText(QStringLiteral("Daily limit (optional)"));
    auto* issue = ui::button(QStringLiteral("Issue"), QStringLiteral("primary"));
    form->addWidget(_account);
    form->addWidget(_kind);
    form->addWidget(_limit);
    form->addWidget(issue);
    root->addWidget(formCard);
    QObject::connect(issue, &QPushButton::clicked, this, [this] { issueCard(); });

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* host = new QWidget;
    _list = new QVBoxLayout(host);
    _list->setContentsMargins(0, 0, 0, 0);
    _list->setSpacing(12);
    _list->setAlignment(Qt::AlignTop);
    scroll->setWidget(host);
    root->addWidget(scroll, 1);
}

void CardsView::issueCard() {
    if (!_account->currentData().isValid()) {
        return;
    }
    const auto limit = _limit->text().trimmed().isEmpty() ? std::optional<std::int64_t>{0}
                                                          : ui::parseToMinor(_limit->text(), 2);
    _cards
        .execute(bank::dto::IssueCard{.accountId = _account->currentData().toLongLong(),
                                      .kind = _kind->currentIndex(),
                                      .dailyLimitMinor = limit.value_or(0)})
        .then([this](bank::dto::CardInfo) { _limit->clear(); refresh(); })
        .onError([](const std::exception_ptr&) {});
}

void CardsView::refresh() {
    _accounts.execute(bank::dto::ListAccounts{})
        .then([this](bank::dto::AccountList list) {
            _account->clear();
            for (const auto& account : list.accounts) {
                if (account.status == static_cast<int>(bank::AccountStatus::Closed)) {
                    continue;
                }
                _account->addItem(QStringLiteral("•••• ") + QString::fromStdString(account.number).right(4),
                                  QVariant::fromValue<qlonglong>(account.id));
            }
        })
        .onError([](const std::exception_ptr&) {});

    _cards.execute(bank::dto::ListCards{})
        .then([this](bank::dto::CardList list) { rebuild(list.cards); })
        .onError([](const std::exception_ptr&) {});
}

void CardsView::rebuild(const std::vector<bank::dto::CardInfo>& cards) {
    ui::clearLayout(_list);
    for (const auto& card : cards) {
        auto* frame = ui::card();
        auto* row = new QHBoxLayout(frame);
        row->setContentsMargins(20, 16, 20, 16);
        row->setSpacing(14);

        auto* info = new QVBoxLayout;
        const QString kind = card.kind == static_cast<int>(bank::CardKind::Credit) ? QStringLiteral("Credit")
                                                                                   : QStringLiteral("Debit");
        info->addWidget(ui::label(kind + QStringLiteral(" card  ••••") + QString::fromStdString(card.panLast4),
                                  QStringLiteral("H2")));
        info->addWidget(ui::label(QStringLiteral("Daily limit ") + ui::formatMinor(card.dailyLimitMinor, 0),
                                  QStringLiteral("Muted")));
        row->addLayout(info);
        row->addStretch();
        row->addWidget(ui::pill(statusName(card.status), statusPill(card.status)));

        const auto id = card.id;
        const bool active = card.status == static_cast<int>(bank::CardStatus::Active);
        const bool cancelled = card.status == static_cast<int>(bank::CardStatus::Cancelled);

        if (!cancelled) {
            auto* toggle = ui::button(active ? QStringLiteral("Freeze") : QStringLiteral("Unfreeze"));
            QObject::connect(toggle, &QPushButton::clicked, this, [this, id, active] {
                auto done = [this](bank::dto::CommandResult) { refresh(); };
                auto fail = [](const std::exception_ptr&) {};
                if (active) {
                    _cards.execute(bank::dto::FreezeCard{.id = id}).then(done).onError(fail);
                } else {
                    _cards.execute(bank::dto::UnfreezeCard{.id = id}).then(done).onError(fail);
                }
            });
            row->addWidget(toggle);

            auto* cancel = ui::button(QStringLiteral("Cancel"), QStringLiteral("danger"));
            QObject::connect(cancel, &QPushButton::clicked, this, [this, id] {
                _cards.execute(bank::dto::CancelCard{.id = id})
                    .then([this](bank::dto::CommandResult) { refresh(); })
                    .onError([](const std::exception_ptr&) {});
            });
            row->addWidget(cancel);
        }
        _list->addWidget(frame);
    }
}

}  // namespace bankgui
