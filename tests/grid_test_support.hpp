// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file grid_test_support.hpp
/// @brief The grids' fixtures: a form whose `specimens` rows are entered in a grid, helpers over a cell's menu, and a
///        model's table as a grid over rows the test holds.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <morph/forms/derive.hpp>
#include <morph/forms/engine/form_session.hpp>
#include <morph/reactive/testing/manual_scheduler.hpp>
#include <morph/table/edits.hpp>
#include <morph/ui/grid.hpp>
#include <morph/ui/locale_table.hpp>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "forms_engine_support.hpp"
#include "table/row_commits.hpp"

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

// Rows whose `value` column edits `choice` or `amount` by the row's `isChoice`, and whose `mass` takes one place in
// row 1 and three in row 2 (`kindPerRowColumns`).
inline constexpr std::string_view kKindPerRow = R"({"type":"object","title":"Rows","properties":{
  "rows":{"type":"array","title":"Rows","x-order":0,"items":{"type":"object","properties":{
    "isChoice":{"type":"boolean","title":"Is choice","x-order":0},
    "choice":{"enum":["low","high",null],"title":"Choice","x-order":1},
    "amount":{"type":["object","null"],"title":"Amount","x-decimalPlaces":1,"x-order":2},
    "mass":{"type":["object","null"],"title":"Mass","x-decimalPlaces":2,"x-order":3}}}}}})";

// The columns over `kKindPerRow`: `value`, a member per row, and `mass`, with places per row.
inline std::vector<ui::GridColumn> kindPerRowColumns() {
    std::vector<ui::GridColumn> columns;
    columns.push_back(ui::GridColumn{
        .id = "value", .label = "Value", .field = {}, .value = {}, .fieldOf = [](forms::RowState const& row) {
            return row.field("isChoice")->draft() == "true" ? std::string{"choice"} : std::string{"amount"};
        }});
    columns.push_back(ui::GridColumn{.id = "mass",
                                     .label = "Mass",
                                     .field = "mass",
                                     .value = {},
                                     .fieldOf = {},
                                     .decimals = [](forms::RowState const& row) {
                                         return std::optional<std::uint32_t>{row.key() == 1 ? 1U : 3U};
                                     }});
    return columns;
}

inline ui::CellRef at(std::int64_t key, std::string column) {
    return ui::CellRef{.key = ui::Key{key}, .column = std::move(column)};
}
// Every fetched key of the source's view, in view order.
inline std::vector<ui::Key> keysOf(ui::GridSource const& source) {
    std::vector<ui::Key> keys;
    for (std::size_t i = 0; i < source.rowCount(); ++i) {
        if (auto key = source.keyAt(i)) {
            keys.push_back(*key);
        }
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
    : GridFixture{GridSpec{.schema = std::move(schema),
                           .body = blankSpecimens(rows),
                           .array = "specimens",
                           .columns = {},
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
            columns.push_back(
                ui::GridColumn{.id = member.name, .label = member.name, .field = member.name, .value = {}});
        }
    }
    source = std::make_unique<ui::FormGridSource>(
        *form, spec.array, std::move(columns),
        ui::FormatContext{.locale = "de", .numeric = ui::withoutGrouping(ui::localeFacts("de").numeric)});
    if (spec.options.scheduler == nullptr) {
        spec.options.scheduler = &scheduler;  // the undo offer's hold
    }
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

// A form of limits laid out as a fixed grid: `density` and `moisture` each a minimum and a maximum (three and one
// places), `method` a choice of A or B, `count` an integer, `weight` a quantity in kg or g, `limit` a derived quantity
// in kg or g, and `specimens` rows of a `mass` (two places).
inline constexpr std::string_view kSetLimits = R"({"type":"object","title":"Limits","properties":{
  "limits":{"type":"object","title":"Limits","x-order":0,"properties":{
    "density":{"type":"object","title":"Density","x-order":0,"properties":{
      "min":{"type":["object","null"],"title":"Min","x-decimalPlaces":3,"x-order":0},
      "max":{"type":["object","null"],"title":"Max","x-decimalPlaces":3,"x-order":1}}},
    "moisture":{"type":"object","title":"Moisture","x-order":1,"properties":{
      "min":{"type":["object","null"],"title":"Min","x-decimalPlaces":1,"x-order":0},
      "max":{"type":["object","null"],"title":"Max","x-decimalPlaces":1,"x-order":1}}},
    "method":{"enum":["A","B"],"title":"Method","x-order":2},
    "count":{"type":["integer","null"],"title":"Count","x-order":3},
    "weight":{"type":["object","null"],"title":"Weight","x-decimalPlaces":3,
      "ExtUnits":{"unitAscii":"kg","unitUnicode":"kg"},
      "x-unitAlternatives":[{"id":"g","display":"g","decimals":1,"num":1,"den":1000}],"x-order":4},
    "limit":{"type":["object","null"],"title":"Limit","x-decimalPlaces":3,"x-readonly":true,"x-derived":true,
      "ExtUnits":{"unitAscii":"kg","unitUnicode":"kg"},
      "x-unitAlternatives":[{"id":"g","display":"g","decimals":1,"num":1,"den":1000}],"x-order":5}}},
  "specimens":{"type":"array","title":"Specimens","x-order":1,"items":{"type":"object","properties":{
    "mass":{"type":["object","null"],"title":"Mass","x-decimalPlaces":2,"x-order":0}}}}}})";

