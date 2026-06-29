// SPDX-License-Identifier: Apache-2.0

#include "PayeesView.hpp"

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
#include "bank/dto/payment_dto.hpp"

namespace bankgui {

PayeesView::PayeesView(BankClient& client, QWidget* parent)
    : Page(parent),
      _client{client},
      _payees{client.bridge(), client.gui()},
      _accounts{client.bridge(), client.gui()},
      _payments{client.bridge(), client.gui()} {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(18);

    // ── Add payee ──────────────────────────────────────────────────────────
    auto* addCard = ui::card();
    auto* add = new QHBoxLayout(addCard);
    add->setContentsMargins(16, 14, 16, 14);
    add->setSpacing(10);
    _name = new QLineEdit;
    _name->setPlaceholderText(QStringLiteral("Payee name"));
    _iban = new QLineEdit;
    _iban->setPlaceholderText(QStringLiteral("IBAN"));
    _bank = new QLineEdit;
    _bank->setPlaceholderText(QStringLiteral("Bank (optional)"));
    auto* addBtn = ui::button(QStringLiteral("Add payee"), QStringLiteral("primary"));
    add->addWidget(_name, 1);
    add->addWidget(_iban, 1);
    add->addWidget(_bank, 1);
    add->addWidget(addBtn);
    root->addWidget(addCard);
    QObject::connect(addBtn, &QPushButton::clicked, this, [this] { addPayee(); });

    // ── Pay a bill ─────────────────────────────────────────────────────────
    auto* payCard = ui::card();
    auto* pay = new QHBoxLayout(payCard);
    pay->setContentsMargins(16, 14, 16, 14);
    pay->setSpacing(10);
    pay->addWidget(ui::label(QStringLiteral("Pay bill"), QStringLiteral("H2")));
    pay->addStretch();
    _payAccount = new QComboBox;
    _payAccount->setMinimumWidth(180);
    _payPayee = new QComboBox;
    _payPayee->setMinimumWidth(180);
    _payAmount = new QLineEdit;
    _payAmount->setPlaceholderText(QStringLiteral("Amount"));
    auto* payBtn = ui::button(QStringLiteral("Pay"), QStringLiteral("primary"));
    pay->addWidget(_payAccount);
    pay->addWidget(_payPayee);
    pay->addWidget(_payAmount);
    pay->addWidget(payBtn);
    root->addWidget(payCard);
    QObject::connect(payBtn, &QPushButton::clicked, this, [this] { payBill(); });

    _status = ui::label(QString(), QStringLiteral("Muted"));
    root->addWidget(_status);

    // ── Payee list ─────────────────────────────────────────────────────────
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

void PayeesView::setStatus(const QString& message, bool error) {
    _status->setObjectName(error ? QStringLiteral("Danger") : QStringLiteral("Success"));
    _status->setText(message);
    ui::repolish(_status);
}

void PayeesView::addPayee() {
    _payees
        .execute(bank::dto::AddPayee{.name = _name->text().toStdString(),
                                     .iban = _iban->text().trimmed().toStdString(),
                                     .bankName = _bank->text().toStdString()})
        .then([this](bank::dto::PayeeInfo) {
            _name->clear();
            _iban->clear();
            _bank->clear();
            setStatus(QStringLiteral("Payee added."), false);
            refresh();
        })
        .onError([this](const std::exception_ptr& err) { setStatus(ui::errorText(err), true); });
}

void PayeesView::payBill() {
    if (!_payAccount->currentData().isValid() || !_payPayee->currentData().isValid()) {
        setStatus(QStringLiteral("Pick an account and payee."), true);
        return;
    }
    const auto minor = ui::parseToMinor(_payAmount->text(), 2);
    if (!minor) {
        setStatus(QStringLiteral("Enter a valid amount."), true);
        return;
    }
    _payments
        .execute(bank::dto::PayBill{.fromAccountId = _payAccount->currentData().toLongLong(),
                                    .payeeId = _payPayee->currentData().toLongLong(),
                                    .amountMinor = *minor})
        .then([this](bank::dto::PaymentInfo) {
            _payAmount->clear();
            setStatus(QStringLiteral("Payment sent."), false);
        })
        .onError([this](const std::exception_ptr& err) { setStatus(ui::errorText(err), true); });
}

void PayeesView::refresh() {
    _accounts.execute(bank::dto::ListAccounts{})
        .then([this](bank::dto::AccountList list) {
            _payAccount->clear();
            for (const auto& account : list.accounts) {
                if (account.status == static_cast<int>(bank::AccountStatus::Closed)) {
                    continue;
                }
                _payAccount->addItem(QStringLiteral("•••• ") + QString::fromStdString(account.number).right(4),
                                     QVariant::fromValue<qlonglong>(account.id));
            }
        })
        .onError([](const std::exception_ptr&) {});

    _payees.execute(bank::dto::ListPayees{})
        .then([this](bank::dto::PayeeList list) { rebuild(list.payees); })
        .onError([](const std::exception_ptr&) {});
}

void PayeesView::rebuild(const std::vector<bank::dto::PayeeInfo>& payees) {
    ui::clearLayout(_list);
    _payPayee->clear();
    for (const auto& payee : payees) {
        _payPayee->addItem(QString::fromStdString(payee.name), QVariant::fromValue<qlonglong>(payee.id));

        auto* frame = ui::card();
        auto* row = new QHBoxLayout(frame);
        row->setContentsMargins(20, 14, 20, 14);
        auto* info = new QVBoxLayout;
        info->addWidget(ui::label(QString::fromStdString(payee.name), QStringLiteral("H2")));
        info->addWidget(ui::label(QString::fromStdString(payee.iban), QStringLiteral("Muted")));
        row->addLayout(info);
        row->addStretch();

        const auto id = payee.id;
        auto* remove = ui::button(QStringLiteral("Remove"), QStringLiteral("danger"));
        QObject::connect(remove, &QPushButton::clicked, this, [this, id] {
            _payees.execute(bank::dto::RemovePayee{.id = id})
                .then([this](bank::dto::CommandResult) { refresh(); })
                .onError([](const std::exception_ptr&) {});
        });
        row->addWidget(remove);
        _list->addWidget(frame);
    }
}

}  // namespace bankgui
