// SPDX-License-Identifier: Apache-2.0
//
// Entry point for the WebAssembly bank GUI. Same controller/QML wiring as the
// native gui/main.cpp, but there is no database: the models persist to an
// in-memory store (see include/bank/wasm/). A demo user + a couple of accounts
// are seeded at startup and auto-signed-in so the hosted page is immediately
// usable.

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QVariantMap>

#include <string>

#include "BankClient.hpp"
#include "bank/core/demo_hash.hpp"
#include "bank/core/types.hpp"
#include "bank/wasm/store.hpp"
#include "bank/wasm/store_ops.hpp"
#include "controllers/AccountController.hpp"
#include "controllers/AppController.hpp"
#include "controllers/CardController.hpp"
#include "controllers/LoanController.hpp"
#include "controllers/PayeeController.hpp"
#include "controllers/TransactionController.hpp"

namespace {

/// The warm, "Claude-inspired" palette, handed to QML as the `theme` object.
QVariantMap makeTheme() {
    return QVariantMap{
        {QStringLiteral("paper"), QStringLiteral("#FAF9F5")},
        {QStringLiteral("surface"), QStringLiteral("#FFFFFF")},
        {QStringLiteral("surfaceAlt"), QStringLiteral("#F2F0E9")},
        {QStringLiteral("ink"), QStringLiteral("#1F1E1D")},
        {QStringLiteral("inkSoft"), QStringLiteral("#6B6862")},
        {QStringLiteral("border"), QStringLiteral("#E7E4DB")},
        {QStringLiteral("accent"), QStringLiteral("#C96442")},
        {QStringLiteral("accentHover"), QStringLiteral("#B5572F")},
        {QStringLiteral("sidebar"), QStringLiteral("#262624")},
        {QStringLiteral("sidebarText"), QStringLiteral("#C9C6BE")},
        {QStringLiteral("sidebarHover"), QStringLiteral("#34322F")},
        {QStringLiteral("good"), QStringLiteral("#2F9E66")},
        {QStringLiteral("warn"), QStringLiteral("#C96442")},
        {QStringLiteral("bad"), QStringLiteral("#C0392B")},
        {QStringLiteral("goodBg"), QStringLiteral("#E5F4EC")},
        {QStringLiteral("warnBg"), QStringLiteral("#FBEDE8")},
        {QStringLiteral("badBg"), QStringLiteral("#FBEDEB")},
        {QStringLiteral("neutralBg"), QStringLiteral("#F2F0E9")},
        {QStringLiteral("dangerBorder"), QStringLiteral("#E7C9C5")},
        {QStringLiteral("radius"), 12},
    };
}

/// Seeds a demo user (password "demo1234"), two accounts, and an opening
/// balance directly in the in-memory store so the hosted demo has data.
void seedDemo() {
    using namespace bank;
    auto& db = wasm::sharedDb();
    if (!db.users.all().empty()) {
        return;  // already seeded
    }
    wasm::UserRow user;
    user.username = "demo";
    user.displayName = "Demo User";
    user.passwordHash = demoHash(std::string{"demo"} + ":" + "demo1234" + ":morph-bank");
    const auto uid = db.users.insert(user);

    wasm::AccountRow checking;
    checking.userId = uid;
    checking.number = "DE00500700100200300400";
    checking.kind = static_cast<int>(AccountKind::Checking);
    checking.currency = static_cast<int>(Currency::EUR);
    checking.status = static_cast<int>(AccountStatus::Open);
    checking.overdraftMinor = 50000;
    const auto chkId = db.accounts.insert(checking);
    auto chkRow = *db.accounts.find(chkId);
    wasm::applyCredit(db, chkRow, 480000, TxnKind::Deposit, 0, "opening deposit");

    wasm::AccountRow savings;
    savings.userId = uid;
    savings.number = "DE00500700100900800700";
    savings.kind = static_cast<int>(AccountKind::Savings);
    savings.currency = static_cast<int>(Currency::EUR);
    savings.status = static_cast<int>(AccountStatus::Open);
    savings.interestBps = 150;
    db.accounts.insert(savings);
}

}  // namespace

int main(int argc, char* argv[]) {
    QGuiApplication app{argc, argv};
    app.setApplicationName(QStringLiteral("Morph Bank"));
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    seedDemo();

    // Connection string is ignored by the WASM BankClient (no ODBC).
    bankgui::BankClient client{std::string{}};

    bankgui::AppController appController{client};
    bankgui::AccountController accountController{client};
    bankgui::TransactionController transactionController{client};
    bankgui::CardController cardController{client};
    bankgui::PayeeController payeeController{client};
    bankgui::LoanController loanController{client};

    QQmlApplicationEngine engine;
    auto* ctx = engine.rootContext();
    ctx->setContextProperty(QStringLiteral("theme"), makeTheme());
    ctx->setContextProperty(QStringLiteral("app"), &appController);
    ctx->setContextProperty(QStringLiteral("accounts"), &accountController);
    ctx->setContextProperty(QStringLiteral("txns"), &transactionController);
    ctx->setContextProperty(QStringLiteral("cards"), &cardController);
    ctx->setContextProperty(QStringLiteral("payees"), &payeeController);
    ctx->setContextProperty(QStringLiteral("loans"), &loanController);

    engine.loadFromModule("BankGui", "Main");
    if (engine.rootObjects().isEmpty()) {
        return -1;
    }

    // Sign the demo user in automatically (resolves on the event loop).
    appController.login(QStringLiteral("demo"), QStringLiteral("demo1234"));

    return app.exec();
}
