// SPDX-License-Identifier: Apache-2.0
//
// The client shell's own part in what a screen shows: the width class it copies from its view backend into the
// session's `env`, and the top bar's head line it shows while the active tab's head has content.

// First, ahead of every header that brings view.hpp: GCC's -Wshadow reports value.hpp's enumerator `Type::Key` when
// view.hpp's `ui::Key` was declared before it.
// clang-format off
#include <morph/ui/value.hpp>
// clang-format on

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <expected>
#include <functional>
#include <memory>
#include <morph/client/shell.hpp>
#include <morph/client/source.hpp>
#include <morph/core/backend.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/wire.hpp>
#include <morph/session/session.hpp>
#include <morph/ui/chat/protocol.hpp>
#include <morph/ui/service.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <testkit/fake_app_context.hpp>
#include <testkit/wait.hpp>
#include <utility>
#include <vector>

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

/// A screen whose lead sits inline beside the title, and an app document that puts the principal in the head.
constexpr std::string_view kInline = R"({
  "vocab": "ui/1", "id": "probe.inline", "leadPlacement": "inline", "subtitle": "Sub",
  "view": { "kind": "text", "testId": "inline", "text": "inline" } })";
constexpr std::string_view kHeadPrincipalApp = R"({"vocab":"ui/1","id":"app","principalPlacement":"head"})";

/// The inline screen and the plain one, with the app document, neither public.
/// @return It, or the refusal.
std::expected<std::shared_ptr<ui::ServedBundle const>, ui::Refusal> placementBundle() {
    ui::UiRegistry served{ui::ApplicationInfo{.id = "probe", .version = "1"}};
    for (auto const& [screenId, text] : {std::pair{"probe.inline", kInline}, std::pair{"probe.plain", kPlain}}) {
        ui::CatalogEntry entry;
        entry.id = screenId;
        entry.title = screenId;
        if (auto added = served.addScreen(std::move(entry), std::string{text}); !added) {
            return std::unexpected(std::move(added.error()));
        }
    }
    if (auto added = served.addFile(
            ui::BundleFile{.kind = "app", .id = "app", .bytes = std::string{kHeadPrincipalApp}, .isPublic = true});
        !added) {
        return std::unexpected(std::move(added.error()));
    }
    return served.build();
}

/// A screen whose head asks for the headline role for its title.
constexpr std::string_view kHeadline = R"({
  "vocab": "ui/1", "id": "probe.headline", "title": "Instrument", "titleRole": "headline",
  "view": { "kind": "text", "testId": "headline", "text": "headline" } })";

/// The headline screen and the plain one, neither public.
/// @return It, or the refusal.
std::expected<std::shared_ptr<ui::ServedBundle const>, ui::Refusal> headlineBundle() {
    ui::UiRegistry served{ui::ApplicationInfo{.id = "probe", .version = "1"}};
    for (auto const& [screenId, text] : {std::pair{"probe.headline", kHeadline}, std::pair{"probe.plain", kPlain}}) {
        ui::CatalogEntry entry;
        entry.id = screenId;
        entry.title = screenId;
        if (auto added = served.addScreen(std::move(entry), std::string{text}); !added) {
            return std::unexpected(std::move(added.error()));
        }
    }
    return served.build();
}

/// A screen that asks for the white page surface.
constexpr std::string_view kPlainPage = R"({
  "vocab": "ui/1", "id": "probe.plainpage", "pageSurface": "plainPage",
  "view": { "kind": "text", "testId": "plainpage", "text": "plainpage" } })";

/// The plain-page screen and the plain one, neither public.
/// @return It, or the refusal.
std::expected<std::shared_ptr<ui::ServedBundle const>, ui::Refusal> pageSurfaceBundle() {
    ui::UiRegistry served{ui::ApplicationInfo{.id = "probe", .version = "1"}};
    for (auto const& [screenId, text] : {std::pair{"probe.plainpage", kPlainPage}, std::pair{"probe.plain", kPlain}}) {
        ui::CatalogEntry entry;
        entry.id = screenId;
        entry.title = screenId;
        if (auto added = served.addScreen(std::move(entry), std::string{text}); !added) {
            return std::unexpected(std::move(added.error()));
        }
    }
    return served.build();
}

/// A screen that asks the callee for an answer, in a dialog or as a tab, and shows what it got.
constexpr std::string_view kAsker = R"({
  "vocab": "ui/1", "id": "probe.asker", "title": "Asker",
  "state": { "got": { "init": "none" } },
  "view": { "kind": "column", "children": [
    { "kind": "text", "testId": "got", "text": { "ref": "got" } },
    { "kind": "button", "testId": "askDialog", "label": "ask in a dialog",
      "onClick": { "navigate": "probe.callee", "as": "dialog",
                   "onResult": [ { "set": "got", "value": { "ref": "event" } } ],
                   "onCancel": [ { "set": "got", "value": "cancelled" } ] } },
    { "kind": "button", "testId": "askTab", "label": "ask in a tab",
      "onClick": { "navigate": "probe.callee",
                   "onResult": [ { "set": "got", "value": { "ref": "event" } } ],
                   "onCancel": [ { "set": "got", "value": "cancelled" } ] } } ] } })";

/// The callee, a singleton: it answers `yes`.
constexpr std::string_view kCallee = R"({
  "vocab": "ui/1", "id": "probe.callee", "title": "Callee",
  "view": { "kind": "button", "testId": "answer", "label": "answer", "onClick": [ { "return": "yes" } ] } })";

