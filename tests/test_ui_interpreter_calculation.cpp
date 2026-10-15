// SPDX-License-Identifier: Apache-2.0

// The server-calculated values corpus of spec 5 §7 and §14: a table whose rows keep a draft, preview a calculation
// through a debounced row query, and write through a serial row mutation whose reply patches the list.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <string>
#include <vector>

#include "ui_interpreter_support.hpp"

namespace ui = morph::ui;
using ui::testing::Harness;
using ui::testing::readJson;
using namespace std::chrono_literals;

namespace {

constexpr char const* kCalc = R"({
  "vocab": "ui/1", "id": "calc", "showErrors": "always",
  "models": { "sample": { "type": "Lims_SampleModel" } },
  "queries": { "results": { "model": "sample", "action": "Lims_ListResults" } },
  "view": { "kind": "table", "id": "grid", "rows": { "query": "results" }, "key": { "row": "id" },
    "scope": {
      "state": { "draft": { "init": { "row": "inputs" } } },
      "queries": { "calc": { "model": "sample", "action": "Lims_EvaluateResult", "debounce": 300,
                             "body": { "obj": { "resultId": { "row": "id" }, "inputs": { "ref": "draft" } } } } },
      "mutations": { "save": { "model": "sample", "action": "Lims_CaptureConcentration", "concurrency": "serial",
                               "serialKey": { "row": "id" },
                               "body": { "obj": { "resultId": { "row": "id" }, "inputs": { "ref": "draft" } } },
                               "patch": { "query": "results", "key": { "ref": "result.id" }, "with": { "ref": "result" } } } } },
    "columns": [
      { "label": "Mass", "cell": { "kind": "textInput", "id": "mass", "testId": "mass", "commitOn": "change",
                                   "value": { "ref": "draft.mass" },
                                   "onChange": [ { "set": "draft.mass", "value": { "ref": "event" } } ],
                                   "onCommit": [ { "run": "save" } ] } },
      { "label": "Result", "cell": { "kind": "text", "testId": "result",
                                     "text": { "fn": "format", "args": [ "{0}", { "fn": "coalesce", "args": [ { "ref": "calc.concentration" }, { "row": "concentration" } ] } ] },
                                     "stale": { "pending": "calc" } } },
      { "label": "Saved", "cell": { "kind": "textInput", "testId": "saved", "value": { "row": "mass" } } },
      { "label": "Error", "cell": { "kind": "text", "testId": "error", "text": { "fn": "coalesce", "args": [ { "error": "calc" }, "" ] } } } ] } })";

constexpr char const* kRows = R"([
  {"id":1,"inputs":{"mass":"10"},"mass":10,"concentration":1},
  {"id":2,"inputs":{"mass":"20"},"mass":20,"concentration":2}])";

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a widget kind, then a test id, as the harness takes them.
std::vector<int> widgets(Harness& h, std::string const& kind, std::string const& testId) {
    std::vector<int> out;
    for (int const id : h.backend.all(kind)) {
        if (h.backend.prop(id, "testId") == testId) {
            out.push_back(id);
        }
    }
    return out;
}

std::string prop(Harness& h, std::string const& kind, std::string const& testId, std::size_t row,
                 std::string const& name) {
    auto const found = widgets(h, kind, testId);
    REQUIRE(found.size() > row);
    return h.backend.prop(found.at(row), name);
}

// Mounts the table with its two rows and answers each row's first calculation.
std::unique_ptr<ui::Screen> mountRows(Harness& h) {
    auto screen = h.mount(kCalc);
    h.reply(0, kRows);
    REQUIRE(h.dispatcher.count("Lims_EvaluateResult") == 2);
    h.reply(1, R"({"concentration":1})");
    h.reply(2, R"({"concentration":2})");
    return screen;
}

}  // namespace

TEST_CASE("interpreter calculation: each row previews its own calculation", "[ui][interpreter]") {
    Harness h;
    auto screen = mountRows(h);
    CHECK(h.dispatcher.calls.at(1).body == R"({"inputs":{"mass":"10"},"resultId":1})");
    CHECK(prop(h, "Text", "result", 0, "text") == "1");
    CHECK(prop(h, "Text", "result", 1, "text") == "2");
    CHECK(prop(h, "TextInput", "mass", 0, "text") == "10");
}

