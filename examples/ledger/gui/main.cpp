// SPDX-License-Identifier: Apache-2.0
//
/// @file
/// ledger's desktop client shell: one `AppContext` (deployment mode chosen
/// from argv), the four QML bridges Tasks 18-21 define built inside
/// `ctx.onReady()`, and a `QQmlApplicationEngine` loading this rung's own QML
/// module.
///
/// Bridges are passed as *initial properties*, not context properties, so the
/// same `Main.qml` also loads with nothing wired -- which is exactly what the
/// offscreen engine-load smoke test does. Same convention, and same reason, as
/// kanban's own `main.cpp`.

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QString>
#include <QUrl>
#include <QVariant>
#include <memory>
#include <optional>

#include "budget_qml_bridge.hpp"
#include "gui/app_context.hpp"
#include "ledger_qml_bridge.hpp"
#include "report_qml_bridge.hpp"
#include "rule_qml_bridge.hpp"

namespace {

/// @brief The `--server <url>` argument, when present.
/// @param argc Argument count.
/// @param argv Argument vector.
/// @return The parsed URL, or `std::nullopt` for Local mode.
[[nodiscard]] std::optional<QUrl> serverUrlFrom(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (QString::fromUtf8(argv[i]) == QStringLiteral("--server")) {
            return QUrl{QString::fromUtf8(argv[i + 1])};
        }
    }
    return std::nullopt;
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app{argc, argv};
    const auto serverUrl = serverUrlFrom(argc, argv);

    // Mirrors AppContext's own construction pattern: pick the mode, then build
    // every bridge from inside onReady() -- a Remote context is not usable
    // until its socket is up.
    ::morph::ladder::gui::AppContext ctx{
        serverUrl ? ::morph::ladder::gui::AppContext::Mode{::morph::ladder::gui::Remote{.url = *serverUrl}}
                  : ::morph::ladder::gui::AppContext::Mode{::morph::ladder::gui::Local{.workers = 4}}};

    QQmlApplicationEngine engine;
    std::unique_ptr<ledger::gui::LedgerQmlBridge> ledgerBridge;
    std::unique_ptr<ledger::gui::BudgetQmlBridge> budgetBridge;
    std::unique_ptr<ledger::gui::RuleQmlBridge> ruleBridge;
    std::unique_ptr<ledger::gui::ReportQmlBridge> reportBridge;

    ctx.onReady([&] {
        // All four are built once here and live until the process exits.
        // Nothing is torn down and rebuilt around login: a login only installs
        // a session on the shared `Bridge`.
        ledgerBridge = std::make_unique<ledger::gui::LedgerQmlBridge>(ctx.bridge(), ctx.executor());
        budgetBridge = std::make_unique<ledger::gui::BudgetQmlBridge>(ctx.bridge(), ctx.executor());
        ruleBridge = std::make_unique<ledger::gui::RuleQmlBridge>(ctx.bridge(), ctx.executor());
        reportBridge = std::make_unique<ledger::gui::ReportQmlBridge>(ctx.bridge(), ctx.executor());

        engine.setInitialProperties({
            {QStringLiteral("ledgerBridge"), QVariant::fromValue(ledgerBridge.get())},
            {QStringLiteral("budgetBridge"), QVariant::fromValue(budgetBridge.get())},
            {QStringLiteral("ruleBridge"), QVariant::fromValue(ruleBridge.get())},
            {QStringLiteral("reportBridge"), QVariant::fromValue(reportBridge.get())},
        });
        engine.loadFromModule(MORPH_LADDER_QML_URI, "Main");
        if (engine.rootObjects().isEmpty()) {
            qWarning("ladder_ledger_gui: QML engine produced no root object");
            QCoreApplication::exit(1);
        }
    });

    if (serverUrl) {
        qInfo("ladder_ledger_gui: connecting to %s ...", qUtf8Printable(serverUrl->toString()));
    }
    return QGuiApplication::exec();
}
