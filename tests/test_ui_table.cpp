// SPDX-License-Identifier: Apache-2.0

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <morph/reactive/detail/graph.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <morph/ui/view.hpp>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "owner_probe_recorder.hpp"
#include "test_support.hpp"
#include "ui_test_support.hpp"

namespace ui = morph::ui;

namespace {

using morph::reactive::Runtime;
using morph::reactive::Signal;
using morph::testing::intKey;
using ui::testing::RecordingBackend;
using Owner = morph::testing::StepExecutor;
using Probe = morph::testing::OwnerProbeRecorder;
using Lines = std::vector<std::string>;

struct Item {
    std::int64_t id = 0;
    std::string name;
    int qty = 0;
    bool operator==(Item const&) const = default;
};

struct Fixture {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::vector<Item>> items{runtime,
                                    {{.id = 1, .name = "apple", .qty = 3}, {.id = 2, .name = "pear", .qty = 5}}};
    Signal<std::vector<ui::Key>> selection{runtime, {}};
    std::vector<ui::Key> activated;

    ui::Node view() {
        return ui::table<Item>(
            {{.label = "Name"}, {.label = "Qty", .width = ui::Sizing::fixed(6)}}, [this] { return items.get(); },
            [](Item const& item) { return ui::Key{item.id}; },
            [](Signal<Item> const& item) {
                return std::vector<ui::Node>{ui::text({.text = [&item] { return item.get().name; }}),
                                             ui::text({.text = [&item] { return std::to_string(item.get().qty); }})};
            },
            {.selectionMode = ui::SelectionMode::Single,
             .selection = [this] { return selection.get(); },
             .onSelectionChange = [this](std::vector<ui::Key> keys) { selection.set(std::move(keys)); },
             .onActivate = [this](ui::Key key) { activated.push_back(std::move(key)); }});
    }
};

ui::Key keyOf(Item const& item) { return ui::Key{item.id}; }

// One cell per column: the name.
std::vector<ui::Node> nameCell(Signal<Item> const& item) {
    return {ui::text({.text = [&item] { return item.get().name; }})};
}

std::vector<Item> fruit() {
    return {{.id = 1, .name = "apple", .qty = 3}, {.id = 2, .name = "pear", .qty = 5}, {.id = 3, .name = "plum"}};
}

void removeItem(Signal<std::vector<Item>>& items, std::int64_t itemId) {
    items.mutate([itemId](std::vector<Item>& rows) {
        std::erase_if(rows, [itemId](Item const& item) { return item.id == itemId; });
    });
}

}  // namespace

TEST_CASE("ui::table: columns, keyed rows of cells, selection after the rows", "[ui]") {
    Fixture fixture;
    ui::Mounted const mounted{fixture.runtime, fixture.backend, fixture.view()};
    CHECK(fixture.backend.dump() ==
          "Table#1 columns=[Name:content,Qty:fixed(6)] selection=[] selectionMode=Single\n"
          "  Row#2 gap=0 rowKey=1\n"
          "    Text#3 role=Normal text=apple\n"
          "    Text#4 role=Normal text=3\n"
          "  Row#5 gap=0 rowKey=2\n"
          "    Text#6 role=Normal text=pear\n"
          "    Text#7 role=Normal text=5\n");
    CHECK(fixture.backend.log().back() == "set Table#1 selection=[]");
}

TEST_CASE("ui::table: a user's selection comes back through the selection prop", "[ui]") {
    Fixture fixture;
    ui::Mounted const mounted{fixture.runtime, fixture.backend, fixture.view()};
    fixture.backend.clearLog();
    fixture.backend.selectRows(1, {intKey(2)});
    fixture.owner.runAll();
    CHECK(fixture.selection.peek() == std::vector<ui::Key>{intKey(2)});
    CHECK(fixture.backend.log() == Lines{"set Table#1 selection=[2]"});
}

