// SPDX-License-Identifier: Apache-2.0
//
// Interpreter measurements: mounting a ui/1 document with a table, and re-running its reactive graph after a
// state change. Hidden, so ctest never runs it; run it by tag on a Release build:
//
//   morph_tests "[.benchmark][interpreter]"

#include <algorithm>
#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <format>
#include <glaze/glaze.hpp>
#include <initializer_list>
#include <memory>
#include <morph/core/wire.hpp>
#include <morph/forms/engine/collection.hpp>
#include <morph/table/data_source.hpp>
#include <morph/table/engine.hpp>
#include <morph/testing/step_executor.hpp>
#include <morph/ui/table_rows.hpp>
#include <morph/ui/value.hpp>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "forms_engine_support.hpp"
#include "qt_quick/wide_rows_document.hpp"
#include "table/wide_table_model.hpp"
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

// ── Step 0 baselines: a `rows` table on the interpreter, and a collection ───
// docs/spec/table/table.md, "Benchmarks: every surface". Each case first REQUIREs that the path it times was reached.
// Run on a Release build:
//
//   morph_tests "[.benchmark][baseline]"

namespace {

using Clock = std::chrono::steady_clock;

struct Spread {
    double median = 0;
    double p10 = 0;
    double p90 = 0;
};

Spread spreadOf(std::vector<double> samples) {
    std::ranges::sort(samples);
    auto const at = [&](double q) {
        auto const index = static_cast<std::size_t>(std::lround(q * static_cast<double>(samples.size() - 1)));
        return samples[std::min(index, samples.size() - 1)];
    };
    return Spread{.median = at(0.5), .p10 = at(0.1), .p90 = at(0.9)};
}

double millisSince(Clock::time_point began) {
    return std::chrono::duration<double, std::milli>(Clock::now() - began).count();
}

void printRow(std::string_view scenario, std::size_t rows, Spread const& spread) {
    std::println("| {} | {} | {:.2f} | {:.2f}-{:.2f} |", scenario, rows, spread.median, spread.p10, spread.p90);
    static_cast<void>(std::fflush(stdout));
}

std::string jsonText(std::string const& text) {
    std::string out;
    REQUIRE_FALSE(glz::write_json(text, out));
    return out;
}

// The rows of a `rows` table over the fixture: `id`, `name` from `t001` and `qty` from `i001`.
std::string fixtureItems(bench::WideTableSource const& source) {
    auto const snapshot = source.snapshot();
    auto const columns = source.columns();
    auto const index = [&](std::string_view id) {
        return static_cast<std::size_t>(std::ranges::find(columns, id, &morph::table::ColumnInfo::id) -
                                        columns.begin());
    };
    auto const name = index("t001");
    auto const qty = index("i001");
    std::string items;
    for (std::size_t row = 0; row < snapshot->rowCount(); ++row) {
        auto const quantity = snapshot->cell(row, qty);
        items += std::format(R"({}{{"id":{},"name":{},"qty":{}}})", row == 0 ? "" : ",", row + 1,
                             jsonText(morph::table::displayText(snapshot->cell(row, name))),
                             std::holds_alternative<std::monostate>(quantity) ? std::string{"null"}
                                                                              : morph::table::displayText(quantity));
    }
    return items;
}

// A table over state `items` whose sort is state `order`; "sort" sorts it by name, "rename" gives the row keyed
// @p renamed a name that sorts last.
std::string sortedDocument(std::string const& items, std::int64_t renamed) {
    return std::format(R"({{
  "vocab": "ui/1", "id": "baselineRows",
  "state": {{ "items": {{ "init": {{ "lit": [{}] }} }}, "order": {{ "init": {{ "lit": [] }} }} }},
  "view": {{ "kind": "column", "children": [
    {{ "kind": "button", "testId": "sort", "label": "sort",
       "onClick": {{ "set": "order", "value": {{ "lit": [ {{ "column": "name", "direction": "asc" }} ] }} }} }},
    {{ "kind": "button", "testId": "rename", "label": "rename",
       "onClick": {{ "set": "items", "value": {{ "map": {{ "ref": "items" }}, "as": "r", "to": {{ "if": [
         {{ "fn": "eq", "args": [ {{ "ref": "r.id" }}, {} ] }},
         {{ "obj": {{ "id": {{ "ref": "r.id" }}, "name": "ÿÿÿ", "qty": {{ "ref": "r.qty" }} }} }},
         {{ "ref": "r" }} ] }} }} }} }},
    {{ "kind": "table", "rows": {{ "ref": "items" }}, "sort": {{ "ref": "order" }},
       "columns": [ {{ "id": "id", "label": "Id" }}, {{ "id": "name", "label": "Name", "sortable": true }},
                    {{ "id": "qty", "label": "Qty" }} ] }} ] }} }})",
                       items, renamed);
}

