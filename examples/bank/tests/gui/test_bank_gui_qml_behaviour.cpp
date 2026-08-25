// SPDX-License-Identifier: Apache-2.0
//
// The bank GUI's behaviour that lives *in* the QML, over a live engine and the
// real controllers (morph#296).
//
// Why an engine rather than a controller-only test, stated up front because it
// is the whole reason this binary exists: the controllers are entirely
// self-consistent when driven from C++. Write `selectedAccount`, refresh,
// deposit, and the money lands in the account that was written, every time.
// The defect morph#296 named was not in the controller at all -- it was that
// `MoveMoneyPage.qml` wrote `selectedAccount` and never read it back, so
// nothing restored the `ComboBox`'s `currentIndex` after
// `TransactionController::refresh()` republished `accounts`. A `ComboBox`
// resets `currentIndex` to 0 whenever its `model` is replaced, and `refresh()`
// is exactly what `deposit()`/`withdraw()`/`transfer()` call on success. The
// picker therefore snapped back to the first account while the controller kept
// the account the user chose, and the next deposit went somewhere the screen
// did not say. Only a live engine over the shipped `.qml` can observe that, so
// only a live engine can hold the regression.
//
// The second case covers the other half of the same issue: `txns.posted` and
// `payees.paid` were emitted and dropped. They are now the success half of
// `Main.qml`'s toast, and the audit next door can only see that the *names* are
// written in a `Connections` block -- whether the handler fires, and with what,
// needs the engine too.
//
// The QML is loaded from the source tree by URL, not from the `BankGui` QML
// module: that module lives inside the `bank_gui` *executable* and cannot be
// linked. QML's implicit import of a component's own directory resolves the
// sibling types (`Panel`, `Picker`, `Field`, `AppButton`) with no `qmldir`, so
// these are the same files the desktop client ships rather than copies.
//
// Neither case is a synthesized-mouse-event flow (examples/TESTING.md,
// presenter rule 6, forbids those). The one user gesture reproduced here is the
// two lines `QQuickComboBoxPrivate::itemClicked` runs when a popup entry is
// chosen: set `currentIndex`, then emit `activated(index)`. Everything else is
// a controller call the QML would have made, and a property value read back off
// the items the engine created.

#include <QList>
#include <QObject>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QString>
#include <QUrl>
#include <QVariant>
#include <QVariantMap>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <morph/core/bridge.hpp>
#include <string>
#include <system_error>

#include "BankClient.hpp"
#include "Theme.hpp"
#include "bank/dto/account_dto.hpp"
#include "bank/models/account_model.hpp"
#include "bank/models/customer_model.hpp"
#include "controllers/AccountController.hpp"
#include "controllers/AppController.hpp"
#include "controllers/BankController.hpp"
#include "controllers/CardController.hpp"
#include "controllers/LoanController.hpp"
#include "controllers/PayeeController.hpp"
#include "controllers/TransactionController.hpp"
#include "testkit/pump.hpp"

using morph::ladder::testkit::awaitQt;
using morph::ladder::testkit::pumpUntil;

namespace {

/// @brief A database of this suite's own -- its own file, not the one
///        `bank_tests` or the surface audit uses, so the binaries can run
///        concurrently -- wiped once per process rather than once per case, so
///        a later case cannot delete the file an earlier one still has open.
/// @return The ODBC connection string for it.
[[nodiscard]] std::string connectionString() {
    static const std::string connection = [] {
        const auto path = std::filesystem::temp_directory_path() / "morph_bank_gui_qml.db";
        std::error_code err;
        std::filesystem::remove(path, err);
        return "DRIVER=SQLite3;Database=" + path.string();
    }();
    return connection;
}

/// @brief URL of one of the GUI's shipped `.qml` files in the source tree.
/// @param fileName Basename, e.g. `"MoveMoneyPage.qml"`.
/// @return A `file:` URL the engine can load.
[[nodiscard]] QUrl qmlUrl(const QString& fileName) {
    return QUrl::fromLocalFile(QStringLiteral(MORPH_LADDER_SOURCE_ROOT "/examples/bank/gui/qml/") + fileName);
}

/// @brief Index of @p id within a controller's published `accounts` list.
/// @param accounts The `QVariantList` of account bags.
/// @param id       Account id to locate.
/// @return Its index, or -1.
[[nodiscard]] int indexOfAccount(const QVariantList& accounts, qlonglong id) {
    for (int i = 0; i < accounts.size(); ++i) {
        if (accounts.at(i).toMap().value(QStringLiteral("id")).toLongLong() == id) {
            return i;
        }
    }
    return -1;
}

}  // namespace