// A fixed grid over the Lims_SetLimits form: columns `min`, `max` (and `v` for the block case), in a comma locale.
struct FixedGridFixture {
    morph::test::formsengine::Harness h;
    std::unique_ptr<forms::FormSession> form;
    std::unique_ptr<ui::FixedGridSource> source;
    std::unique_ptr<ui::GridController> grid;
    // Prefills `body`, builds the source over `rows` and `columns` and the controller with default options.
    FixedGridFixture(std::string_view body, std::vector<std::variant<ui::FixedRow, ui::FixedBlock>> rows,
                     std::vector<ui::GridColumn> columns);
    [[nodiscard]] std::string json(std::string_view path) const;
};

inline FixedGridFixture::FixedGridFixture(std::string_view body,
                                          std::vector<std::variant<ui::FixedRow, ui::FixedBlock>> rows,
                                          std::vector<ui::GridColumn> columns) {
    form = h.session("Lims_SetLimits", kSetLimits,
                     forms::FormSessionOptions{.locale = ui::localeFacts("de").numeric, .bcp47 = "de"});
    form->prefill(body);
    h.settle();
    source = std::make_unique<ui::FixedGridSource>(
        h.rt, form.get(), std::move(rows), std::move(columns),
        ui::FormatContext{.locale = "de", .numeric = ui::withoutGrouping(ui::localeFacts("de").numeric)});
    grid = std::make_unique<ui::GridController>(h.rt, *source, ui::GridOptions{});
}

inline std::string FixedGridFixture::json(std::string_view path) const {
    forms::Encoding const& encoding = form->field(path).encoded().get();
    return encoding.has_value() && encoding->has_value() ? (*encoding)->json : std::string{};
}

// A fixed cell editing a field.
inline ui::FixedCell fieldCell(std::string path, std::size_t columns = 1) {
    ui::FixedCell cell;
    cell.field = std::move(path);
    cell.columns = columns;
    return cell;
}

// A fixed cell showing a literal text.
inline ui::FixedCell textCell(std::string text) {
    ui::FixedCell cell;
    cell.text = std::move(text);
    return cell;
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
        // NOLINTNEXTLINE(performance-inefficient-vector-operation): `labels` is a fixture list built once; sizing it first would not change what the case checks
        labels.push_back(item.label.evaluate());
    }
    return labels;
}

// The entry with a label; it must exist.
inline ui::MenuItem const& entry(std::vector<ui::MenuItem> const& items, std::string_view label) {
    auto const found =
        std::ranges::find_if(items, [label](ui::MenuItem const& item) { return item.label.evaluate() == label; });
    REQUIRE(found != items.end());
    return *found;
}