// The texts of the first row the table shows, in column order.
std::vector<std::string> firstRowTexts(Harness const& h) {
    auto const tables = h.backend.all("Table");
    REQUIRE(tables.size() == 1);
    auto const rows = h.backend.children(tables.front());
    REQUIRE_FALSE(rows.empty());
    std::vector<std::string> texts;
    std::vector<int> pending{rows.front()};
    while (!pending.empty()) {
        int const id = pending.front();
        pending.erase(pending.begin());
        if (h.backend.kindOf(id) == "Text") {
            texts.push_back(h.backend.prop(id, "text"));
        }
        auto const children = h.backend.children(id);
        pending.insert(pending.begin(), children.begin(), children.end());
    }
    return texts;
}

std::string realizedWindow(Harness const& h) {
    return h.backend.prop(h.backend.all("Table").front(), "realizedWindow");
}

}  // namespace

TEST_CASE("interpreter baseline: U1 U2 a rows table of 100,000", "[.benchmark][baseline]") {
    constexpr std::size_t kRows = 100'000;
    bench::WideTableSource const source{{.rows = kRows, .columns = 300, .seed = 1}};
    std::string const items = fixtureItems(source);
    // The row U2 renames is the one the sorted table shows first, read from one mount before the timed runs.
    std::int64_t shownFirst = 0;
    {
        Harness probe;
        auto const screen = probe.mount(sortedDocument(items, 0));
        probe.backend.click(probe.byTestId("Button", "sort"));
        probe.settle();
        auto const texts = firstRowTexts(probe);
        REQUIRE_FALSE(texts.empty());
        shownFirst = std::stoll(texts.front());
    }
    std::string const document = sortedDocument(items, shownFirst);
    std::println(
        "\nU1, U2: a document rows table of 100,000 rows on the recording backend, click to settled\n\n"
        "| scenario | rows | median ms | p10-p90 ms |\n|---|---:|---:|---:|");
    std::vector<double> sorts;
    std::vector<double> renames;
    for (int run = 0; run < 7; ++run) {
        Harness h;
        auto const screen = h.mount(document);
        REQUIRE(realizedWindow(h).ends_with("/100000"));
        auto const before = firstRowTexts(h);
        auto began = Clock::now();
        h.backend.click(h.byTestId("Button", "sort"));
        h.settle();
        sorts.push_back(millisSince(began));
        auto const sorted = firstRowTexts(h);
        REQUIRE(sorted.size() >= 2);
        REQUIRE(sorted != before);
        began = Clock::now();
        h.backend.click(h.byTestId("Button", "rename"));
        h.settle();
        renames.push_back(millisSince(began));
        auto const after = firstRowTexts(h);
        REQUIRE(after.size() >= 2);
        REQUIRE(after.front() != sorted.front());  // the renamed row moved from the top
    }
    printRow("U1: sort on name, click to settled", kRows, spreadOf(sorts));
    printRow("U2: one row renamed in the sorted table, click to settled", kRows, spreadOf(renames));
}

namespace {

// The bytes a document value holds, as the cells' `approximateCellBytes` counts a cell: the `Value` itself, a string's
// heap buffer when it has one, a list's or a record's container and its elements, with each map node's pointers.
// A list or record shared between values is counted once; allocator overhead is not counted, as for the cells.
class ValueBytes {
public:
    [[nodiscard]] std::size_t of(ui::Value const& value) { return sizeof(ui::Value) + held(value); }

private:
    // What @p value holds outside its own `sizeof`.
    // NOLINTNEXTLINE(misc-no-recursion): `held` measures a value that may nest lists and records, recursing once per level of the document's own nesting
    std::size_t held(ui::Value const& value) {
        if (std::string const* const text = value.asString()) {
            return heap(*text);
        }
        if (ui::Quantity const* const quantity = value.asQuantity()) {
            return heap(quantity->unit);
        }
        if (ui::ValueList const* const list = value.asList()) {
            if (!_seen.insert(list).second) {
                return 0;
            }
            std::size_t bytes = kControlBlock + sizeof(ui::ValueList) + (list->capacity() * sizeof(ui::Value));
            for (ui::Value const& item : *list) {
                bytes += held(item);
            }
            return bytes;
        }
        if (ui::ValueRecord const* const record = value.asRecord()) {
            if (!_seen.insert(record).second) {
                return 0;
            }
            std::size_t bytes = kControlBlock + sizeof(ui::ValueRecord) + kMapNode;  // the map's sentinel node
            for (auto const& [key, field] : *record) {
                bytes += kMapNode + heap(key) + held(field);
            }
            return bytes;
        }
        return 0;
    }

