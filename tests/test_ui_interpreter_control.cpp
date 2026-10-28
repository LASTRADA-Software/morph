// SPDX-License-Identifier: Apache-2.0

// The control corpus of spec 5 §14: queries (latest wins, debounce, timed refresh, the cache), mutations (the four
// concurrency modes and optimistic writes) and boundaries, mounted on RecordingBackend with a scripted dispatcher.

#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>

#include "ui_interpreter_support.hpp"

namespace ui = morph::ui;
using ui::testing::Harness;
using ui::testing::readJson;
using namespace std::chrono_literals;

namespace {

// A query keyed on state `id`, which a text input sets; the text shows the reply's `name`.
constexpr char const* kKeyed = R"({
  "vocab": "ui/1", "id": "keyed",
  "models": { "sample": { "type": "Lims_SampleModel" } },
  "state": { "id": { "init": 1 } },
  "queries": { "q": { "model": "sample", "action": "Lims_ListResults", "debounce": DEBOUNCE, "refreshEvery": EVERY,
                      "body": { "obj": { "id": { "ref": "id" } } } } },
  "view": { "kind": "column", "children": [
    { "kind": "textInput", "testId": "in", "onChange": [ { "set": "id", "value": { "ref": "event" } } ] },
    { "kind": "text", "testId": "out", "text": { "ref": "q.name" } },
    { "kind": "text", "testId": "pending", "text": { "fn": "format", "args": [ "{0}", { "pending": "q" } ] } },
    { "kind": "text", "testId": "error", "text": { "error": "q" } } ] } })";

std::string keyed(int debounce = 0, int every = 0) {
    std::string text = kKeyed;
    text.replace(text.find("DEBOUNCE"), 8, std::to_string(debounce));
    text.replace(text.find("EVERY"), 5, std::to_string(every));
    return text;
}

}  // namespace

TEST_CASE("interpreter control: a query fetches its key at mount and shows the reply", "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(keyed());
    REQUIRE(h.dispatcher.calls.size() == 1);
    CHECK(h.dispatcher.calls.at(0).action == "Lims_ListResults");
    CHECK(h.dispatcher.calls.at(0).body == R"({"id":1})");
    CHECK(h.dispatcher.calls.at(0).typeId == "Lims_SampleModel");
    CHECK(h.dispatcher.binds == std::vector<std::string>{"Lims_SampleModel private "});
    CHECK(h.text("pending") == "true");
    h.reply(0, R"({"name":"one"})");
    CHECK(h.text("out") == "one");
    CHECK(h.text("pending") == "false");
}

TEST_CASE("interpreter control: the latest key wins whatever order the replies arrive in", "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(keyed());
    h.reply(0, R"({"name":"one"})");
    int const input = h.byTestId("TextInput", "in");
    h.backend.edit(input, "2");
    h.settle();
    h.backend.edit(input, "3");
    h.settle();
    REQUIRE(h.dispatcher.calls.size() == 3);
    CHECK(h.dispatcher.calls.at(1).body == R"({"id":"2"})");
    CHECK(h.dispatcher.calls.at(2).body == R"({"id":"3"})");
    // The value of the previous key stays while the new one is in flight.
    CHECK(h.text("out") == "one");
    h.reply(2, R"({"name":"three"})");
    CHECK(h.text("out") == "three");
    h.reply(1, R"({"name":"two"})");
    CHECK(h.text("out") == "three");
}

TEST_CASE("interpreter control: a failure for a new key clears the old value; a failed refetch keeps it",
          "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(keyed());
    h.reply(0, R"({"name":"one"})");
    h.backend.edit(h.byTestId("TextInput", "in"), "2");
    h.settle();
    h.fail(1);
    CHECK(h.text("out").empty());
    CHECK(h.text("error") == "it failed");
    CHECK(readJson(*screen, R"({"errorKind": "q"})") == R"("error")");
}

TEST_CASE("interpreter control: debounce sends only the last key of a burst", "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(keyed(300));
    REQUIRE(h.dispatcher.calls.size() == 1);
    h.reply(0, R"({"name":"one"})");
    int const input = h.byTestId("TextInput", "in");
    h.backend.edit(input, "2");
    h.settle();
    h.advance(100ms);
    h.backend.edit(input, "3");
    h.settle();
    CHECK(h.text("pending") == "true");
    h.advance(299ms);
    CHECK(h.dispatcher.calls.size() == 1);
    h.advance(1ms);
    REQUIRE(h.dispatcher.calls.size() == 2);
    CHECK(h.dispatcher.calls.at(1).body == R"({"id":"3"})");
}

TEST_CASE("interpreter control: refreshEvery skips a tick while a request is in flight", "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(keyed(0, 1000));
    REQUIRE(h.dispatcher.calls.size() == 1);
    h.advance(1000ms);
    CHECK(h.dispatcher.calls.size() == 1);
    h.reply(0, R"({"name":"one"})");
    h.advance(1000ms);
    REQUIRE(h.dispatcher.calls.size() == 2);
    CHECK(h.dispatcher.calls.at(1).body == R"({"id":1})");
}

TEST_CASE("interpreter control: `when` false and a null body make a query idle", "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(R"({
      "vocab": "ui/1", "id": "idle",
      "models": { "sample": { "type": "Lims_SampleModel" } },
      "state": { "on": { "init": false }, "id": { "type": "int?" } },
      "queries": {
        "gated": { "model": "sample", "action": "Lims_ListResults", "when": { "ref": "on" } },
        "nobody": { "model": "sample", "action": "Lims_GetLimits", "body": { "ref": "id" } } },
      "view": { "kind": "button", "testId": "go", "label": "go",
                "onClick": [ { "set": "on", "value": true }, { "set": "id", "value": 4 } ] } })");
    CHECK(h.dispatcher.calls.empty());
    CHECK(readJson(*screen, R"({"pending": "gated"})") == "false");
    h.backend.click(h.byTestId("Button", "go"));
    h.settle();
    REQUIRE(h.dispatcher.calls.size() == 2);
    CHECK(h.dispatcher.count("Lims_ListResults") == 1);
    CHECK(h.dispatcher.calls.at(h.dispatcher.last("Lims_GetLimits")).body == "4");
}

