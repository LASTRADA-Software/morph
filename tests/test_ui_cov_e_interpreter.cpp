// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <morph/ui/interpreter.hpp>
#include <morph/ui/value.hpp>
#include <string>
#include <string_view>
#include <utility>

#include "ui_interpreter_support.hpp"

namespace ui = morph::ui;
using ui::testing::Harness;

namespace {

constexpr char const* kApp = R"({
  "vocab": "ui/1", "id": "app", "principalPlacement": "head",
  "models": { "clients": { "type": "Lims_ClientModel" }, "sample": { "type": "Lims_SampleModel" } },
  "state": { "tenant": { "init": "north" } },
  "queries": { "clientList": { "model": "clients", "action": "Lims_ClientList" } },
  "mutations": { "attach": { "model": "sample", "action": "Lims_Attach", "body": { "obj": { "kind": "csv" } } } } })";

std::unique_ptr<ui::AppRoot> mountApp(Harness& h, char const* text = kApp) {
    auto root = h.interpreter.mountApp(ui::testing::loadOrFail(text, ui::testing::corpusCatalog()));
    REQUIRE(root.has_value());
    h.settle();
    return std::move(*root);
}

}  // namespace

// Mutation: make `principalPlacement` always answer `Sidebar`.
TEST_CASE("cove: the app root reports where the document puts the principal", "[ui][interpreter][cove]") {
    Harness h;
    auto const root = mountApp(h);
    CHECK(root->principalPlacement() == ui::PrincipalPlacement::Head);
    Harness plain;
    auto const sidebar = mountApp(plain, R"({ "vocab": "ui/1", "id": "app" })");
    CHECK(sidebar->principalPlacement() == ui::PrincipalPlacement::Sidebar);
}

// Mutation: make `query` report any name as declared, or `read` evaluate an unparsed expression.
TEST_CASE("cove: the app root answers only for the names it declares", "[ui][interpreter][cove]") {
    Harness h;
    auto const root = mountApp(h);
    CHECK_FALSE(root->query("absent").declared);
    CHECK_FALSE(root->query("tenant").declared);
    CHECK_FALSE(root->query("attach").declared);
    ui::AppRoot::QueryRead const clients = root->query("clientList");
    CHECK(clients.declared);
    CHECK(clients.pending);
    h.reply(h.dispatcher.last("Lims_ClientList"), R"([{"id": 1}])");
    CHECK_FALSE(root->query("clientList").pending);
    CHECK(root->query("clientList").value.asList() != nullptr);

    auto const unparsed = ui::parseJson(R"({"fn": "noSuchFunction", "args": []})");
    REQUIRE(unparsed.has_value());
    CHECK(root->read(*unparsed).isNull());

    CHECK_FALSE(root->setState("absent", ui::Value{1}));
    CHECK_FALSE(root->setState("clientList", ui::Value{1}));
    CHECK(root->setState("tenant", ui::Value{"south"}));
    auto const tenant = ui::parseJson(R"({"ref": "tenant"})");
    REQUIRE(tenant.has_value());
    CHECK(*root->read(*tenant).asString() == "south");
}

// Mutation: make `runMutation` accept a name that is not a mutation, or call `done` with the wrong outcome.
TEST_CASE("cove: the app root runs only its mutations, and says how the run settled", "[ui][interpreter][cove]") {
    Harness h;
    auto const root = mountApp(h);
    CHECK_FALSE(root->runMutation("absent", ui::Value{}));
    CHECK_FALSE(root->runMutation("tenant", ui::Value{}));
    CHECK(h.dispatcher.count("Lims_Attach") == 0);

    CHECK(root->runMutation("attach", ui::Value{}));
    h.settle();
    REQUIRE(h.dispatcher.count("Lims_Attach") == 1);
    h.reply(h.dispatcher.last("Lims_Attach"), "{}");

    int succeeded = 0;
    int failed = 0;
    CHECK(root->runMutation("attach", ui::Value{}, [&](bool ok) { ++(ok ? succeeded : failed); }));
    h.settle();
    h.reply(h.dispatcher.last("Lims_Attach"), "{}");
    CHECK(succeeded == 1);
    CHECK(root->runMutation("attach", ui::Value{}, [&](bool ok) { ++(ok ? succeeded : failed); }));
    h.settle();
    h.fail(h.dispatcher.last("Lims_Attach"));
    CHECK(failed == 1);
    CHECK(succeeded == 1);
}

