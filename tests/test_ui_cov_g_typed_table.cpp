// SPDX-License-Identifier: Apache-2.0

// A table over typed rows: the select column and its select-all, a row's tone, label, heading and spans, the sort the
// headers show, and the header entries a caller adds to a column's menu.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <functional>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/testing/step_executor.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <morph/ui/view.hpp>
#include <string>
#include <vector>

namespace {

namespace ui = morph::ui;
using morph::reactive::Runtime;
using morph::reactive::Signal;
using morph::ui::Key;
using morph::ui::testing::RecordingBackend;

struct Item {
    std::int64_t id = 0;
    std::string name;
    bool heading = false;
};

struct Rig {
    morph::testing::StepExecutor owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::vector<Item>> items{runtime,
                                    std::vector<Item>{{1, "one", false}, {2, "two", false}, {3, "head", true}}};
    Signal<std::string> label{runtime, std::string{"Everything"}};
    std::vector<std::string> events;
    std::unique_ptr<ui::Mounted> view;
    int table = 0;

    explicit Rig(ui::TableOptions options = {}) {
        std::vector<ui::TableColumn> columns{ui::TableColumn{.id = "id", .label = "Id", .sortable = true},
                                             ui::TableColumn{.id = "name", .label = "Name", .sortable = true}};
        std::function<ui::TableRowView(Signal<Item> const&)> rowView = [](Signal<Item> const& row) {
            ui::TableRowView made;
            if (row.peek().heading) {
                made.heading = true;
                made.cells.push_back(ui::text({.text = [&row] { return row.get().name; }}));
                return made;
            }
            made.cells.push_back(ui::text({.text = [&row] { return std::to_string(row.get().id); }}));
            made.cells.push_back(ui::text({.text = [&row] { return row.get().name; }}));
            made.tone = ui::Prop<std::optional<ui::Tone>>{[&row] {
                return row.get().id == 1 ? std::optional<ui::Tone>{ui::Tone::Warn} : std::optional<ui::Tone>{};
            }};
            made.label = ui::Prop<std::string>{[&row] { return "Item " + row.get().name; }};
            if (row.peek().id == 1) {
                made.spans = ui::Prop<std::vector<ui::RowSpan>>{
                    [] { return std::vector<ui::RowSpan>{ui::RowSpan{.column = 0, .rows = 2}}; }};
            }
            return made;
        };
        view = std::make_unique<ui::Mounted>(
            runtime, backend,
            ui::table<Item>(
                std::move(columns), [this] { return items.get(); }, [](Item const& item) { return Key{item.id}; },
                std::move(rowView), std::move(options)));
        owner.runAll();
        table = backend.all("Table").front();
    }
};

int rowWith(RecordingBackend const& backend, int table, std::string const& key) {
    for (int const row : backend.children(table)) {
        if (backend.prop(row, "rowKey") == key) {
            return row;
        }
    }
    FAIL("no row " << key);
    return -1;
}

}  // namespace

TEST_CASE("covg typed table: a select column draws its label and sends select-all", "[covg][ui][mount]") {
    int selectAlls = 0;
    ui::TableOptions options;
    options.selectionMode = ui::SelectionMode::Multiple;
    options.selectColumn = true;
    options.onSelectAll = [&selectAlls] { ++selectAlls; };
    Rig rig{std::move(options)};
    CHECK(rig.backend.prop(rig.table, "selectColumn") == "true");
    CHECK(rig.backend.prop(rig.table, "selectAllLabel") == "Select all");
    rig.backend.selectAll(rig.table);
    CHECK(selectAlls == 1);
}

TEST_CASE("covg typed table: a table with no select column draws none and takes no select-all", "[covg][ui][mount]") {
    Rig rig;
    CHECK(rig.backend.prop(rig.table, "selectColumn") != "true");
    CHECK_THROWS_AS(rig.backend.selectAll(rig.table), std::logic_error);
}

TEST_CASE("covg typed table: a row's tone, label, spans and heading reach the backend", "[covg][ui][mount]") {
    Rig rig;
    int const first = rowWith(rig.backend, rig.table, "1");
    int const second = rowWith(rig.backend, rig.table, "2");
    int const head = rowWith(rig.backend, rig.table, "3");
    CHECK(rig.backend.prop(first, "tone") == "Warn");
    CHECK(rig.backend.prop(second, "tone") == "none");
    CHECK(rig.backend.prop(first, "label") == "Item one");
    CHECK(rig.backend.prop(first, "rowSpans") == "0:2");
    CHECK(rig.backend.prop(head, "heading") == "true");
    CHECK(rig.backend.prop(first, "heading") != "true");
    rig.items.set(std::vector<Item>{{1, "uno", false}, {2, "two", false}, {3, "head", true}});
    rig.owner.runAll();
    CHECK(rig.backend.prop(rowWith(rig.backend, rig.table, "1"), "label") == "Item uno");
}