namespace {

constexpr char const* kMutations = R"({
  "vocab": "ui/1", "id": "mutations",
  "models": { "sample": { "type": "Lims_SampleModel" } },
  "state": { "n": { "init": 0 } },
  "queries": { "rows": { "model": "sample", "action": "Lims_ListResults" },
               "other": { "model": "sample", "action": "Lims_GetLimits" } },
  "mutations": {
    "m": { "model": "sample", "action": "Lims_Save", "concurrency": "MODE", SERIAL
           "body": { "obj": { "n": { "ref": "n" } } },
           "onSuccess": [ { "set": "n", "value": { "fn": "add", "args": [ { "ref": "n" }, 1 ] } } ],
           "onError": [ { "notify": { "error": "m" }, "role": "error" } ] } },
  "view": { "kind": "column", "children": [
    { "kind": "button", "testId": "go", "label": "go", "onClick": { "run": "m" } },
    { "kind": "text", "testId": "result", "text": { "ref": "last" } },
    { "kind": "text", "testId": "pending", "text": { "fn": "format", "args": [ "{0}", { "pending": "m" } ] } } ] },
  "let": { "last": { "fn": "format", "args": [ "{0}", { "fn": "coalesce", "args": [ { "result": "m" }, "none" ] } ] } } })";

std::string mutations(std::string const& mode, std::string const& serial = {}) {
    std::string text = kMutations;
    text.replace(text.find("MODE"), 4, mode);
    text.replace(text.find("SERIAL"), 6, serial);
    return text;
}

// Lets the two queries of the mutation document load.
void loadQueries(Harness& h) {
    h.reply(h.dispatcher.last("Lims_ListResults"), "[]");
    h.reply(h.dispatcher.last("Lims_GetLimits"), "{}");
}

}  // namespace

TEST_CASE("interpreter control: an exclusive mutation refuses a run while one is in flight", "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(mutations("exclusive"));
    loadQueries(h);
    int const go = h.byTestId("Button", "go");
    h.backend.click(go);
    h.settle();
    h.backend.click(go);
    h.settle();
    CHECK(h.dispatcher.count("Lims_Save") == 1);
    CHECK(h.text("pending") == "true");
    h.reply(h.dispatcher.last("Lims_Save"), "7");
    CHECK(h.text("pending") == "false");
    CHECK(h.text("result") == "7");
    CHECK(readJson(*screen, R"({"ref": "n"})") == "1");
    h.backend.click(go);
    h.settle();
    CHECK(h.dispatcher.count("Lims_Save") == 2);
    CHECK(h.dispatcher.calls.at(h.dispatcher.last("Lims_Save")).body == R"({"n":1})");
}

TEST_CASE("interpreter control: a serial mutation queues runs per key and sends them in order", "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(mutations("serial", R"("serialKey": "k",)"));
    loadQueries(h);
    int const go = h.byTestId("Button", "go");
    h.backend.click(go);
    h.settle();
    h.backend.click(go);
    h.settle();
    CHECK(h.dispatcher.count("Lims_Save") == 1);
    CHECK(h.text("pending") == "true");
    h.reply(h.dispatcher.last("Lims_Save"), "1");
    REQUIRE(h.dispatcher.count("Lims_Save") == 2);
    CHECK(h.text("pending") == "true");
    h.reply(h.dispatcher.last("Lims_Save"), "2");
    CHECK(h.text("pending") == "false");
    CHECK(h.text("result") == "2");
}

// Mutation: evaluate serialKey without the `body` binding -> every key is null, one queue, task 2 waits for task 1.
TEST_CASE("interpreter control: a serial key reads the run's body, so runs queue per item", "[ui][interpreter]") {
    Harness h;
    auto const screen = h.mount(R"({
      "vocab": "ui/1", "id": "per-item",
      "models": { "sample": { "type": "Lims_SampleModel" } },
      "mutations": { "move": { "model": "sample", "action": "Lims_Save", "concurrency": "serial",
                               "serialKey": { "ref": "body.task" } } },
      "view": { "kind": "column", "children": [
        { "kind": "button", "testId": "one", "label": "1", "onClick": { "run": "move", "body": { "obj": { "task": 1 } } } },
        { "kind": "button", "testId": "two", "label": "2", "onClick": { "run": "move", "body": { "obj": { "task": 2 } } } } ] } })");
    INFO((h.diagnostics.empty() ? std::string{} : h.diagnostics.front().message()));
    REQUIRE(h.diagnostics.empty());
    h.backend.click(h.byTestId("Button", "one"));
    h.settle();
    h.backend.click(h.byTestId("Button", "two"));
    h.settle();
    h.backend.click(h.byTestId("Button", "one"));
    h.settle();
    // Tasks 1 and 2 are in flight together; the second run of task 1 waits for the first.
    REQUIRE(h.dispatcher.count("Lims_Save") == 2);
    CHECK(h.dispatcher.calls.at(h.dispatcher.last("Lims_Save")).body == R"({"task":2})");
    h.reply(h.dispatcher.last("Lims_Save"), "{}");
    CHECK(h.dispatcher.count("Lims_Save") == 2);
    h.reply(h.dispatcher.last("Lims_Save") - 1, "{}");
    REQUIRE(h.dispatcher.count("Lims_Save") == 3);
    CHECK(h.dispatcher.calls.at(h.dispatcher.last("Lims_Save")).body == R"({"task":1})");
}

TEST_CASE("interpreter control: a serial key may not read event, which only some commands have", "[ui][interpreter]") {
    auto const loaded = ui::loadDocumentText(R"({
      "vocab": "ui/1", "id": "per-event",
      "models": { "sample": { "type": "Lims_SampleModel" } },
      "mutations": { "move": { "model": "sample", "action": "Lims_Save", "concurrency": "serial",
                               "serialKey": { "ref": "event" } } },
      "view": { "kind": "spacer" } })",
                                             ui::LoadOptions{.catalog = ui::testing::corpusCatalog()});
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().reason == "`event` is read only inside a command list");
}

TEST_CASE("interpreter control: a latest mutation applies only the newest reply", "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(mutations("latest"));
    loadQueries(h);
    int const go = h.byTestId("Button", "go");
    h.backend.click(go);
    h.settle();
    h.backend.click(go);
    h.settle();
    REQUIRE(h.dispatcher.count("Lims_Save") == 2);
    std::size_t const second = h.dispatcher.last("Lims_Save");
    h.reply(second, "2");
    CHECK(h.text("result") == "2");
    CHECK(h.text("pending") == "true");
    h.reply(second - 1, "1");
    CHECK(h.text("result") == "2");
    CHECK(h.text("pending") == "false");
    CHECK(readJson(*screen, R"({"ref": "n"})") == "1");
}

