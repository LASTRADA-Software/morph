// SPDX-License-Identifier: Apache-2.0
//
/// @file
/// ledger's desktop client shell: one `AppContext` (deployment mode chosen
/// from argv), a login step (morph#242) before any domain bridge is usable,
/// the four QML bridges Tasks 18-21 define built inside `ctx.onReady()`, and
/// a `QQmlApplicationEngine` loading this rung's own QML module.
///
/// Bridges are passed as *initial properties*, not context properties, so the
/// same `Main.qml` also loads with nothing wired -- which is exactly what the
/// offscreen engine-load smoke test does. Same convention, and same reason, as
/// kanban's own `main.cpp`.
///
/// @par Login, and why the two modes differ
/// Every mutating `LedgerModel`/`BudgetModel`/`RuleModel` action refuses an
/// empty principal (design spec §11, `EmptyPrincipalError`), so a bridge
/// built against a bare, unauthenticated session cannot do anything useful.
///
///  - **Local mode** runs no authorizer at all (`LocalBackend`), so
///    `AppContext::login()` -- a principal with no token -- is exactly what
///    `bank::gui::AppController::adopt()` installs for its own single-user
///    deployment, and it is sufficient here for the identical reason: nothing
///    ever verifies it. A fixed, hardcoded principal is appropriate for
///    exactly the same reason main.cpp's Local-mode `TokenIssuer` secret is
///    hardcoded in bookmarks'/kanban's own `main.cpp` (see either file's
///    comment) -- there is no real user to distinguish in a single-process
///    developer convenience deployment.
///  - **Remote mode** talks to a `ladder_ledger_server` running a real
///    `LedgerAuthorizer`, which -- per `docs/spec/security.md` -- clears any
///    principal it cannot verify. `AppContext::login()` alone (no token)
///    would therefore be silently discarded on the first dispatch. This file
///    performs the same dispatched `Login` a schema-driven login screen would
///    (`bookmarks::gui::FormsBridge::onLoginSucceeded`'s identical shape),
///    just without a screen of its own: ledger's QML is hand-built rather
///    than schema-driven (`gui/qml/*.qml` have no `MorphForms` dependency), so
///    there is no `DynamicForm` to render `Login`'s one field against. A
///    fixed dev-mode username is used for the same reason the Local-mode
///    secret above is fixed: this demonstrates the auth *plumbing* working
///    end-to-end, the same property `AuthModel::execute(const Login&)`'s own
///    doc comment states about dev-mode login generally -- a real deployment
///    replaces the username source (a login prompt) without touching anything
///    downstream of it.

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QString>
#include <QUrl>
#include <QVariant>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/session/session.hpp>
#include <optional>
#include <string>

#include "budget_qml_bridge.hpp"
#include "gui/app_context.hpp"
#include "ledger/models/auth_model.hpp"
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

/// @brief Dev-mode principal this shell logs in as. See this file's `@file`
///        comment ("Login, and why the two modes differ") for why a fixed
///        value is the right choice here, in both modes.
constexpr auto kDevPrincipal = "demo";

/// @brief Builds the four QML bridges against @p ctx and loads `Main.qml`.
///        Runs once the session (Local: a bare principal; Remote: a verified
///        token) is already installed on @p ctx's bridge.
/// @param ctx    The ready `AppContext` to build every bridge against.
/// @param engine The engine to load `Main.qml` into.
/// @param ledgerBridge Out parameter: kept alive by the caller.
/// @param budgetBridge Out parameter: kept alive by the caller.
/// @param ruleBridge   Out parameter: kept alive by the caller.
/// @param reportBridge Out parameter: kept alive by the caller.
void buildBridgesAndLoadUi(::morph::ladder::gui::AppContext& ctx, QQmlApplicationEngine& engine,
                           std::unique_ptr<ledger::gui::LedgerQmlBridge>& ledgerBridge,
                           std::unique_ptr<ledger::gui::BudgetQmlBridge>& budgetBridge,
                           std::unique_ptr<ledger::gui::RuleQmlBridge>& ruleBridge,
                           std::unique_ptr<ledger::gui::ReportQmlBridge>& reportBridge) {
    // All four are built once here and live until the process exits. Nothing
    // is torn down and rebuilt around login: a login only installs a session
    // on the shared `Bridge`, before this function ever runs.
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
    // Kept alive only long enough for the Remote-mode Login dispatch below to
    // settle; the domain bridges above never reuse it.
    std::unique_ptr<::morph::bridge::BridgeHandler<ledger::AuthModel>> authHandler;

    ctx.onReady([&] {
        if (!serverUrl) {
            // Local mode: LocalBackend runs no authorizer, so a bare
            // principal is the whole session -- see this file's @file
            // comment.
            ctx.login(kDevPrincipal);
            buildBridgesAndLoadUi(ctx, engine, ledgerBridge, budgetBridge, ruleBridge, reportBridge);
            return;
        }

        // Remote mode: dispatch the real Login action and install the
        // server-verified token before building anything that depends on it.
        // See this file's @file comment for why this is a direct dispatch
        // rather than a login screen.
        authHandler = std::make_unique<::morph::bridge::BridgeHandler<ledger::AuthModel>>(ctx.bridge(), ctx.executor());
        authHandler->execute(ledger::Login{.username = kDevPrincipal})
            .then([&](ledger::LoginResult result) {
                morph::session::Context session;
                session.principal = result.principal;
                session.token = result.token.hasValue() ? *result.token : std::string{};
                ctx.bridge().setDefaultSession(session);
                authHandler.reset();
                buildBridgesAndLoadUi(ctx, engine, ledgerBridge, budgetBridge, ruleBridge, reportBridge);
            })
            .onError([&](const std::exception_ptr&) {
                qWarning("ladder_ledger_gui: Login failed against %s", qUtf8Printable(serverUrl->toString()));
                authHandler.reset();
                QCoreApplication::exit(1);
            });
    });

    if (serverUrl) {
        qInfo("ladder_ledger_gui: connecting to %s ...", qUtf8Printable(serverUrl->toString()));
    }
    return QGuiApplication::exec();
}