    [[nodiscard]] static std::size_t heap(std::string const& text) {
        return text.capacity() > std::string{}.capacity() ? text.capacity() + 1 : 0;
    }

    // A shared_ptr's control block: two counts and a vtable pointer.
    static constexpr std::size_t kControlBlock = (2 * sizeof(long)) + sizeof(void*);
    // A map node: its key and value, three links and its colour, padded.
    static constexpr std::size_t kMapNode = sizeof(std::pair<std::string const, ui::Value>) + (4 * sizeof(void*));
    std::unordered_set<void const*> _seen;
};

}  // namespace

TEST_CASE("interpreter baseline: U4 memory of a rows table", "[.benchmark][baseline]") {
    constexpr std::size_t kRows = 10'000;
    bench::WideTableSource const source{{.rows = kRows, .columns = 300, .seed = 1}};
    auto const snapshot = source.snapshot();
    std::size_t cellBytes = 0;
    for (std::size_t row = 0; row < kRows; ++row) {
        for (std::size_t column = 0; column < source.columns().size(); ++column) {
            cellBytes += bench::approximateCellBytes(snapshot->cell(row, column));
        }
    }
    std::string const document = std::format(
        R"({{ "vocab": "ui/1", "id": "baselineMemory", "state": {{ "items": {{ "init": {{ "lit": [{}] }} }} }},
  "view": {{ "kind": "table", "rows": {{ "ref": "items" }}, "sort": {{ "lit": [ {{ "column": "t001", "direction": "asc" }} ] }},
             "columns": [{}] }} }})",
        bench::wideRowsJson(source), bench::wideColumnsJson(source));
    Harness h;
    auto screen = h.mount(document);
    REQUIRE(realizedWindow(h).ends_with("/10000"));
    auto const reference = ui::parseJson(R"({"ref": "items"})");
    REQUIRE(reference.has_value());
    ui::Value const items = screen->read(*reference);
    REQUIRE(items.asList() != nullptr);
    REQUIRE(items.asList()->size() == kRows);
    auto const valueBytes = ValueBytes{}.of(items);
    std::println(
        "\nU4: a rows table of 10,000 x 300 sorted on one column\n\n| held as | bytes | MiB | bytes per cell |\n"
        "|---|---:|---:|---:|\n"
        "| cells (approximateCellBytes) | {} | {:.1f} | {:.1f} |\n"
        "| the state's document values | {} | {:.1f} | {:.1f} |",
        cellBytes, static_cast<double>(cellBytes) / (1024.0 * 1024.0),
        static_cast<double>(cellBytes) / (kRows * 300.0), valueBytes,
        static_cast<double>(valueBytes) / (1024.0 * 1024.0), static_cast<double>(valueBytes) / (kRows * 300.0));
    static_cast<void>(std::fflush(stdout));
}

namespace {

constexpr std::string_view kCollectionView = R"({
  "v-kind": "collection", "v-title": "Rows", "v-query": "ListRows", "v-rowKey": "id",
  "v-columns": [
    {"field": "id", "label": "ID", "kind": "integer", "v-hidden": true},
    {"field": "name", "label": "Name", "kind": "text"},
    {"field": "qty", "label": "Qty", "kind": "integer"}]})";

// A ListRows reply of the fixture's rows; @p renamed, when not 0, is the key of a row whose name is changed.
std::string listReply(bench::WideTableSource const& source, std::int64_t renamed) {
    auto const snapshot = source.snapshot();
    auto const columns = source.columns();
    auto const index = [&](std::string_view id) {
        return static_cast<std::size_t>(std::ranges::find(columns, id, &morph::table::ColumnInfo::id) -
                                        columns.begin());
    };
    auto const name = index("t001");
    auto const qty = index("i001");
    std::string out = R"({"rows":[)";
    for (std::size_t row = 0; row < snapshot->rowCount(); ++row) {
        auto const key = static_cast<std::int64_t>(row) + 1;
        auto const quantity = snapshot->cell(row, qty);
        out += std::format(
            R"({}{{"id":{},"name":{},"qty":{}}})", row == 0 ? "" : ",", key,
            jsonText(key == renamed ? std::string{"renamed"} : morph::table::displayText(snapshot->cell(row, name))),
            std::holds_alternative<std::monostate>(quantity) ? std::string{"null"}
                                                             : morph::table::displayText(quantity));
    }
    out += "]}";
    return out;
}

}  // namespace