// Whether an entry is enabled, as bound now.
inline bool enabledOf(ui::MenuItem const& item) { return item.enabled.evaluate(); }

// An entry's description, as bound now.
inline std::string descriptionOf(ui::MenuItem const& item) { return item.description.evaluate(); }

// ── Cells of a model's table ────────────────────────────────────────────────

// Rows of a model's table held by the test: a view of keys, some not fetched, with states, locks and unloaded levels.
class FakeRows final : public ui::TableRows {
public:
    struct Row {
        table::RowId key;
        ui::Value record;
        bool fetched = true;
        std::set<std::string> unloaded;  // the levels the row has not loaded
    };
    // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes): the fake source's view, row states and recorded moves, which the cases arrange and read directly
    std::vector<Row> view;  // in view order
    ui::RowStates rowStates;
    std::vector<std::pair<table::RowId, table::RowId>> moved;  // moveKey's calls
    // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

    // Marks the row fetched; reports it changed.
    void deliver(std::size_t index) {
        view.at(index).fetched = true;
        announce({table::ViewOp{.kind = table::ViewOp::Kind::Changed, .first = index, .count = 1, .to = 0}});
    }
    // The view in this order; reports the moves.
    void reorder(std::vector<table::RowId> const& keys) {
        std::vector<table::ViewOp> ops;
        for (std::size_t to = 0; to < keys.size(); ++to) {
            auto const at = std::ranges::find(view, keys.at(to), &Row::key);
            auto const from = static_cast<std::size_t>(at - view.begin());
            if (from == to) {
                continue;
            }
            Row row = std::move(*at);
            view.erase(at);
            view.insert(view.begin() + static_cast<std::ptrdiff_t>(to), std::move(row));
            ops.push_back(table::ViewOp{.kind = table::ViewOp::Kind::Moved, .first = from, .count = 1, .to = to});
        }
        announce(std::move(ops));
    }
    // Out of the view; `restore` puts it back.
    Row drop(table::RowId const& key) {
        auto const at = std::ranges::find(view, key, &Row::key);
        auto const index = static_cast<std::size_t>(at - view.begin());
        Row row = std::move(*at);
        view.erase(at);
        announce({table::ViewOp{.kind = table::ViewOp::Kind::Removed, .first = index, .count = 1, .to = 0}});
        return row;
    }
    void restore(Row row, std::size_t index) {
        view.insert(view.begin() + static_cast<std::ptrdiff_t>(index), std::move(row));
        announce({table::ViewOp{.kind = table::ViewOp::Kind::Inserted, .first = index, .count = 1, .to = 0}});
    }
    // Replaces the record; reports it changed.
    void refetch(table::RowId const& key, ui::Value record) {
        auto const at = std::ranges::find(view, key, &Row::key);
        at->record = std::move(record);
        at->fetched = true;
        auto const index = static_cast<std::size_t>(at - view.begin());
        announce({table::ViewOp{.kind = table::ViewOp::Kind::Changed, .first = index, .count = 1, .to = 0}});
    }
    [[nodiscard]] std::size_t handlers() const { return _handlerSets; }  // how often onChange was set
    // Gives a row a new key; reports it changed.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): `rekey(from, to)` is a test helper whose arguments read differently at each call; a swap would fail the case's own assertion
    void rekey(table::RowId const& from, table::RowId const& to) {
        auto const at = std::ranges::find(view, from, &Row::key);
        at->key = to;
        ui::ValueRecord record = *at->record.asRecord();
        record.insert_or_assign("key", std::visit([](auto const& value) { return ui::Value{value}; }, to));
        at->record = ui::Value{std::move(record)};
        auto const index = static_cast<std::size_t>(at - view.begin());
        announce({table::ViewOp{.kind = table::ViewOp::Kind::Changed, .first = index, .count = 1, .to = 0}});
    }
    // Reports a change of several operations, as an engine's view change is.
    void announceChange(std::vector<table::ViewOp> ops) { announce(std::move(ops)); }
    // Reports the whole view replaced, as a new query does.
    void reset() { announce({table::ViewOp{.kind = table::ViewOp::Kind::Reset, .first = 0, .count = 0, .to = 0}}); }
    // Answers every `locateRow` asked so far with the key's place in `view`, fetched or not.
    void answerLocates() {
        auto asked = std::exchange(_locates, {});
        for (auto& [key, done] : asked) {
            auto const at = std::ranges::find(view, key, &Row::key);
            // Assigned, not cast: the iterator difference is `std::ptrdiff_t`, which is `std::int64_t` only on some
            // targets.
            std::int64_t const index = at - view.begin();
            done(at == view.end() ? ui::RowLocation{std::nullopt} : ui::RowLocation{index});
        }
    }
    [[nodiscard]] std::size_t locatesAsked() const { return _locates.size(); }
    // Rows past `view` the total counts and nothing holds: a large view's window.
    // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes): the fake source's unheld-row count and read counter, which the cases set and read directly
    std::int64_t unheld = 0;
    // How often `row` was read, and at which indices, in order.
    mutable std::size_t rowReads = 0;
    mutable std::vector<std::int64_t> readIndices;
    // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

