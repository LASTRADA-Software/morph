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

        auto const plain = backend.find("Button", "testId", "nav_probe.plain");
        REQUIRE(plain.has_value());
        backend.click(*plain);
        CHECK(pumpUntil(context.owner(),
                        [&backend] { return propOf(backend, "Row", "shell_head_inline", "visible") == "false"; }));
    }
    source.close();
}