TEST_CASE("interpreter baseline: U5 a collection of 20,000", "[.benchmark][baseline]") {
    constexpr std::size_t kRows = 20'000;
    bench::WideTableSource const source{{.rows = kRows, .columns = 30, .seed = 1}};
    std::string const original = listReply(source, 0);
    std::string const edited = listReply(source, 1);
    auto const model = morph::forms::CollectionModel::fromSchema(kCollectionView);
    REQUIRE(model.has_value());
    std::println(
        "\nU5: a collection of 20,000 rows over a ListRows reply\n\n"
        "| scenario | rows | median ms | p10-p90 ms |\n|---|---:|---:|---:|");
    std::vector<double> mounts;
    std::vector<double> sorts;
    std::vector<double> edits;
    for (int run = 0; run < 15; ++run) {
        morph::test::formsengine::Harness harness;
        std::string const* reply = &original;
        harness.server.replyWith([&reply](morph::test::formsengine::Call const& call) -> std::optional<std::string> {
            if (call.action == "ListRows") {
                return *reply;
            }
            return std::string{R"({"ok":true})"};
        });
        auto began = Clock::now();
        morph::forms::CollectionSession collection{harness.rt, *model,
                                                   [](std::string_view) { return std::optional<std::string>{}; },
                                                   harness.server.submitter(), harness.server.fetcher()};
        harness.settle();
        mounts.push_back(millisSince(began));
        REQUIRE(collection.rows().size() == kRows);

        morph::table::Engine engine{collection.source()};
        began = Clock::now();
        REQUIRE(engine.setSort({{.column = "name", .dir = morph::table::SortDirection::Ascending}}).has_value());
        engine.settleNow();
        sorts.push_back(millisSince(began));
        REQUIRE(engine.viewRowCount() == kRows);

        reply = &edited;
        began = Clock::now();
        collection.refresh();
        harness.settle();
        engine.settleNow();
        edits.push_back(millisSince(began));
        REQUIRE(collection.rows().size() == kRows);
        REQUIRE(collection.rows().front().cells.front() == "renamed");
    }
    printRow("U5: mount (list reply decoded into the collection)", kRows, spreadOf(mounts));
    printRow("U5: sort on name through the table engine", kRows, spreadOf(sorts));
    printRow("U5: one row edited (the list reloaded) and the sorted view repaired", kRows, spreadOf(edits));
}

// U5 on a screen: a `collection` node over the corpus's sample list, its rows a table on the engine. Mount is the
// list's reply to settled; sort is a header click on Name to settled; an edit is a row's delete answered, then the
// reloaded list with one row renamed answered, to settled. docs/spec/table/table.md, "Performance". Run on a
// Release build:
//
//   morph_tests "interpreter bench: a collection of 20\,000 rows*"   (Catch2 reads an unescaped comma as a list)
TEST_CASE("interpreter bench: a collection of 20,000 rows: mount, sort, one row edited", "[.benchmark][interpreter]") {
    constexpr std::size_t kRows = 20'000;
    bench::WideTableSource const source{{.rows = kRows, .columns = 30, .seed = 1}};
    std::string const original = listReply(source, 0);
    std::string const edited = listReply(source, 1);
    constexpr std::string_view kDocument = R"({
  "vocab": "ui/1", "id": "bench",
  "models": { "sample": { "type": "Lims_SampleModel" } },
  "view": { "kind": "collection", "model": "sample", "action": "Lims_ListSamples" } })";
    std::println(
        "\nU5: a collection node of 20,000 rows on the table engine, call to settled\n\n"
        "| scenario | rows | median ms | p10-p90 ms |\n|---|---:|---:|---:|");
    std::vector<double> mounts;
    std::vector<double> sorts;
    std::vector<double> edits;
    for (int run = 0; run < 7; ++run) {
        Harness h;
        auto const screen = h.mount(kDocument);
        auto began = Clock::now();
        h.reply(h.dispatcher.last("Lims_ListSamples"), original);
        mounts.push_back(millisSince(began));
        int const table = h.byTestId("Table", "collection_rows");
        REQUIRE_FALSE(h.backend.children(table).empty());

        began = Clock::now();
        h.backend.clickHeader(table, 1, ui::HeaderClick::Plain);  // Name
        h.settle();
        sorts.push_back(millisSince(began));
        REQUIRE(h.backend.prop(table, "sort") == "1:Ascending");

        std::string const key = h.backend.prop(h.backend.children(table).front(), "rowKey");
        h.backend.click(h.byTestId("Button", "rowAction_Lims_DeleteSample_" + key));
        h.settle();
        h.backend.click(h.byTestId("Button", "confirm_accept"));
        h.settle();
        h.reply(h.dispatcher.last("Lims_DeleteSample"), "{}");
        began = Clock::now();
        h.reply(h.dispatcher.last("Lims_ListSamples"), edited);
        edits.push_back(millisSince(began));
        REQUIRE(h.backend.prop(table, "sort") == "1:Ascending");
    }
    printRow("U5: mount (the list's reply to settled)", kRows, spreadOf(mounts));
    printRow("U5: sort on Name from the header, click to settled", kRows, spreadOf(sorts));
    printRow("U5: one row edited (the list reloaded), reply to settled", kRows, spreadOf(edits));
}