namespace {

constexpr char const* kHead = R"({
  "vocab": "ui/1", "id": "head.actions", "state": { "ran": { "init": 0 } },
  "menuLabel": "Tools",
  "actions": [
    { "id": "run", "label": "Run", "onClick": [ { "set": "ran", "value": 1 } ] },
    { "id": "frozen", "label": "Frozen", "enabled": false },
    { "id": "gone", "label": "Gone", "visible": false } ],
  "view": { "kind": "text", "testId": "ran", "text": { "ref": "ran" } } })";

}  // namespace

// Mutation: run a head action that is hidden or disabled, or report the wrong reason for it.
TEST_CASE("cove: a head action runs only when it is shown and enabled, and the refusal says which",
          "[ui][interpreter][cove]") {
    Harness h;
    auto const screen = h.mount(kHead);
    CHECK_FALSE(screen->runAction("nothing"));
    CHECK_FALSE(screen->runAction("frozen"));
    CHECK_FALSE(screen->runAction("gone"));
    REQUIRE(h.diagnostics.size() == 3);
    CHECK(h.diagnostics.at(0).reason == "head action nothing is unknown");
    CHECK(h.diagnostics.at(1).reason == "head action frozen is disabled");
    CHECK(h.diagnostics.at(2).reason == "head action gone is hidden");
    CHECK(h.text("ran") == "0");
    CHECK(screen->runAction("run"));
    h.settle();
    CHECK(h.text("ran") == "1");
}

// Mutation: ignore the document's `menuLabel`, or drop the default.
TEST_CASE("cove: the head menu's trigger is named by the document or by default", "[ui][interpreter][cove]") {
    Harness h;
    CHECK(h.mount(kHead)->menuLabel() == "Tools");
    Harness plain;
    CHECK(plain.mount(R"({ "vocab": "ui/1", "id": "plain", "view": { "kind": "text", "text": "x" } })")->menuLabel() ==
          "More actions");
}

namespace {

constexpr char const* kLive = R"({
  "vocab": "ui/1", "id": "live",
  "models": { "clients": { "type": "Lims_ClientModel" } },
  "state": { "go": { "init": false }, "term": { "init": "a" } },
  "queries": { "found": { "model": "clients", "action": "Lims_ClientList", "when": { "ref": "go" },
                          "body": { "obj": { "q": { "ref": "term" } } }, "debounce": 100, "refreshEvery": 1000 } },
  "view": { "kind": "column", "children": [
    { "kind": "button", "testId": "go", "label": "go", "onClick": { "set": "go", "value": true } },
    { "kind": "button", "testId": "stop", "label": "stop", "onClick": { "set": "go", "value": false } },
    { "kind": "button", "testId": "term", "label": "term", "onClick": { "set": "term", "value": "b" } },
    { "kind": "text", "testId": "n", "text": { "fn": "format", "args": [ "{0}", { "fn": "count", "args": [
        { "fn": "coalesce", "args": [ { "ref": "found" }, { "list": [] } ] } ] } ] } } ] } })";

}  // namespace

