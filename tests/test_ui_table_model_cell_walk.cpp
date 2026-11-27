// SPDX-License-Identifier: Apache-2.0

// Walking a model-backed table's cells: where Enter lands, and the one change slot the mount and drafts share.
//
// Part of the client table model tests; see docs/spec/table/table.md.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <map>
#include <memory>
#include <morph/core/registry.hpp>
#include <morph/forms/forms.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/table/data_source.hpp>
#include <morph/table/description.hpp>
#include <morph/table/edits.hpp>
#include <morph/table/query.hpp>
#include <morph/table/sort.hpp>
#include <morph/table/view_change.hpp>
#include <morph/ui/grid.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/table_rows.hpp>
#include <morph/ui/value.hpp>
#include <numeric>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "grid_test_support.hpp"
#include "ui_table_model_support.hpp"
#include "ui_table_support.hpp"

using namespace table_model_support;

using morph::ui::Value;
using namespace std::chrono_literals;
using morph::test::grid::at;
using morph::test::grid::committedOk;
using morph::test::grid::ModelGridFixture;
using morph::test::grid::specimenRecord;

// Mutations: the walk passing over a row not fetched yet as if it were read-only (focus lands on row 5); the
// placeholder's key read untracked in GridController::place (the binding over the focus never hears row 3 arrive).

TEST_CASE("model table: the walk stops at a row not fetched yet", "[ui][table][edit]") {
    ModelGridFixture f{{1, 2, 3, 4, 5}};
    f.rows->view.at(2).fetched = false;
    std::optional<morph::ui::CellRef> heard;
    morph::reactive::Effect const following{f.h.rt, [&heard, &f] { heard = f.grid->focus(); }};
    f.grid->focusCell(at(2, "count"));
    f.grid->type("1");
    f.grid->press("Enter");
    f.h.settle();
    CHECK_FALSE(f.grid->focus().has_value());
    CHECK(f.grid->place() == morph::ui::GridFocus{std::nullopt, 2, "count"});
    CHECK_FALSE(f.source->keyAt(2).has_value());
    CHECK(f.commits.sent.size() == 1);  // row 2 only
    CHECK_FALSE(heard.has_value());
    CHECK(f.grid->press("Enter") == morph::ui::KeyOutcome::Handled);  // before the row arrives
    CHECK(f.grid->place() == morph::ui::GridFocus{std::nullopt, 2, "count"});
    f.rows->deliver(2);
    f.h.settle();
    CHECK(heard == at(3, "count"));  // no further gesture
    CHECK(f.grid->place() == morph::ui::GridFocus{key(3), 2, "count"});
    f.grid->type("2");
    f.grid->press("Enter");
    CHECK(f.commits.sent.back().row == row(3));
}

// Mutation: the walk following the edited row instead of the view position at the commit.

TEST_CASE("model table: Enter after an edit that moves its row lands on the row that was below", "[ui][table][edit]") {
    ModelGridFixture f{{1, 2, 3}};
    f.grid->focusCell(at(1, "count"));
    f.grid->type("9");
    f.grid->press("Enter");
    f.rows->reorder({row(2), row(3), row(1)});
    CHECK(f.grid->focus() == at(2, "count"));
}

namespace {

// The text of the cell of a mounted table's row in a column.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): `mountedText(backend, rowWidget, column)` is a test helper whose arguments read differently at each call; a swap would fail the case's own assertion
std::string mountedText(morph::ui::testing::RecordingBackend const& backend, int rowWidget, std::size_t column) {
    for (int const cell : backend.children(rowWidget)) {
        if (backend.prop(cell, "column") == std::to_string(column)) {
            return backend.prop(cell, "text");
        }
    }
    return {};
}

}  // namespace

// Mutation: ModelGridSource handing the mount's handler to the rows' own slot (the mount's handler replaces the
// decorator's: the modified mark never clears).

TEST_CASE("model table: the mount and the drafts share the rows' one change slot", "[ui][table][edit]") {
    ModelGridFixture f{{1, 2}};
    morph::ui::testing::RecordingBackend backend;
    morph::ui::TableSource base;
    base.rows = std::shared_ptr<morph::ui::TableRows>(f.rows, &f.source->view());
    base.cellView = [](std::size_t column, morph::reactive::Signal<Value> const& record) {
        std::string const id = std::vector<std::string>{"label", "remark", "count", "note", "sampled",
                                                        "taken", "day",    "at",    "mass", "weight"}
                                   .at(column);
        return morph::ui::text(morph::ui::Text{.text = morph::ui::Prop<std::string>{[&record, id] {
                                                   Value const* const value = record.get().field(id);
                                                   if (value == nullptr) {
                                                       return std::string{};
                                                   }
                                                   if (std::int64_t const* const number = value->asInt()) {
                                                       return std::to_string(*number);
                                                   }
                                                   return value->asString() != nullptr ? *value->asString()
                                                                                       : std::string{};
                                               }}});
    };
    morph::ui::TableOptions options;
    options.common.testId = "m";
    std::optional<morph::ui::Mounted> view;
    view.emplace(f.h.rt, backend, f.grid->node(ModelGridFixture::tableColumns(), std::move(options), base));
    f.h.settle();
    int const table = backend.find("Table", "testId", "m").value_or(0);
    REQUIRE(table != 0);
    CHECK(f.rows->handlers() == 1);  // the decorator's, once
    f.grid->focusCell(at(1, "count"));
    f.grid->type("9");
    f.grid->press("Enter");
    f.commits.done.at(0)(committedOk());
    f.h.settle();
    std::optional<int> const first = backend.tableRow(table, key(1));
    REQUIRE(first.has_value());
    CHECK(backend.prop(*first, "marks:2") == "m");
    f.rows->refetch(row(1), specimenRecord(1, 9));
    f.h.settle();
    std::optional<int> const again = backend.tableRow(table, key(1));
    REQUIRE(again.has_value());
    CHECK(mountedText(backend, *again, 2) == "9");   // the mount's handler ran
    CHECK(backend.prop(*again, "marks:2").empty());  // rowFetched ran
    view.reset();
}