TEST_CASE("interpreter control: a parallel mutation applies every reply as it arrives", "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(mutations("parallel"));
    loadQueries(h);
    int const go = h.byTestId("Button", "go");
    h.backend.click(go);
    h.settle();
    h.backend.click(go);
    h.settle();
    REQUIRE(h.dispatcher.count("Lims_Save") == 2);
    std::size_t const second = h.dispatcher.last("Lims_Save");
    h.reply(second, "2");
    h.reply(second - 1, "1");
    CHECK(h.text("result") == "1");
    CHECK(readJson(*screen, R"({"ref": "n"})") == "2");
}

TEST_CASE("interpreter control: a failure sets the error and runs onError", "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(mutations("exclusive"));
    loadQueries(h);
    h.backend.click(h.byTestId("Button", "go"));
    h.settle();
    std::size_t const before = h.dispatcher.calls.size();
    h.fail(h.dispatcher.last("Lims_Save"), "denied");
    CHECK(h.host.log.back() == "notify error it failed");
    CHECK(readJson(*screen, R"({"errorKind": "m"})") == R"("denied")");
    CHECK(h.host.stale == 1);
    // A failure fetches nothing.
    CHECK(h.dispatcher.calls.size() == before);
}

namespace {

constexpr char const* kRows = R"({
  "vocab": "ui/1", "id": "rows",
  "models": { "sample": { "type": "Lims_SampleModel" } },
  "queries": { "results": { "model": "sample", "action": "Lims_ListResults" } },
  "mutations": {
    "guess": { "model": "sample", "action": "Lims_Save", "concurrency": "parallel",
               "optimistic": { "query": "results", "key": { "ref": "body.id" },
                               "with": { "obj": { "mass": { "ref": "body.mass" } } } },
               "onError": [ { "notify": { "fn": "format", "args": [ "row 2 at {0}", { "ref": "row2.mass" } ] } } ] } },
  "let": { "row2": { "fn": "first", "args": [ { "filter": { "query": "results" }, "as": "r",
               "where": { "fn": "eq", "args": [ { "ref": "r.id" }, 2 ] } } ] } },
  "view": { "kind": "column", "children": [
    { "kind": "textInput", "testId": "guess", "onSubmit": { "run": "guess", "body": { "obj": { "id": 2, "mass": { "ref": "event" } } } } },
    { "kind": "forEach", "rows": { "query": "results" }, "key": { "row": "id" },
      "children": [ { "kind": "text", "text": { "fn": "format", "args": [ "{0}:{1}", { "row": "id" }, { "row": "mass" } ] } } ] } ] } })";

std::vector<std::string> rowTexts(Harness& h) {
    std::vector<std::string> texts;
    for (int const id : h.backend.all("Text")) {
        texts.push_back(h.backend.prop(id, "text"));
    }
    return texts;
}

}  // namespace

TEST_CASE("interpreter control: an optimistic write shows before the reply and rolls back on failure",
          "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(kRows);
    h.reply(0, R"([{"id":1,"mass":10},{"id":2,"mass":20}])");
    int const guess = h.byTestId("TextInput", "guess");

    SECTION("the row shows `with` at once and keeps it when the run succeeds") {
        h.backend.submit(guess, "30");
        h.settle();
        CHECK(rowTexts(h) == std::vector<std::string>{"1:10", "2:30"});
        h.reply(h.dispatcher.last("Lims_Save"), "{}");
        CHECK(rowTexts(h) == std::vector<std::string>{"1:10", "2:30"});
    }
    SECTION("a failure rolls the row back before onError runs") {
        h.backend.submit(guess, "30");
        h.settle();
        h.fail(h.dispatcher.last("Lims_Save"));
        CHECK(rowTexts(h) == std::vector<std::string>{"1:10", "2:20"});
        REQUIRE_FALSE(h.host.log.empty());
        CHECK(h.host.log.back() == "notify  row 2 at 20");
    }
    SECTION("two overlapping runs end at the last confirmed value") {
        h.backend.submit(guess, "30");
        h.settle();
        h.backend.submit(guess, "40");
        h.settle();
        CHECK(rowTexts(h) == std::vector<std::string>{"1:10", "2:40"});
        std::size_t const second = h.dispatcher.last("Lims_Save");
        h.fail(second - 1);
        CHECK(rowTexts(h) == std::vector<std::string>{"1:10", "2:40"});
        h.fail(second);
        CHECK(rowTexts(h) == std::vector<std::string>{"1:10", "2:20"});
    }
    SECTION("a confirmed run survives the failure of a later one") {
        h.backend.submit(guess, "30");
        h.settle();
        h.backend.submit(guess, "40");
        h.settle();
        std::size_t const second = h.dispatcher.last("Lims_Save");
        h.reply(second - 1, "{}");
        h.fail(second);
        CHECK(rowTexts(h) == std::vector<std::string>{"1:10", "2:30"});
    }
}

namespace {

// The rows of `results` live in the record member its `at` names, as a list action that answers
// `{"results": [...]}` gives them. AT is replaced with the `at` member of the optimistic write, or with nothing.
constexpr char const* kRecordRows = R"({
  "vocab": "ui/1", "id": "recordrows",
  "models": { "sample": { "type": "Lims_SampleModel" } },
  "queries": { "results": { "model": "sample", "action": "Lims_ListResults" } },
  "mutations": {
    "guess": { "model": "sample", "action": "Lims_Save", "concurrency": "parallel",
               "optimistic": { "query": "results", AT "key": { "ref": "body.id" },
                               "with": { "obj": { "mass": { "ref": "body.mass" } } } } } },
  "view": { "kind": "textInput", "testId": "guess", "onSubmit": { "run": "guess", "body": { "obj": { "id": 2, "mass": { "ref": "event" } } } } } })";

std::string recordRows(std::string_view at) {
    std::string text = kRecordRows;
    for (std::size_t found = text.find("AT"); found != std::string::npos; found = text.find("AT", found)) {
        text.replace(found, 2, at);
        found += at.size();
    }
    return text;
}

constexpr char const* kRecordReply = R"({"results":[{"id":1,"mass":10},{"id":2,"mass":20}]})";

}  // namespace