TEST_CASE("ui::table: activating a row reports its key", "[ui]") {
    Fixture fixture;
    ui::Mounted const mounted{fixture.runtime, fixture.backend, fixture.view()};
    fixture.backend.activateRow(1, intKey(1));
    CHECK(fixture.activated == std::vector<ui::Key>{intKey(1)});
}

TEST_CASE("ui::table: a new row is told its key once; a reorder only moves", "[ui]") {
    Fixture fixture;
    ui::Mounted const mounted{fixture.runtime, fixture.backend, fixture.view()};

    fixture.backend.clearLog();
    fixture.items.mutate([](std::vector<Item>& rows) { rows.push_back({.id = 3, .name = "plum", .qty = 1}); });
    fixture.owner.runAll();
    CHECK(fixture.backend.log() == Lines{"create Row#8 in Table#1", "set Row#8 gap=0", "create Text#9 in Row#8",
                                         "set Text#9 text=plum", "set Text#9 role=Normal", "create Text#10 in Row#8",
                                         "set Text#10 text=1", "set Text#10 role=Normal", "set Row#8 rowKey=3"});

    fixture.backend.clearLog();
    fixture.items.set({{.id = 2, .name = "pear", .qty = 5},
                       {.id = 1, .name = "apple", .qty = 3},
                       {.id = 3, .name = "plum", .qty = 1}});
    fixture.owner.runAll();
    CHECK(fixture.backend.log() == Lines{"move Row#2 to 1"});
}

TEST_CASE("ui::table: an empty rows, key or cells function is refused where it is written", "[ui]") {
    std::vector<ui::TableColumn> const columns{{.label = "Name"}};
    auto const rows = [] { return fruit(); };
    CHECK_THROWS_AS(ui::table<Item>(columns, {}, keyOf, nameCell), std::invalid_argument);
    CHECK_THROWS_AS(ui::table<Item>(columns, rows, {}, nameCell), std::invalid_argument);
    CHECK_THROWS_AS(ui::table<Item>(columns, rows, keyOf, {}), std::invalid_argument);
}

TEST_CASE("ui::table: column captions are escaped as list items", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    ui::Mounted const mounted{runtime, backend,
                              ui::table<Item>(
                                  {{.label = "a,b"}, {.label = "x=[y]:z", .width = ui::Sizing::stretch(2)}},
                                  [] { return std::vector<Item>{}; }, keyOf,
                                  [](Signal<Item> const& item) {
                                      return std::vector<ui::Node>{
                                          ui::text({.text = [&item] { return item.get().name; }}),
                                          ui::text({.text = [&item] { return std::to_string(item.get().qty); }})};
                                  })};
    CHECK(backend.prop(1, "columns") == "[a\\,b:content,x\\=\\[y\\]:z:stretch(2)]");
}

TEST_CASE("ui::table: a row whose cells are not one per column is refused and reported; the others mount", "[ui]") {
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::vector<Item>> items{runtime, {{.id = 1, .name = "apple"}, {.id = 2, .name = "pear"}}};
    // Two columns. Pear gets one cell; plum gets a null second cell.
    ui::Mounted const mounted{
        runtime, backend,
        ui::table<Item>(
            {{.label = "Name"}, {.label = "Qty"}}, [&items] { return items.get(); }, keyOf,
            [](Signal<Item> const& item) {
                std::vector<ui::Node> cells{ui::text({.text = [&item] { return item.get().name; }})};
                if (item.peek().id == 1) {
                    cells.push_back(ui::text({.text = "-"}));
                } else if (item.peek().id == 3) {
                    cells.push_back(nullptr);
                }
                return cells;
            })};
    CHECK(probe.count(morph::reactive::detail::site::kEffectThrew) == 1);
    CHECK(backend.dump() ==
          "Table#1 columns=[Name:content,Qty:content] selection=[] selectionMode=None\n"
          "  Row#2 gap=0 rowKey=1\n"
          "    Text#3 role=Normal text=apple\n"
          "    Text#4 role=Normal text=-\n");

    items.set({{.id = 1, .name = "apple"}, {.id = 3, .name = "plum"}});
    owner.runAll();
    CHECK(probe.count(morph::reactive::detail::site::kEffectThrew) == 2);
    CHECK(backend.all("Row") == std::vector<int>{2});
}