/// The bundle of the asker and the callee, neither public.
/// @return It, or the refusal.
std::expected<std::shared_ptr<ui::ServedBundle const>, ui::Refusal> callBundle() {
    ui::UiRegistry served{ui::ApplicationInfo{.id = "probe", .version = "1"}};
    for (auto const& [screenId, text] : {std::pair{"probe.asker", kAsker}, std::pair{"probe.callee", kCallee}}) {
        ui::CatalogEntry entry;
        entry.id = screenId;
        entry.title = screenId;
        entry.singleton = screenId == std::string_view{"probe.callee"};
        if (auto added = served.addScreen(std::move(entry), std::string{text}); !added) {
            return std::unexpected(std::move(added.error()));
        }
    }
    return served.build();
}

/// An app document that counts `n` on the plain screen's entry while it is not 0.
constexpr std::string_view kCountingApp =
    R"({"vocab":"ui/1","id":"app","state":{"n":{"init":3}},"navigation":[)"
    R"({"screen":"probe.plain","text":{"fn":"format","args":["{0}",{"ref":"n"}]},"tone":"info",)"
    R"("visible":{"fn":"ne","args":[{"ref":"n"},0]}}]})";

/// A screen that sets the app document's `n`.
constexpr std::string_view kCounter = R"({
  "vocab": "ui/1", "id": "probe.counter",
  "view": { "kind": "column", "children": [
    { "kind": "button", "testId": "five", "label": "five", "onClick": { "set": "app.n", "value": 5 } },
    { "kind": "button", "testId": "none", "label": "none", "onClick": { "set": "app.n", "value": 0 } } ] } })";

/// The counter, the plain screen and the counting app document, no screen public.
/// @return It, or the refusal.
std::expected<std::shared_ptr<ui::ServedBundle const>, ui::Refusal> countingBundle() {
    ui::UiRegistry served{ui::ApplicationInfo{.id = "probe", .version = "1"}};
    for (auto const& [screenId, text] : {std::pair{"probe.counter", kCounter}, std::pair{"probe.plain", kPlain}}) {
        ui::CatalogEntry entry;
        entry.id = screenId;
        entry.title = screenId;
        if (auto added = served.addScreen(std::move(entry), std::string{text}); !added) {
            return std::unexpected(std::move(added.error()));
        }
    }
    if (auto added = served.addFile(
            ui::BundleFile{.kind = "app", .id = "app", .bytes = std::string{kCountingApp}, .isPublic = true});
        !added) {
        return std::unexpected(std::move(added.error()));
    }
    return served.build();
}

/// A screen whose title is runs that follow its state: a plate ID in the mono face, renamed by a button.
constexpr std::string_view kTitled = R"({
  "vocab": "ui/1", "id": "probe.titled", "title": "Plate PL-0193",
  "titleSpans": { "list": [ "Plate ", { "obj": { "text": { "ref": "plate" }, "mono": true } } ] },
  "state": { "plate": { "init": "PL-0193" } },
  "view": { "kind": "button", "testId": "rename", "label": "rename",
            "onClick": [ { "set": "plate", "value": "PL-0200" } ] } })";

/// The bundle of that one screen, not public, so the shell starts signed in.
/// @return It, or the refusal.
std::expected<std::shared_ptr<ui::ServedBundle const>, ui::Refusal> titledBundle() {
    ui::UiRegistry served{ui::ApplicationInfo{.id = "probe", .version = "1"}};
    ui::CatalogEntry entry;
    entry.id = "probe.titled";
    entry.title = "probe.titled";
    if (auto added = served.addScreen(std::move(entry), std::string{kTitled}); !added) {
        return std::unexpected(std::move(added.error()));
    }
    return served.build();
}

/// A screen with an eyebrow above its title.
constexpr std::string_view kEyebrowed = R"({
  "vocab": "ui/1", "id": "probe.eyebrowed", "title": "Results",
  "eyebrow": "Step 2 of 4",
  "view": { "kind": "text", "testId": "eyebrowed", "text": "eyebrowed" } })";

/// The bundle of that one screen, not public, so the shell starts signed in.
/// @return It, or the refusal.
std::expected<std::shared_ptr<ui::ServedBundle const>, ui::Refusal> eyebrowBundle() {
    ui::UiRegistry served{ui::ApplicationInfo{.id = "probe", .version = "1"}};
    ui::CatalogEntry entry;
    entry.id = "probe.eyebrowed";
    entry.title = "probe.eyebrowed";
    if (auto added = served.addScreen(std::move(entry), std::string{kEyebrowed}); !added) {
        return std::unexpected(std::move(added.error()));
    }
    return served.build();
}

/// A screen whose root lays itself at the page's edges, and the plain screen.
constexpr std::string_view kEdgeScreen = R"({
  "vocab": "ui/1", "id": "probe.edge",
  "view": { "kind": "column", "pageEdge": true, "children": [
    { "kind": "text", "testId": "edge", "text": "edge" } ] } })";

/// The bundle of those two screens, neither public, so the shell starts signed in with its navigator.
/// @return It, or the refusal.
std::expected<std::shared_ptr<ui::ServedBundle const>, ui::Refusal> edgeBundle() {
    ui::UiRegistry served{ui::ApplicationInfo{.id = "probe", .version = "1"}};
    for (auto const& [screenId, text] : {std::pair{"probe.edge", kEdgeScreen}, std::pair{"probe.plain", kPlain}}) {
        ui::CatalogEntry entry;
        entry.id = screenId;
        entry.title = screenId;
        if (auto added = served.addScreen(std::move(entry), std::string{text}); !added) {
            return std::unexpected(std::move(added.error()));
        }
    }
    return served.build();
}