TEST_CASE("interpreter control: an optimistic write with `at` writes a row of the list inside the query's record",
          "[ui][interpreter]") {
    Harness h;
    auto const screen = h.mount(recordRows(R"("at": "results",)"));
    h.reply(0, kRecordReply);
    h.backend.submit(h.byTestId("TextInput", "guess"), "30");
    h.settle();
    CHECK(readJson(*screen, R"({"ref": "results"})") == R"({"results":[{"id":1,"mass":10},{"id":2,"mass":"30"}]})");
    h.fail(h.dispatcher.last("Lims_Save"));
    CHECK(readJson(*screen, R"({"ref": "results"})") == kRecordReply);
    CHECK(h.diagnostics.empty());
}

TEST_CASE("interpreter control: an optimistic write that finds no list in the query's value is reported",
          "[ui][interpreter]") {
    Harness h;
    SECTION("a record value and no `at`") {
        auto const screen = h.mount(recordRows(""));
        h.reply(0, kRecordReply);
        h.backend.submit(h.byTestId("TextInput", "guess"), "30");
        h.settle();
        CHECK(readJson(*screen, R"({"ref": "results"})") == kRecordReply);
        REQUIRE(h.diagnostics.size() == 1);
        CHECK(h.diagnostics.front().path == "/mutations/guess/optimistic");
        CHECK(h.diagnostics.front().reason.contains("not a list"));
    }
    SECTION("an `at` the record does not have") {
        auto const screen = h.mount(recordRows(R"("at": "rows",)"));
        h.reply(0, kRecordReply);
        h.backend.submit(h.byTestId("TextInput", "guess"), "30");
        h.settle();
        CHECK(readJson(*screen, R"({"ref": "results"})") == kRecordReply);
        REQUIRE(h.diagnostics.size() == 1);
        CHECK(h.diagnostics.front().path == "/mutations/guess/optimistic");
        CHECK(h.diagnostics.front().reason.contains("`rows`"));
    }
}

TEST_CASE("interpreter control: two queries with an equal key share one request", "[ui][interpreter]") {
    Harness h;
    char const* const doc = R"({
      "vocab": "ui/1", "id": "shared",
      "models": { "clients": { "type": "Lims_ClientModel" } },
      "queries": { "a": { "model": "clients", "action": "Lims_ClientList", "staleTime": 60000 },
                   "b": { "model": "clients", "action": "Lims_ClientList", "staleTime": 60000 } },
      "view": { "kind": "text", "testId": "t", "text": { "fn": "format", "args": [ "{0}/{1}", { "query": "a" }, { "query": "b" } ] } } })";
    auto first = h.mount(doc);
    REQUIRE(h.dispatcher.calls.size() == 1);
    CHECK(h.dispatcher.binds == std::vector<std::string>{"Lims_ClientModel private "});
    h.reply(0, "3");
    CHECK(h.text("t") == "3/3");

    SECTION("a second screen on the same key mounts on the warm cache within staleTime") {
        ui::testing::RecordingBackend other;
        auto second = h.interpreter.mount(ui::testing::loadOrFail(doc, ui::testing::corpusCatalog()), {}, other);
        REQUIRE(second);
        h.settle();
        CHECK(h.dispatcher.calls.size() == 1);
        h.advance(60000ms);
        auto third = h.interpreter.mount(ui::testing::loadOrFail(doc, ui::testing::corpusCatalog()), {}, other);
        h.settle();
        CHECK(h.dispatcher.calls.size() == 2);
    }
    SECTION("signOut clears the cache") {
        CHECK(h.session.cacheSize() == 1);
        h.session.clear();
        CHECK(h.session.cacheSize() == 0);
    }
}

TEST_CASE("interpreter control: a boundary shows loading, then its children, or its error and retries",
          "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(R"({
      "vocab": "ui/1", "id": "boundary",
      "models": { "sample": { "type": "Lims_SampleModel" } },
      "queries": { "q": { "model": "sample", "action": "Lims_ListResults" } },
      "let": { "count": { "fn": "count", "args": [ { "query": "q" } ] } },
      "view": { "kind": "boundary",
        "loading": [ { "kind": "text", "testId": "loading", "text": "loading" } ],
        "error": [ { "kind": "text", "testId": "error", "text": { "ref": "boundary.error" } },
                   { "kind": "button", "testId": "retry", "label": "retry", "onClick": [ { "retry": true } ] } ],
        "children": [ { "kind": "text", "testId": "count", "text": { "fn": "format", "args": [ "{0}", { "ref": "count" } ] } } ] } })");
    CHECK(h.text("loading") == "loading");
    h.fail(0);
    CHECK(h.text("error") == "it failed");
    h.backend.click(h.byTestId("Button", "retry"));
    h.settle();
    REQUIRE(h.dispatcher.calls.size() == 2);
    h.reply(1, "[1,2]");
    CHECK(h.text("count") == "2");
    // A refetch of a query that has a value keeps the children.
    h.backend.clearLog();
    h.session.backendChanged("primary");
    h.settle();
    CHECK(h.dispatcher.calls.size() == 3);
    CHECK(h.text("count") == "2");
}

TEST_CASE("interpreter control: a boundary owns the queries read anywhere inside its children", "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(R"({
      "vocab": "ui/1", "id": "nested",
      "models": { "sample": { "type": "Lims_SampleModel" } },
      "queries": { "a": { "model": "sample", "action": "Lims_ListResults" },
                   "b": { "model": "sample", "action": "Lims_GetLimits" },
                   "inner": { "model": "sample", "action": "Lims_Info" } },
      "view": { "kind": "boundary", "children": [
        { "kind": "column", "children": [
          { "kind": "tabs", "tabs": [ { "label": "t", "children": [ { "kind": "text", "testId": "a", "text": { "fn": "format", "args": [ "{0}", { "pending": "a" } ] } } ] } ] },
          { "kind": "text", "testId": "b", "text": { "fn": "coalesce", "args": [ { "error": "b" }, { "errorKind": "b" }, "-" ] } },
          { "kind": "boundary", "children": [ { "kind": "text", "text": { "query": "inner" } } ] } ] } ] } })");
    // The outer boundary shows its (empty) loading content until `a` and `b` settle; `inner` is the nested
    // boundary's.
    CHECK(h.backend.find("Text", "testId", "a") == std::nullopt);
    h.reply(h.dispatcher.last("Lims_ListResults"), "[]");
    CHECK(h.backend.find("Text", "testId", "a") == std::nullopt);
    h.reply(h.dispatcher.last("Lims_GetLimits"), "{}");
    CHECK(h.text("b") == "-");
}

