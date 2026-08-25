// SPDX-License-Identifier: Apache-2.0
//
// The QML-visible surface of all six bank GUI controllers, audited against
// `gui/qml/` itself.
//
// The bank GUI had no test of any kind before this file: `bank_tests` links
// `bank_lib` and drives the models, and nothing linked `gui/controllers/*` at
// all. So the thirteen `.qml` files under `gui/qml/` and the six `QObject`s
// they bind by string have never been checked against each other. QML binds by
// string, which means a renamed `Q_INVOKABLE`, a `Connections` handler for a
// signal that no longer exists, or a property read resolving to `undefined` is
// not a compile error, not a test failure, and not a QML warning — the pane
// simply stays empty.
//
// This file is that check, and it is not hand-written: it points
// `morph::ladder::testkit::QmlSurfaceAudit`
// (`examples/common/testkit/qml_surface.hpp`) at the GUI's own QML and lets
// those files be the expectation, in both directions. See that header for what
// the audit does and does not cover.
//
// The alias mapping below is the one per-rung fact the audit cannot derive,
// and bank's is the simplest possible shape — for a reason worth stating,
// because it is not the shape the ladder rungs have. `gui/main.cpp` publishes
// each controller with `QQmlContext::setContextProperty`, not
// `setInitialProperties`: they are root-context names, visible under the same
// name in every one of the thirteen files, and no sub-view re-exposes one
// under a property of its own. So one `bind()` per controller covers every
// file, and no `bindIn()` is needed — unlike ledger, whose sub-views each call
// their own bridge plain `bridge`.
//
// bank is not a ladder rung (it is absent from `examples/rungs.txt` and never
// calls `morph_add_rung()`), so this binary is wired by hand in
// `examples/bank/CMakeLists.txt` rather than by the rung glob. It exists only
// in a `-DMORPH_BUILD_BANK_GUI=ON -DMORPH_BUILD_TESTS=ON` configure.

#include <QString>
#include <QStringList>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <utility>

#include "BankClient.hpp"
#include "controllers/AccountController.hpp"
#include "controllers/AppController.hpp"
#include "controllers/CardController.hpp"
#include "controllers/LoanController.hpp"
#include "controllers/PayeeController.hpp"
#include "controllers/TransactionController.hpp"
#include "testkit/qml_surface.hpp"

namespace {

using morph::ladder::testkit::QmlSurfaceAudit;

}  // namespace

TEST_CASE("Every bank controller exposes exactly the surface gui/qml binds, and nothing more",
          "[bank][gui][qml-surface]") {
    // A real BankClient, because every controller holds `BridgeHandler`s
    // constructed from one. Nothing here dispatches an action — the audit reads
    // metaobjects and text — but `BankClient`'s constructor runs the schema
    // migrations, so it needs a database like any other bank test does. Its own
    // file, not the one `bank_tests` shares, so the two binaries can run
    // concurrently.
    const auto dbPath = std::filesystem::temp_directory_path() / "morph_bank_qml_surface.db";
    bankgui::BankClient client{"DRIVER=SQLite3;Database=" + dbPath.string()};

    bankgui::AppController appController{client};
    bankgui::AccountController accountController{client};
    bankgui::TransactionController transactionController{client};
    bankgui::CardController cardController{client};
    bankgui::PayeeController payeeController{client};
    bankgui::LoanController loanController{client};

    QmlSurfaceAudit audit{QStringLiteral(MORPH_LADDER_SOURCE_ROOT "/examples/bank/gui/qml")};
    audit.bind(QStringLiteral("app"), appController);
    audit.bind(QStringLiteral("accounts"), accountController);
    audit.bind(QStringLiteral("txns"), transactionController);
    audit.bind(QStringLiteral("cards"), cardController);
    audit.bind(QStringLiteral("payees"), payeeController);
    audit.bind(QStringLiteral("loans"), loanController);

    // ── `refresh`, reached only through a `var` array ─────────────────────
    // Not a backlog and not a defect: this is the audit's documented blind
    // spot, met head on. `AppShell.qml` holds the five page controllers in
    //
    //     readonly property var controllers: [accounts, txns, cards, payees, loans]
    //
    // and refreshes the visible page with
    // `shell.controllers[shell.current].refresh()` (AppShell.qml:12-14). That
    // is dynamic member access — the scanner sees an index expression, never
    // the name `refresh` against an alias — so every one of these five is
    // reachable, exercised on every page switch, and invisible to a text scan.
    // `app` is not in that array and has no `refresh`, which is why only five
    // controllers appear here.
    //
    // Recorded rather than worked around: rewriting AppShell to a five-armed
    // switch purely so a scanner can see the call would be bending the rung
    // around its guard.
    const QString dynamic = QStringLiteral(
        "called dynamically via AppShell.qml's `controllers[current].refresh()`; "
        "dynamic member access is outside what a text scan can see");
    for (const char* alias : {"accounts", "txns", "cards", "payees", "loans"}) {
        audit.allowUnbound(QString::fromLatin1(alias), QStringLiteral("refresh"), dynamic);
    }

    // ── The pre-existing backlog, recorded rather than swallowed ──────────
    // The first run of this audit reported four members these six controllers
    // publish that no file under gui/qml/ binds — over and above the five
    // dynamic `refresh` calls above. The other direction was clean: no QML file
    // binds a name its controller lacks, so no screen is broken. Each is either
    // dead surface or a missing control, and deciding which is per-member work
    // this file does not do. They are listed here so the guard goes live now
    // and catches the *next* drift in either direction, with the backlog
    // itemised instead of hidden behind a lowered bar.
    //
    // The list is checked in both directions too: an exemption for a member
    // that has since been deleted, or one QML has since bound, fails this test
    // (testkit/qml_surface.hpp). It can only shrink deliberately.
    //
    // Same shape as ledger's (morph#239), lims' (morph#287) and kanban's
    // (morph#291).
    const QString backlog = QStringLiteral("unbound controller surface, tracked in morph#296");
    for (const auto& [alias, member] : std::initializer_list<std::pair<const char*, const char*>>{
             // `txns.selectAccount(id)` is called, but the property it writes
             // is never read back, so the account picker cannot reflect a
             // selection the controller made itself (TransactionController.cpp
             // auto-selects the first account on refresh).
             {"txns", "selectedAccount"},
             {"txns", "selectedChanged"},
             // Emitted on every deposit/withdraw/transfer, and on every bill
             // payment — no QML handles either.
             {"txns", "posted"},
             {"payees", "paid"},
         }) {
        audit.allowUnbound(QString::fromLatin1(alias), QString::fromLatin1(member), backlog);
    }

    const QStringList findings = audit.run();
    INFO(findings.join(QStringLiteral("\n")).toStdString());
    CHECK(findings.isEmpty());

    // The audit is only as good as the files it found: the thirteen gui/qml
    // ships, which is also the list gui/CMakeLists.txt hands qt_add_qml_module.
    CHECK(audit.scannedFiles() ==
          QStringList{QStringLiteral("AccountsPage.qml"), QStringLiteral("AppButton.qml"),
                      QStringLiteral("AppShell.qml"), QStringLiteral("CardsPage.qml"), QStringLiteral("Field.qml"),
                      QStringLiteral("LoansPage.qml"), QStringLiteral("Login.qml"), QStringLiteral("Main.qml"),
                      QStringLiteral("MoveMoneyPage.qml"), QStringLiteral("Panel.qml"),
                      QStringLiteral("PayeesPage.qml"), QStringLiteral("Picker.qml"), QStringLiteral("Pill.qml")});
}