/// A widget's property, or nothing while no widget of that kind has that test id.
// NOLINTBEGIN(bugprone-easily-swappable-parameters): a kind, a test id and a property name, as the backend reads them.
std::optional<std::string> propOf(ui::testing::RecordingBackend const& backend, std::string_view kind,
                                  std::string_view testId, std::string_view name) {
    auto const widget = backend.find(kind, "testId", testId);
    if (!widget) {
        return std::nullopt;
    }
    return backend.prop(*widget, name);
}
// NOLINTEND(bugprone-easily-swappable-parameters)

/// The workspace's tab captions, as its bar shows them.
std::string tabCaptions(ui::testing::RecordingBackend const& backend) {
    std::vector<int> const bars = backend.all("Tabs");
    return bars.empty() ? std::string{} : backend.prop(bars.front(), "tabs");
}

/// The text the screen's `width` text shows, or nothing before it is mounted.
std::optional<std::string> shownWidth(ui::testing::RecordingBackend const& backend) {
    auto const text = backend.find("Text", "testId", "width");
    if (!text) {
        return std::nullopt;
    }
    return backend.prop(*text, "text");
}

/// A public screen that shows the `env` values the shell writes besides the width class.
constexpr std::string_view kEnvScreen = R"({
  "vocab": "ui/1", "id": "probe.env",
  "view": { "kind": "column", "children": [
    { "kind": "text", "testId": "locale", "text": { "fn": "coalesce", "args": [ { "env": "locale" }, "null" ] } },
    { "kind": "text", "testId": "platform", "text": { "fn": "coalesce", "args": [ { "env": "platform" }, "null" ] } },
    { "kind": "text", "testId": "backend", "text": { "fn": "coalesce", "args": [ { "env": "backend" }, "null" ] } },
    { "kind": "text", "testId": "online", "text": { "if": [ { "env": "online" }, "on", "off" ] } } ] } })";

/// The bundle of that one screen.
/// @return It, or the refusal.
std::expected<std::shared_ptr<ui::ServedBundle const>, ui::Refusal> envBundle() {
    ui::UiRegistry served{ui::ApplicationInfo{.id = "probe", .version = "1"}};
    ui::CatalogEntry entry;
    entry.id = "probe.env";
    entry.isPublic = true;
    if (auto added = served.addScreen(std::move(entry), std::string{kEnvScreen}); !added) {
        return std::unexpected(std::move(added.error()));
    }
    return served.build();
}

/// A local backend whose connect and disconnect hooks the test fires, as a transport does.
class HookedBackend final : public morph::backend::LocalBackend {
public:
    using LocalBackend::LocalBackend;
    void setConnectHandler(std::function<void()> const& handler) override { *connect = handler; }
    void setDisconnectHandler(std::function<void()> const& handler) override { *disconnect = handler; }
    std::shared_ptr<std::function<void()>> connect = std::make_shared<std::function<void()>>();
    std::shared_ptr<std::function<void()>> disconnect = std::make_shared<std::function<void()>>();
};

/// The env screen's application behind a transport the test can drop and restore.
class DroppingSource final : public client::Source {
public:
    // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes): the test fires these.
    std::shared_ptr<std::function<void()>> connectHook;
    std::shared_ptr<std::function<void()>> disconnectHook;
    // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

    DroppingSource() = default;
    ~DroppingSource() override { close(); }
    DroppingSource(DroppingSource const&) = delete;
    DroppingSource& operator=(DroppingSource const&) = delete;
    DroppingSource(DroppingSource&&) = delete;
    DroppingSource& operator=(DroppingSource&&) = delete;

    /// @return `remote`, as a source over a network transport is.
    [[nodiscard]] std::string backendName() const override { return "remote"; }

protected:
    [[nodiscard]] std::expected<Transport, ui::ConnectError> connect(ui::AppContext& /*context*/) override {
        auto served = envBundle();
        REQUIRE(served.has_value());
        _service = std::make_shared<ui::UiService>(std::move(*served), morph::model::detail::defaultDispatcher());
        _workers = std::make_unique<morph::exec::ThreadPoolExecutor>(1);
        auto made = std::make_unique<HookedBackend>(*_workers);
        connectHook = made->connect;
        disconnectHook = made->disconnect;
        return Transport{
            .backend = std::move(made),
            .exchange = [this](morph::wire::Envelope const& request,
                               std::function<void(ui::ExchangeReply)> const& done) {
                done(morph::wire::decode(_service->handle(request, *morph::session::allowAllAuthorizer())));
            }};
    }
    void disconnect() override {
        _workers.reset();
        _service.reset();
    }

private:
    std::shared_ptr<ui::UiService> _service;
    std::unique_ptr<morph::exec::ThreadPoolExecutor> _workers;
};

/// A second screen, so a shell has two tabs to switch between.
constexpr std::string_view kOtherScreen = R"({
  "vocab": "ui/1", "id": "probe.other",
  "view": { "kind": "text", "testId": "other", "text": "Other" } })";

/// The env screen as one a signed-in person opens, so the frame shows its chrome.
std::expected<std::shared_ptr<ui::ServedBundle const>, ui::Refusal> signedInBundle() {
    ui::UiRegistry served{ui::ApplicationInfo{.id = "probe", .version = "1"}};
    ui::CatalogEntry entry;
    entry.id = "probe.env";
    if (auto added = served.addScreen(std::move(entry), std::string{kEnvScreen}); !added) {
        return std::unexpected(std::move(added.error()));
    }
    ui::CatalogEntry other;
    other.id = "probe.other";
    other.title = "Other";
    if (auto added = served.addScreen(std::move(other), std::string{kOtherScreen}); !added) {
        return std::unexpected(std::move(added.error()));
    }
    return served.build();
}