TEST_CASE("interpreter control: an entry no query uses is dropped after five minutes", "[ui][interpreter]") {
    Harness h;
    // A shared instance: a private one is a new instance, and so a new entry, on every mount.
    std::string const doc = R"({
      "vocab": "ui/1", "id": "evict",
      "models": { "clients": { "type": "Lims_ClientModel" } },
      "queries": { "q": { "model": "clients", "action": "Lims_ClientList" } },
      "view": { "kind": "text", "testId": "out", "text": { "ref": "q.name" } } })";
    auto screen = h.mount(doc);
    h.reply(0, R"({"name":"one"})");
    CHECK(h.session.cacheSize() == 1);
    screen.reset();
    h.advance(std::chrono::minutes{4});
    CHECK(h.session.cacheSize() == 1);
    auto again = h.mount(doc);
    // Remounted within the five minutes, the entry is shown at once (and refetched, its staleTime being 0).
    CHECK(h.text("out") == "one");
    again.reset();
    h.advance(std::chrono::minutes{5});
    CHECK(h.session.cacheSize() == 0);
}

TEST_CASE("interpreter control: refetch ends a debounce wait; a reply that is not a document value fails",
          "[ui][interpreter]") {
    Harness h;
    std::string doc = keyed(300);
    doc.replace(doc.find(R"({ "kind": "text", "testId": "out")"), 0,
                R"({ "kind": "button", "testId": "refetch", "label": "r", "onClick": { "refetch": "q" } },)");
    auto screen = h.mount(doc);
    h.reply(0, "1.5");
    CHECK(h.text("error").starts_with("the reply is not a document value"));
    h.backend.edit(h.byTestId("TextInput", "in"), "2");
    h.settle();
    h.backend.click(h.byTestId("Button", "refetch"));
    h.settle();
    REQUIRE(h.dispatcher.calls.size() == 2);
    CHECK(h.dispatcher.calls.at(1).body == R"({"id":"2"})");
    h.backend.click(h.byTestId("Button", "refetch"));
    h.settle();
    CHECK(h.dispatcher.calls.size() == 3);
    h.reply(2, R"({"name":"two"})");
    h.reply(1, R"({"name":"old"})");
    CHECK(h.text("out") == "two");
}

TEST_CASE(
    "interpreter control: a mutation whose reply is not a document value fails; an optimistic write to an idle query "
    "is a no-op",
    "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(R"({
      "vocab": "ui/1", "id": "badreply",
      "models": { "sample": { "type": "Lims_SampleModel" } },
      "state": { "on": { "init": false } },
      "queries": { "rows": { "model": "sample", "action": "Lims_ListResults", "when": { "ref": "on" } } },
      "mutations": { "m": { "model": "sample", "action": "Lims_Save",
                            "optimistic": { "query": "rows", "key": 1, "with": { "ref": "body" } } } },
      "view": { "kind": "button", "testId": "go", "label": "go", "onClick": { "run": "m" } } })");
    h.backend.click(h.byTestId("Button", "go"));
    h.settle();
    h.reply(0, "2.5");
    CHECK(readJson(*screen, R"({"error": "m"})").contains("not a document value"));
    h.backend.click(h.byTestId("Button", "go"));
    h.settle();
    h.reply(1, R"({"id":1})");
    CHECK(readJson(*screen, R"({"result": "m"})") == R"({"id":1})");
}

// Mutation: lower the index as the constant 0 (picking tab 1 then moves only the highlight).
TEST_CASE("interpreter control: picking a tab shows its page, mounted once and kept", "[ui][interpreter]") {
    Harness h;
    auto const screen = h.mount(R"({
      "vocab": "ui/1", "id": "pages",
      "view": { "kind": "tabs", "testId": "bar", "tabs": [
        { "label": "One", "children": [ { "kind": "text", "testId": "first", "text": "1" } ] },
        { "label": "Two", "children": [ { "kind": "text", "testId": "second", "text": "2" } ] } ] } })");
    int const bar = h.backend.find("Tabs", "testId", "bar").value();
    CHECK(h.text("first") == "1");
    CHECK(h.backend.find("Text", "testId", "second") == std::nullopt);
    h.backend.chooseIndex(bar, 1);
    h.settle();
    REQUIRE(h.backend.find("Text", "testId", "second").has_value());
    CHECK(h.text("second") == "2");
    CHECK(h.backend.prop(bar, "selected") == "1");
    h.backend.chooseIndex(bar, 0);
    h.settle();
    // The second page is hidden, not destroyed.
    CHECK(h.backend.find("Text", "testId", "second").has_value());
    CHECK(h.text("first") == "1");
}

// Mutation: read each tab's label once at lowering (the bar keeps `Comments · 0` after the click).
TEST_CASE("interpreter control: a tab label follows the data, and the bar keeps the pick", "[ui][interpreter]") {
    Harness h;
    auto const screen = h.mount(R"({
      "vocab": "ui/1", "id": "labels",
      "state": { "n": { "init": 0 } },
      "view": { "kind": "column", "children": [
        { "kind": "button", "testId": "add", "label": "Add",
          "onClick": [ { "set": "n", "value": { "fn": "add", "args": [ { "ref": "n" }, 1 ] } } ] },
        { "kind": "tabs", "testId": "bar", "tabs": [
          { "label": "Overview", "children": [ { "kind": "text", "text": "o" } ] },
          { "label": { "fn": "format", "args": [ "Comments \u00b7 {0}", { "ref": "n" } ] },
            "children": [ { "kind": "text", "text": "c" } ] } ] } ] } })");
    int const bar = h.backend.find("Tabs", "testId", "bar").value();
    CHECK(h.backend.prop(bar, "tabs") == "[Overview,Comments \u00b7 0]");
    h.backend.chooseIndex(bar, 1);
    h.settle();
    h.backend.click(h.byTestId("Button", "add"));
    h.settle();
    CHECK(h.backend.prop(bar, "tabs") == "[Overview,Comments \u00b7 1]");
    CHECK(h.backend.prop(bar, "selected") == "1");
}