    [[nodiscard]] ui::TableTotal total() const override {
        return {
            .rows = static_cast<std::int64_t>(view.size()) + unheld, .kind = table::TotalKind::Exact, .known = true};
    }
    [[nodiscard]] std::vector<std::pair<std::int64_t, std::int64_t>> heldRanges() const override {
        return {{0, static_cast<std::int64_t>(view.size())}};
    }
    [[nodiscard]] std::optional<ui::TableRowRef> row(std::int64_t index) const override {
        ++rowReads;
        readIndices.push_back(index);
        if (index < 0 || static_cast<std::size_t>(index) >= view.size()) {
            return std::nullopt;
        }
        Row const& held = view.at(static_cast<std::size_t>(index));
        if (!held.fetched) {
            return std::nullopt;
        }
        return ui::TableRowRef{.key = held.key, .row = held.record};
    }
    [[nodiscard]] table::UnloadedLevels unloadedLevels(std::int64_t index) const override {
        if (index < 0 || static_cast<std::size_t>(index) >= view.size()) {
            return {};
        }
        _levels.assign(view.at(static_cast<std::size_t>(index)).unloaded.begin(),
                       view.at(static_cast<std::size_t>(index)).unloaded.end());
        return table::UnloadedLevels{.levels = _levels, .failed = {}};
    }
    void visible(std::int64_t, std::int64_t, std::span<std::string const>) override {}
    async::Completion<std::expected<std::vector<table::RowId>, table::TableError>> keysOf(std::int64_t,
                                                                                          std::int64_t) override {
        return async::Completion<std::expected<std::vector<table::RowId>, table::TableError>>::makeSettleable(nullptr)
            .first;
    }
    void locateRow(table::RowId key, ::core::async::StopToken,
                   std::function<void(ui::RowLocation const&)> done) override {
        _locates.emplace_back(std::move(key), std::move(done));
    }
    async::Completion<std::expected<std::vector<ui::FetchedRow>, table::TableError>> fetchRows(
        std::vector<table::RowId>, std::vector<std::string>) override {
        return async::Completion<std::expected<std::vector<ui::FetchedRow>, table::TableError>>::makeSettleable(
                   nullptr)
            .first;
    }
    std::expected<void, table::TableError> setSort(ui::SortChain) override { return {}; }
    std::expected<void, table::TableError> setFilter(table::FilterSpec) override { return {}; }
    async::Completion<std::expected<table::DistinctValues, table::TableError>> distinctValues(
        table::DistinctRequest) override {
        return async::Completion<std::expected<table::DistinctValues, table::TableError>>::makeSettleable(nullptr)
            .first;
    }
    [[nodiscard]] ui::TableCountsView counts() const override { return ui::TableCountsView{.view = total()}; }
    [[nodiscard]] ui::RowStates& states() override { return rowStates; }
    [[nodiscard]] ui::RowStates const& states() const override { return rowStates; }
    void moveKey(table::RowId const& from, table::RowId const& to) override { moved.emplace_back(from, to); }
    void onChange(std::function<void(table::ViewChange const&)> handler) override {
        _handler = std::move(handler);
        ++_handlerSets;
    }

private:
    void announce(std::vector<table::ViewOp> ops) {
        if (_handler && !ops.empty()) {
            _handler(table::ViewChange{.ops = std::move(ops)});
        }
    }
    std::function<void(table::ViewChange const&)> _handler;
    std::size_t _handlerSets = 0;
    mutable std::vector<std::string> _levels;
    std::vector<std::pair<table::RowId, std::function<void(ui::RowLocation const&)>>> _locates;
};