TEST_CASE("ui::table: a selected key without a row marks nothing until its row arrives, and stays selected", "[ui]") {
    Fixture fixture;
    ui::Mounted const mounted{fixture.runtime, fixture.backend, fixture.view()};
    fixture.backend.clearLog();
    fixture.selection.set({intKey(3)});
    fixture.owner.runAll();
    CHECK(fixture.backend.log() == Lines{"set Table#1 selection=[]"});

    fixture.backend.clearLog();
    fixture.items.mutate([](std::vector<Item>& rows) { rows.push_back({.id = 3, .name = "plum", .qty = 1}); });
    fixture.owner.runAll();
    CHECK(fixture.backend.prop(1, "selection") == "[3]");
    CHECK(fixture.backend.log().back() == "set Row#8 rowKey=3");  // marked by the widget, not by a setter

    // A reorder keeps the mark.
    fixture.items.set({{.id = 3, .name = "plum", .qty = 1},
                       {.id = 1, .name = "apple", .qty = 3},
                       {.id = 2, .name = "pear", .qty = 5}});
    fixture.owner.runAll();
    CHECK(fixture.backend.prop(1, "selection") == "[3]");

    // Its row going unmarks it; the key stays requested, so its row coming back marks it again.
    removeItem(fixture.items, 3);
    fixture.owner.runAll();
    CHECK(fixture.backend.prop(1, "selection") == "[]");
    fixture.items.mutate([](std::vector<Item>& rows) { rows.push_back({.id = 3, .name = "plum", .qty = 1}); });
    fixture.owner.runAll();
    CHECK(fixture.backend.prop(1, "selection") == "[3]");

    // None of that was the user's doing: the handler never ran, so the application's selection is untouched.
    CHECK(fixture.selection.peek() == std::vector<ui::Key>{intKey(3)});
}

TEST_CASE("ui::table: a Multiple table reports every selected key, in the user's order", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    std::vector<std::vector<ui::Key>> reported;
    ui::Mounted const mounted{runtime, backend,
                              ui::table<Item>({{.label = "Name"}}, [] { return fruit(); }, keyOf, nameCell,
                                              {.selectionMode = ui::SelectionMode::Multiple,
                                               .onSelectionChange = [&reported](std::vector<ui::Key> keys) {
                                                   reported.push_back(std::move(keys));
                                               }})};
    backend.selectRows(1, {intKey(3), intKey(1)});
    CHECK(reported == std::vector<std::vector<ui::Key>>{{intKey(3), intKey(1)}});
    CHECK(backend.prop(1, "selection") == "[3,1]");
}

TEST_CASE("ui::table: a user's change reports the selected rows only, and drops keys still waiting for a row",
          "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::vector<Item>> items{runtime, {{.id = 1, .name = "apple"}, {.id = 3, .name = "plum"}}};
    std::vector<std::vector<ui::Key>> reported;
    ui::Mounted const mounted{runtime, backend,
                              ui::table<Item>({{.label = "Name"}}, [&items] { return items.get(); }, keyOf, nameCell,
                                              {.selectionMode = ui::SelectionMode::Multiple,
                                               .selection = std::vector<ui::Key>{intKey(1), intKey(2)},
                                               .onSelectionChange = [&reported](std::vector<ui::Key> keys) {
                                                   reported.push_back(std::move(keys));
                                               }})};
    CHECK(backend.prop(1, "selection") == "[1]");  // 2 waits for its row

    backend.selectRows(1, {intKey(3), intKey(1)});
    CHECK(reported == std::vector<std::vector<ui::Key>>{{intKey(3), intKey(1)}});
    CHECK(backend.prop(1, "selection") == "[3,1]");

    // The user's change replaced the requested keys: row 2 arriving now is not selected.
    items.mutate([](std::vector<Item>& rows) { rows.push_back({.id = 2, .name = "pear"}); });
    owner.runAll();
    CHECK(backend.prop(1, "selection") == "[3,1]");
    CHECK(reported.size() == 1);
}