/// The env screen's application, with chat kinds answered by the test: `chatOffered` says whether the probe finds a
/// chat, and an end of chat waits for the test unless the exchange is failing.
class ChatSource final : public client::Source {
public:
    // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes): the test reads and sets these.
    bool chatOffered = true;
    bool failing = false;
    std::vector<std::string> kinds;
    std::vector<morph::wire::Envelope> turns;
    std::function<void(ui::ExchangeReply)> pendingEnd;
    // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

    ChatSource() = default;
    ~ChatSource() override { close(); }
    ChatSource(ChatSource const&) = delete;
    ChatSource& operator=(ChatSource const&) = delete;
    ChatSource(ChatSource&&) = delete;
    ChatSource& operator=(ChatSource&&) = delete;

protected:
    [[nodiscard]] std::expected<Transport, ui::ConnectError> connect(ui::AppContext& /*context*/) override {
        auto served = signedInBundle();
        REQUIRE(served.has_value());
        _service = std::make_shared<ui::UiService>(std::move(*served), morph::model::detail::defaultDispatcher());
        _workers = std::make_unique<morph::exec::ThreadPoolExecutor>(1);
        return Transport{
            .backend = std::make_unique<morph::backend::LocalBackend>(*_workers),
            .exchange = [this](morph::wire::Envelope const& request,
                               std::function<void(ui::ExchangeReply)> const& done) {
                kinds.push_back(request.kind);
                if (request.kind == ui::chat::kEndKind) {
                    if (failing) {
                        done(std::unexpected(std::string{"the connection is closed"}));
                    } else {
                        pendingEnd = done;
                    }
                } else if (request.kind == ui::chat::kStateKind) {
                    done(chatOffered ? morph::wire::makeOk(request.callId, "{}")
                                     : morph::wire::makeErr("unknown envelope kind", request.callId));
                } else if (request.kind == ui::chat::kTurnKind) {
                    turns.push_back(request);
                    done(morph::wire::makeOk(request.callId, R"({"turnId":1})"));
                } else {
                    done(morph::wire::decode(_service->handle(request, *morph::session::allowAllAuthorizer())));
                }
            }};
    }
    void disconnect() override {
        _workers.reset();
        _service.reset();
    }

private:
    std::shared_ptr<ui::UiService> _service;
    std::unique_ptr<morph::exec::ThreadPoolExecutor> _workers;
};

/// The text a test id shows, or an empty string while it is not mounted.
std::string shown(ui::testing::RecordingBackend const& backend, std::string_view testId) {
    return propOf(backend, "Text", testId, "text").value_or("");
}

}  // namespace

// Mutation: drop followConnection from the shell's start (every one of the four reads `null`).
TEST_CASE("Client shell: a screen reads the client's locale, platform, backend and connection state",
          "[client][shell]") {
    FakeAppContext context;
    ui::testing::RecordingBackend backend;
    client::LocalSource source{client::LocalApp{.name = "probe", .bundle = &envBundle}, client::LocalSetup{}};
    std::optional<std::expected<ui::Bundle, ui::ConnectError>> opened;
    source.open(context,
                [&opened](std::expected<ui::Bundle, ui::ConnectError> result) { opened = std::move(result); });
    REQUIRE(pumpUntil(context.owner(), [&opened] { return opened.has_value(); }));
    REQUIRE(opened->has_value());
    {
        auto const shell = client::mountShell(context, backend, std::move(*opened), source,
                                              client::ShellOptions{.startScreen = "probe.env"});
        REQUIRE(pumpUntil(context.owner(), [&backend] { return !shown(backend, "online").empty(); }));
        CHECK(shown(backend, "locale") == "en");
        CHECK(shown(backend, "backend") == "local");
        CHECK(shown(backend, "online") == "on");
        std::string const platform = shown(backend, "platform");
        CHECK((platform == "linux" || platform == "macos" || platform == "windows" || platform == "web" ||
               platform == "android"));
    }
    source.close();
}

// Mutation: drop the shell's onOnline listener (the screen keeps reading `on` after the drop).
TEST_CASE("Client shell: env online follows the transport's drop and reconnect", "[client][shell]") {
    FakeAppContext context;
    ui::testing::RecordingBackend backend;
    DroppingSource source;
    std::optional<std::expected<ui::Bundle, ui::ConnectError>> opened;
    source.open(context,
                [&opened](std::expected<ui::Bundle, ui::ConnectError> result) { opened = std::move(result); });
    REQUIRE(pumpUntil(context.owner(), [&opened] { return opened.has_value(); }));
    REQUIRE(opened->has_value());
    {
        auto const shell = client::mountShell(context, backend, std::move(*opened), source,
                                              client::ShellOptions{.startScreen = "probe.env"});
        REQUIRE(pumpUntil(context.owner(), [&backend] { return shown(backend, "online") == "on"; }));
        CHECK(shown(backend, "backend") == "remote");
        (*source.disconnectHook)();
        REQUIRE(pumpUntil(context.owner(), [&backend] { return shown(backend, "online") == "off"; }));
        CHECK_FALSE(source.online());
        (*source.connectHook)();
        REQUIRE(pumpUntil(context.owner(), [&backend] { return shown(backend, "online") == "on"; }));
    }
    source.close();
}

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

