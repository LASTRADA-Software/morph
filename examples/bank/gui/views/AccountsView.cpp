// SPDX-License-Identifier: Apache-2.0

#include "AccountsView.hpp"

#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QScrollArea>
#include <QVBoxLayout>

#include <optional>

#include "../Ui.hpp"
#include "bank/core/types.hpp"

namespace bankgui {

namespace {

QString kindName(int kind) {
    switch (static_cast<bank::AccountKind>(kind)) {
        case bank::AccountKind::Checking: return QStringLiteral("Checking");
        case bank::AccountKind::Savings: return QStringLiteral("Savings");
        case bank::AccountKind::Credit: return QStringLiteral("Credit");
    }
    return QStringLiteral("Account");
}

QFrame* accountCard(const bank::dto::AccountInfo& account) {
    auto* card = ui::card();
    card->setMinimumWidth(260);
    auto* box = new QVBoxLayout(card);
    box->setContentsMargins(20, 18, 20, 18);
    box->setSpacing(6);

    auto* top = new QHBoxLayout;
    auto* type = ui::label(kindName(account.kind), QStringLiteral("H2"));
    const bool closed = account.status == static_cast<int>(bank::AccountStatus::Closed);
    auto* status = ui::pill(closed ? QStringLiteral("Closed") : QStringLiteral("Open"),
                            closed ? QStringLiteral("neutral") : QStringLiteral("good"));
    top->addWidget(type);
    top->addStretch();
    top->addWidget(status);
    box->addLayout(top);

    const QString last4 = QString::fromStdString(account.number).right(4);
    box->addWidget(ui::label(QStringLiteral("•••• •••• ") + last4, QStringLiteral("Muted")));

    auto* balance = new QLabel(ui::formatMinor(account.balanceMinor, account.currency));
    balance->setStyleSheet(QStringLiteral("font-size:24px; font-weight:700; color:#1F1E1D;"));
    box->addSpacing(6);
    box->addWidget(balance);

    if (account.overdraftMinor > 0) {
        box->addWidget(ui::label(QStringLiteral("Overdraft ") +
                                     ui::formatMinor(account.overdraftMinor, account.currency),
                                 QStringLiteral("Muted")));
    }
    return card;
}

}  // namespace

AccountsView::AccountsView(BankClient& client, QWidget* parent)
    : Page(parent), _client{client}, _accounts{client.bridge(), client.gui()} {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(18);

    // Summary stat card.
    auto* stat = new QFrame;
    stat->setObjectName(QStringLiteral("StatCard"));
    stat->setFixedHeight(96);
    auto* statBox = new QVBoxLayout(stat);
    statBox->setContentsMargins(24, 16, 24, 16);
    _statValue = ui::label(QStringLiteral("—"), QStringLiteral("StatValue"));
    _statLabel = ui::label(QStringLiteral("total balance"), QStringLiteral("StatLabel"));
    statBox->addWidget(_statValue);
    statBox->addWidget(_statLabel);
    root->addWidget(stat);

    // Inline "open account" form.
    auto* formCard = ui::card();
    auto* form = new QHBoxLayout(formCard);
    form->setContentsMargins(16, 14, 16, 14);
    form->setSpacing(10);
    _kind = new QComboBox;
    _kind->addItems({QStringLiteral("Checking"), QStringLiteral("Savings"), QStringLiteral("Credit")});
    _currency = new QComboBox;
    _currency->addItems({QStringLiteral("USD"), QStringLiteral("EUR"), QStringLiteral("GBP"),
                         QStringLiteral("CHF"), QStringLiteral("JPY")});
    _overdraft = new QLineEdit;
    _overdraft->setPlaceholderText(QStringLiteral("Overdraft (optional)"));
    auto* open = ui::button(QStringLiteral("Open account"), QStringLiteral("primary"));
    form->addWidget(ui::label(QStringLiteral("New account"), QStringLiteral("H2")));
    form->addStretch();
    form->addWidget(_kind);
    form->addWidget(_currency);
    form->addWidget(_overdraft);
    form->addWidget(open);
    root->addWidget(formCard);
    QObject::connect(open, &QPushButton::clicked, this, [this] { openAccount(); });

    // Scrollable grid of account cards.
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* gridHost = new QWidget;
    _grid = new QGridLayout(gridHost);
    _grid->setContentsMargins(0, 0, 0, 0);
    _grid->setSpacing(16);
    _grid->setAlignment(Qt::AlignTop);
    scroll->setWidget(gridHost);
    root->addWidget(scroll, 1);
}

void AccountsView::openAccount() {
    const std::optional<std::int64_t> overdraft =
        _overdraft->text().trimmed().isEmpty() ? std::optional<std::int64_t>{0}
                                               : ui::parseToMinor(_overdraft->text(), 2);
    _accounts
        .execute(bank::dto::OpenAccount{.kind = _kind->currentIndex(),
                                        .currency = _currency->currentIndex(),
                                        .overdraftMinor = overdraft.value_or(0)})
        .then([this](bank::dto::AccountInfo) {
            _overdraft->clear();
            refresh();
        })
        .onError([](const std::exception_ptr&) {});
}

void AccountsView::refresh() {
    _accounts.execute(bank::dto::ListAccounts{})
        .then([this](bank::dto::AccountList list) { rebuild(list.accounts); })
        .onError([](const std::exception_ptr&) {});
}

void AccountsView::rebuild(const std::vector<bank::dto::AccountInfo>& accounts) {
    ui::clearLayout(_grid);

    std::int64_t total = 0;
    bool sameCurrency = true;
    int currency = accounts.empty() ? 0 : accounts.front().currency;
    int openCount = 0;
    for (const auto& account : accounts) {
        if (account.status == static_cast<int>(bank::AccountStatus::Closed)) {
            continue;
        }
        ++openCount;
        total += account.balanceMinor;
        if (account.currency != currency) {
            sameCurrency = false;
        }
    }

    if (sameCurrency && openCount > 0) {
        _statValue->setText(ui::formatMinor(total, currency));
        _statLabel->setText(QStringLiteral("total across %1 open account(s)").arg(openCount));
    } else {
        _statValue->setText(QString::number(openCount));
        _statLabel->setText(QStringLiteral("open accounts"));
    }

    int row = 0;
    int col = 0;
    for (const auto& account : accounts) {
        _grid->addWidget(accountCard(account), row, col);
        if (++col == 3) {
            col = 0;
            ++row;
        }
    }
}

}  // namespace bankgui