// The edit actions of the model table's cases: a count by `CellEdit`, a label and a remark by `RowEdit`.
inline constexpr std::string_view kEditSpecimenCount =
    R"({"type":"object","x-cellEditOf":"Lims_SampleModel.samples","properties":{"key":{"type":"integer"},)"
    R"("value":{"type":"integer","maximum":10}},"required":["key","value"]})";
inline constexpr std::string_view kEditSpecimenRow =
    R"({"type":"object","x-rowEditOf":"Lims_SampleModel.samples","properties":{"key":{"type":"integer"},)"
    R"("values":{"type":"object","properties":{"label":{"type":["string","null"]},)"
    R"("remarkText":{"type":["string","null"]},"sampled":{"type":["string","null"],"format":"date"},)"
    R"("taken":{"type":["string","null"],"format":"date-time"},)"
    R"("day":{"type":["integer","null"],"x-widget":"epochDays"},)"
    R"("at":{"type":["integer","null"],"x-widget":"epochSeconds"},)"
    R"("mass":{"type":["object","null"],"x-decimalPlaces":2},)"
    R"("weight":{"type":["object","null"],"x-decimalPlaces":3,"ExtUnits":{"unitAscii":"kg","unitUnicode":"kg"},)"
    R"("x-unitAlternatives":[{"id":"g","display":"g","decimals":1,"num":1,"den":1000}]}}}},"required":["key"]})";

// The description of the model table's cases: `key` (the key), `label` and `remark` (Text, `Lims_EditSpecimenRow`'s
// `label` and `remarkText`), `count` (Integer of level `counts`, `Lims_EditSpecimenCount`, frozen by `countFrozen`),
// `note` (Text, no edit), and the companions `countFrozen` and `lockedBy`, the row's lock.
inline table::TableDescription specimenTableDescription() {
    table::TableDescription d;
    d.id = "samples";
    d.label = "Samples";
    d.key = "key";
    auto const add = [&d](std::string id, table::ColumnKind kind, std::optional<table::EditInfo> edit = {},
                          std::string level = {}) {
        table::TableColumn column;
        column.info = table::ColumnInfo{.id = std::move(id), .kind = kind};
        column.info.level = std::move(level);
        column.label = column.info.id;
        column.edit = std::move(edit);
        d.columns.push_back(std::move(column));
    };
    add("key", table::ColumnKind::Integer);
    add("label", table::ColumnKind::Text, table::EditInfo{.action = "Lims_EditSpecimenRow", .member = "label"});
    add("remark", table::ColumnKind::Text, table::EditInfo{.action = "Lims_EditSpecimenRow", .member = "remarkText"});
    add("count", table::ColumnKind::Integer,
        table::EditInfo{.action = "Lims_EditSpecimenCount", .member = {}, .readonly = "countFrozen"}, "counts");
    add("note", table::ColumnKind::Text);
    add("countFrozen", table::ColumnKind::Bool);
    add("lockedBy", table::ColumnKind::Text);
    // One column per kind a row's member edits: a date and a date-time as the forms' own fields and as epoch counts,
    // and a decimal.
    add("sampled", table::ColumnKind::Date, table::EditInfo{.action = "Lims_EditSpecimenRow", .member = "sampled"});
    add("taken", table::ColumnKind::DateTime, table::EditInfo{.action = "Lims_EditSpecimenRow", .member = "taken"});
    add("day", table::ColumnKind::Date, table::EditInfo{.action = "Lims_EditSpecimenRow", .member = "day"});
    add("at", table::ColumnKind::DateTime, table::EditInfo{.action = "Lims_EditSpecimenRow", .member = "at"});
    add("mass", table::ColumnKind::Decimal, table::EditInfo{.action = "Lims_EditSpecimenRow", .member = "mass"});
    // A measure, each cell in its own unit of mass: the field's value is in kg.
    add("weight", table::ColumnKind::Measure, table::EditInfo{.action = "Lims_EditSpecimenRow", .member = "weight"});
    d.units = {
        table::UnitInfo{.id = "kg",
                        .symbol = "kg",
                        .dimension = "mass",
                        .toBase = morph::math::Rational{morph::math::Numerator{1}, morph::math::Denominator{1},
                                                        morph::math::DecimalPlaces{0}},
                        .decimals = 3},
        table::UnitInfo{.id = "g",
                        .symbol = "g",
                        .dimension = "mass",
                        .toBase = morph::math::Rational{morph::math::Numerator{1}, morph::math::Denominator{1000},
                                                        morph::math::DecimalPlaces{3}},
                        .decimals = 1},
        table::UnitInfo{.id = "percent",
                        .symbol = "%",
                        .dimension = "ratio",
                        .toBase = morph::math::Rational{morph::math::Numerator{1}, morph::math::Denominator{100},
                                                        morph::math::DecimalPlaces{2}},
                        .decimals = 1}};
    d.rowLock = "lockedBy";
    return table::describe(std::move(d));
}

