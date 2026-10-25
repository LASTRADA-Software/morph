// SPDX-License-Identifier: Apache-2.0

// A table over rows, lowered by the interpreter and mounted on RecordingBackend: its columns' options and its
// selection.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

#include "ui_interpreter_support.hpp"

namespace ui = morph::ui;
using ui::testing::Harness;

namespace {

// The roles of the default cells, row by row, left to right. A row's first cell sits inside the column that holds
// the row's scope.
std::vector<std::string> cellRoles(Harness const& h) {
    std::vector<std::string> roles;
    for (int const row : h.backend.all("Row")) {
        for (int cell : h.backend.children(row)) {
            while (h.backend.kindOf(cell) != "Text" && !h.backend.children(cell).empty()) {
                cell = h.backend.children(cell).front();
            }
            roles.push_back(h.backend.prop(cell, "role"));
        }
    }
    return roles;
}

}  // namespace

// Mutation: map `mono` to `Normal` in `roleNamed` (the first column reads `Normal`).
TEST_CASE("table: a column's mono role reaches its default cell, and a computed role that names none is reported once",
          "[ui][interpreter]") {
    Harness h;
    auto screen = h.mount(R"({
      "vocab": "ui/1", "id": "mono",
      "state": { "kind": { "init": "monospace" },
                 "items": { "init": { "lit": [ { "id": "S-1", "n": 1 }, { "id": "S-2", "n": 2 } ] } } },
      "view": { "kind": "table", "rows": { "ref": "items" },
                "columns": [ { "id": "id", "label": "Sample", "role": "mono" },
                             { "id": "n", "label": "N", "role": { "ref": "kind" } } ] } })");
    CHECK(cellRoles(h) == std::vector<std::string>{"Mono", "Normal", "Mono", "Normal"});
    auto const reports = std::ranges::count_if(h.diagnostics, [](ui::Refusal const& refusal) {
        return refusal.reason.contains(R"(role is "monospace")") && refusal.reason.contains("mono");
    });
    CHECK(reports == 1);
}
