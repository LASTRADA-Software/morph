// SPDX-License-Identifier: Apache-2.0
//
// Entry point for the QML bank GUI. Wires a BankClient (local backend + Qt
// executor) and the per-domain controllers, exposes them to QML as context
// properties, and loads the QML front-end.

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QVariantMap>

#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>

#include <cstdlib>
#include <filesystem>
#include <string>

#include "BankClient.hpp"
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

}  // namespace

int main(int argc, char* argv[]) {
    QGuiApplication app{argc, argv};
    app.setApplicationName(QStringLiteral("Morph Bank"));
    QQuickStyle::setStyle(QStringLiteral("Basic"));  // so our custom styling applies

    const auto dbPath = std::filesystem::temp_directory_path() / "morph_bank_gui.db";
    bankgui::BankClient client{"DRIVER=SQLite3;Database=" + dbPath.string()};

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

    // Headless screenshot smoke test: seed data, sign in, and grab each page.
    if (const char* outEnv = std::getenv("BANK_GUI_SMOKE")) {
        const QString out = QString::fromUtf8(outEnv);
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst());
        const auto pump = [](int ms) {
            QElapsedTimer timer;
            timer.start();
            while (timer.elapsed() < ms) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                QThread::msleep(5);
            }
        };
        const auto firstAccountId = [&] {
            const auto list = accountController.accounts();
            return list.isEmpty() ? 0LL : list.constFirst().toMap().value("id").toLongLong();
        };

        pump(400);
        if (window) {
            window->grabWindow().save(out + "/qml_login.png");
        }

        const QByteArray seedUser = qgetenv("BANK_SEED_USER");
        const QByteArray seedPass = qgetenv("BANK_SEED_PASS");
        appController.registerUser(seedUser.isEmpty() ? QStringLiteral("gui-demo")
                                                      : QString::fromUtf8(seedUser),
                                   seedPass.isEmpty() ? QStringLiteral("demo1234")
                                                      : QString::fromUtf8(seedPass),
                                   QStringLiteral("Demo User"));
        pump(600);
        accountController.openAccount(0, 0, "500");
        pump(300);
        accountController.openAccount(1, 0, "");
        pump(300);
        const auto checking = firstAccountId();
        transactionController.selectAccount(checking);
        transactionController.deposit("4800");
        pump(300);
        cardController.issue(checking, 0, "1000");
        loanController.apply(checking, "12000", 600, 12);
        payeeController.addPayee("City Power", "DE89370400440532013000", "Stadtbank");
        pump(400);

        if (auto* shell = window ? window->findChild<QObject*>("appShell") : nullptr) {
            accountController.refresh();  // page 0 is already current; force its data to reload
            const char* names[] = {"accounts", "move-money", "cards", "payees", "loans"};
            for (int page = 0; page < 5; ++page) {
                shell->setProperty("current", page);
                pump(500);
                window->grabWindow().save(out + QStringLiteral("/qml_%1.png").arg(names[page]));
            }
        }
        return 0;
    }

    return app.exec();
}