namespace {

// `T_Page` holds `T_Row` keyed by `code` at `rows`; `T_TablePage` holds it opaquely, as `Table.<id>` does.
ui::Catalog k4Catalog() {
    ui::Catalog catalog = ui::testing::corpusCatalog();
    auto const base = catalog.action;
    catalog.action = [base](std::string_view action) -> std::optional<ui::ActionInfo> {
        auto info = base(action);
        if (info && action == "T_Page") {
            info->records = {ui::ActionRecordInfo{
                .record = "T_Row", .keyField = "code", .paths = {"rows"}, .open = {}, .partial = {}, .opaque = false}};
        }
        if (info && action == "T_TablePage") {
            info->records = {ui::ActionRecordInfo{
                .record = "T_Row", .keyField = "code", .paths = {}, .open = {}, .partial = {}, .opaque = true}};
        }
        return info;
    };
    return catalog;
}

std::string k4Document(std::string_view action) {
    return std::format(R"({{
  "vocab": "ui/1", "id": "k4",
  "models": {{ "sample": {{ "type": "Lims_SampleModel" }} }},
  "queries": {{ "page": {{ "model": "sample", "action": "{}" }} }},
  "view": {{ "kind": "text", "text": "k4" }} }})",
                       action);
}

}  // namespace

TEST_CASE("interpreter bench: K4 the query cache under an event and a wide page", "[.benchmark][interpreter]") {
    std::string rows;
    for (int row = 0; row < 200; ++row) {
        rows += std::format(R"({}{{"code":{},"name":"row-{}"}})", row == 0 ? "" : ",", row, row);
    }
    morph::wire::ChangesBody event;
    event.records.push_back(morph::effects::RecordChange{.record = "T_Row", .keys = std::vector<glz::raw_json>{}});
    for (int key = 0; key < 4'096; ++key) {
        event.records.back().keys->emplace_back(std::to_string(100'000 + key));
    }
    std::vector<double> events;
    for (int run = 0; run < 15; ++run) {
        Harness h{k4Catalog()};
        auto const screen = h.mount(k4Document("T_Page"));
        h.reply(0, std::format(R"({{"rows":[{}]}})", rows));
        auto const began = std::chrono::steady_clock::now();
        h.session.applyChanges(event);
        events.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count());
        REQUIRE(h.dispatcher.calls.size() == 1);
    }

    std::string page = R"({"revision":"r","columns":[)";
    for (int column = 0; column < 300; ++column) {
        page += std::format(R"({}"c{:03}")", column == 0 ? "" : ",", column);
    }
    page += R"(],"keys":[)";
    for (int row = 0; row < 200; ++row) {
        page += std::format("{}{}", row == 0 ? "" : ",", row);
    }
    page += R"(],"rows":[)";
    for (int row = 0; row < 200; ++row) {
        page += row == 0 ? "[" : ",[";
        for (int column = 0; column < 300; ++column) {
            page += std::format("{}{}.5", column == 0 ? "" : ",", row + column);
        }
        page += ']';
    }
    page += R"(],"total":200,"totalKind":"exact","offset":0})";
    std::vector<double> stores;
    for (int run = 0; run < 7; ++run) {
        Harness h{k4Catalog()};
        auto const screen = h.mount(k4Document("T_TablePage"));
        auto const began = std::chrono::steady_clock::now();
        h.reply(0, page);
        stores.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count());
    }
    std::ranges::sort(events);
    std::ranges::sort(stores);
    std::println("\n| K4 | a 4,096-key event against an entry of 200 rows | {:.3f} ms median ({:.3f}-{:.3f}) |",
                 events[events.size() / 2], events.front(), events.back());
    std::println(
        "| K4 | storing a decoded 200 x 300 page of fractional numbers in an opaque entry | {:.3f} ms median "
        "({:.3f}-{:.3f}) |",
        stores[stores.size() / 2], stores.front(), stores.back());
    static_cast<void>(std::fflush(stdout));
}

// U1, U2 and U4 on the engine: a document's rows through `ValueRowsSource`. Timings only; their count gates are the
// "value rows source" cases of test_ui_table_model_value_rows.cpp.

