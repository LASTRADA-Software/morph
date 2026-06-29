// SPDX-License-Identifier: Apache-2.0

#include "MoveMoneyView.hpp"

#include <QColor>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <optional>

#include "../Ui.hpp"
#include "bank/core/types.hpp"

namespace bankgui {

namespace {

QString txnKindName(int kind) {
    switch (static_cast<bank::TxnKind>(kind)) {
        case bank::TxnKind::Deposit: return QStringLiteral("Deposit");
        case bank::TxnKind::Withdrawal: return QStringLiteral("Withdrawal");
        case bank::TxnKind::TransferIn: return QStringLiteral("Transfer in");
        case bank::TxnKind::TransferOut: return QStringLiteral("Transfer out");
        case bank::TxnKind::Payment: return QStringLiteral("Payment");
        case bank::TxnKind::Fee: return QStringLiteral("Fee");
        case bank::TxnKind::Interest: return QStringLiteral("Interest");
        case bank::TxnKind::LoanDisbursement: return QStringLiteral("Loan in");
        case bank::TxnKind::LoanRepayment: return QStringLiteral("Loan repay");
        case bank::TxnKind::CardPurchase: return QStringLiteral("Card");
        case bank::TxnKind::Exchange: return QStringLiteral("Exchange");
    }
    return QStringLiteral("Entry");
}

QString accountLabel(const bank::dto::AccountInfo& account) {
    return QStringLiteral("•••• %1  (%2)")
        .arg(QString::fromStdString(account.number).right(4),
             ui::formatMinor(account.balanceMinor, account.currency));
}

}  // namespace

MoveMoneyView::MoveMoneyView(BankClient& client, QWidget* parent)
    : Page(parent),
      _client{client},
      _accounts{client.bridge(), client.gui()},
      _txns{client.bridge(), client.gui()} {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(18);

    // ── Move money card ────────────────────────────────────────────────────
    auto* moveCard = ui::card();
    auto* move = new QVBoxLayout(moveCard);
    move->setContentsMargins(20, 18, 20, 18);
    move->setSpacing(12);
    move->addWidget(ui::label(QStringLiteral("Move money"), QStringLiteral("H2")));

    auto* accRow = new QHBoxLayout;
    accRow->addWidget(ui::label(QStringLiteral("Account"), QStringLiteral("Muted")));
    _account = new QComboBox;
    _account->setMinimumWidth(280);
    accRow->addWidget(_account, 1);
    move->addLayout(accRow);
    QObject::connect(_account, &QComboBox::currentIndexChanged, this, [this] { reloadHistory(); });

    // Deposit + withdraw row.
    auto* dwRow = new QHBoxLayout;
    _amount = new QLineEdit;
    _amount->setPlaceholderText(QStringLiteral("Amount"));
    auto* deposit = ui::button(QStringLiteral("Deposit"), QStringLiteral("primary"));
    auto* withdraw = ui::button(QStringLiteral("Withdraw"));
    dwRow->addWidget(_amount, 1);
    dwRow->addWidget(deposit);
    dwRow->addWidget(withdraw);
    move->addLayout(dwRow);

    // Transfer row.
    auto* tRow = new QHBoxLayout;
    _target = new QComboBox;
    _target->setMinimumWidth(220);
    _transferAmount = new QLineEdit;
    _transferAmount->setPlaceholderText(QStringLiteral("Amount"));
    auto* transfer = ui::button(QStringLiteral("Transfer to"), QStringLiteral("primary"));
    tRow->addWidget(transfer);
    tRow->addWidget(_target, 1);
    tRow->addWidget(_transferAmount, 1);
    move->addLayout(tRow);

    _status = ui::label(QString(), QStringLiteral("Muted"));
    _status->setWordWrap(true);
    move->addWidget(_status);
    root->addWidget(moveCard);

    // ── History card ─────────────────────────────────────────────────────────
    auto* histCard = ui::card();
    auto* hist = new QVBoxLayout(histCard);
    hist->setContentsMargins(20, 18, 20, 18);
    hist->setSpacing(12);
    hist->addWidget(ui::label(QStringLiteral("Recent activity"), QStringLiteral("H2")));
    _history = new QTableWidget(0, 3);
    _history->setHorizontalHeaderLabels({QStringLiteral("Type"), QStringLiteral("Amount"),
                                         QStringLiteral("Balance")});
    _history->horizontalHeader()->setStretchLastSection(true);
    _history->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    _history->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    _history->verticalHeader()->setVisible(false);
    _history->setEditTriggers(QAbstractItemView::NoEditTriggers);
    _history->setSelectionMode(QAbstractItemView::NoSelection);
    _history->setShowGrid(false);
    hist->addWidget(_history);
    root->addWidget(histCard, 1);

    // ── Wiring ────────────────────────────────────────────────────────────────
    QObject::connect(deposit, &QPushButton::clicked, this, [this] {
        auto minor = ui::parseToMinor(_amount->text(), bank::currencyDecimals(static_cast<bank::Currency>(selectedCurrency())));
        if (!minor || selectedAccountId() == 0) {
            setStatus(QStringLiteral("Enter a valid amount."), true);
            return;
        }
        _txns.execute(bank::dto::Deposit{.accountId = selectedAccountId(), .amountMinor = *minor})
            .then([this](bank::dto::TxnInfo) { _amount->clear(); setStatus(QStringLiteral("Deposit complete."), false); refresh(); })
            .onError([this](const std::exception_ptr& err) { setStatus(ui::errorText(err), true); });
    });
    QObject::connect(withdraw, &QPushButton::clicked, this, [this] {
        auto minor = ui::parseToMinor(_amount->text(), bank::currencyDecimals(static_cast<bank::Currency>(selectedCurrency())));
        if (!minor || selectedAccountId() == 0) {
            setStatus(QStringLiteral("Enter a valid amount."), true);
            return;
        }
        _txns.execute(bank::dto::Withdraw{.accountId = selectedAccountId(), .amountMinor = *minor})
            .then([this](bank::dto::TxnInfo) { _amount->clear(); setStatus(QStringLiteral("Withdrawal complete."), false); refresh(); })
            .onError([this](const std::exception_ptr& err) { setStatus(ui::errorText(err), true); });
    });
    QObject::connect(transfer, &QPushButton::clicked, this, [this] {
        auto minor = ui::parseToMinor(_transferAmount->text(), bank::currencyDecimals(static_cast<bank::Currency>(selectedCurrency())));
        const auto to = _target->currentData().toLongLong();
        if (!minor || selectedAccountId() == 0 || to == 0) {
            setStatus(QStringLiteral("Pick a target account and amount."), true);
            return;
        }
        _txns.execute(bank::dto::Transfer{.fromAccountId = selectedAccountId(), .toAccountId = to, .amountMinor = *minor})
            .then([this](bank::dto::TransferResult) { _transferAmount->clear(); setStatus(QStringLiteral("Transfer complete."), false); refresh(); })
            .onError([this](const std::exception_ptr& err) { setStatus(ui::errorText(err), true); });
    });
}

std::int64_t MoveMoneyView::selectedAccountId() const {
    return _account->currentData().isValid() ? _account->currentData().toLongLong() : 0;
}

int MoveMoneyView::selectedCurrency() const {
    const auto id = selectedAccountId();
    for (const auto& account : _cache) {
        if (account.id == id) {
            return account.currency;
        }
    }
    return 0;
}

void MoveMoneyView::setStatus(const QString& message, bool error) {
    _status->setObjectName(error ? QStringLiteral("Danger") : QStringLiteral("Success"));
    _status->setText(message);
    ui::repolish(_status);
}

void MoveMoneyView::refresh() {
    _accounts.execute(bank::dto::ListAccounts{})
        .then([this](bank::dto::AccountList list) {
            const auto previous = selectedAccountId();
            _cache.clear();
            _account->clear();
            _target->clear();
            for (const auto& account : list.accounts) {
                if (account.status == static_cast<int>(bank::AccountStatus::Closed)) {
                    continue;
                }
                _cache.push_back(account);
                _account->addItem(accountLabel(account), QVariant::fromValue<qlonglong>(account.id));
                _target->addItem(accountLabel(account), QVariant::fromValue<qlonglong>(account.id));
            }
            // Restore prior selection if still present.
            const int idx = _account->findData(QVariant::fromValue<qlonglong>(previous));
            if (idx >= 0) {
                _account->setCurrentIndex(idx);
            }
            reloadHistory();
        })
        .onError([](const std::exception_ptr&) {});
}

void MoveMoneyView::reloadHistory() {
    const auto id = selectedAccountId();
    if (id == 0) {
        _history->setRowCount(0);
        return;
    }
    _txns.execute(bank::dto::History{.accountId = id, .limit = 50})
        .then([this](bank::dto::HistoryPage page) {
            _history->setRowCount(static_cast<int>(page.entries.size()));
            int row = 0;
            for (const auto& entry : page.entries) {
                const bool credit = entry.direction == static_cast<int>(bank::TxnDirection::Credit);
                const QString sign = credit ? QStringLiteral("+") : QStringLiteral("−");
                auto* kind = new QTableWidgetItem(txnKindName(entry.kind));
                auto* amount = new QTableWidgetItem(sign + ui::formatMinor(entry.amountMinor, entry.currency));
                amount->setForeground(QColor(credit ? "#2F9E66" : "#C0392B"));
                auto* balance = new QTableWidgetItem(ui::formatMinor(entry.balanceAfterMinor, entry.currency));
                _history->setItem(row, 0, kind);
                _history->setItem(row, 1, amount);
                _history->setItem(row, 2, balance);
                ++row;
            }
        })
        .onError([](const std::exception_ptr&) {});
}

}  // namespace bankgui