// Mutation: drop the shell's effect that shows the head slots (it stays hidden with the headed screen active).
TEST_CASE("Client shell: the top bar's head slots show while the active tab's head has content", "[client][shell]") {
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
            auto const line = backend.find("Row", "testId", "shell_head");
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

// Control: route `as` to the tab path in the shell's navigation (the bar gains a tab, and no dialog is drawn); or drop
// the host's onScreenClosed (the callee's tab stays after its return).
TEST_CASE("Client shell: a navigate as a dialog draws a dialog, and a screen that returns from a tab loses it",
          "[client][shell]") {
    FakeAppContext context;
    ui::testing::RecordingBackend backend;
    client::LocalSource source{client::LocalApp{.name = "probe", .bundle = &callBundle}, client::LocalSetup{}};
    std::optional<std::expected<ui::Bundle, ui::ConnectError>> opened;
    source.open(context,
                [&opened](std::expected<ui::Bundle, ui::ConnectError> result) { opened = std::move(result); });
    REQUIRE(pumpUntil(context.owner(), [&opened] { return opened.has_value(); }));
    REQUIRE(opened->has_value());
    std::vector<std::string> problems;
    {
        auto const shell = client::mountShell(
            context, backend, std::move(*opened), source,
            client::ShellOptions{.startScreen = "probe.asker",
                                 .onProblem = [&problems](std::string const& text) { problems.push_back(text); }});
        REQUIRE(pumpUntil(context.owner(), [&backend] { return propOf(backend, "Text", "got", "text") == "none"; }));
        backend.click(*backend.find("Button", "testId", "askDialog"));
        REQUIRE(pumpUntil(context.owner(), [&backend] { return !backend.all("Dialog").empty(); }));
        CHECK(backend.prop(backend.all("Dialog").front(), "open") == "true");
        CHECK(tabCaptions(backend) == "[Asker]");
        backend.click(*backend.find("Button", "testId", "answer"));
        REQUIRE(pumpUntil(context.owner(), [&backend] { return propOf(backend, "Text", "got", "text") == "yes"; }));
        CHECK(pumpUntil(context.owner(), [&backend] { return backend.all("Dialog").empty(); }));

        backend.click(*backend.find("Button", "testId", "askTab"));
        REQUIRE(pumpUntil(context.owner(), [&backend] { return tabCaptions(backend) == "[Asker,Callee]"; }));
        backend.click(*backend.find("Button", "testId", "answer"));
        CHECK(pumpUntil(context.owner(), [&backend] { return tabCaptions(backend) == "[Asker]"; }));
        CHECK(propOf(backend, "Text", "got", "text") == "yes");
    }
    source.close();
    CHECK(problems.empty());
}

// Control: drop the shell's settle of a navigation that only focused a tab (the asker reads `none`).
TEST_CASE("Client shell: a navigate with a call to a screen whose tab is open is cancelled, and reported",
          "[client][shell]") {
    FakeAppContext context;
    ui::testing::RecordingBackend backend;
    client::LocalSource source{client::LocalApp{.name = "probe", .bundle = &callBundle}, client::LocalSetup{}};
    std::optional<std::expected<ui::Bundle, ui::ConnectError>> opened;
    source.open(context,
                [&opened](std::expected<ui::Bundle, ui::ConnectError> result) { opened = std::move(result); });
    REQUIRE(pumpUntil(context.owner(), [&opened] { return opened.has_value(); }));
    REQUIRE(opened->has_value());
    std::vector<std::string> problems;
    {
        auto const shell = client::mountShell(
            context, backend, std::move(*opened), source,
            client::ShellOptions{.startScreen = "probe.callee",
                                 .onProblem = [&problems](std::string const& text) { problems.push_back(text); }});
        REQUIRE(pumpUntil(context.owner(), [&backend] { return tabCaptions(backend) == "[Callee]"; }));
        backend.click(*backend.find("Button", "testId", "nav_probe.asker"));
        REQUIRE(pumpUntil(context.owner(), [&backend] { return propOf(backend, "Text", "got", "text") == "none"; }));
        backend.click(*backend.find("Button", "testId", "askTab"));
        CHECK(
            pumpUntil(context.owner(), [&backend] { return propOf(backend, "Text", "got", "text") == "cancelled"; }));
        CHECK(tabCaptions(backend) == "[Callee,Asker]");
    }
    source.close();
    REQUIRE(problems.size() == 1);
    CHECK(problems.front().contains("navigate to probe.callee mounted no screen; its call was cancelled"));
}

// Control: compact the frame on `compact` only (the medium window keeps its sidebar).
TEST_CASE("Client shell: the frame is compact below an expanded window", "[client][shell]") {
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
                                              client::ShellOptions{.startScreen = "probe.plain"});
        auto const sidebar = [&backend] { return propOf(backend, "Row", "shell_sidebar", "visible"); };
        REQUIRE(
            pumpUntil(context.owner(), [&backend] { return backend.find("Text", "testId", "plain").has_value(); }));
        CHECK(sidebar() == "true");
        backend.setWidthClass("medium");
        CHECK(pumpUntil(context.owner(), [&sidebar] { return sidebar() == "false"; }));
        CHECK(propOf(backend, "Menu", "shell_nav_menu", "visible") == "true");
        backend.setWidthClass("expanded");
        CHECK(pumpUntil(context.owner(), [&sidebar] { return sidebar() == "true"; }));
        backend.setWidthClass("compact");
        CHECK(pumpUntil(context.owner(), [&sidebar] { return sidebar() == "false"; }));
    }
    source.close();
}

