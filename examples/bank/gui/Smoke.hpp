// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/bridge.hpp>

#include <QApplication>
#include <QEventLoop>
#include <QStackedWidget>
#include <QString>
#include <QThread>
#include <QTimer>

#include "BankClient.hpp"
#include "MainWindow.hpp"
#include "bank/dto/account_dto.hpp"
#include "bank/dto/card_dto.hpp"
#include "bank/dto/loan_dto.hpp"
#include "bank/dto/payee_dto.hpp"
#include "bank/dto/transaction_dto.hpp"
#include "bank/models/account_model.hpp"
#include "bank/models/auth_model.hpp"
#include "bank/models/card_model.hpp"
#include "bank/models/loan_model.hpp"
#include "bank/models/payee_model.hpp"
#include "bank/models/transaction_model.hpp"

/// @file
/// Headless screenshot smoke test, run only when BANK_GUI_SMOKE is set. It seeds
/// representative demo data, logs in, and saves a PNG of each page — used to
/// verify the GUI renders (and looks right) without a display.

namespace bankgui::smoke {

/// Pumps the Qt event loop until @p completion resolves.
template <typename Completion>
void pumpAwait(Completion completion) {
    bool done = false;
    completion.then([&](auto&&) { done = true; }).onError([&](const std::exception_ptr&) { done = true; });
    while (!done) {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
    }
}

/// Seeds a representative account/transaction/payee/card/loan set for @p principal.
inline void seed(BankClient& client, const QString& principal) {
    morph::bridge::BridgeHandler<bank::AuthModel> auth{client.bridge(), client.gui()};
    pumpAwait(auth.execute(bank::dto::RegisterUser{
        .username = principal.toStdString(), .password = "demo1234", .displayName = "Demo User"}));
    client.login(principal, QStringLiteral("Demo User"));

    morph::bridge::BridgeHandler<bank::AccountModel> accounts{client.bridge(), client.gui()};
    morph::bridge::BridgeHandler<bank::TransactionModel> txns{client.bridge(), client.gui()};
    morph::bridge::BridgeHandler<bank::PayeeModel> payees{client.bridge(), client.gui()};
    morph::bridge::BridgeHandler<bank::CardModel> cards{client.bridge(), client.gui()};
    morph::bridge::BridgeHandler<bank::LoanModel> loans{client.bridge(), client.gui()};

    pumpAwait(accounts.execute(bank::dto::OpenAccount{.kind = 0, .currency = 0, .overdraftMinor = 50000}));
    pumpAwait(accounts.execute(bank::dto::OpenAccount{.kind = 1, .currency = 0}));

    auto list = bank::dto::AccountList{};
    {
        bool done = false;
        accounts.execute(bank::dto::ListAccounts{})
            .then([&](bank::dto::AccountList result) { list = std::move(result); done = true; })
            .onError([&](const std::exception_ptr&) { done = true; });
        while (!done) {
            QApplication::processEvents(QEventLoop::AllEvents, 10);
        }
    }
    if (list.accounts.size() >= 2) {
        const auto checking = list.accounts[0].id;
        const auto savings = list.accounts[1].id;
        pumpAwait(txns.execute(bank::dto::Deposit{.accountId = checking, .amountMinor = 480000,
                                                  .description = "Salary"}));
        pumpAwait(txns.execute(bank::dto::Transfer{.fromAccountId = checking, .toAccountId = savings,
                                                   .amountMinor = 120000, .description = "Savings"}));
        pumpAwait(txns.execute(bank::dto::Withdraw{.accountId = checking, .amountMinor = 3200,
                                                   .description = "Groceries"}));
        pumpAwait(cards.execute(bank::dto::IssueCard{.accountId = checking, .kind = 0,
                                                     .dailyLimitMinor = 100000}));
        pumpAwait(loans.execute(bank::dto::ApplyLoan{.accountId = checking, .principalMinor = 1200000,
                                                     .rateBps = 600, .termMonths = 12}));
    }
    pumpAwait(payees.execute(bank::dto::AddPayee{
        .name = "City Power", .iban = "DE89370400440532013000", .bankName = "Stadtbank"}));
}

/// Logs in, builds the main window, and screenshots each page, then quits.
inline void run(BankClient& client, QStackedWidget* window, const QString& outDir) {
    // Capture the login screen (currently shown) before signing in.
    QApplication::processEvents(QEventLoop::AllEvents, 50);
    window->grab().save(outDir + QStringLiteral("/bank_login.png"));

    const QString principal = QStringLiteral("gui-demo");
    seed(client, principal);

    auto* main = new MainWindow{client};
    const int index = window->addWidget(main);
    window->setCurrentIndex(index);

    QTimer::singleShot(300, window, [main, window, outDir] {
        const char* names[] = {"accounts", "move-money", "cards", "payees", "loans"};
        for (int page = 0; page < main->pageCount(); ++page) {
            main->selectPage(page);
            // Let the page's async refresh (account list, history, etc.) settle
            // before capturing — completions arrive via the Qt event loop.
            for (int tick = 0; tick < 40; ++tick) {
                QApplication::processEvents(QEventLoop::AllEvents, 15);
                QThread::msleep(5);
            }
            window->grab().save(outDir + QStringLiteral("/bank_%1.png").arg(names[page]));
        }
        QApplication::quit();
    });
}

}  // namespace bankgui::smoke
