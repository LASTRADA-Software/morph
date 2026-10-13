// SPDX-License-Identifier: Apache-2.0
//
// The gallery's render smoke (spec 4 §7 rule 4): every screen opened in the Qt Quick renderer, offscreen, through
// the generic client's sources and shell, in local mode and against a server. This executable owns no
// QGuiApplication of its own: the local case leaves it to the frontend, as a client's main does, and the remote case
// makes one first, because its server needs the event loop too. Under ctest each case runs in a process of its own: a
// second frontend run in one process logs a scene-graph warning, which the smoke counts as a failure.

#include <QGuiApplication>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QString>
#include <algorithm>
#include <app/app_source.hpp>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/core/executor.hpp>
#include <morph/core/remote.hpp>
#include <morph/qt/qt_websocket_server.hpp>
#include <morph/ui/service.hpp>
#include <string>
#include <testkit/render_smoke.hpp>
#include <vector>

#include "gallery/gallery.hpp"

using morph::examples::testing::runRenderSmoke;
using morph::examples::testing::SmokeReport;

namespace {

// QGuiApplication keeps a reference to the count, so both live as long as the process.
int& argCount() {
    static int count = 1;
    return count;
}

char** argValues() {
    static std::array<char, 24> name{"gallery_render_smoke"};
    static std::array<char*, 2> values{name.data(), nullptr};
    return values.data();
}

void show(SmokeReport const& report) {
    UNSCOPED_INFO("opened " << report.opened << ", exit " << report.exitCode << ", " << report.mounted.size() << " of "
                            << report.screens.size() << " screens mounted");
    for (std::string const& failure : report.failures) {
        UNSCOPED_INFO(failure);
    }
}

}  // namespace

TEST_CASE("gallery render smoke: local mode, every screen, offscreen", "[gallery][render-smoke]") {
    morph::examples::LocalAppSource source{gallery::localApp(), morph::examples::LocalSetup{}};
    SmokeReport const report = runRenderSmoke(argCount(), argValues(), source);
    CHECK(report.mounted == std::vector<std::string>{std::string{gallery::kHomeScreen}});
    // Just before the verdict: unscoped messages attach to the next assertion, so earlier they would ride on one
    // that passes and never be shown.
    show(report);
    CHECK(report.clean());
}

TEST_CASE("gallery render smoke: remote mode, against a server, offscreen", "[gallery][render-smoke]") {
    // What the frontend chooses when it makes the application itself, chosen here because this case makes it: both
    // are process-wide and fixed once Qt Quick has loaded.
    if (QQuickStyle::name().isEmpty()) {
        QQuickStyle::setStyle(QStringLiteral("Basic"));
    }
    if (QQuickWindow::graphicsApi() != QSGRendererInterface::Software) {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    }
    QGuiApplication const app{argCount(), argValues()};
    auto bundle = gallery::bundle();
    REQUIRE(bundle.has_value());
    morph::exec::ThreadPoolExecutor workers{2};
    morph::backend::ServerConfig config;
    config.services.push_back(std::make_shared<morph::ui::UiService>(*bundle));
    auto server = std::make_shared<morph::backend::RemoteServer>(workers, config);
    morph::qt::QtWebSocketServer socket{*server, 0};
    REQUIRE(socket.listen());
    morph::examples::RemoteAppSource source{"ws://127.0.0.1:" + std::to_string(static_cast<unsigned>(socket.port()))};
    SmokeReport const report = runRenderSmoke(argCount(), argValues(), source);
    CHECK(report.mounted == std::vector<std::string>{std::string{gallery::kHomeScreen}});
    // Just before the verdict: unscoped messages attach to the next assertion, so earlier they would ride on one
    // that passes and never be shown.
    show(report);
    CHECK(report.clean());
}

// Mutation: report clean() without checking that a screen mounted — this case passes a source with no screens.
TEST_CASE("render smoke: a source that opens nothing is not clean", "[gallery][render-smoke]") {
    morph::examples::LocalApp const empty{
        .name = "empty",
        .bundle =
            [] {
                morph::ui::UiRegistry const served{morph::ui::ApplicationInfo{.id = "empty", .version = "1"}};
                return served.build();
            },
        .setUp = {}};
    morph::examples::LocalAppSource source{empty, morph::examples::LocalSetup{}};
    SmokeReport const report = runRenderSmoke(argCount(), argValues(), source);
    CHECK(report.opened);
    CHECK(report.mounted.empty());
    CHECK_FALSE(report.clean());
}

// Mutation: drop the message handler's record — the planted warning is not reported.
TEST_CASE("render smoke: a Qt warning during the run is a failure", "[gallery][render-smoke]") {
    morph::examples::LocalApp warned = gallery::localApp();
    warned.setUp = [](morph::examples::LocalSetup const& /*setup*/) { qWarning("planted warning"); };
    morph::examples::LocalAppSource source{warned, morph::examples::LocalSetup{}};
    SmokeReport const report = runRenderSmoke(argCount(), argValues(), source);
    CHECK(report.mounted == std::vector<std::string>{std::string{gallery::kHomeScreen}});
    CHECK(std::ranges::find(report.failures, std::string{"Qt: planted warning"}) != report.failures.end());
    CHECK_FALSE(report.clean());
}