// Control: drop the shell's effect over the app root's navigation badges (no count is drawn).
TEST_CASE("Client shell: the app document's navigation counts follow its state on the navigator's entries",
          "[client][shell]") {
    FakeAppContext context;
    ui::testing::RecordingBackend backend;
    client::LocalSource source{client::LocalApp{.name = "probe", .bundle = &countingBundle}, client::LocalSetup{}};
    std::optional<std::expected<ui::Bundle, ui::ConnectError>> opened;
    source.open(context,
                [&opened](std::expected<ui::Bundle, ui::ConnectError> result) { opened = std::move(result); });
    REQUIRE(pumpUntil(context.owner(), [&opened] { return opened.has_value(); }));
    REQUIRE(opened->has_value());
    std::vector<std::string> problems;
    {
        auto const shell = client::mountShell(
            context, backend, std::move(*opened), source,
            client::ShellOptions{.startScreen = "probe.counter",
                                 .onProblem = [&problems](std::string const& text) { problems.push_back(text); }});
        auto const count = [&backend] { return propOf(backend, "Badge", "nav_badge_probe.plain", "text"); };
        auto const countShown = [&backend] { return propOf(backend, "Badge", "nav_badge_probe.plain", "visible"); };
        REQUIRE(pumpUntil(context.owner(), [&count] { return count() == "3"; }));
        CHECK(countShown() == "true");
        CHECK(propOf(backend, "Button", "nav_probe.plain", "a11yName") == "probe.plain, 3");
        backend.click(*backend.find("Button", "testId", "five"));
        CHECK(pumpUntil(context.owner(), [&count] { return count() == "5"; }));
        backend.click(*backend.find("Button", "testId", "none"));
        CHECK(pumpUntil(context.owner(), [&countShown] { return countShown() == "false"; }));
    }
    source.close();
    CHECK(problems.empty());
}

// Control: set the title's runs once in `showActiveTitle`, and the second check fails; drop the effect, and the first
// does.
TEST_CASE("Client shell: the top bar's title shows a screen's runs, and follows them without a tab switch",
          "[client][shell]") {
    FakeAppContext context;
    ui::testing::RecordingBackend backend;
    client::LocalSource source{client::LocalApp{.name = "probe", .bundle = &titledBundle}, client::LocalSetup{}};
    std::optional<std::expected<ui::Bundle, ui::ConnectError>> opened;
    source.open(context,
                [&opened](std::expected<ui::Bundle, ui::ConnectError> result) { opened = std::move(result); });
    REQUIRE(pumpUntil(context.owner(), [&opened] { return opened.has_value(); }));
    REQUIRE(opened->has_value());
    {
        auto const shell = client::mountShell(context, backend, std::move(*opened), source,
                                              client::ShellOptions{.startScreen = "probe.titled"});
        auto const runs = [&backend] { return propOf(backend, "Text", "shell_title", "spans"); };
        CHECK(pumpUntil(context.owner(), [&runs] { return runs() == "[Plate ,PL-0193;mono]"; }));
        backend.click(*backend.find("Button", "testId", "rename"));
        CHECK(pumpUntil(context.owner(), [&runs] { return runs() == "[Plate ,PL-0200;mono]"; }));
    }
    source.close();
}

// Control: never mount the eyebrow part (no `head_eyebrow`), or mount it in the lead slot (it is not in the eyebrow
// slot).
TEST_CASE("Client shell: a screen's eyebrow is mounted in the frame's eyebrow slot, above the title",
          "[client][shell]") {
    FakeAppContext context;
    ui::testing::RecordingBackend backend;
    client::LocalSource source{client::LocalApp{.name = "probe", .bundle = &eyebrowBundle}, client::LocalSetup{}};
    std::optional<std::expected<ui::Bundle, ui::ConnectError>> opened;
    source.open(context,
                [&opened](std::expected<ui::Bundle, ui::ConnectError> result) { opened = std::move(result); });
    REQUIRE(pumpUntil(context.owner(), [&opened] { return opened.has_value(); }));
    REQUIRE(opened->has_value());
    {
        auto const shell = client::mountShell(context, backend, std::move(*opened), source,
                                              client::ShellOptions{.startScreen = "probe.eyebrowed"});
        REQUIRE(pumpUntil(context.owner(),
                          [&backend] { return backend.find("Text", "testId", "head_eyebrow").has_value(); }));
        int const slot = backend.find("Column", "testId", "shell_head_eyebrow").value();
        int const eyebrow = backend.find("Text", "testId", "head_eyebrow").value();
        CHECK(pumpUntil(context.owner(), [&] { return backend.children(slot) == std::vector<int>{eyebrow}; }));
        CHECK(backend.prop(eyebrow, "spans") == "[Step 2 of 4]");
        CHECK(propOf(backend, "Column", "shell_head_eyebrow", "visible") == "true");
    }
    source.close();
}

// Control: drop the shell's effect over the page inset (the spacers stay shown for the pinned screen).
TEST_CASE(
    "Client shell: the page is uninset around a screen that insets itself, and inset again for one that does not",
    "[client][shell]") {
    FakeAppContext context;
    ui::testing::RecordingBackend backend;
    client::LocalSource source{client::LocalApp{.name = "probe", .bundle = &edgeBundle}, client::LocalSetup{}};
    std::optional<std::expected<ui::Bundle, ui::ConnectError>> opened;
    source.open(context,
                [&opened](std::expected<ui::Bundle, ui::ConnectError> result) { opened = std::move(result); });
    REQUIRE(pumpUntil(context.owner(), [&opened] { return opened.has_value(); }));
    REQUIRE(opened->has_value());
    {
        auto const shell = client::mountShell(context, backend, std::move(*opened), source,
                                              client::ShellOptions{.startScreen = "probe.edge"});
        REQUIRE(pumpUntil(context.owner(), [&backend] { return backend.find("Text", "testId", "edge").has_value(); }));
        int const page = backend.find("Column", "testId", "shell_page").value();
        auto const insetShown = [&backend, page] { return backend.prop(backend.children(page).at(0), "visible"); };
        CHECK(pumpUntil(context.owner(), [&insetShown] { return insetShown() == "false"; }));
        auto const plain = backend.find("Button", "testId", "nav_probe.plain");
        REQUIRE(plain.has_value());
        backend.click(*plain);
        REQUIRE(
            pumpUntil(context.owner(), [&backend] { return backend.find("Text", "testId", "plain").has_value(); }));
        CHECK(pumpUntil(context.owner(), [&insetShown] { return insetShown() == "true"; }));
    }
    source.close();
}