TEST_CASE("ui::table: selectRows and activateRow refuse what a user cannot do", "[ui]") {
    Fixture fixture;
    ui::Mounted const mounted{fixture.runtime, fixture.backend, fixture.view()};
    CHECK_THROWS_AS(fixture.backend.selectRows(1, {intKey(9)}), std::logic_error);             // no such row
    CHECK_THROWS_AS(fixture.backend.selectRows(1, {intKey(1), intKey(2)}), std::logic_error);  // Single
    CHECK_THROWS_AS(fixture.backend.selectRows(1, {intKey(1), intKey(1)}), std::logic_error);  // one row twice
    CHECK_THROWS_AS(fixture.backend.activateRow(1, intKey(9)), std::logic_error);
    CHECK_THROWS_AS(fixture.backend.selectRows(3, {intKey(1)}), std::logic_error);  // a Text
    CHECK_THROWS_AS(fixture.backend.activateRow(2, intKey(1)), std::logic_error);   // a Row
    CHECK(fixture.activated.empty());
    CHECK(fixture.selection.peek().empty());

    // A table without selection still activates.
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    std::vector<ui::Key> activated;
    ui::Mounted const plain{
        runtime, backend,
        ui::table<Item>({{.label = "Name"}}, [] { return fruit(); }, keyOf, nameCell,
                        {.onActivate = [&activated](ui::Key key) { activated.push_back(std::move(key)); }})};
    CHECK_THROWS_AS(backend.selectRows(1, {intKey(1)}), std::logic_error);
    backend.activateRow(1, intKey(2));
    CHECK(activated == std::vector<ui::Key>{intKey(2)});
}

TEST_CASE("ui::table: a hidden or disabled table takes no selection and no activation", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<bool> shown{runtime, false};
    Signal<bool> enabled{runtime, true};
    int calls = 0;
    ui::Mounted const mounted{runtime, backend,
                              ui::table<Item>({{.label = "Name"}}, [] { return fruit(); }, keyOf, nameCell,
                                              {.selectionMode = ui::SelectionMode::Single,
                                               .onSelectionChange = [&calls](std::vector<ui::Key> const&) { ++calls; },
                                               .onActivate = [&calls](ui::Key const&) { ++calls; },
                                               .common = {.visible = [&shown] { return shown.get(); },
                                                          .enabled = [&enabled] { return enabled.get(); }}})};
    backend.selectRows(1, {intKey(1)});
    backend.activateRow(1, intKey(1));
    CHECK(calls == 0);
    CHECK(backend.prop(1, "selection") == "[]");

    shown.set(true);
    enabled.set(false);
    owner.runAll();
    backend.selectRows(1, {intKey(1)});
    backend.activateRow(1, intKey(1));
    CHECK(calls == 0);

    enabled.set(true);
    owner.runAll();
    backend.selectRows(1, {intKey(1)});
    backend.activateRow(1, intKey(1));
    CHECK(calls == 2);
}

TEST_CASE("ui::table: a callback that throws is reported and goes no further", "[ui]") {
    Owner owner;
    Probe const probe{owner.coreExecutor()};
    Runtime runtime{owner};
    RecordingBackend backend;
    ui::Mounted const mounted{
        runtime, backend,
        ui::table<Item>(
            {{.label = "Name"}}, [] { return fruit(); }, keyOf, nameCell,
            {.selectionMode = ui::SelectionMode::Single,
             .onSelectionChange = [](std::vector<ui::Key> const&) { throw std::runtime_error{"selection failed"}; },
             .onActivate = [](ui::Key const&) { throw std::runtime_error{"activation failed"}; }})};
    CHECK_NOTHROW(backend.selectRows(1, {intKey(2)}));
    CHECK_NOTHROW(backend.activateRow(1, intKey(2)));
    CHECK(probe.count(ui::detail::site::kCallbackThrew) == 2);
}

