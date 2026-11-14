// SPDX-License-Identifier: Apache-2.0
//
// Interpreter measurements: mounting a ui/1 document with a table, and re-running its reactive graph after a
// state change. Hidden, so ctest never runs it; run it by tag on a Release build:
//
//   morph_tests "[.benchmark][interpreter]"

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <format>
#include <string>

#include "ui_interpreter_support.hpp"

namespace ui = morph::ui;
using ui::testing::Harness;

namespace {

/// A document with a state-driven table of `rows` rows, a few computed texts and a button that bumps a counter every
/// text reads.
std::string benchDocument(std::size_t rows) {
    std::string items;
    for (std::size_t i = 0; i < rows; ++i) {
        items += std::format(R"({}{{ "id": {}, "name": "row-{}", "qty": {} }})", i == 0 ? "" : ",", i, i, i % 17);
    }
    return std::format(R"({{
  "vocab": "ui/1", "id": "bench",
  "state": {{ "count": {{ "init": 0 }}, "items": {{ "init": {{ "lit": [{}] }} }} }},
  "view": {{ "kind": "column", "children": [
    {{ "kind": "button", "testId": "bump", "label": "bump",
       "onClick": {{ "set": "count", "value": {{ "fn": "add", "args": [ {{ "ref": "count" }}, 1 ] }} }} }},
    {{ "kind": "text", "testId": "shown", "text": {{ "fn": "format", "args": [ "{{0}} clicks", {{ "ref": "count" }} ] }} }},
    {{ "kind": "table", "rows": {{ "ref": "items" }},
       "columns": [ {{ "id": "id", "label": "Id" }}, {{ "id": "name", "label": "Name" }}, {{ "id": "qty", "label": "Qty" }} ] }} ] }} }})",
                       items);
}

}  // namespace

TEST_CASE("interpreter bench: mount a document with a table", "[.benchmark][interpreter]") {
    std::string const doc = benchDocument(200);
    BENCHMARK("mount, 200 rows") {
        Harness h;
        auto screen = h.mount(doc);
        return h.backend.all("Table").size();
    };
}

TEST_CASE("interpreter bench: update state read by a mounted screen", "[.benchmark][interpreter]") {
    std::string const doc = benchDocument(200);
    Harness h;
    auto screen = h.mount(doc);
    int const bump = h.byTestId("Button", "bump");
    // Guards the measurement: a click that changed nothing would time an empty path.
    REQUIRE(h.text("shown") == "0 clicks");
    h.backend.click(bump);
    h.settle();
    REQUIRE(h.text("shown") == "1 clicks");
    BENCHMARK("click and settle") {
        h.backend.click(bump);
        h.settle();
        return h.backend.all("Text").size();
    };
}
