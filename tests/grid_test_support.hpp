// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file grid_test_support.hpp
/// @brief The grids' fixtures: a form whose `specimens` rows are entered in a grid, and helpers over a cell's menu.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <morph/forms/engine/form_session.hpp>
#include <morph/ui/grid.hpp>
#include <morph/ui/locale_table.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "forms_engine_support.hpp"

namespace morph::test::grid {

// A form whose `specimens` rows are entered in a grid. `density` is computed, `limit` derived (Task 20 adds the
// `x-derived` reading; until then its `x-readonly` makes it read-only). `reviewer` freezes the grid by rule.
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

struct GridFixture {
    morph::test::formsengine::Harness h;
    std::unique_ptr<forms::FormSession> form;
    std::unique_ptr<ui::FormGridSource> source;
    std::unique_ptr<ui::GridController> grid;
    // Loads `rows` blank rows (keys 1..rows) with sampleId 1, in a comma locale, columns in schema order.
    explicit GridFixture(std::size_t rows, ui::GridOptions options = {}, std::string schema = std::string{kSpecimens});
    [[nodiscard]] std::string json(std::string_view path) const;  // the field's encoded JSON, "" when none
};
inline ui::CellRef at(std::int64_t key, std::string column) { return ui::CellRef{ui::Key{key}, std::move(column)}; }
// Every fetched key of the source's view, in view order.
inline std::vector<ui::Key> keysOf(ui::GridSource const& source) {
    std::vector<ui::Key> keys;
    for (std::size_t i = 0; i < source.rowCount(); ++i) {
        if (auto key = source.keyAt(i)) keys.push_back(*key);
    }
    return keys;
}

inline GridFixture::GridFixture(std::size_t rows, ui::GridOptions options, std::string schema) {
    form = h.session("Lims_RecordSpecimens", schema,
                     forms::FormSessionOptions{.locale = ui::localeFacts("de").numeric, .bcp47 = "de"});
    std::string body = R"({"sampleId":1,"specimens":[)";
    for (std::size_t row = 0; row < rows; ++row) {
        body += row == 0 ? "{}" : ",{}";
    }
    body += "]}";
    form->prefill(body);
    h.settle();
    std::vector<ui::GridColumn> columns;
    for (forms::FieldSpec const& member : form->field("specimens").spec().children) {
        columns.push_back(ui::GridColumn{.id = member.name, .label = member.name, .field = member.name, .value = {}});
    }
    source = std::make_unique<ui::FormGridSource>(
        *form, "specimens", std::move(columns),
        ui::FormatContext{.locale = "de", .numeric = ui::withoutGrouping(ui::localeFacts("de").numeric)});
    grid = std::make_unique<ui::GridController>(h.rt, *source, std::move(options));
    source->onBeforeSubmit([controller = grid.get()] { controller->leave(); });
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