TEST_CASE("ui::table: activating a row may remove it, and a selection may empty the table", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    Signal<std::vector<Item>> items{runtime, fruit()};
    // Larger than std::function's inline buffer, so each handler's closure lives on the heap, where ASan watches it.
    std::string const note(64, 'n');
    bool rowAliveAfterWrite = false;
    std::string seenAfter;
    ui::Mounted const mounted{
        runtime, backend,
        ui::table<Item>({{.label = "Name"}}, [&items] { return items.get(); }, keyOf, nameCell,
                        {.selectionMode = ui::SelectionMode::Single,
                         .onSelectionChange =
                             [&items, &seenAfter, note = note](std::vector<ui::Key> const&) {
                                 items.set({});
                                 seenAfter = note + "cleared";
                             },
                         .onActivate =
                             [&items, &backend, &rowAliveAfterWrite, &seenAfter, note = note](ui::Key const& key) {
                                 removeItem(items, std::get<std::int64_t>(key));
                                 rowAliveAfterWrite = backend.exists(4);
                                 seenAfter = note + "removed";
                             }})};
    std::size_t const mountedNodes = runtime.core()->liveNodes();

    backend.clearLog();
    backend.activateRow(1, intKey(2));  // Row#4, with Text#5
    CHECK(rowAliveAfterWrite);
    CHECK(seenAfter == note + "removed");
    owner.runAll();
    CHECK(backend.log() == Lines{"destroy Text#5", "destroy Row#4"});
    // The row's signal, and its text's Computed and Effect, are gone with it.
    CHECK(runtime.core()->liveNodes() == mountedNodes - 3);

    backend.selectRows(1, {intKey(3)});
    CHECK(seenAfter == note + "cleared");
    owner.runAll();
    CHECK(backend.dump() == "Table#1 columns=[Name:content] selection=[] selectionMode=Single\n");
}

TEST_CASE("ui::table: a callback may destroy the whole view, its table included", "[ui]") {
    Owner owner;
    Runtime runtime{owner};
    RecordingBackend backend;
    std::size_t const before = runtime.core()->liveNodes();
    std::string const note(64, 'n');
    std::unique_ptr<ui::Mounted> view;
    std::string seenAfter;
    auto const build = [&] {
        view = std::make_unique<ui::Mounted>(
            runtime, backend,
            ui::table<Item>({{.label = "Name"}}, [] { return fruit(); }, keyOf, nameCell,
                            {.selectionMode = ui::SelectionMode::Single,
                             .onSelectionChange =
                                 [&view, &seenAfter, note = note](std::vector<ui::Key> const& keys) {
                                     view.reset();
                                     seenAfter = note + std::to_string(keys.size());
                                 },
                             .onActivate =
                                 [&view, &seenAfter, note = note](ui::Key const& key) {
                                     view.reset();
                                     seenAfter = note + std::to_string(std::get<std::int64_t>(key));
                                 }}));
    };

    build();
    int const table = backend.idOf(view->root());
    backend.activateRow(table, intKey(2));
    CHECK(view == nullptr);
    CHECK_FALSE(backend.exists(table));
    CHECK(seenAfter == note + "2");
    CHECK(runtime.core()->liveNodes() == before);

    build();
    int const second = backend.idOf(view->root());
    backend.selectRows(second, {intKey(1)});
    CHECK(view == nullptr);
    CHECK_FALSE(backend.exists(second));
    CHECK(seenAfter == note + "1");
    CHECK(runtime.core()->liveNodes() == before);
}
