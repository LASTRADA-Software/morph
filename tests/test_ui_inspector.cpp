// SPDX-License-Identifier: Apache-2.0

// The development inspector (spec 6 §21; spec 6 §12, "Inspector"): it lists every mounted node with its evaluated
// properties and every query with its key, state and age, logs the command lists a screen ran, reads the reactive
// counters, and refetches only through the document's own `refetch` command. A screen of a session not in
// development mode records nothing and gives no inspector.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <morph/ui/inspector.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ui_interpreter_support.hpp"

using morph::ui::InspectedNode;
using morph::ui::InspectedQuery;
using morph::ui::Inspector;
using morph::ui::Value;
using morph::ui::testing::Harness;

namespace {

constexpr std::string_view kInspected = R"({
  "vocab": "ui/1", "id": "inspected",
  "state": { "filter": { "type": "string", "init": "all" },
             "items": { "init": { "list": [ { "obj": { "id": 1 } }, { "obj": { "id": 2 } } ] } } },
  "models": { "sample": { "type": "Lims_SampleModel" } },
  "queries": { "info": { "model": "sample", "action": "Lims_Info", "staleTime": 1000 },
               "later": { "model": "sample", "action": "Lims_Info", "when": false } },
  "mutations": { "save": { "model": "sample", "action": "Lims_Attach", "body": { "obj": {} } } },
  "view": { "kind": "column", "children": [
    { "kind": "text", "testId": "title", "text": { "fn": "format", "args": [ "filter {0}", { "ref": "filter" } ] } },
    { "kind": "button", "id": "go", "testId": "go", "label": "Go", "onClick": [ { "set": "filter", "value": "open" } ] },
    { "kind": "forEach", "rows": { "ref": "items" }, "key": { "row": "id" },
      "children": [ { "kind": "text", "testId": "row", "text": { "fn": "format", "args": [ "row {0}", { "row": "id" } ] } } ] } ] } })";

Inspector inspectorOf(morph::ui::Screen& screen) {
    std::optional<Inspector> inspector = Inspector::over(screen);
    REQUIRE(inspector.has_value());
    return *inspector;
}

std::vector<InspectedNode> ofKind(std::vector<InspectedNode> const& nodes, std::string_view kind) {
    std::vector<InspectedNode> out;
    std::ranges::copy_if(nodes, std::back_inserter(out),
                         [kind](InspectedNode const& node) { return node.kind == kind; });
    return out;
}

InspectedQuery const& named(std::vector<InspectedQuery> const& queries, std::string_view name) {
    auto const found = std::ranges::find(queries, name, &InspectedQuery::name);
    REQUIRE(found != queries.end());
    return *found;
}

}  // namespace

// Mutation: skip the record in ScreenCore::lower — nodes() is empty. Mutation: evaluate a property against the
// document's literal instead of the scope — the title reads its expression, not "filter open".
TEST_CASE("The inspector lists every mounted node with its kind, id, testId and evaluated properties",
          "[ui][inspector]") {
    Harness harness;
    auto screen = harness.mount(kInspected);
    Inspector const inspector = inspectorOf(*screen);
    CHECK(inspector.screenId() == "inspected");

    std::vector<InspectedNode> nodes = inspector.nodes();
    REQUIRE(nodes.size() == 6U);  // the column, the title, the button, the forEach, and one text per row
    CHECK(nodes.front().kind == "column");
    CHECK(nodes.front().path == "/view");
    auto const texts = ofKind(nodes, "text");
    REQUIRE(texts.size() == 3U);
    CHECK(texts.at(0).testId == "title");
    CHECK(texts.at(0).properties.at("text") == Value{std::string{"filter all"}});
    CHECK(texts.at(1).properties.at("text") == Value{std::string{"row 1"}});
    CHECK(texts.at(2).properties.at("text") == Value{std::string{"row 2"}});
    auto const buttons = ofKind(nodes, "button");
    REQUIRE(buttons.size() == 1U);
    CHECK(buttons.front().id == "go");
    CHECK(buttons.front().testId == "go");
    CHECK(buttons.front().properties.at("label") == Value{std::string{"Go"}});

    // Evaluated now, not when lowered.
    harness.backend.click(harness.byTestId("Button", "go"));
    harness.settle();
    CHECK(ofKind(inspector.nodes(), "text").at(0).properties.at("text") == Value{std::string{"filter open"}});
    CHECK(screen->diagnostics().empty());
}

// Mutation: report `fresh` whatever the entry's age — the second check reads fresh. Mutation: leave the age out — the
// age check fails.
TEST_CASE("The inspector lists every query with its key, state and age, and each mutation", "[ui][inspector]") {
    Harness harness;
    auto screen = harness.mount(kInspected);
    Inspector const inspector = inspectorOf(*screen);

    std::vector<InspectedQuery> const queries = inspector.queries();
    REQUIRE(queries.size() == 3U);
    InspectedQuery const& pending = named(queries, "info");
    CHECK(pending.scope == "screen");
    CHECK_FALSE(pending.mutation);
    CHECK(pending.model == "Lims_SampleModel");
    CHECK(pending.action == "Lims_Info");
    CHECK(pending.state == "pending");
    CHECK(pending.key.contains("Lims_Info"));
    CHECK_FALSE(pending.age.has_value());
    InspectedQuery const& idle = named(queries, "later");
    CHECK(idle.state == "idle");
    CHECK(idle.key.empty());
    InspectedQuery const& save = named(queries, "save");
    CHECK(save.mutation);
    CHECK(save.state == "idle");

    REQUIRE(harness.dispatcher.calls.size() == 1U);
    harness.reply(0, R"({"name":"one"})");
    harness.advance(std::chrono::milliseconds{400});
    InspectedQuery const fresh = named(inspector.queries(), "info");
    CHECK(fresh.state == "fresh");
    CHECK(fresh.age == std::chrono::milliseconds{400});
    CHECK(fresh.key == pending.key);

    harness.advance(std::chrono::milliseconds{600});
    InspectedQuery const stale = named(inspector.queries(), "info");
    CHECK(stale.state == "stale");
    CHECK(stale.age == std::chrono::milliseconds{1000});
}