namespace {

// A runtime of its own, a source over @p columns keyed by `id`, and an engine over it.
struct RowsOnEngine {
    explicit RowsOnEngine(std::vector<ui::ValueColumn> columns)
        : source{std::make_shared<ui::ValueRowsSource>(
              runtime, std::move(columns), [](ui::Value const& row) { return ui::Key{*row.field("id")->asInt()}; })},
          engine{source, morph::table::EngineOptions{.owner = &owner}} {}

    morph::testing::StepExecutor owner;
    morph::reactive::Runtime runtime{owner};
    std::shared_ptr<ui::ValueRowsSource> source;
    morph::table::Engine engine;
};

ui::ValueList listOf(std::string const& items) {
    auto const json = ui::parseJson("[" + items + "]");
    REQUIRE(json.has_value());
    auto const value = ui::valueFromJson(*json);
    REQUIRE(value.has_value());
    REQUIRE(value->asList() != nullptr);
    return *value->asList();
}

// A copy of @p rows whose row @p at has the name "ÿÿÿ", which sorts last; the other rows share storage with @p rows.
ui::ValueList renamed(ui::ValueList rows, std::size_t at) {
    ui::ValueRecord fields = *rows.at(at).asRecord();
    fields.insert_or_assign("name", ui::Value{std::string{"ÿÿÿ"}});
    rows.at(at) = ui::Value{std::move(fields)};
    return rows;
}

std::vector<ui::ValueColumn> plainColumns(std::initializer_list<std::string_view> ids) {
    std::vector<ui::ValueColumn> columns;
    for (auto const id : ids) {
        columns.push_back(ui::ValueColumn{.id = std::string{id}, .kind = std::nullopt, .sortBy = {}});
    }
    return columns;
}

}  // namespace

TEST_CASE("interpreter bench: U1 U2 a rows table of 100,000 on the engine", "[.benchmark][interpreter]") {
    constexpr std::size_t kRows = 100'000;
    bench::WideTableSource const fixture{{.rows = kRows, .columns = 300, .seed = 1}};
    std::string const items = fixtureItems(fixture);
    ui::ValueList const rows = listOf(items);
    std::println(
        "\nU1, U2: a document's rows of 100,000 through ValueRowsSource and the engine, call to settled\n\n"
        "| scenario | rows | median ms | p10-p90 ms |\n|---|---:|---:|---:|");
    std::vector<double> sorts;
    std::vector<double> shared;
    std::vector<double> fresh;
    for (int run = 0; run < 7; ++run) {
        RowsOnEngine h{plainColumns({"id", "name", "qty"})};
        h.source->assign(rows);
        h.owner.runAll();
        auto began = Clock::now();
        h.source->use("name");
        REQUIRE(h.engine.setSort({{.column = "name", .dir = morph::table::SortDirection::Ascending}}));
        h.owner.runAll();
        sorts.push_back(millisSince(began));
        auto const first = std::get<std::int64_t>(h.engine.rowIdAt(0));
        auto const firstRow = static_cast<std::size_t>(first - 1);
        began = Clock::now();
        h.source->assign(renamed(rows, firstRow));
        h.owner.runAll();
        shared.push_back(millisSince(began));
        REQUIRE(std::get<std::int64_t>(h.engine.rowIdAt(kRows - 1)) == first);  // the renamed row moved to the end
        // The held rows with one more renamed, written to JSON and read back: no row shares storage.
        std::string const held = ui::detail::canonicalJson(ui::Value{renamed(renamed(rows, firstRow), firstRow + 1)});
        ui::ValueList const decoded = listOf(held.substr(1, held.size() - 2));
        began = Clock::now();
        h.source->assign(decoded);
        h.owner.runAll();
        fresh.push_back(millisSince(began));
        REQUIRE(std::get<std::int64_t>(h.engine.rowIdAt(kRows - 1)) == first + 1);
    }
    printRow("U1: use and sort on name, call to settled", kRows, spreadOf(sorts));
    printRow("U2: one row renamed, other rows shared", kRows, spreadOf(shared));
    printRow("U2: one row renamed, a fresh decode of every row", kRows, spreadOf(fresh));
}