// Mutation: lower `selected` from the node's own pick again (the button's write moves nothing).
TEST_CASE("interpreter control: a command chooses a tab through the state `selected` reads", "[ui][interpreter]") {
    Harness h;
    auto const screen = h.mount(R"({
      "vocab": "ui/1", "id": "record",
      "state": { "tab": { "init": "overview" }, "chose": { "init": "" } },
      "view": { "kind": "column", "children": [
        { "kind": "button", "testId": "book", "label": "Book", "onClick": [ { "set": "tab", "value": "bookings" } ] },
        { "kind": "text", "testId": "chose", "text": { "ref": "chose" } },
        { "kind": "text", "testId": "shown", "text": { "ref": "record.selected" } },
        { "kind": "tabs", "id": "record", "testId": "bar", "selected": { "ref": "tab" },
          "onChoose": [ { "set": "chose", "value": { "ref": "event" } } ],
          "tabs": [
            { "id": "overview", "label": "Overview", "children": [ { "kind": "text", "testId": "first", "text": "1" } ] },
            { "id": "bookings", "label": "Bookings", "children": [ { "kind": "text", "testId": "second", "text": "2" } ] } ] } ] } })");
    int const bar = h.backend.find("Tabs", "testId", "bar").value();
    CHECK(h.backend.prop(bar, "selected") == "0");
    CHECK(h.text("shown") == "overview");
    h.backend.click(h.byTestId("Button", "book"));
    h.settle();
    CHECK(h.backend.prop(bar, "selected") == "1");
    REQUIRE(h.backend.find("Text", "testId", "second").has_value());
    CHECK(h.text("shown") == "bookings");
    // A pick runs onChoose with the tab's id; `tab` is not written, so the bar goes back to the tab it names.
    h.backend.chooseIndex(bar, 0);
    h.settle();
    CHECK(h.text("chose") == "overview");
    CHECK(h.backend.prop(bar, "selected") == "1");
}

// Mutation: apply the surface once in the mount (the panel keeps `outOfSpec` after the flag clears).
TEST_CASE("interpreter control: a surface follows the data", "[ui][interpreter]") {
    Harness h;
    auto const screen = h.mount(R"({
      "vocab": "ui/1", "id": "surfaces",
      "state": { "flag": { "init": true } },
      "view": { "kind": "column", "children": [
        { "kind": "button", "testId": "clear", "label": "Clear", "onClick": [ { "set": "flag", "value": false } ] },
        { "kind": "panel", "testId": "card", "title": "Card",
          "surface": { "if": [ { "ref": "flag" }, "outOfSpec", "" ] },
          "children": [ { "kind": "text", "text": "x" } ] },
        { "kind": "panel", "testId": "odd", "title": "Odd", "surface": { "if": [ { "ref": "flag" }, 3, null ] },
          "children": [ { "kind": "text", "text": "y" } ] } ] } })");
    int const card = h.byTestId("Panel", "card");
    CHECK(h.backend.prop(card, "surface") == "outOfSpec");
    h.backend.click(h.byTestId("Button", "clear"));
    h.settle();
    CHECK(h.backend.prop(card, "surface").empty());
    // A value that is not a string is reported and drawn as no surface; `null` is no surface.
    CHECK(h.backend.prop(h.byTestId("Panel", "odd"), "surface").empty());
    REQUIRE(screen->diagnostics().size() == 1);
    CHECK(screen->diagnostics().front().path == "/view/children/2/surface");
}

// Mutation: build the `forEach` without `commonOf` (no Column has the test id `rows`).
TEST_CASE("interpreter control: a forEach honours its common properties", "[ui][interpreter]") {
    Harness h;
    auto const screen = h.mount(R"({
      "vocab": "ui/1", "id": "rows",
      "state": { "shown": { "init": true } },
      "view": { "kind": "column", "children": [
        { "kind": "button", "testId": "hide", "label": "Hide", "onClick": [ { "set": "shown", "value": false } ] },
        { "kind": "forEach", "testId": "rows", "visible": { "ref": "shown" },
          "sizing": { "lit": { "fixed": 20 } }, "rows": { "lit": [ 1, 2 ] },
          "children": [ { "kind": "text", "testId": "row", "text": { "fn": "format", "args": [ "{0}", { "row": "" } ] } } ] } ] } })");
    int const rows = h.byTestId("Column", "rows");
    CHECK(h.backend.prop(rows, "layout") == "fixed(20)/content");
    CHECK(h.backend.prop(rows, "visible") == "true");
    h.backend.click(h.byTestId("Button", "hide"));
    h.settle();
    CHECK(h.backend.prop(rows, "visible") == "false");
    // Hiding the container keeps its rows mounted.
    std::size_t texts = 0;
    for (int const text : h.backend.all("Text")) {
        texts += h.backend.prop(text, "testId") == "row" ? 1U : 0U;
    }
    CHECK(texts == 2);
}

// Mutation: leave `sizes` unbound (the splitter records `sizes=[]`).
TEST_CASE("interpreter control: a splitter's sizes reach the splitter", "[ui][interpreter]") {
    Harness h;
    auto const screen = h.mount(R"({
      "vocab": "ui/1", "id": "split",
      "state": { "wide": { "init": 50 } },
      "view": { "kind": "column", "children": [
        { "kind": "splitter", "testId": "given", "sizes": { "lit": [ 37, 96 ] },
          "children": [ { "kind": "spacer" }, { "kind": "spacer" } ] },
        { "kind": "splitter", "testId": "computed",
          "sizes": { "list": [ { "fn": "div", "args": [ 59, 2 ] }, { "ref": "wide" } ] },
          "children": [ { "kind": "spacer" }, { "kind": "spacer" } ] },
        { "kind": "splitter", "testId": "wrong", "sizes": { "list": [ "a", 2 ] },
          "children": [ { "kind": "spacer" }, { "kind": "spacer" } ] } ] } })");
    CHECK(h.backend.prop(h.byTestId("Splitter", "given"), "sizes") == "[37,96]");
    // Each size is read as an integer property is: a decimal truncates toward zero.
    CHECK(h.backend.prop(h.byTestId("Splitter", "computed"), "sizes") == "[29,50]");
    // A value that is not a list of numbers is reported and shares the space equally.
    CHECK(h.backend.prop(h.byTestId("Splitter", "wrong"), "sizes") == "[]");
    REQUIRE(h.diagnostics.size() == 1);
    CHECK(h.diagnostics.front().reason.contains("sizes is a list of numbers"));
}

