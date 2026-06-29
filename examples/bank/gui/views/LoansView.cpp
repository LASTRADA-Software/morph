// SPDX-License-Identifier: Apache-2.0

#include "LoansView.hpp"

#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QScrollArea>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <optional>

#include "../Ui.hpp"
#include "bank/core/types.hpp"
#include "bank/dto/account_dto.hpp"

namespace bankgui {

namespace {

QString loanStatusName(int status) {
    switch (static_cast<bank::LoanStatus>(status)) {
        case bank::LoanStatus::Active: return QStringLiteral("Active");
        case bank::LoanStatus::PaidOff: return QStringLiteral("Paid off");
        case bank::LoanStatus::Defaulted: return QStringLiteral("Defaulted");
    }
    return QStringLiteral("—");
}

}  // namespace

LoansView::LoansView(BankClient& client, QWidget* parent)
    : Page(parent),
      _client{client},
      _loans{client.bridge(), client.gui()},
      _accounts{client.bridge(), client.gui()} {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(18);

    // ── Apply ──────────────────────────────────────────────────────────────
    auto* applyCard = ui::card();
    auto* apply = new QHBoxLayout(applyCard);
    apply->setContentsMargins(16, 14, 16, 14);
    apply->setSpacing(10);
    apply->addWidget(ui::label(QStringLiteral("Apply for a loan"), QStringLiteral("H2")));
    apply->addStretch();
    _account = new QComboBox;
    _account->setMinimumWidth(160);
    _principal = new QLineEdit;
    _principal->setPlaceholderText(QStringLiteral("Principal"));
    _rate = new QLineEdit;
    _rate->setPlaceholderText(QStringLiteral("Rate (bps)"));
    _rate->setMaximumWidth(110);
    _term = new QLineEdit;
    _term->setPlaceholderText(QStringLiteral("Months"));
    _term->setMaximumWidth(90);
    auto* applyBtn = ui::button(QStringLiteral("Apply"), QStringLiteral("primary"));
    apply->addWidget(_account);
    apply->addWidget(_principal);
    apply->addWidget(_rate);
    apply->addWidget(_term);
    apply->addWidget(applyBtn);
    root->addWidget(applyCard);
    QObject::connect(applyBtn, &QPushButton::clicked, this, [this] { applyLoan(); });

    _status = ui::label(QString(), QStringLiteral("Muted"));
    root->addWidget(_status);

    // ── Body: loan list (top) + schedule (below), stacked full-width ───────────
    auto* body = new QVBoxLayout;
    body->setSpacing(16);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* host = new QWidget;
    _list = new QVBoxLayout(host);
    _list->setContentsMargins(0, 0, 0, 0);
    _list->setSpacing(12);
    _list->setAlignment(Qt::AlignTop);
    scroll->setWidget(host);
    body->addWidget(scroll, 1);

    auto* schedCard = ui::card();
    auto* sched = new QVBoxLayout(schedCard);
    sched->setContentsMargins(16, 14, 16, 14);
    sched->addWidget(ui::label(QStringLiteral("Amortization schedule"), QStringLiteral("H2")));
    _schedule = new QTableWidget(0, 4);
    _schedule->setHorizontalHeaderLabels(
        {QStringLiteral("#"), QStringLiteral("Principal"), QStringLiteral("Interest"), QStringLiteral("Remaining")});
    _schedule->horizontalHeader()->setStretchLastSection(true);
    _schedule->verticalHeader()->setVisible(false);
    _schedule->setEditTriggers(QAbstractItemView::NoEditTriggers);
    _schedule->setSelectionMode(QAbstractItemView::NoSelection);
    _schedule->setShowGrid(false);
    sched->addWidget(_schedule);
    body->addWidget(schedCard, 1);

    root->addLayout(body, 1);
}

void LoansView::setStatus(const QString& message, bool error) {
    _status->setObjectName(error ? QStringLiteral("Danger") : QStringLiteral("Success"));
    _status->setText(message);
    ui::repolish(_status);
}

void LoansView::applyLoan() {
    if (!_account->currentData().isValid()) {
        setStatus(QStringLiteral("Pick an account."), true);
        return;
    }
    const auto principal = ui::parseToMinor(_principal->text(), 2);
    bool rateOk = false;
    bool termOk = false;
    const int rate = _rate->text().trimmed().toInt(&rateOk);
    const int term = _term->text().trimmed().toInt(&termOk);
    if (!principal || !rateOk || !termOk || term <= 0) {
        setStatus(QStringLiteral("Enter principal, rate (bps), and term (months)."), true);
        return;
    }
    _loans
        .execute(bank::dto::ApplyLoan{.accountId = _account->currentData().toLongLong(),
                                      .principalMinor = *principal,
                                      .rateBps = rate,
                                      .termMonths = term})
        .then([this](bank::dto::LoanInfo) {
            _principal->clear();
            _rate->clear();
            _term->clear();
            setStatus(QStringLiteral("Loan disbursed."), false);
            refresh();
        })
        .onError([this](const std::exception_ptr& err) { setStatus(ui::errorText(err), true); });
}

void LoansView::refresh() {
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

    _loans.execute(bank::dto::ListLoans{})
        .then([this](bank::dto::LoanList list) { rebuild(list.loans); })
        .onError([](const std::exception_ptr&) {});
}

void LoansView::rebuild(const std::vector<bank::dto::LoanInfo>& loans) {
    ui::clearLayout(_list);
    for (const auto& loan : loans) {
        auto* frame = ui::card();
        auto* row = new QHBoxLayout(frame);
        row->setContentsMargins(20, 14, 20, 14);

        auto* info = new QVBoxLayout;
        info->addWidget(ui::label(QStringLiteral("Loan #%1").arg(loan.id), QStringLiteral("H2")));
        info->addWidget(ui::label(QStringLiteral("Outstanding ") +
                                      ui::formatMinor(loan.outstandingMinor, loan.currency) +
                                      QStringLiteral("  ·  %1 bps  ·  %2 mo").arg(loan.rateBps).arg(loan.termMonths),
                                  QStringLiteral("Muted")));
        row->addLayout(info);
        row->addStretch();

        const bool paid = loan.status == static_cast<int>(bank::LoanStatus::PaidOff);
        row->addWidget(ui::pill(loanStatusName(loan.status),
                                paid ? QStringLiteral("good") : QStringLiteral("neutral")));

        const auto id = loan.id;
        const auto accountId = loan.accountId;
        const auto outstanding = loan.outstandingMinor;
        auto* sched = ui::button(QStringLiteral("Schedule"));
        QObject::connect(sched, &QPushButton::clicked, this, [this, id] { showSchedule(id); });
        row->addWidget(sched);

        if (!paid) {
            auto* repay = ui::button(QStringLiteral("Repay"), QStringLiteral("primary"));
            QObject::connect(repay, &QPushButton::clicked, this, [this, id, accountId, outstanding] {
                _loans
                    .execute(bank::dto::RepayLoan{.loanId = id, .fromAccountId = accountId,
                                                  .amountMinor = outstanding})
                    .then([this](bank::dto::LoanInfo) { setStatus(QStringLiteral("Repayment made."), false); refresh(); })
                    .onError([this](const std::exception_ptr& err) { setStatus(ui::errorText(err), true); });
            });
            row->addWidget(repay);
        }
        _list->addWidget(frame);
    }
}

void LoansView::showSchedule(std::int64_t loanId) {
    _loans.execute(bank::dto::LoanScheduleRequest{.loanId = loanId})
        .then([this](bank::dto::LoanScheduleResult result) {
            _schedule->setRowCount(static_cast<int>(result.installments.size()));
            int row = 0;
            for (const auto& inst : result.installments) {
                _schedule->setItem(row, 0, new QTableWidgetItem(QString::number(inst.month)));
                _schedule->setItem(row, 1, new QTableWidgetItem(ui::formatMinor(inst.principalMinor, 0)));
                _schedule->setItem(row, 2, new QTableWidgetItem(ui::formatMinor(inst.interestMinor, 0)));
                _schedule->setItem(row, 3, new QTableWidgetItem(ui::formatMinor(inst.remainingMinor, 0)));
                ++row;
            }
        })
        .onError([](const std::exception_ptr&) {});
}

}  // namespace bankgui