// A row of the model table's cases: label `L<key>`, remark `R<key>`, count the key, note `n`, not frozen, not locked.
inline ui::Value specimenRecord(std::int64_t key, std::int64_t count = -1) {
    ui::ValueRecord record;
    record.insert_or_assign("key", ui::Value{key});
    record.insert_or_assign("label", ui::Value{"L" + std::to_string(key)});
    record.insert_or_assign("remark", ui::Value{"R" + std::to_string(key)});
    record.insert_or_assign("count", ui::Value{count < 0 ? key : count});
    record.insert_or_assign("note", ui::Value{std::string{"n"}});
    record.insert_or_assign("countFrozen", ui::Value{false});
    record.insert_or_assign("lockedBy", ui::Value{std::string{}});
    // As the window holds them: a `Date` cell as `YYYY-MM-DD`, a `DateTime` cell as a timestamp, a decimal exact.
    ui::Value const instant{morph::time::DateTime::fromIso8601("2026-11-20T10:30:00Z").value()};
    record.insert_or_assign("sampled", ui::Value{std::string{"2026-11-20"}});
    record.insert_or_assign("taken", instant);
    record.insert_or_assign("day", ui::Value{std::string{"2026-11-20"}});
    record.insert_or_assign("at", instant);
    record.insert_or_assign("mass", ui::Value{ui::Decimal{table::parseDecimal("12.50").value()}});
    record.insert_or_assign("weight",
                            ui::Value{ui::Quantity{.value = table::parseDecimal("200").value(), .unit = "g"}});
    return ui::Value{std::move(record)};
}

// Sets a field of a held row's record.
inline void setField(FakeRows& rows, std::int64_t key, std::string const& field, ui::Value value) {
    auto const at = std::ranges::find(rows.view, table::RowId{key}, &FakeRows::Row::key);
    REQUIRE(at != rows.view.end());
    ui::ValueRecord record = *at->record.asRecord();
    record.insert_or_assign(field, std::move(value));
    at->record = ui::Value{std::move(record)};
}