TEST_CASE("interpreter calculation: a reply from a superseded request is dropped", "[ui][interpreter]") {
    Harness h;
    auto screen = mountRows(h);
    int const mass = widgets(h, "TextInput", "mass").at(0);
    h.backend.edit(mass, "11");
    h.settle();
    h.advance(300ms);
    std::size_t const first = h.dispatcher.last("Lims_EvaluateResult");
    h.backend.edit(mass, "12");
    h.settle();
    h.advance(300ms);
    std::size_t const second = h.dispatcher.last("Lims_EvaluateResult");
    REQUIRE(second == first + 1);
    CHECK(h.dispatcher.calls.at(second).body == R"({"inputs":{"mass":"12"},"resultId":1})");
    h.reply(second, R"({"concentration":12})");
    h.reply(first, R"({"concentration":11})");
    CHECK(prop(h, "Text", "result", 0, "text") == "12");
}

TEST_CASE("interpreter calculation: typing while a calculation is in flight keeps the cells stale, never blank",
          "[ui][interpreter]") {
    Harness h;
    auto screen = mountRows(h);
    int const mass = widgets(h, "TextInput", "mass").at(0);
    h.backend.edit(mass, "11");
    h.settle();
    CHECK(prop(h, "Text", "result", 0, "stale") == "true");
    CHECK(prop(h, "Text", "result", 0, "text") == "1");
    h.advance(300ms);
    std::size_t const first = h.dispatcher.last("Lims_EvaluateResult");
    h.backend.edit(mass, "12");
    h.settle();
    h.reply(first, R"({"concentration":11})");
    CHECK(prop(h, "Text", "result", 0, "stale") == "true");
    h.advance(300ms);
    h.reply(h.dispatcher.last("Lims_EvaluateResult"), R"({"concentration":12})");
    CHECK(prop(h, "Text", "result", 0, "stale") == "false");
    CHECK(prop(h, "Text", "result", 0, "text") == "12");
    // The other row was never touched.
    CHECK(prop(h, "Text", "result", 1, "text") == "2");
    CHECK(prop(h, "Text", "result", 1, "stale") == "false");
}

TEST_CASE("interpreter calculation: a reply never overwrites an input the user changed after the request",
          "[ui][interpreter]") {
    Harness h;
    auto screen = mountRows(h);
    int const saved = widgets(h, "TextInput", "saved").at(0);
    int const mass = widgets(h, "TextInput", "mass").at(0);
    h.backend.edit(mass, "15");
    h.settle();
    h.backend.commit(mass);
    h.settle();
    std::size_t const write = h.dispatcher.last("Lims_CaptureConcentration");
    CHECK(h.dispatcher.calls.at(write).body == R"({"inputs":{"mass":"15"},"resultId":1})");
    // The user types into the input the reply will also write.
    h.backend.edit(saved, "16");
    h.settle();
    h.reply(write, R"({"id":1,"inputs":{"mass":"15"},"mass":15,"concentration":2})");
    CHECK(readJson(*screen, R"({"fn": "count", "args": [ { "query": "results" } ]})") == "2");
    CHECK(h.diagnostics.empty());
    CHECK(prop(h, "TextInput", "saved", 0, "text") == "16");
    // An input the user left alone follows the reply.
    h.backend.commit(widgets(h, "TextInput", "mass").at(1));
    h.settle();
    h.reply(h.dispatcher.last("Lims_CaptureConcentration"), R"({"id":2,"inputs":{"mass":"20"},"mass":21})");
    CHECK(prop(h, "TextInput", "saved", 1, "text") == "21");
}