TEST_CASE("MoveMoneyPage's picker keeps naming the account the next deposit will land in",
          "[bank][gui][qml][move-money]") {
    bankgui::BankClient client{connectionString()};

    bankgui::AppController app{client};
    app.registerUser(QStringLiteral("gui-move-money"), QStringLiteral("hunter2demo"), QStringLiteral("Picker"));
    REQUIRE(pumpUntil([&app] { return app.authenticated(); }));

    morph::bridge::BridgeHandler<bank::CustomerModel> customer{client.bridge(), client.gui()};
    morph::bridge::BridgeHandler<bank::AccountModel> accountReads{client.bridge(), client.gui()};
    const auto balanceOf = [&accountReads](qlonglong id) {
        return awaitQt(accountReads.execute(bank::dto::GetAccount{.id = id})).balanceMinor;
    };

    // Two open accounts, so "the first one" and "the one the user picked" can
    // differ at all. Both USD: the currency is not what is under test, and a
    // second currency would only add a minor-unit variable to the amounts.
    const auto checking =
        static_cast<qlonglong>(awaitQt(customer.execute(bank::dto::OpenAccount{.kind = 0, .currency = 0})).id);
    const auto savings =
        static_cast<qlonglong>(awaitQt(customer.execute(bank::dto::OpenAccount{.kind = 1, .currency = 0})).id);

    bankgui::TransactionController txns{client};
    int accountsPublished = 0;
    QObject::connect(&txns, &bankgui::TransactionController::accountsChanged,
                     [&accountsPublished] { ++accountsPublished; });

    // ── the page, as `gui/main.cpp` wires it ──────────────────────────────
    QQmlEngine engine;
    QString firstWarning;
    QObject::connect(&engine, &QQmlEngine::warnings, [&firstWarning](const QList<QQmlError>& warnings) {
        if (firstWarning.isEmpty() && !warnings.isEmpty()) {
            firstWarning = warnings.front().toString();
        }
    });
    engine.rootContext()->setContextProperty(QStringLiteral("theme"), bankgui::makeTheme());
    engine.rootContext()->setContextProperty(QStringLiteral("txns"), &txns);

    QQmlComponent component{&engine, qmlUrl(QStringLiteral("MoveMoneyPage.qml"))};
    INFO(component.errorString().toStdString());
    REQUIRE(component.isReady());
    const std::unique_ptr<QObject> page{component.create()};
    REQUIRE(page != nullptr);
    CHECK(firstWarning.toStdString() == std::string{});

    QObject* picker = page->findChild<QObject*>(QStringLiteral("accountPicker"));
    REQUIRE(picker != nullptr);

    // ── AppShell's `Component.onCompleted: refreshCurrent()` ──────────────
    txns.refresh();
    REQUIRE(pumpUntil([&txns] { return txns.accounts().size() == 2; }));
    // The controller auto-selected the first account; the picker agrees,
    // because a ComboBox handed a fresh model starts at index 0. This is the
    // state the bug hides behind: on a freshly opened page the two *do* match.
    REQUIRE(txns.selectedAccount() == checking);
    CHECK(picker->property("currentValue").toLongLong() == txns.selectedAccount());

    // ── the user picks the savings account ────────────────────────────────
    // `QQuickComboBoxPrivate::itemClicked`, minus the popup: set the index,
    // then emit `activated`, which is what runs MoveMoneyPage's
    // `onActivated: txns.selectAccount(currentValue)`.
    const int savingsIndex = indexOfAccount(txns.accounts(), savings);
    REQUIRE(savingsIndex == 1);
    REQUIRE(picker->setProperty("currentIndex", savingsIndex));
    REQUIRE(QMetaObject::invokeMethod(picker, "activated", Q_ARG(int, savingsIndex)));
    REQUIRE(txns.selectedAccount() == savings);
    REQUIRE(picker->property("currentValue").toLongLong() == savings);

    // ── a deposit, which makes the controller republish `accounts` ────────
    const int publishedBefore = accountsPublished;
    txns.deposit(QStringLiteral("50.00"));
    REQUIRE(pumpUntil([&accountsPublished, publishedBefore] { return accountsPublished > publishedBefore; }));
    REQUIRE(pumpUntil([&txns] { return txns.accounts().size() == 2; }));
    REQUIRE(balanceOf(savings) == 5000);

    // The picker must still name the savings account. Before morph#296's fix
    // it named the checking account here: replacing a ComboBox's `model` resets
    // its `currentIndex` to 0, and nothing read `selectedAccount` back.
    CHECK(picker->property("currentValue").toLongLong() == txns.selectedAccount());

    // ── and it is not cosmetic: the money follows the label ───────────────
    // The user reads the picker, believes that is where the next deposit goes,
    // and presses Deposit again. `shown` is what the screen said at that
    // moment, so `balanceOf(shown)` is "did the money land where the user was
    // told it would".
    const auto shown = picker->property("currentValue").toLongLong();
    const int publishedBeforeSecond = accountsPublished;
    txns.deposit(QStringLiteral("10.00"));
    REQUIRE(
        pumpUntil([&accountsPublished, publishedBeforeSecond] { return accountsPublished > publishedBeforeSecond; }));
    CHECK(balanceOf(shown) == 6000);
    CHECK(balanceOf(savings) == 6000);
    CHECK(balanceOf(checking) == 0);
}