// Mutation: run the query's refetch directly instead of through the command list — the log has no refetch entry.
TEST_CASE("The inspector refetches through the document's refetch command, and logs what ran with its batch",
          "[ui][inspector]") {
    Harness harness;
    auto screen = harness.mount(kInspected);
    Inspector inspector = inspectorOf(*screen);
    harness.reply(0, R"({"name":"one"})");

    harness.backend.click(harness.byTestId("Button", "go"));
    harness.settle();
    auto log = inspector.commandLog();
    REQUIRE_FALSE(log.empty());
    CHECK(log.back().event == "/view/children/1/onClick");
    REQUIRE(log.back().commands.size() == 1U);
    CHECK(log.back().commands.front().kind == "set");
    CHECK(log.back().commands.front().target == "filter");
    CHECK(log.back().batch > 0U);
    std::uint64_t const clickBatch = log.back().batch;

    CHECK_FALSE(inspector.refetch("nothing"));
    CHECK_FALSE(inspector.refetch("save"));  // a mutation is not refetched
    REQUIRE(inspector.refetch("info"));
    harness.settle();
    CHECK(harness.dispatcher.calls.size() == 2U);
    CHECK(named(inspector.queries(), "info").state == "pending");
    log = inspector.commandLog();
    CHECK(log.back().event == "/inspector");
    REQUIRE(log.back().commands.size() == 1U);
    CHECK(log.back().commands.front().kind == "refetch");
    CHECK(log.back().commands.front().target == "info");
    CHECK(log.back().batch > clickBatch);

    harness.dispatcher.fail(1, "error", "the server is down");
    harness.settle();
    InspectedQuery const failed = named(inspector.queries(), "info");
    CHECK(failed.state == "error");
    CHECK(failed.lastError == "the server is down");
}

// Mutation: drop the flush counter's increment — flushes stays 0.
TEST_CASE("The inspector reads the reactive runtime's counters", "[ui][inspector]") {
    Harness harness;
    auto screen = harness.mount(kInspected);
    Inspector const inspector = inspectorOf(*screen);
    morph::reactive::RuntimeStats const before = inspector.counters();
    harness.backend.click(harness.byTestId("Button", "go"));
    harness.settle();
    morph::reactive::RuntimeStats const after = inspector.counters();
    CHECK(after.flushes > before.flushes);
    CHECK(after.effectsRun > before.effectsRun);
    CHECK(after.lastFlushEffects > 0U);
    CHECK(after.maxFlushEffects >= after.lastFlushEffects);
    CHECK(after.batches > before.batches);
    CHECK(after.misuses == 0U);
    CHECK(after.recentMisuses.empty());
}

namespace {

constexpr std::string_view kSaving = R"({
  "vocab": "ui/1", "id": "saving",
  "models": { "sample": { "type": "Lims_SampleModel" } },
  "mutations": { "save": { "model": "sample", "action": "Lims_Attach", "body": { "obj": {} } } },
  "view": { "kind": "button", "testId": "save", "label": "Save", "onClick": [ { "run": "save" } ] } })";

constexpr std::string_view kApp = R"({
  "vocab": "ui/1", "id": "app",
  "models": { "clients": { "type": "Lims_ClientModel" } },
  "queries": { "clientList": { "model": "clients", "action": "Lims_ClientList" } } })";

}  // namespace

// Mutation: report a mutation as idle whatever its runs did, or leave the app document's queries out.
TEST_CASE("The inspector follows a mutation from pending to fresh or error, and lists the app document's queries",
          "[ui][inspector]") {
    Harness harness;
    auto const app = harness.interpreter.mountApp(morph::ui::testing::loadOrFail(kApp));
    REQUIRE(app.has_value());
    auto screen = harness.mount(kSaving);
    Inspector const inspector = inspectorOf(*screen);
    CHECK(named(inspector.queries(), "clientList").scope == "app");

    harness.backend.click(harness.byTestId("Button", "save"));
    harness.settle();
    CHECK(named(inspector.queries(), "save").state == "pending");
    harness.reply(harness.dispatcher.last("Lims_Attach"), R"({"ok":true})");
    CHECK(named(inspector.queries(), "save").state == "fresh");

    harness.backend.click(harness.byTestId("Button", "save"));
    harness.settle();
    harness.fail(harness.dispatcher.last("Lims_Attach"));
    InspectedQuery const failed = named(inspector.queries(), "save");
    CHECK(failed.state == "error");
    CHECK_FALSE(failed.lastError.empty());
}

TEST_CASE("A screen of a session not in development mode records nothing and gives no inspector", "[ui][inspector]") {
    Harness off{morph::ui::DevelopmentTools::Off};
    auto quiet = off.mount(kInspected);
    CHECK_FALSE(Inspector::over(*quiet).has_value());

    // The control: the same screen in development mode lists its nodes and logs a command list it ran.
    Harness on{morph::ui::DevelopmentTools::On};
    auto screen = on.mount(kInspected);
    on.backend.click(on.byTestId("Button", "go"));
    on.settle();
    Inspector const inspector = inspectorOf(*screen);
    CHECK_FALSE(inspector.nodes().empty());
    CHECK_FALSE(inspector.commandLog().empty());
}