// Mutation: lower no tracks (the grid gets `columns=1` and no `tracks`).
TEST_CASE("interpreter control: a grid's tracks reach the grid in place of a column count", "[ui][interpreter]") {
    Harness h;
    auto const screen = h.mount(R"({
      "vocab": "ui/1", "id": "tracks",
      "view": { "kind": "column", "children": [
        { "kind": "grid", "testId": "plate", "tracks": [
          { "width": { "lit": { "fixed": 3 } } },
          { "width": { "lit": { "fixed": 6 } }, "align": "center" },
          { "align": "end" } ],
          "children": [ { "kind": "text", "text": "A" } ] },
        { "kind": "grid", "testId": "plain", "columns": 2, "children": [] } ] } })");
    int const plate = h.byTestId("Grid", "plate");
    CHECK(h.backend.prop(plate, "tracks") == "[fixed(3),fixed(6);align=center,stretch(1);align=end]");
    CHECK_FALSE(h.backend.hasProp(plate, "columns"));
    int const plain = h.byTestId("Grid", "plain");
    CHECK(h.backend.prop(plain, "columns") == "2");
    CHECK_FALSE(h.backend.hasProp(plain, "tracks"));
}

// Mutation: lower `text` beside `spans`, and the fallback text is recorded; drop the report, and the diagnostic is
// missing.
TEST_CASE("interpreter control: a text's and a banner's spans record their runs in place of the text",
          "[ui][interpreter]") {
    Harness h;
    auto const screen = h.mount(R"({"vocab": "ui/1", "id": "spans",
      "state": {"attempts": {"init": 3}, "count": {"init": 7}},
      "view": {"kind": "column", "children": [
        {"kind": "banner", "tone": "err", "testId": "refused",
         "spans": {"list": [{"obj": {"text": "Password incorrect.", "strong": true}},
                            {"fn": "format", "args": [" {0} of 5 attempts left before the account locks.",
                                                      {"ref": "attempts"}]}]}},
        {"kind": "text", "testId": "lot", "text": "fallback",
         "spans": {"list": ["Lot ", {"obj": {"text": "L-26-0381", "mono": true}}]}},
        {"kind": "text", "testId": "count", "spans": {"list": ["n=", {"ref": "count"}]}},
        {"kind": "text", "testId": "bad",
         "spans": {"list": ["a", {"ref": "count"}, {"if": [true, {"obj": {"x": {"ref": "count"}}}, "z"]}]}},
        {"kind": "button", "testId": "fewer", "label": "Fewer",
         "onClick": [{"set": "attempts", "value": 2}, {"set": "count", "value": 8}]}]}})");
    int const banner = h.byTestId("Banner", "refused");
    CHECK(h.backend.prop(banner, "spans") ==
          "[Password incorrect.;strong, 3 of 5 attempts left before the account locks.]");
    CHECK(h.backend.prop(banner, "text").empty());
    int const lot = h.byTestId("Text", "lot");
    CHECK(h.backend.prop(lot, "spans") == "[Lot ,L-26-0381;mono]");
    CHECK_FALSE(h.backend.dump().contains("text=fallback"));
    CHECK(h.backend.prop(h.byTestId("Text", "count"), "spans") == "[n\\=,7]");
    int const bad = h.byTestId("Text", "bad");
    CHECK(h.backend.prop(bad, "spans") == "[a,7]");
    CHECK(std::ranges::count_if(h.diagnostics, [](ui::Refusal const& refusal) {
              return refusal.path == "/view/children/3/spans";
          }) == 1);
    h.backend.click(h.byTestId("Button", "fewer"));
    h.settle();
    CHECK(h.backend.prop(banner, "spans") ==
          "[Password incorrect.;strong, 2 of 5 attempts left before the account locks.]");
    CHECK(h.backend.prop(bad, "spans") == "[a,8]");
    CHECK(std::ranges::count_if(h.diagnostics, [](ui::Refusal const& refusal) {
              return refusal.path == "/view/children/3/spans";
          }) == 1);
}

// Mutation: read every item's tone as none (the `;tone=Err` is not recorded).
TEST_CASE("interpreter control: a stacked key-value records its captions, its items' runs and their tones",
          "[ui][interpreter]") {
    Harness h;
    auto const screen = h.mount(R"({"vocab": "ui/1", "id": "kv",
      "state": {"enough": {"init": false}, "line": {"init": "180 needed, 150 in stock"}},
      "view": {"kind": "column", "children": [
        {"kind": "keyValue", "testId": "kv", "fieldLayout": "stacked",
         "items": {"list": [
           {"obj": {"label": "Serial number", "spans": {"list": [{"obj": {"text": "DEAB-902114", "mono": true}}]}}},
           {"obj": {"label": "Containers", "value": {"ref": "line"},
                    "tone": {"if": [{"ref": "enough"}, null, "err"]}}},
           {"obj": {"label": "Note; tone=Ok", "value": "a;b", "tone": "loud"}}]}},
        {"kind": "button", "testId": "stock", "label": "Stock", "onClick": [{"set": "enough", "value": true}]}]}})");
    int const kv = h.byTestId("KeyValue", "kv");
    CHECK(h.backend.prop(kv, "captions") == "Above");
    CHECK(h.backend.prop(kv, "items") == "[Serial number:[DEAB-902114;mono],Containers:180 needed\\, 150 in "
                                         "stock;tone=Err,Note\\; tone\\=Ok:a\\;b]");
    CHECK(std::ranges::count_if(h.diagnostics, [](ui::Refusal const& refusal) {
              return refusal.path == "/view/children/0/items" && refusal.reason.contains("\"loud\"");
          }) == 1);
    h.backend.click(h.byTestId("Button", "stock"));
    h.settle();
    CHECK(h.backend.prop(kv, "items") ==
          "[Serial number:[DEAB-902114;mono],Containers:180 needed\\, 150 in stock,Note\\; tone\\=Ok:a\\;b]");
}

namespace {

constexpr char const* kToned = R"({"vocab": "ui/1", "id": "toned",
  "models": {"sample": {"type": "Lims_SampleModel"}},
  "view": {"kind": "column", "children": [
    {"kind": "button", "testId": "open", "label": "open", "onClick": {"open": "dlg"}},
    {"kind": "text", "testId": "ready", "text": {"fn": "format", "args": ["{0}", {"ref": "dlg.ready"}]}},
    {"kind": "dialog", "id": "dlg", "title": "Result",
     "scope": {"mutations": {"save": {"model": "sample", "action": "Lims_Save", "body": {"obj": {}}}}},
     "children": [
       {"kind": "textInput", "id": "impurity", "testId": "impurity", "required": true,
        "description": "Out of spec: above 0.20", "tone": "err" EXTRA},
       {"kind": "button", "testId": "save", "label": "save", "onClick": {"run": "save"}}]}]}})";

std::string toned(std::string const& extra) {
    std::string text = kToned;
    text.replace(text.find("EXTRA"), 5, extra);
    return text;
}

}  // namespace