TEST_CASE("Main.qml confirms a posted transaction and a paid bill in the toast", "[bank][gui][qml][toast]") {
    bankgui::BankClient client{connectionString()};

    bankgui::AppController app{client};
    bankgui::AccountController accountsController{client};
    bankgui::TransactionController txns{client};
    bankgui::CardController cards{client};
    bankgui::PayeeController payees{client};
    bankgui::LoanController loans{client};

    // Any controller error would land in the same toast under a *different*
    // message, so capture them: without this a failure below reads as "the
    // toast never said anything" instead of naming what went wrong.
    QString lastError;
    for (const bankgui::BankController* controller : std::initializer_list<const bankgui::BankController*>{
             &app, &accountsController, &txns, &cards, &payees, &loans}) {
        QObject::connect(controller, &bankgui::BankController::error,
                         [&lastError](const QString& message) { lastError = message; });
    }

    // ── the whole shell, as `gui/main.cpp` wires it ───────────────────────
    QQmlEngine engine;
    auto* context = engine.rootContext();
    context->setContextProperty(QStringLiteral("theme"), bankgui::makeTheme());
    context->setContextProperty(QStringLiteral("app"), &app);
    context->setContextProperty(QStringLiteral("accounts"), &accountsController);
    context->setContextProperty(QStringLiteral("txns"), &txns);
    context->setContextProperty(QStringLiteral("cards"), &cards);
    context->setContextProperty(QStringLiteral("payees"), &payees);
    context->setContextProperty(QStringLiteral("loans"), &loans);

    QQmlComponent component{&engine, qmlUrl(QStringLiteral("Main.qml"))};
    INFO(component.errorString().toStdString());
    REQUIRE(component.isReady());
    const std::unique_ptr<QObject> window{component.create()};
    REQUIRE(window != nullptr);

    QObject* toastText = window->findChild<QObject*>(QStringLiteral("toastText"));
    REQUIRE(toastText != nullptr);
    const auto toastSays = [toastText] { return toastText->property("text").toString(); };
    REQUIRE(toastSays().isEmpty());

    app.registerUser(QStringLiteral("gui-toast"), QStringLiteral("hunter2demo"), QStringLiteral("Toast"));
    REQUIRE(pumpUntil([&app] { return app.authenticated(); }));

    morph::bridge::BridgeHandler<bank::CustomerModel> customer{client.bridge(), client.gui()};
    const auto account =
        static_cast<qlonglong>(awaitQt(customer.execute(bank::dto::OpenAccount{.kind = 0, .currency = 0})).id);

    // ── a deposit: TransactionController::posted ──────────────────────────
    txns.refresh();
    REQUIRE(pumpUntil([&txns] { return !txns.accounts().isEmpty(); }));
    txns.selectAccount(account);
    txns.deposit(QStringLiteral("200.00"));
    INFO(lastError.toStdString());
    REQUIRE(pumpUntil([&toastSays] { return !toastSays().isEmpty(); }));
    CHECK(toastSays().toStdString() == std::string{"Transaction posted"});

    // ── a bill payment: PayeeController::paid ─────────────────────────────
    // The one that had no other feedback at all: `payBill` reloads nothing on
    // success, so before this binding a successful payment changed nothing on
    // screen.
    payees.addPayee(QStringLiteral("City Power"), QStringLiteral("DE89370400440532013000"),
                    QStringLiteral("Stadtbank"));
    REQUIRE(pumpUntil([&payees] { return !payees.payees().isEmpty(); }));
    const auto payee = payees.payees().constFirst().toMap().value(QStringLiteral("id")).toLongLong();

    payees.payBill(account, payee, QStringLiteral("12.50"));
    INFO(lastError.toStdString());
    REQUIRE(pumpUntil([&toastSays] { return toastSays() != QStringLiteral("Transaction posted"); }));
    CHECK(toastSays().toStdString() == std::string{"Bill paid"});
}
