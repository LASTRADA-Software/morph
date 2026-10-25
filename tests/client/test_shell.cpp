// SPDX-License-Identifier: Apache-2.0
//
// The client shell's own part in what a screen shows: the width class it copies from its view backend into the
// session's `env`, and the top bar's head line it shows while the active tab's head has content.

// First, ahead of every header that brings view.hpp: GCC's -Wshadow reports value.hpp's enumerator `Type::Key` when
// view.hpp's `ui::Key` was declared before it.
// clang-format off
#include <morph/ui/value.hpp>
// clang-format on

#include <catch2/catch_test_macros.hpp>
#include <expected>
#include <memory>
#include <morph/client/shell.hpp>
#include <morph/client/source.hpp>
#include <morph/ui/service.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <testkit/fake_app_context.hpp>
#include <testkit/wait.hpp>
#include <utility>

namespace client = morph::client;
namespace ui = morph::ui;
using morph::examples::testing::FakeAppContext;
using morph::examples::testing::pumpUntil;

namespace {

/// A public screen that shows the width class it reads.
constexpr std::string_view kWidthScreen = R"({
  "vocab": "ui/1", "id": "probe.width",
  "view": { "kind": "text", "testId": "width",
            "text": { "fn": "coalesce", "args": [ { "env": "widthClass" }, "" ] } } })";

/// The bundle of that one screen.
/// @return It, or the refusal.
std::expected<std::shared_ptr<ui::ServedBundle const>, ui::Refusal> widthBundle() {
    ui::UiRegistry served{ui::ApplicationInfo{.id = "probe", .version = "1"}};
    ui::CatalogEntry entry;
    entry.id = "probe.width";
    entry.isPublic = true;
    if (auto added = served.addScreen(std::move(entry), std::string{kWidthScreen}); !added) {
        return std::unexpected(std::move(added.error()));
    }
    return served.build();
}

/// A screen with a head action, and one without a head.
constexpr std::string_view kHeaded = R"({
  "vocab": "ui/1", "id": "probe.headed",
  "actions": [ { "id": "refresh", "label": "Refresh", "onClick": [] } ],
  "view": { "kind": "text", "testId": "headed", "text": "headed" } })";
constexpr std::string_view kPlain = R"({
  "vocab": "ui/1", "id": "probe.plain",
  "view": { "kind": "text", "testId": "plain", "text": "plain" } })";

/// The bundle of those two screens, neither public, so the shell starts signed in with its navigator.
/// @return It, or the refusal.
std::expected<std::shared_ptr<ui::ServedBundle const>, ui::Refusal> headBundle() {
    ui::UiRegistry served{ui::ApplicationInfo{.id = "probe", .version = "1"}};
    for (auto const& [screenId, text] : {std::pair{"probe.headed", kHeaded}, std::pair{"probe.plain", kPlain}}) {
        ui::CatalogEntry entry;
        entry.id = screenId;
        entry.title = screenId;
        if (auto added = served.addScreen(std::move(entry), std::string{text}); !added) {
            return std::unexpected(std::move(added.error()));
        }
    }
    return served.build();
}

/// The text the screen's `width` text shows, or nothing before it is mounted.
std::optional<std::string> shownWidth(ui::testing::RecordingBackend const& backend) {
    auto const text = backend.find("Text", "testId", "width");
    if (!text) {
        return std::nullopt;
    }
    return backend.prop(*text, "text");
}

}  // namespace

// Mutation: drop the shell's setEnvironmentValue calls (the screen reads an empty text).
TEST_CASE("Client shell: a screen reads the backend's width class, and follows a change", "[client][shell]") {
    FakeAppContext context;
    ui::testing::RecordingBackend backend;
    backend.setWidthClass("compact");
    client::LocalSource source{client::LocalApp{.name = "probe", .bundle = &widthBundle}, client::LocalSetup{}};
    std::optional<std::expected<ui::Bundle, ui::ConnectError>> opened;
    source.open(context,
                [&opened](std::expected<ui::Bundle, ui::ConnectError> result) { opened = std::move(result); });
    REQUIRE(pumpUntil(context.owner(), [&opened] { return opened.has_value(); }));
    REQUIRE(opened->has_value());
    {
        auto const shell = client::mountShell(context, backend, std::move(*opened), source,
                                              client::ShellOptions{.startScreen = "probe.width"});
        REQUIRE(pumpUntil(context.owner(), [&backend] { return shownWidth(backend).has_value(); }));
        CHECK(shownWidth(backend) == "compact");
        backend.setWidthClass("medium");
        REQUIRE(pumpUntil(context.owner(), [&backend] { return shownWidth(backend) == "medium"; }));
    }
    source.close();
}

// Mutation: drop the shell's effect that shows the head line (it stays hidden with the headed screen active).
TEST_CASE("Client shell: the top bar's head line shows while the active tab's head has content", "[client][shell]") {
    FakeAppContext context;
    ui::testing::RecordingBackend backend;
    client::LocalSource source{client::LocalApp{.name = "probe", .bundle = &headBundle}, client::LocalSetup{}};
    std::optional<std::expected<ui::Bundle, ui::ConnectError>> opened;
    source.open(context,
                [&opened](std::expected<ui::Bundle, ui::ConnectError> result) { opened = std::move(result); });
    REQUIRE(pumpUntil(context.owner(), [&opened] { return opened.has_value(); }));
    REQUIRE(opened->has_value());
    {
        auto const shell = client::mountShell(context, backend, std::move(*opened), source,
                                              client::ShellOptions{.startScreen = "probe.headed"});
        auto const lineShown = [&backend] {
            auto const line = backend.find("Row", "testId", "shell_head_line");
            return line.has_value() && backend.prop(*line, "visible") == "true";
        };
        REQUIRE(
            pumpUntil(context.owner(), [&backend] { return backend.find("Text", "testId", "headed").has_value(); }));
        CHECK(pumpUntil(context.owner(), lineShown));
        auto const plain = backend.find("Button", "testId", "nav_probe.plain");
        REQUIRE(plain.has_value());
        backend.click(*plain);
        REQUIRE(
            pumpUntil(context.owner(), [&backend] { return backend.find("Text", "testId", "plain").has_value(); }));
        CHECK(pumpUntil(context.owner(), [&lineShown] { return !lineShown(); }));
    }
    source.close();
}
