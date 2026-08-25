// SPDX-License-Identifier: Apache-2.0
//
// The QML-visible surface of all six bank GUI controllers, audited against
// `gui/qml/` itself.
//
// TEMPORARY BASELINE FORM: no exemptions, so the first run prints the real
// backlog. Replaced by the recorded form once the findings are known.

#include <QString>
#include <QStringList>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <string>

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

TEST_CASE("bank baseline", "[bank][gui][qml-surface]") {
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

    const QStringList findings = audit.run();
    INFO(findings.join(QStringLiteral("\n")).toStdString());
    CHECK(findings.isEmpty());
    CHECK(audit.scannedFiles() == QStringList{});
}