// A model's table as a grid over `FakeRows`: its columns `label`, `remark`, `count` and `note`, its edits sent to
// `commits`, its actions permitted while `permitted` names them.
struct ModelGridFixture {
    morph::test::formsengine::Harness h;
    std::shared_ptr<FakeRows> rows = std::make_shared<FakeRows>();
    RowCommits commits;
    table::CellEdits edits{commits.commit()};
    std::set<std::string, std::less<>> permitted{"Lims_EditSpecimenCount", "Lims_EditSpecimenRow"};
    std::unique_ptr<ui::ModelGridSource> source;
    std::unique_ptr<ui::GridController> grid;
    // One fetched row per key, in that order. In `EditMode::Row` Enter moves along the row, so a row's cells are
    // entered with Enter as with Tab.
    explicit ModelGridFixture(std::vector<std::int64_t> const& keys, ui::EditMode mode = ui::EditMode::Cell);
    // The table columns the grid shows, for a mount: one per grid column.
    [[nodiscard]] static std::vector<ui::TableColumn> tableColumns();
};

inline ModelGridFixture::ModelGridFixture(std::vector<std::int64_t> const& keys, ui::EditMode mode) {
    for (std::int64_t const key : keys) {
        rows->view.push_back(
            FakeRows::Row{.key = table::RowId{key}, .record = specimenRecord(key), .fetched = true, .unloaded = {}});
    }
    auto const count = std::make_shared<forms::FormModel const>(
        morph::test::formsengine::modelOf("Lims_EditSpecimenCount", kEditSpecimenCount));
    auto const row = std::make_shared<forms::FormModel const>(
        morph::test::formsengine::modelOf("Lims_EditSpecimenRow", kEditSpecimenRow));
    table::TableDescription const description = specimenTableDescription();
    auto const editOf = [&description](std::string_view id) {
        auto const found = std::ranges::find_if(
            description.columns, [id](table::TableColumn const& column) { return column.info.id == id; });
        return found->edit;
    };
    std::vector<ui::ModelColumn> columns;
    columns.push_back(ui::ModelColumn{.id = "label", .edit = editOf("label"), .model = row, .path = "values.label"});
    columns.push_back(
        ui::ModelColumn{.id = "remark", .edit = editOf("remark"), .model = row, .path = "values.remarkText"});
    columns.push_back(ui::ModelColumn{.id = "count", .edit = editOf("count"), .model = count, .path = "value"});
    columns.push_back(ui::ModelColumn{.id = "note", .edit = std::nullopt, .model = nullptr, .path = {}});
    for (std::string const member : {"sampled", "taken", "day", "at", "mass", "weight"}) {
        columns.push_back(
            ui::ModelColumn{.id = member, .edit = editOf(member), .model = row, .path = "values." + member});
    }
    source = std::make_unique<ui::ModelGridSource>(
        rows, edits, std::move(columns), mode,
        ui::ModelGridContext{.runtime = &h.rt,
                             .choices = h.server.fetcher(),
                             .session = {},
                             .format = ui::FormatContext{},
                             .may = [this](std::string_view action) { return permitted.contains(action); }});
    source->setDescription(description);
    ui::GridOptions options;
    if (mode == ui::EditMode::Row) {
        options.enterMoves = ui::EnterMoves::Right;
    }
    grid = std::make_unique<ui::GridController>(h.rt, *source, std::move(options));
}

inline std::vector<ui::TableColumn> ModelGridFixture::tableColumns() {
    return {ui::TableColumn{.id = "label", .label = "label"},     ui::TableColumn{.id = "remark", .label = "remark"},
            ui::TableColumn{.id = "count", .label = "count"},     ui::TableColumn{.id = "note", .label = "note"},
            ui::TableColumn{.id = "sampled", .label = "sampled"}, ui::TableColumn{.id = "taken", .label = "taken"},
            ui::TableColumn{.id = "day", .label = "day"},         ui::TableColumn{.id = "at", .label = "at"},
            ui::TableColumn{.id = "mass", .label = "mass"},       ui::TableColumn{.id = "weight", .label = "weight"}};
}

// The value of a settled success, and of a refusal naming fields.
inline std::expected<void, std::vector<table::FieldError>> committedOk() { return {}; }
inline std::expected<void, std::vector<table::FieldError>> refusedWith(std::vector<table::FieldError> errors) {
    return std::unexpected(std::move(errors));
}

}  // namespace morph::test::grid