// Control: a client that never mounts `_headInline` (the lead stays in no slot), or never reads `principalPlacement`
// (the sidebar keeps the name).
TEST_CASE("Client shell: an inline lead mounts beside the title, and the app document can move the principal",
          "[client][shell]") {
    FakeAppContext context;
    ui::testing::RecordingBackend backend;
    client::LocalSource source{client::LocalApp{.name = "probe", .bundle = &placementBundle},
                               client::LocalSetup{.db = {}, .user = "Dr. A. Meyer"}};
    std::optional<std::expected<ui::Bundle, ui::ConnectError>> opened;
    source.open(context,
                [&opened](std::expected<ui::Bundle, ui::ConnectError> result) { opened = std::move(result); });
    REQUIRE(pumpUntil(context.owner(), [&opened] { return opened.has_value(); }));
    REQUIRE(opened->has_value());
    {
        auto const shell = client::mountShell(context, backend, std::move(*opened), source,
                                              client::ShellOptions{.startScreen = "probe.inline"});
        REQUIRE(pumpUntil(context.owner(),
                          [&backend] { return backend.find("Row", "testId", "head_lead_inline").has_value(); }));
        int const slot = backend.find("Row", "testId", "shell_head_inline").value();
        int const lead = backend.find("Row", "testId", "head_lead_inline").value();
        CHECK(pumpUntil(context.owner(), [&] { return backend.children(slot) == std::vector<int>{lead}; }));
        CHECK(propOf(backend, "Row", "shell_head_inline", "visible") == "true");
        CHECK(propOf(backend, "Row", "head_lead_inline", "visible") == "true");
        CHECK(propOf(backend, "Row", "head_lead", "visible") == "false");
        // One line: the top line's items are centred.
        CHECK(propOf(backend, "Row", "shell_topbar_main", "align") == "center");
        // The principal is the top bar's.
        CHECK(propOf(backend, "Text", "shell_head_principal", "text") == "Dr. A. Meyer");
        CHECK(propOf(backend, "Text", "shell_head_principal", "visible") == "true");
        CHECK(propOf(backend, "Text", "shell_principal", "visible") == "false");
        // It ends the head's trail, which shows with it.
        int const trail = backend.find("Row", "testId", "shell_head_trail").value();
        CHECK(backend.children(trail).back() == backend.find("Text", "testId", "shell_head_principal").value());
        CHECK(propOf(backend, "Row", "shell_head_trail", "visible") == "true");

        auto const plain = backend.find("Button", "testId", "nav_probe.plain");
        REQUIRE(plain.has_value());
        backend.click(*plain);
        CHECK(pumpUntil(context.owner(),
                        [&backend] { return propOf(backend, "Row", "shell_head_inline", "visible") == "false"; }));
    }
    source.close();
}

// Control: a client that never calls `setPageSurface`.
TEST_CASE("Client shell: a screen's page surface is the page area's, and the next screen restores page",
          "[client][shell]") {
    FakeAppContext context;
    ui::testing::RecordingBackend backend;
    client::LocalSource source{client::LocalApp{.name = "probe", .bundle = &pageSurfaceBundle},
                               client::LocalSetup{.db = {}, .user = "Dr. A. Meyer"}};
    std::optional<std::expected<ui::Bundle, ui::ConnectError>> opened;
    source.open(context,
                [&opened](std::expected<ui::Bundle, ui::ConnectError> result) { opened = std::move(result); });
    REQUIRE(pumpUntil(context.owner(), [&opened] { return opened.has_value(); }));
    REQUIRE(opened->has_value());
    {
        auto const shell = client::mountShell(context, backend, std::move(*opened), source,
                                              client::ShellOptions{.startScreen = "probe.plainpage"});
        REQUIRE(pumpUntil(context.owner(),
                          [&backend] { return propOf(backend, "Column", "shell_page", "surface") == "plainPage"; }));
        auto const plain = backend.find("Button", "testId", "nav_probe.plain");
        REQUIRE(plain.has_value());
        backend.click(*plain);
        CHECK(pumpUntil(context.owner(),
                        [&backend] { return propOf(backend, "Column", "shell_page", "surface") == "page"; }));
    }
    source.close();
}

// Control: a client that never calls `setTitleRole`, or one that calls it outside the head's layout effect, which a
// tab change recreates (the role stays `Headline` on the plain screen).
TEST_CASE("Client shell: a screen's title role is the top bar's title, and the next screen restores the heading",
          "[client][shell]") {
    FakeAppContext context;
    ui::testing::RecordingBackend backend;
    client::LocalSource source{client::LocalApp{.name = "probe", .bundle = &headlineBundle},
                               client::LocalSetup{.db = {}, .user = "Dr. A. Meyer"}};
    std::optional<std::expected<ui::Bundle, ui::ConnectError>> opened;
    source.open(context,
                [&opened](std::expected<ui::Bundle, ui::ConnectError> result) { opened = std::move(result); });
    REQUIRE(pumpUntil(context.owner(), [&opened] { return opened.has_value(); }));
    REQUIRE(opened->has_value());
    {
        auto const shell = client::mountShell(context, backend, std::move(*opened), source,
                                              client::ShellOptions{.startScreen = "probe.headline"});
        REQUIRE(pumpUntil(context.owner(),
                          [&backend] { return propOf(backend, "Text", "shell_title", "role") == "Headline"; }));
        auto const plain = backend.find("Button", "testId", "nav_probe.plain");
        REQUIRE(plain.has_value());
        backend.click(*plain);
        CHECK(pumpUntil(context.owner(),
                        [&backend] { return propOf(backend, "Text", "shell_title", "role") == "Heading"; }));
        auto const headline = backend.find("Button", "testId", "nav_probe.headline");
        REQUIRE(headline.has_value());
        backend.click(*headline);
        CHECK(pumpUntil(context.owner(),
                        [&backend] { return propOf(backend, "Text", "shell_title", "role") == "Headline"; }));
    }
    source.close();
}

