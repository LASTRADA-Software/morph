// SPDX-License-Identifier: Apache-2.0

// The control corpus of spec 5 §14: queries (latest wins, debounce, timed refresh, the cache), mutations (the four
// concurrency modes, patch, optimistic writes, one-batch invalidation) and boundaries, mounted on RecordingBackend
// with a scripted dispatcher.

#include <catch2/catch_test_macros.hpp>
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
           "invalidates": [ "rows", "other" ],
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
    // A failure invalidates nothing.
    CHECK(h.dispatcher.calls.size() == before);
}

TEST_CASE("interpreter control: invalidation refetches every named query in the success batch", "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(mutations("exclusive"));
    loadQueries(h);
    h.backend.click(h.byTestId("Button", "go"));
    h.settle();
    h.dispatcher.reply(h.dispatcher.last("Lims_Save"), "1");
    // The delivery is one task of the owner: the result, the cleared pending flag and both refetches.
    REQUIRE(h.owner.runOne());
    CHECK(h.dispatcher.count("Lims_ListResults") == 2);
    CHECK(h.dispatcher.count("Lims_GetLimits") == 2);
    h.settle();
    CHECK(h.text("result") == "1");
}

namespace {

constexpr char const* kRows = R"({
  "vocab": "ui/1", "id": "rows",
  "models": { "sample": { "type": "Lims_SampleModel" } },
  "queries": { "results": { "model": "sample", "action": "Lims_ListResults" } },
  "mutations": {
    "capture": { "model": "sample", "action": "Lims_CaptureConcentration", "concurrency": "parallel",
                 "patch": { "query": "results", "key": { "ref": "result.id" }, "with": { "ref": "result" } } },
    "guess": { "model": "sample", "action": "Lims_Save", "concurrency": "parallel",
               "optimistic": { "query": "results", "key": { "ref": "body.id" },
                               "with": { "obj": { "mass": { "ref": "body.mass" } } } },
               "onError": [ { "notify": { "fn": "format", "args": [ "row 2 at {0}", { "ref": "row2.mass" } ] } } ] } },
  "let": { "row2": { "fn": "first", "args": [ { "filter": { "query": "results" }, "as": "r",
               "where": { "fn": "eq", "args": [ { "ref": "r.id" }, 2 ] } } ] } },
  "view": { "kind": "column", "children": [
    { "kind": "textInput", "testId": "capture", "onSubmit": { "run": "capture", "body": { "obj": { "id": 2, "mass": { "ref": "event" } } } } },
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

TEST_CASE("interpreter control: patch replaces the replied row without refetching", "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(kRows);
    h.reply(0, R"([{"id":1,"mass":10},{"id":2,"mass":20}])");
    CHECK(rowTexts(h) == std::vector<std::string>{"1:10", "2:20"});
    h.backend.submit(h.byTestId("TextInput", "capture"), "25");
    h.settle();
    h.reply(h.dispatcher.last("Lims_CaptureConcentration"), R"({"id":2,"mass":25,"concentration":5})");
    CHECK(rowTexts(h) == std::vector<std::string>{"1:10", "2:25"});
    CHECK(h.dispatcher.count("Lims_ListResults") == 1);
}

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
// `{"results": [...]}` gives them. AT is replaced with the `at` member of both patches, or with nothing.
constexpr char const* kRecordRows = R"({
  "vocab": "ui/1", "id": "recordrows",
  "models": { "sample": { "type": "Lims_SampleModel" } },
  "queries": { "results": { "model": "sample", "action": "Lims_ListResults" } },
  "mutations": {
    "capture": { "model": "sample", "action": "Lims_CaptureConcentration", "concurrency": "parallel",
                 "patch": { "query": "results", AT "key": { "ref": "result.id" }, "with": { "ref": "result" } } },
    "guess": { "model": "sample", "action": "Lims_Save", "concurrency": "parallel",
               "optimistic": { "query": "results", AT "key": { "ref": "body.id" },
                               "with": { "obj": { "mass": { "ref": "body.mass" } } } } } },
  "view": { "kind": "column", "children": [
    { "kind": "textInput", "testId": "capture", "onSubmit": { "run": "capture", "body": { "obj": { "id": 2, "mass": { "ref": "event" } } } } },
    { "kind": "textInput", "testId": "guess", "onSubmit": { "run": "guess", "body": { "obj": { "id": 2, "mass": { "ref": "event" } } } } } ] } })";

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

TEST_CASE("interpreter control: a patch with `at` replaces a row of the list inside the query's record",
          "[ui][interpreter]") {
    Harness h;
    auto const screen = h.mount(recordRows(R"("at": "results",)"));
    h.reply(0, kRecordReply);

    SECTION("patch") {
        h.backend.submit(h.byTestId("TextInput", "capture"), "25");
        h.settle();
        h.reply(h.dispatcher.last("Lims_CaptureConcentration"), R"({"id":2,"mass":25})");
        CHECK(readJson(*screen, R"({"ref": "results"})") == R"({"results":[{"id":1,"mass":10},{"id":2,"mass":25}]})");
        CHECK(h.dispatcher.count("Lims_ListResults") == 1);
    }
    SECTION("optimistic, then rolled back on failure") {
        h.backend.submit(h.byTestId("TextInput", "guess"), "30");
        h.settle();
        CHECK(readJson(*screen, R"({"ref": "results"})") ==
              R"({"results":[{"id":1,"mass":10},{"id":2,"mass":"30"}]})");
        h.fail(h.dispatcher.last("Lims_Save"));
        CHECK(readJson(*screen, R"({"ref": "results"})") == kRecordReply);
    }
    CHECK(h.diagnostics.empty());
}

TEST_CASE("interpreter control: a patch that finds no list in the query's value is reported", "[ui][interpreter]") {
    Harness h;
    SECTION("a record value and no `at`") {
        auto const screen = h.mount(recordRows(""));
        h.reply(0, kRecordReply);
        h.backend.submit(h.byTestId("TextInput", "capture"), "25");
        h.settle();
        h.reply(h.dispatcher.last("Lims_CaptureConcentration"), R"({"id":2,"mass":25})");
        CHECK(readJson(*screen, R"({"ref": "results"})") == kRecordReply);
        REQUIRE(h.diagnostics.size() == 1);
        CHECK(h.diagnostics.front().path == "/mutations/capture/patch");
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

TEST_CASE("interpreter control: refreshOn refetches a query of the same model type in another screen",
          "[ui][interpreter]") {
    Harness h;
    auto lists = h.mount(R"({
      "vocab": "ui/1", "id": "list",
      "models": { "sample": { "type": "Lims_SampleModel" } },
      "queries": { "results": { "model": "sample", "action": "Lims_ListResults", "refreshOn": [ "Lims_CaptureConcentration" ] } },
      "view": { "kind": "text", "text": { "query": "results" } } })");
    h.reply(0, "1");
    ui::testing::RecordingBackend other;
    auto capture = h.interpreter.mount(ui::testing::loadOrFail(R"({
      "vocab": "ui/1", "id": "capture",
      "models": { "work": { "type": "Lims_SampleModel", "instance": "private" } },
      "mutations": { "c": { "model": "work", "action": "Lims_CaptureConcentration" },
                     "s": { "model": "work", "action": "Lims_Save" } },
      "view": { "kind": "column", "children": [
        { "kind": "button", "testId": "c", "label": "c", "onClick": { "run": "c" } },
        { "kind": "button", "testId": "s", "label": "s", "onClick": { "run": "s" } } ] } })",
                                                               ui::testing::corpusCatalog()),
                                       {}, other);
    REQUIRE(capture);
    h.settle();
    other.click(*other.find("Button", "testId", "s"));
    h.settle();
    h.reply(h.dispatcher.last("Lims_Save"), "{}");
    CHECK(h.dispatcher.count("Lims_ListResults") == 1);
    other.click(*other.find("Button", "testId", "c"));
    h.settle();
    h.reply(h.dispatcher.last("Lims_CaptureConcentration"), "{}");
    CHECK(h.dispatcher.count("Lims_ListResults") == 2);
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
    "interpreter control: a mutation whose reply is not a document value fails; patch of an idle query is a no-op",
    "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(R"({
      "vocab": "ui/1", "id": "badreply",
      "models": { "sample": { "type": "Lims_SampleModel" } },
      "state": { "on": { "init": false } },
      "queries": { "rows": { "model": "sample", "action": "Lims_ListResults", "when": { "ref": "on" } } },
      "mutations": { "m": { "model": "sample", "action": "Lims_Save",
                            "patch": { "query": "rows", "key": 1, "with": { "ref": "result" } },
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
