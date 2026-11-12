// SPDX-License-Identifier: Apache-2.0

// A table row's spans (`TableRowView::spans`): what the mount resolves and tells the widget, as the recording backend
// shows it, and the document's `rowSpans`.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/testing/owner_probe_recorder.hpp>
#include <morph/testing/step_executor.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <morph/ui/view.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "test_support.hpp"
#include "ui_interpreter_support.hpp"
#include "ui_test_support.hpp"

namespace ui = morph::ui;

namespace {

using morph::reactive::Runtime;
using morph::reactive::Signal;
using ui::testing::RecordingBackend;
using Owner = morph::testing::StepExecutor;
using Probe = morph::testing::OwnerProbeRecorder;
namespace site = ui::detail::site;

struct Line {
    std::int64_t id = 0;
    std::vector<ui::RowSpan> spans;
    bool heading = false;
    bool operator==(Line const&) const = default;
};

// Three columns of text; a row's spans and heading come from the line.
struct Fixture {
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::vector<Line>> lines;
    Signal<std::optional<ui::TableSort>> sort{runtime, std::nullopt};

    explicit Fixture(std::vector<Line> initial) : lines{runtime, std::move(initial)} {}

    ui::Node view(bool sortable = false, std::size_t threshold = 2000) {
        std::vector<ui::TableColumn> columns{{.label = "A"}, {.label = "B"}, {.label = "C"}};
        columns.front().sortable = sortable;
        return ui::table<Line>(
            std::move(columns), [this] { return lines.get(); }, [](Line const& line) { return ui::Key{line.id}; },
            [](Signal<Line> const& line) {
                std::size_t const count = line.peek().heading ? 1 : 3;
                std::vector<ui::Node> cells;
                for (std::size_t index = 0; index < count; ++index) {
                    cells.push_back(ui::text({.text = std::to_string(line.peek().id)}));
                }
                ui::TableRowView made{.cells = std::move(cells), .heading = line.peek().heading};
                made.spans = ui::Prop<std::vector<ui::RowSpan>>{[&line] { return line.get().spans; }};
                return made;
            },
            {.virtualize = {.threshold = threshold}, .sort = [this] { return sort.get(); }});
    }

    // The recorded `rowSpans` of the row with key @p id, `-` while it was never told any.
    [[nodiscard]] std::string spansOf(std::int64_t id) const {
        for (int const row : backend.all("Row")) {
            if (backend.prop(row, "rowKey") == std::to_string(id)) {
                std::string const value = backend.prop(row, "rowSpans");
                return value.empty() ? "-" : value;
            }
        }
        return "no row";
    }
};

std::vector<Line> three() { return {{.id = 1, .spans = {{.column = 0, .rows = 3}}}, {.id = 2}, {.id = 3}}; }

}  // namespace

// Mutation: leave `spans` out of the row the mount builds (the first row records no spans).
TEST_CASE("row spans: a row's span reaches the widget, and a row without one is told nothing", "[ui][table]") {
    Fixture fixture{three()};
    ui::Mounted const mounted{fixture.runtime, fixture.backend, fixture.view()};
    CHECK(fixture.spansOf(1) == "0:3");
    CHECK(fixture.spansOf(2) == "-");
    CHECK(fixture.spansOf(3) == "-");
    CHECK(fixture.probe.count(site::kRowSpanOverrun) == 0);
}

// Mutation: skip the resolve after a reconcile (the span then stays after its covered rows are gone).
TEST_CASE("row spans: removing a covered row makes the span run past the last row; it is refused and reported",
          "[ui][table]") {
    Fixture fixture{{{.id = 1, .spans = {{.column = 0, .rows = 3}}}, {.id = 2}, {.id = 3}, {.id = 4}}};
    ui::Mounted const mounted{fixture.runtime, fixture.backend, fixture.view()};
    CHECK(fixture.spansOf(1) == "0:3");
    fixture.lines.mutate([](std::vector<Line>& rows) { rows.pop_back(); });
    fixture.owner.runAll();
    CHECK(fixture.spansOf(1) == "0:3");
    CHECK(fixture.probe.count(site::kRowSpanOverrun) == 0);
    fixture.lines.mutate([](std::vector<Line>& rows) { rows.pop_back(); });
    fixture.owner.runAll();
    CHECK(fixture.spansOf(1) == "none");
    CHECK(fixture.probe.count(site::kRowSpanOverrun) == 1);
}