TEST_CASE("interpreter calculation: a reply's field errors attach to the row's element of that name",
          "[ui][interpreter]") {
    Harness h;
    auto screen = mountRows(h);
    int const mass = widgets(h, "TextInput", "mass").at(1);
    h.backend.edit(mass, "-1");
    h.settle();
    h.advance(300ms);
    h.reply(h.dispatcher.last("Lims_EvaluateResult"),
            R"({"concentration":null,"errors":{"mass":"must be positive"}})");
    CHECK(prop(h, "TextInput", "mass", 1, "errors") == "[must be positive]");
    CHECK(prop(h, "TextInput", "mass", 0, "errors") == "[]");
}

TEST_CASE("interpreter calculation: a transport failure becomes the row's error and keeps the stale value",
          "[ui][interpreter]") {
    Harness h;
    auto screen = mountRows(h);
    h.backend.edit(widgets(h, "TextInput", "mass").at(0), "11");
    h.settle();
    h.advance(300ms);
    h.fail(h.dispatcher.last("Lims_EvaluateResult"), "offline");
    CHECK(prop(h, "Text", "error", 0, "text") == "it failed");
    CHECK(prop(h, "Text", "result", 0, "text") == "1");
    CHECK(prop(h, "Text", "error", 1, "text").empty());
}

TEST_CASE("interpreter calculation: write-through is serial per row, and rows do not wait for each other",
          "[ui][interpreter]") {
    Harness h;
    auto screen = mountRows(h);
    auto const masses = widgets(h, "TextInput", "mass");
    h.backend.commit(masses.at(0));
    h.settle();
    h.backend.commit(masses.at(0));
    h.settle();
    h.backend.commit(masses.at(1));
    h.settle();
    REQUIRE(h.dispatcher.count("Lims_CaptureConcentration") == 2);
    std::size_t const rowTwo = h.dispatcher.last("Lims_CaptureConcentration");
    CHECK(h.dispatcher.calls.at(rowTwo).body == R"({"inputs":{"mass":"20"},"resultId":2})");
    h.reply(rowTwo, R"({"id":2,"inputs":{"mass":"20"},"mass":20})");
    CHECK(h.dispatcher.count("Lims_CaptureConcentration") == 2);
    h.reply(rowTwo - 1, R"({"id":1,"inputs":{"mass":"10"},"mass":10})");
    CHECK(h.dispatcher.count("Lims_CaptureConcentration") == 3);
    CHECK(h.dispatcher.calls.at(h.dispatcher.last("Lims_CaptureConcentration")).body ==
          R"({"inputs":{"mass":"10"},"resultId":1})");
}

// Mutation: make parseDecimal ignore its places -> "12.5" is sent at 1 place, not 3, and the body differs.
TEST_CASE("interpreter calculation: parseDecimal turns a cell's text into the number the body carries",
          "[ui][interpreter]") {
    Harness h;
    std::string document = kCalc;
    std::string const typed = R"("onChange": [ { "set": "draft.mass", "value": { "ref": "event" } } ])";
    document.replace(document.find(typed), typed.size(),
                     R"("onChange": [ { "set": "draft.mass",
                         "value": { "fn": "parseDecimal", "args": [ { "ref": "event" }, 3 ] } } ])");
    auto const screen = h.mount(document);
    h.reply(0, kRows);
    h.reply(1, R"({"concentration":1})");
    h.reply(2, R"({"concentration":2})");
    int const mass = widgets(h, "TextInput", "mass").at(0);
    // Part-way through typing the text spells no number: the draft is null, and the input keeps what was typed.
    h.backend.edit(mass, "12.");
    h.settle();
    CHECK(prop(h, "TextInput", "mass", 0, "text") == "12.");
    h.advance(300ms);
    CHECK(h.dispatcher.calls.at(h.dispatcher.last("Lims_EvaluateResult")).body ==
          R"({"inputs":{"mass":null},"resultId":1})");
    h.backend.edit(mass, "12.5");
    h.settle();
    CHECK(prop(h, "TextInput", "mass", 0, "text") == "12.5");
    h.advance(300ms);
    CHECK(h.dispatcher.calls.at(h.dispatcher.last("Lims_EvaluateResult")).body ==
          R"({"inputs":{"mass":{"num":25,"den":2,"dp":3}},"resultId":1})");
}
