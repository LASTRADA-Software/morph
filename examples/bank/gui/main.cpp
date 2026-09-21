// SPDX-License-Identifier: Apache-2.0
//
// Entry point for the QML bank GUI. Wires a BankClient (local backend + Qt
// executor) and the per-domain controllers, exposes them to QML as context
// properties, and loads the QML front-end.

#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStringList>
#include <QThread>
#include <QVariantMap>
#include <filesystem>
#include <string>

#include "BankClient.hpp"
#include "Theme.hpp"
#include "controllers/AccountController.hpp"
#include "controllers/AppController.hpp"
#include "controllers/CardController.hpp"
#include "controllers/LoanController.hpp"
#include "controllers/PayeeController.hpp"
#include "controllers/TransactionController.hpp"

int main(int argc, char* argv[]) {
    const QGuiApplication app{argc, argv};
    QGuiApplication::setApplicationName(QStringLiteral("Morph Bank"));
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
    ctx->setContextProperty(QStringLiteral("theme"), bankgui::makeTheme());
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
    // qgetenv rather than std::getenv: the latter is concurrency-mt-unsafe, and
    // the two seed variables below already read the environment the Qt way.
    if (const QByteArray outEnv = qgetenv("BANK_GUI_SMOKE"); !outEnv.isEmpty()) {
        const QString out = QString::fromUtf8(outEnv);
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst());
        const auto pump = [](int milliseconds) {
            QElapsedTimer timer;
            timer.start();
            while (timer.elapsed() < milliseconds) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                QThread::msleep(5);
            }
        };
        const auto firstAccountId = [&] {
            const auto list = accountController.accounts();
            return list.isEmpty() ? 0LL : list.constFirst().toMap().value("id").toLongLong();
        };

        pump(400);
        if (window != nullptr) {
            window->grabWindow().save(out + "/qml_login.png");
        }

        const QByteArray seedUser = qgetenv("BANK_SEED_USER");
        const QByteArray seedPass = qgetenv("BANK_SEED_PASS");
        appController.registerUser(seedUser.isEmpty() ? QStringLiteral("gui-demo") : QString::fromUtf8(seedUser),
                                   seedPass.isEmpty() ? QStringLiteral("demo1234") : QString::fromUtf8(seedPass),
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

        if (auto* shell = window != nullptr ? window->findChild<QObject*>("appShell") : nullptr) {
            accountController.refresh();  // page 0 is already current; force its data to reload
            const QStringList names{QStringLiteral("accounts"), QStringLiteral("move-money"), QStringLiteral("cards"),
                                    QStringLiteral("payees"), QStringLiteral("loans")};
            for (int page = 0; page < names.size(); ++page) {
                shell->setProperty("current", page);
                pump(500);
                window->grabWindow().save(out + QStringLiteral("/qml_%1.png").arg(names.at(page)));
            }
        }
        return 0;
    }

    return QGuiApplication::exec();
}