namespace {

// A shell on a ChatSource, opened on the env screen.
struct ChatRig {
    FakeAppContext context;
    ui::testing::RecordingBackend backend;
    ChatSource source;
    std::unique_ptr<ui::MountedShell> shell;

    explicit ChatRig(bool offered = true) {
        source.chatOffered = offered;
        std::optional<std::expected<ui::Bundle, ui::ConnectError>> opened;
        source.open(context,
                    [&opened](std::expected<ui::Bundle, ui::ConnectError> result) { opened = std::move(result); });
        REQUIRE(pumpUntil(context.owner(), [&opened] { return opened.has_value(); }));
        REQUIRE(opened->has_value());
        shell = client::mountShell(context, backend, std::move(*opened), source,
                                   client::ShellOptions{.startScreen = "probe.env"});
        REQUIRE(pumpUntil(context.owner(), [this] { return shown(backend, "online") == "on"; }));
        // Lets a probe's answer reach the frame.
        static_cast<void>(pumpUntil(context.owner(), [] { return false; }, std::chrono::milliseconds{20}));
    }

    ~ChatRig() {
        shell.reset();
        source.close();
    }
    ChatRig(ChatRig const&) = delete;
    ChatRig& operator=(ChatRig const&) = delete;
    ChatRig(ChatRig&&) = delete;
    ChatRig& operator=(ChatRig&&) = delete;

    [[nodiscard]] bool panelShown() const {
        auto const chat = backend.find("Row", "testId", "shell_chat");
        return chat.has_value() && backend.prop(*chat, "visible") != "false";
    }
};

}  // namespace

// Mutation: show the slot whatever the probe said.
TEST_CASE("the_shell_shows_the_panel_only_when_the_server_has_a_chat", "[client][chat]") {
    ChatRig const offered;
    CHECK(offered.panelShown());
    ChatRig const refused{false};
    CHECK_FALSE(refused.panelShown());
}

// Mutation: send the panel's context from the shell without the active tab's screen.
TEST_CASE("sending_from_the_panel_carries_the_open_screen", "[client][chat]") {
    ChatRig rig;
    auto const input = rig.backend.find("TextInput", "testId", "chat_input");
    REQUIRE(input.has_value());
    rig.backend.submit(*input, "add ignition test");
    REQUIRE(rig.source.turns.size() == 1);
    auto const body = ui::chat::readTurnRequest(rig.source.turns.front().body);
    REQUIRE(body.has_value());
    CHECK(body->screenId == "probe.env");
    CHECK(body->text == "add ignition test");
}

// Mutation: send the end of chat after the sign-out's reload.
TEST_CASE("signing_out_sends_end_of_chat_first", "[client][chat]") {
    ChatRig rig;
    auto const button = rig.backend.find("Button", "testId", "shell_signOut");
    REQUIRE(button.has_value());
    rig.source.kinds.clear();
    rig.backend.click(*button);
    static_cast<void>(pumpUntil(rig.context.owner(), [] { return false; }, std::chrono::milliseconds{20}));
    REQUIRE_FALSE(rig.source.kinds.empty());
    CHECK(rig.source.kinds.front() == ui::chat::kEndKind);
    // Nothing else goes until the server has answered it.
    CHECK(rig.source.kinds.size() == 1);
    REQUIRE(rig.source.pendingEnd);
    rig.source.pendingEnd(morph::wire::makeOk(0));
    CHECK(pumpUntil(rig.context.owner(), [&] { return rig.source.kinds.size() > 1; }));
}

// Mutation: wait for an answer only when the exchange succeeds.
TEST_CASE("signing_out_does_not_hang_on_a_lost_connection", "[client][chat]") {
    ChatRig rig;
    auto const button = rig.backend.find("Button", "testId", "shell_signOut");
    REQUIRE(button.has_value());
    rig.source.failing = true;
    rig.source.kinds.clear();
    rig.backend.click(*button);
    CHECK(pumpUntil(rig.context.owner(), [&] { return rig.source.kinds.size() > 1; }));
    CHECK(rig.source.kinds.front() == ui::chat::kEndKind);
}

// Mutation: leave the conversation selected when the active tab changes.
TEST_CASE("switching_tabs_switches_the_conversation", "[client][chat]") {
    ChatRig rig;
    auto const input = rig.backend.find("TextInput", "testId", "chat_input");
    REQUIRE(input.has_value());
    rig.backend.submit(*input, "on the first screen");
    REQUIRE(
        pumpUntil(rig.context.owner(), [&] { return rig.backend.find("Text", "testId", "chat_entry").has_value(); }));
    auto const other = rig.backend.find("Button", "label", "Other");
    REQUIRE(other.has_value());
    rig.backend.click(*other);
    REQUIRE(pumpUntil(rig.context.owner(), [&] { return !shown(rig.backend, "other").empty(); }));
    CHECK(
        pumpUntil(rig.context.owner(), [&] { return !rig.backend.find("Text", "testId", "chat_entry").has_value(); }));
}
