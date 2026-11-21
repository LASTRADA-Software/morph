// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file grid_test_support.hpp
/// @brief The grids' fixtures: a form whose `specimens` rows are entered in a grid, and helpers over a cell's menu.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <morph/forms/derive.hpp>
#include <morph/forms/engine/form_session.hpp>
#include <morph/reactive/testing/manual_scheduler.hpp>
#include <morph/ui/grid.hpp>
#include <morph/ui/locale_table.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "forms_engine_support.hpp"

namespace morph::test::grid {

// A form whose `specimens` rows are entered in a grid. `density` is computed, `limit` derived and read-only.
// `reviewer` freezes the grid by rule.
inline constexpr std::string_view kSpecimens = R"({"type":"object","title":"Specimens","properties":{
  "sampleId":{"type":"integer","title":"Sample","x-order":0},
  "reviewer":{"type":["string","null"],"title":"Reviewer","x-order":1},
  "specimens":{"type":"array","title":"Specimens","x-order":2,"minItems":0,"maxItems":5,
    "items":{"type":"object","properties":{
      "label":{"type":["string","null"],"title":"Label","x-order":0},
      "mass":{"type":["object","null"],"title":"Mass","x-decimalPlaces":2,"x-order":1},
      "volume":{"type":["object","null"],"title":"Volume","x-decimalPlaces":1,"x-order":2},
      "density":{"type":["object","null"],"title":"Density","x-decimalPlaces":3,"x-readonly":true,
                 "x-computed":{"inputs":["mass","volume"]},"x-order":3},
      "limit":{"type":["object","null"],"title":"Limit","x-decimalPlaces":2,"x-readonly":true,"x-derived":true,
               "x-order":4},
      "note":{"type":["string","null"],"title":"Note","x-order":5}}}}},
  "required":["sampleId"],
  "x-rules":[{"kind":"readonlyWhen","fields":["specimens"],"when":{"kind":"engaged","fields":["reviewer"]}}]})";

// A grid of any form: its schema, what it is prefilled with, its collection, its columns (empty: one per member of the
// row type, in schema order) and its options.
struct GridSpec {
    std::string schema;
    std::string body;
    std::string array = "specimens";
    std::vector<ui::GridColumn> columns;
    ui::GridOptions options;
};

struct GridFixture {
    morph::test::formsengine::Harness h;
    reactive::testing::ManualScheduler scheduler;
    // The evaluation and the derive call `enableDerive` routes rounds to; every call stays pending until resolved.
    morph::test::formsengine::FakeServer evaluate{h.owner};
    morph::test::formsengine::FakeServer derive{h.owner};
    std::unique_ptr<forms::FormSession> form;
    std::unique_ptr<ui::FormGridSource> source;
    std::unique_ptr<ui::GridController> grid;
    // Loads `rows` blank rows (keys 1..rows) with sampleId 1, in a comma locale, columns in schema order.
    explicit GridFixture(std::size_t rows, ui::GridOptions options = {}, std::string schema = std::string{kSpecimens});
    // Loads a grid of any form, in a comma locale.
    explicit GridFixture(GridSpec spec);
    [[nodiscard]] std::string json(std::string_view path) const;  // the field's encoded JSON, "" when none
    // Rounds of derived values over `evaluate` and `derive`, on `scheduler`, with the default debounce of 150 ms.
    void enableDerive();
};
// `kSpecimens` with a Boolean member `checked` and an enumerated member `unit` (g or kg) after `note`.
inline std::string specimensWithChoices() {
    std::string schema{kSpecimens};
    std::string_view const note = R"("note":{"type":["string","null"],"title":"Note","x-order":5})";
    schema.replace(schema.find(note), note.size(),
                   std::string{note} + R"(,"checked":{"type":"boolean","title":"Checked","x-order":6},)" +
                       R"("unit":{"enum":["g","kg",null],"title":"Unit","x-order":7})");
    return schema;
}

// Rows whose `value` column edits `choice` or `amount` by the row's `isChoice`, and whose `mass` takes one place in row 1
// and three in row 2 (`kindPerRowColumns`).
inline constexpr std::string_view kKindPerRow = R"({"type":"object","title":"Rows","properties":{
  "rows":{"type":"array","title":"Rows","x-order":0,"items":{"type":"object","properties":{
    "isChoice":{"type":"boolean","title":"Is choice","x-order":0},
    "choice":{"enum":["low","high",null],"title":"Choice","x-order":1},
    "amount":{"type":["object","null"],"title":"Amount","x-decimalPlaces":1,"x-order":2},
    "mass":{"type":["object","null"],"title":"Mass","x-decimalPlaces":2,"x-order":3}}}}}})";