TEST_CASE("interpreter bench: U4 memory of a rows table on the engine", "[.benchmark][interpreter]") {
    constexpr std::size_t kRows = 10'000;
    bench::WideTableSource const fixture{{.rows = kRows, .columns = 300, .seed = 1}};
    std::vector<ui::ValueColumn> columns;
    for (auto const& column : fixture.columns()) {
        columns.push_back(ui::ValueColumn{.id = column.id, .kind = std::nullopt, .sortBy = {}});
    }
    RowsOnEngine h{std::move(columns)};
    h.source->assign(listOf(bench::wideRowsJson(fixture)));
    h.source->use("t001");
    REQUIRE(h.engine.setSort({{.column = "t001", .dir = morph::table::SortDirection::Ascending}}));
    h.owner.runAll();
    REQUIRE(h.engine.viewRowCount() == kRows);
    auto const snapshot = h.source->snapshot();
    auto const sorted = static_cast<std::size_t>(
        std::ranges::find(h.source->columns(), std::string{"t001"}, &morph::table::ColumnInfo::id) -
        h.source->columns().begin());
    std::size_t heldBytes = 0;
    std::size_t allBytes = 0;
    for (std::size_t row = 0; row < kRows; ++row) {
        for (std::size_t column = 0; column < fixture.columns().size(); ++column) {
            std::size_t const bytes = bench::approximateCellBytes(fixture.snapshot()->cell(row, column));
            allBytes += bytes;
        }
        heldBytes += bench::approximateCellBytes(snapshot->cell(row, sorted));
    }
    std::println(
        "\nU4: a rows table of 10,000 x 300 through ValueRowsSource, sorted on one column\n\n"
        "| held as | cells | bytes | MiB |\n|---|---:|---:|---:|\n"
        "| the source's cells (one column in use) | {} | {} | {:.2f} |\n"
        "| every column as cells (approximateCellBytes) | {} | {} | {:.1f} |",
        h.source->cellsHeld(), heldBytes, static_cast<double>(heldBytes) / (1024.0 * 1024.0),
        kRows * fixture.columns().size(), allBytes, static_cast<double>(allBytes) / (1024.0 * 1024.0));
    static_cast<void>(std::fflush(stdout));
}