// Mutation: ignore `when`, the debounce or the refresh timer in `QueryRuntime::start`.
TEST_CASE("cove: a query waits for its condition, debounces a changed key and refreshes on its timer",
          "[ui][interpreter][cove]") {
    Harness h;
    auto const screen = h.mount(kLive);
    CHECK(h.dispatcher.count("Lims_ClientList") == 0);
    h.advance(std::chrono::milliseconds{5000});
    CHECK(h.dispatcher.count("Lims_ClientList") == 0);

    h.backend.click(h.byTestId("Button", "go"));
    h.settle();
    CHECK(h.dispatcher.count("Lims_ClientList") == 0);  // the key left idle: the debounce applies
    h.advance(std::chrono::milliseconds{100});
    REQUIRE(h.dispatcher.count("Lims_ClientList") == 1);
    h.advance(std::chrono::milliseconds{1000});  // the first request is still in flight: no refresh
    CHECK(h.dispatcher.count("Lims_ClientList") == 1);
    h.reply(h.dispatcher.last("Lims_ClientList"), R"([{"id": 1}])");
    CHECK(h.text("n") == "1");

    h.backend.click(h.byTestId("Button", "term"));
    h.settle();
    CHECK(h.dispatcher.count("Lims_ClientList") == 1);  // waiting out the debounce
    h.advance(std::chrono::milliseconds{50});
    CHECK(h.dispatcher.count("Lims_ClientList") == 1);
    h.advance(std::chrono::milliseconds{60});
    REQUIRE(h.dispatcher.count("Lims_ClientList") == 2);
    h.reply(h.dispatcher.last("Lims_ClientList"), R"([{"id": 1}, {"id": 2}])");
    CHECK(h.text("n") == "2");

    h.advance(std::chrono::milliseconds{1000});  // idle and unchanged: the timer fetches again
    CHECK(h.dispatcher.count("Lims_ClientList") == 3);

    h.backend.click(h.byTestId("Button", "stop"));  // an idle key switches at once, with no debounce
    h.settle();
    h.reply(h.dispatcher.last("Lims_ClientList"), R"([{"id": 1}])");
    h.advance(std::chrono::milliseconds{5000});
    CHECK(h.dispatcher.count("Lims_ClientList") == 3);
}

namespace {

constexpr char const* kReady = R"({
  "vocab": "ui/1", "id": "ready", "state": { "fine": { "init": false } },
  "view": { "kind": "column", "children": [
    { "kind": "button", "testId": "open", "label": "open", "onClick": { "open": "dlg" } },
    { "kind": "button", "testId": "fine", "label": "fine", "onClick": { "set": "fine", "value": true } },
    { "kind": "text", "testId": "ready", "text": { "fn": "format", "args": [ "{0}", { "ref": "dlg.ready" } ] } },
    { "kind": "dialog", "id": "dlg", "title": "Capture",
      "scope": { "checks": [
        { "id": "soft", "assert": { "fn": "eq", "args": [ 1, 2 ] }, "message": "advice", "blocking": false },
        { "id": "hard", "assert": { "ref": "fine" }, "message": "not yet", "blocking": true } ] },
      "children": [
        { "kind": "slider", "id": "amount", "testId": "amount", "required": true, "min": 0, "max": 10 },
        { "kind": "textInput", "id": "hidden", "testId": "hidden", "required": true, "visible": false } ] } ] } })";

}  // namespace

// Mutation: count a hidden element's errors, or a check that is not blocking, against `ready`.
TEST_CASE("cove: a dialog is ready past a hidden element's errors and a failing check that is not blocking",
          "[ui][interpreter][cove]") {
    Harness h;
    auto const screen = h.mount(kReady);
    h.backend.click(h.byTestId("Button", "open"));
    h.settle();
    CHECK(h.text("ready") == "false");  // the visible required slider holds no value
    h.backend.slide(h.byTestId("Slider", "amount"), 4);
    h.settle();
    CHECK(h.backend.prop(h.byTestId("Slider", "amount"), "errors") == "[]");
    CHECK(h.text("ready") == "false");  // the blocking check fails
    h.backend.click(h.byTestId("Button", "fine"));
    h.settle();
    CHECK(h.text("ready") == "true");
}

// Mutation: leave a unit-bearing reading as bare JSON, or read an `app.` name with no app document.
TEST_CASE("cove: a reference to the app that is not there reads as null", "[ui][interpreter][cove]") {
    Harness h;
    auto const screen = h.mount(R"({ "vocab": "ui/1", "id": "refs", "view": { "kind": "column", "children": [
      { "kind": "text", "testId": "app", "text": { "fn": "coalesce", "args": [ { "ref": "app" }, "none" ] } },
      { "kind": "text", "testId": "app2", "text": { "fn": "coalesce", "args": [ { "ref": "app.name" }, "none" ] } } ] } })");
    CHECK(h.text("app") == "none");
    CHECK(h.text("app2") == "none");
}