// The columns over `kKindPerRow`: `value`, a member per row, and `mass`, with places per row.
inline std::vector<ui::GridColumn> kindPerRowColumns() {
    std::vector<ui::GridColumn> columns;
    columns.push_back(ui::GridColumn{.id = "value", .label = "Value", .field = {}, .value = {},
                                     .fieldOf = [](forms::RowState const& row) {
                                         return row.field("isChoice")->draft() == "true" ? std::string{"choice"}
                                                                                          : std::string{"amount"};
                                     }});
    columns.push_back(ui::GridColumn{.id = "mass", .label = "Mass", .field = "mass", .value = {}, .fieldOf = {},
                                     .decimals = [](forms::RowState const& row) {
                                         return std::optional<std::uint32_t>{row.key() == 1 ? 1U : 3U};
                                     }});
    return columns;
}

inline ui::CellRef at(std::int64_t key, std::string column) { return ui::CellRef{ui::Key{key}, std::move(column)}; }
// Every fetched key of the source's view, in view order.
inline std::vector<ui::Key> keysOf(ui::GridSource const& source) {
    std::vector<ui::Key> keys;
    for (std::size_t i = 0; i < source.rowCount(); ++i) {
        if (auto key = source.keyAt(i)) keys.push_back(*key);
    }
    return keys;
}

// Blank rows (keys 1..rows) with sampleId 1.
inline std::string blankSpecimens(std::size_t rows) {
    std::string body = R"({"sampleId":1,"specimens":[)";
    for (std::size_t row = 0; row < rows; ++row) {
        body += row == 0 ? "{}" : ",{}";
    }
    return body + "]}";
}

inline GridFixture::GridFixture(std::size_t rows, ui::GridOptions options, std::string schema)
    : GridFixture{GridSpec{.schema = std::move(schema), .body = blankSpecimens(rows), .array = "specimens", .columns = {},
                           .options = std::move(options)}} {}

inline GridFixture::GridFixture(GridSpec spec) {
    evaluate.replyWith([](morph::test::formsengine::Call const&) { return std::optional<std::string>{}; });
    derive.replyWith([](morph::test::formsengine::Call const&) { return std::optional<std::string>{}; });
    form = h.session("Lims_RecordSpecimens", spec.schema,
                     forms::FormSessionOptions{.locale = ui::localeFacts("de").numeric, .bcp47 = "de"});
    form->prefill(spec.body);
    h.settle();
    std::vector<ui::GridColumn> columns = std::move(spec.columns);
    if (columns.empty()) {
        for (forms::FieldSpec const& member : form->field(spec.array).spec().children) {
            columns.push_back(ui::GridColumn{.id = member.name, .label = member.name, .field = member.name, .value = {}});
        }
    }
    source = std::make_unique<ui::FormGridSource>(
        *form, spec.array, std::move(columns),
        ui::FormatContext{.locale = "de", .numeric = ui::withoutGrouping(ui::localeFacts("de").numeric)});
    grid = std::make_unique<ui::GridController>(h.rt, *source, std::move(spec.options));
    source->onBeforeSubmit([controller = grid.get()] { controller->leave(); });
}

inline void GridFixture::enableDerive() {
    form->deriveWith(forms::DeriveOptions{
        .evaluate = [this](std::string body) { return evaluate.submitter()("Lims_RecordSpecimens", std::move(body)); },
        .derive = [this](std::string body) { return derive.submitter()("Lims_DeriveSpecimens", std::move(body)); },
        .scheduler = &scheduler,
        .debounce = std::chrono::milliseconds{150},
        .refused = {}});
}

inline std::string GridFixture::json(std::string_view path) const {
    forms::Encoding const& encoding = form->field(path).encoded().get();
    return encoding.has_value() && encoding->has_value() ? (*encoding)->json : std::string{};
}

// One table column per member of the row type, in schema order, its id and label the member's name.
inline std::vector<ui::TableColumn> columnsOf(GridFixture const& f) {
    std::vector<ui::TableColumn> columns;
    for (forms::FieldSpec const& member : f.form->field("specimens").spec().children) {
        columns.push_back(ui::TableColumn{.id = member.name, .label = member.name});
    }
    return columns;
}

// Each entry's label, as bound now.
inline std::vector<std::string> labelsOf(std::vector<ui::MenuItem> const& items) {
    std::vector<std::string> labels;
    for (ui::MenuItem const& item : items) {
        labels.push_back(item.label.evaluate());
    }
    return labels;
}

// The entry with a label; it must exist.
inline ui::MenuItem const& entry(std::vector<ui::MenuItem> const& items, std::string_view label) {
    auto const found = std::ranges::find_if(items, [label](ui::MenuItem const& item) { return item.label.evaluate() == label; });
    REQUIRE(found != items.end());
    return *found;
}

// Whether an entry is enabled, as bound now.
inline bool enabledOf(ui::MenuItem const& item) { return item.enabled.evaluate(); }

}  // namespace morph::test::grid