// B1 as a `rows` table runs it: the engine with no worker, in owner steps of 4 ms, over `ValueRowsSource`, with the
// collator a rows table orders by. A timing: the bounds are table.md's, read off the printed rows.
TEST_CASE("interpreter bench: B1 a rows table's sort and filter in owner steps", "[.benchmark][interpreter]") {
    std::println(
        "\nB1: a three-key sort then a text filter on a rows table's engine, no worker, 4 ms steps\n\n"
        "| scenario | rows | median ms | p10-p90 ms |\n|---|---:|---:|---:|");
    for (std::size_t const rows : {std::size_t{10'000}, std::size_t{20'000}, std::size_t{100'000}}) {
        bench::WideTableSource const fixture{{.rows = static_cast<std::int64_t>(rows), .columns = 300, .seed = 1}};
        ui::ValueList const items = listOf(fixtureItems(fixture));
        std::vector<double> results;
        std::vector<double> steps;
        for (int run = 0; run < 7; ++run) {
            morph::testing::StepExecutor owner;
            morph::reactive::Runtime runtime{owner};
            auto const source = std::make_shared<ui::ValueRowsSource>(
                runtime, plainColumns({"id", "name", "qty"}),
                [](ui::Value const& row) { return ui::Key{*row.field("id")->asInt()}; });
            morph::table::EngineOptions options;
            options.owner = &owner;
            options.frameBudget = std::chrono::milliseconds{4};
            options.services.collator = std::make_shared<ui::ValueRowsCollator const>();
            morph::table::Engine engine{source, std::move(options)};
            source->assign(items);
            for (auto const* const column : {"name", "qty", "id"}) {
                source->use(column);
            }
            owner.runAll();
            morph::table::EngineStats const before = engine.stats();
            auto const began = Clock::now();
            REQUIRE(engine.setSort({{.column = "name", .dir = morph::table::SortDirection::Ascending},
                                    {.column = "qty", .dir = morph::table::SortDirection::Descending},
                                    {.column = "id", .dir = morph::table::SortDirection::Ascending}}));
            morph::table::FilterSpec filter;
            filter.columns["name"].include.push_back(morph::table::FilterEntry{.contains = "a"});
            auto const filtered = engine.setFilter(std::move(filter));
            REQUIRE(filtered);
            owner.runAll();
            results.push_back(millisSince(began));
            REQUIRE(engine.stats().ownerSteps - before.ownerSteps > 1);  // the work ran in steps, not in one call
            steps.push_back(static_cast<double>(engine.stats().maxOwnerStepMicros) / 1000.0);
        }
        printRow("B1: request to result", rows, spreadOf(results));
        printRow("B1: the longest owner step", rows, spreadOf(steps));
    }
}

// Where U1's and U2's time goes on the engine: the source's part (putting a column in use, matching a new list) and
// the engine's (absorbing the change, sorting or repairing), each call to settled. Timings only.
TEST_CASE("interpreter bench: U1 U2 split between the source and the engine", "[.benchmark][interpreter]") {
    constexpr std::size_t kRows = 100'000;
    bench::WideTableSource const fixture{{.rows = kRows, .columns = 300, .seed = 1}};
    ui::ValueList const rows = listOf(fixtureItems(fixture));
    std::println(
        "\nU1, U2 split: a document's rows of 100,000, the source's call and the engine's settling\n\n"
        "| scenario | rows | median ms | p10-p90 ms |\n|---|---:|---:|---:|");
    std::vector<double> use;
    std::vector<double> useSettled;
    std::vector<double> sort;
    std::vector<double> assign;
    std::vector<double> repair;
    for (int run = 0; run < 7; ++run) {
        RowsOnEngine h{plainColumns({"id", "name", "qty"})};
        h.source->assign(rows);
        h.owner.runAll();
        auto began = Clock::now();
        h.source->use("name");
        use.push_back(millisSince(began));
        began = Clock::now();
        h.owner.runAll();
        useSettled.push_back(millisSince(began));
        began = Clock::now();
        REQUIRE(h.engine.setSort({{.column = "name", .dir = morph::table::SortDirection::Ascending}}));
        h.owner.runAll();
        sort.push_back(millisSince(began));
        auto const first = std::get<std::int64_t>(h.engine.rowIdAt(0));
        ui::ValueList const next = renamed(rows, static_cast<std::size_t>(first - 1));
        began = Clock::now();
        h.source->assign(next);
        assign.push_back(millisSince(began));
        began = Clock::now();
        h.owner.runAll();
        repair.push_back(millisSince(began));
        REQUIRE(std::get<std::int64_t>(h.engine.rowIdAt(kRows - 1)) == first);
    }
    printRow("U1 split: use(name), the source deriving the column's cells", kRows, spreadOf(use));
    printRow("U1 split: the engine absorbing the new column (no sort yet)", kRows, spreadOf(useSettled));
    printRow("U1 split: setSort(name) to settled", kRows, spreadOf(sort));
    printRow("U2 split: assign of the list with one row renamed, the source matching it", kRows, spreadOf(assign));
    printRow("U2 split: the engine absorbing and repairing it", kRows, spreadOf(repair));
}

// D7: the Shift-click that adds a third key to a settled two-key sort of 100,000 document rows, click to settled.
// docs/spec/table/table.md, "Performance"; budget 50 ms. Run on a Release build:
//
//   morph_tests "interpreter bench: a third sort key on 100,000 rows"
TEST_CASE("interpreter bench: a third sort key on 100,000 rows", "[.benchmark][interpreter]") {
    constexpr std::size_t kRows = 100'000;
    std::string items;
    for (std::size_t i = 0; i < kRows; ++i) {
        items += std::format(R"({}{{"id":{},"a":{},"b":{},"c":{}}})", i == 0 ? "" : ",", i, i % 7, i % 13, i % 101);
    }
    std::string const document = std::format(R"({{
  "vocab": "ui/1", "id": "chain",
  "state": {{ "items": {{ "init": {{ "lit": [{}] }} }} }},
  "view": {{ "kind": "table", "id": "t", "rows": {{ "ref": "items" }},
             "columns": [ {{ "id": "a", "label": "A", "sortable": true }}, {{ "id": "b", "label": "B", "sortable": true }},
                          {{ "id": "c", "label": "C", "sortable": true }} ] }} }})",
                                             items);
    Harness h;
    auto const screen = h.mount(document);
    int const table = h.backend.all("Table").front();
    h.backend.clickHeader(table, 0, ui::HeaderClick::Plain);
    h.backend.clickHeader(table, 1, ui::HeaderClick::Shift);
    h.settle();
    REQUIRE(h.backend.prop(table, "sort") == "0:Ascending,1:Ascending");
    std::println(
        "\nD7: a third sort key added from the header to a two-key sort of 100,000 rows, click to settled\n\n"
        "| scenario | rows | median ms | p10-p90 ms |\n|---|---:|---:|---:|");
    std::vector<double> adds;
    for (int run = 0; run < 7; ++run) {
        auto const began = Clock::now();
        h.backend.clickHeader(table, 2, ui::HeaderClick::Shift);
        h.settle();
        adds.push_back(millisSince(began));
        REQUIRE(h.backend.prop(table, "sort") == "0:Ascending,1:Ascending,2:Ascending");
        h.backend.chooseMenu(table, 2, {"Remove from sort"});  // untimed: back to two keys
        h.settle();
        REQUIRE(h.backend.prop(table, "sort") == "0:Ascending,1:Ascending");
    }
    printRow("D7: Shift-click a third key, click to settled", kRows, spreadOf(adds));
}