// Mutation: leave the heading out of the resolve (the span crosses it).
TEST_CASE("row spans: a span over a heading row is refused alone, the row's other span stays", "[ui][table]") {
    Fixture fixture{{{.id = 1, .spans = {{.column = 0, .rows = 3}, {.column = 2, .rows = 2}}},
                     {.id = 2},
                     {.id = 3, .heading = true},
                     {.id = 4}}};
    ui::Mounted const mounted{fixture.runtime, fixture.backend, fixture.view()};
    CHECK(fixture.spansOf(1) == "2:2");
    CHECK(fixture.probe.count(site::kRowSpanOverrun) == 1);
}

// Mutation: do not refuse a span that starts under an earlier one (the second records).
TEST_CASE("row spans: a span starting in a covered column is refused; the earlier span wins", "[ui][table]") {
    Fixture fixture{{{.id = 1, .spans = {{.column = 0, .rows = 3}}},
                     {.id = 2, .spans = {{.column = 0, .rows = 2}, {.column = 1, .rows = 2}}},
                     {.id = 3},
                     {.id = 4}}};
    ui::Mounted const mounted{fixture.runtime, fixture.backend, fixture.view()};
    CHECK(fixture.spansOf(1) == "0:3");
    CHECK(fixture.spansOf(2) == "1:2");
    CHECK(fixture.probe.count(site::kRowSpanOverlap) == 1);
}

// Mutation: accept a column past the table's, a zero count, a repeated column or a heading's span.
TEST_CASE("row spans: an unknown column, a count of 0, a repeated column and a heading's span are refused",
          "[ui][table]") {
    Fixture fixture{{{.id = 1,
                      .spans = {{.column = 7, .rows = 2},
                                {.column = 1, .rows = 0},
                                {.column = 2, .rows = 2},
                                {.column = 2, .rows = 2}}},
                     {.id = 2},
                     {.id = 3, .spans = {{.column = 0, .rows = 2}}, .heading = true},
                     {.id = 4}}};
    ui::Mounted const mounted{fixture.runtime, fixture.backend, fixture.view()};
    CHECK(fixture.spansOf(1) == "2:2");
    CHECK(fixture.spansOf(3) == "-");
    CHECK(fixture.probe.count(site::kRowSpanInvalid) == 1);
}

// Mutation: ignore the sort (the spans stay while it is engaged).
TEST_CASE("row spans: a sorted table draws no spans, silently, and has them back when the sort is cleared",
          "[ui][table]") {
    Fixture fixture{three()};
    ui::Mounted const mounted{fixture.runtime, fixture.backend, fixture.view(true)};
    CHECK(fixture.spansOf(1) == "0:3");
    fixture.sort.set(ui::TableSort{.column = 0, .direction = ui::SortDirection::Ascending});
    fixture.owner.runAll();
    CHECK(fixture.spansOf(1) == "none");
    CHECK(fixture.probe.count(site::kRowSpanOverrun) == 0);
    fixture.sort.set(std::nullopt);
    fixture.owner.runAll();
    CHECK(fixture.spansOf(1) == "0:3");
}

// Mutation: leave the threshold out of the resolve (a virtualized table keeps its spans).
TEST_CASE("row spans: a table above its virtualization threshold draws none and says so once", "[ui][table]") {
    Fixture fixture{three()};
    ui::Mounted const mounted{fixture.runtime, fixture.backend, fixture.view(false, 2)};
    CHECK(fixture.spansOf(1) == "-");
    CHECK(fixture.probe.count(site::kRowSpanVirtualized) == 1);
}

// Mutation: ignore the document's key (no row records a span).
TEST_CASE("row spans: a document's rowSpans names a column by its id and reaches the widget",
          "[ui][interpreter][table]") {
    ui::testing::Harness h;
    auto const screen = h.mount(R"({ "vocab": "ui/1", "id": "grid",
      "view": { "kind": "table", "testId": "grid", "key": { "row": "id" },
        "rows": { "lit": [ { "id": 1, "spans": [ { "column": "b", "rows": 2 } ] }, { "id": 2, "spans": null } ] },
        "rowSpans": { "row": "spans" },
        "columns": [ { "id": "a", "label": "A" }, { "id": "b", "label": "B" } ] } })");
    std::vector<int> const rows = h.backend.all("Row");
    REQUIRE(rows.size() == 2);
    CHECK(h.backend.prop(rows.at(0), "rowSpans") == "1:2");
    CHECK(h.backend.prop(rows.at(1), "rowSpans").empty());
}