// Mutation: route `tone` into `errors` in fieldOf (the toned field is then invalid, and the run is refused).
TEST_CASE("interpreter control: a field's tone evaluates its value without making it invalid", "[ui][interpreter]") {
    {
        Harness h;
        auto const screen = h.mount(toned(""));
        h.backend.click(h.byTestId("Button", "open"));
        h.settle();
        int const input = h.byTestId("TextInput", "impurity");
        CHECK(h.backend.prop(input, "tone") == "Err");
        CHECK(h.backend.prop(input, "description") == "Out of spec: above 0.20");
        h.backend.edit(input, "0.31");
        h.settle();
        CHECK(h.text("ready") == "true");
        h.backend.click(h.byTestId("Button", "save"));
        h.settle();
        CHECK(h.dispatcher.count("Lims_Save") == 1);
    }
    Harness h;
    auto const screen = h.mount(toned(R"(, "errors": {"list": ["x"]})"));
    h.backend.click(h.byTestId("Button", "open"));
    h.settle();
    h.backend.edit(h.byTestId("TextInput", "impurity"), "0.31");
    h.settle();
    CHECK(h.text("ready") == "false");
}

// Mutation: drop the unit's binding (no `unit` is recorded); drop the computed-mode check (the mount throws).
TEST_CASE("interpreter control: a text input's unit is bound, and a computed multiline mode drops it with a report",
          "[ui][interpreter]") {
    Harness h;
    auto const screen = h.mount(R"({"vocab": "ui/1", "id": "unit",
      "state": {"unit": {"init": "°C"}, "mode": {"init": "multiline"}},
      "view": {"kind": "column", "children": [
        {"kind": "textInput", "testId": "fixed", "label": "Temperature", "unit": "°C"},
        {"kind": "textInput", "testId": "bound", "label": "Assay", "unit": {"ref": "unit"}},
        {"kind": "textInput", "testId": "area", "label": "Note", "mode": {"ref": "mode"}, "unit": "%",
         "icon": "search"},
        {"kind": "button", "testId": "percent", "label": "%", "onClick": [{"set": "unit", "value": "% LC"}]}]}})");
    CHECK(h.backend.prop(h.byTestId("TextInput", "fixed"), "unit") == "°C");
    int const bound = h.byTestId("TextInput", "bound");
    CHECK(h.backend.prop(bound, "unit") == "°C");
    h.backend.click(h.byTestId("Button", "percent"));
    h.settle();
    CHECK(h.backend.prop(bound, "unit") == "% LC");
    int const area = h.byTestId("TextInput", "area");
    CHECK_FALSE(h.backend.hasProp(area, "unit"));
    CHECK_FALSE(h.backend.hasProp(area, "icon"));
    CHECK(std::ranges::count_if(h.diagnostics, [](ui::Refusal const& refusal) {
              return refusal.reason == "a multiline input shows no unit";
          }) == 1);
    CHECK(std::ranges::count_if(h.diagnostics, [](ui::Refusal const& refusal) {
              return refusal.reason == "a multiline input shows no icon";
          }) == 1);
}

// Mutation: take a computed style of no known name as radio (the select records `style=Radio`).
TEST_CASE("interpreter control: a computed select style naming none is reported once and drawn as a dropdown",
          "[ui][interpreter]") {
    Harness h;
    auto const screen = h.mount(R"({"vocab": "ui/1", "id": "style", "state": {"style": {"init": "radios"}},
      "view": {"kind": "column", "children": [
        {"kind": "select", "testId": "odd", "label": "Show", "style": {"ref": "style"}, "options": {"list": []}},
        {"kind": "select", "testId": "seg", "label": "Show", "style": "segmented", "options": {"list": []}},
        {"kind": "badge", "testId": "stat", "text": "STAT", "style": "outline"},
        {"kind": "button", "testId": "reject", "label": "Reject", "emphasis": "dangerOutline"}]}})");
    CHECK(h.backend.prop(h.byTestId("Select", "odd"), "style") == "Dropdown");
    CHECK(h.backend.prop(h.byTestId("Select", "seg"), "style") == "Segmented");
    CHECK(h.backend.prop(h.byTestId("Badge", "stat"), "style") == "Outline");
    CHECK(h.backend.prop(h.byTestId("Button", "reject"), "emphasis") == "DangerOutline");
    CHECK(std::ranges::count_if(h.diagnostics, [](ui::Refusal const& refusal) {
              return refusal.reason.contains("a select's style is dropdown, radio or segmented");
          }) == 1);
}

// Mutation: have mountApp skip `formatting` (both the input and the text read ISO).
TEST_CASE("interpreter control: the app document's date patterns reach a screen's inputs and its date texts",
          "[ui][interpreter]") {
    Harness h;
    auto root = h.interpreter.mountApp(ui::testing::loadOrFail(
        R"({"vocab": "ui/1", "id": "app", "formatting": {"date": "dd-MMM-yyyy", "dateTime": "dd-MMM-yyyy HH:mm"}})",
        ui::testing::corpusCatalog()));
    REQUIRE(root.has_value());
    auto const screen = h.mount(R"({"vocab": "ui/1", "id": "dates",
      "state": {"when": {"init": "2026-10-24T09:41:00Z"}},
      "view": {"kind": "column", "children": [
        {"kind": "dateTimeInput", "testId": "pulled", "label": "Pulled", "value": {"ref": "when"}},
        {"kind": "dateTimeInput", "testId": "day", "label": "Day", "mode": "date", "value": {"ref": "when"}},
        {"kind": "text", "testId": "shown", "text": {"fn": "dateTime", "args": [{"ref": "when"}]}}]}})");
    CHECK(h.backend.prop(h.byTestId("DateTimeInput", "pulled"), "pattern") == "dd-MMM-yyyy HH:mm");
    CHECK(h.backend.prop(h.byTestId("DateTimeInput", "day"), "pattern") == "dd-MMM-yyyy");
    CHECK(h.text("shown") == "24-Oct-2026 09:41");
}

// Mutation: drop the emphasis in the textInput lowering (no `emphasis` is recorded).
TEST_CASE("interpreter control: a prominent text input records its emphasis", "[ui][interpreter]") {
    Harness h;
    auto const screen = h.mount(R"({"vocab": "ui/1", "id": "scan",
      "view": {"kind": "textInput", "testId": "scan", "label": "Scan", "emphasis": "prominent"}})");
    CHECK(h.backend.prop(h.byTestId("TextInput", "scan"), "emphasis") == "Prominent");
}
