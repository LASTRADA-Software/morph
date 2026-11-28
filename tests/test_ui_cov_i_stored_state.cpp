// SPDX-License-Identifier: Apache-2.0

// A table's stored state that arrives after the rows were let through: applied when the user has not touched the
// table meanwhile, and not otherwise, for a table over `rows` and for a collection's.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ui_table_model_support.hpp"
#include "ui_table_support.hpp"

namespace {

using namespace std::chrono_literals;
using morph::ui::testing::elementValue;
using morph::ui::testing::GatedTableStateStore;
using morph::ui::testing::kMixedSamplesView;
using morph::ui::testing::kSamplesCollection;
using morph::ui::testing::rowKeys;
using morph::ui::testing::storeExecutor;
using morph::ui::testing::TableHarness;
using table_model_support::kFourSamples;

constexpr std::string_view kRowsTable = R"({"vocab":"ui/1","id":"d",
  "state":{"items":{"init":{"lit":[{"id":1,"a":"x","b":"p"},{"id":2,"a":"y","b":"q"},{"id":3,"a":"x","b":"r"}]}}},
  "view":{"kind":"table","id":"t","testId":"t","rows":{"ref":"items"},"key":{"row":"id"},
    "columns":[{"id":"a","label":"A","sortable":true},{"id":"b","label":"B","sortable":true}]}})";

constexpr std::string_view kSortedByBDescending = R"({"version":1,"sort":[{"column":"b","direction":"desc"}]})";

}  // namespace

TEST_CASE("covi stored state: a table over rows takes a state that arrives late unless it was touched",
          "[covi][ui][table][state]") {
    auto const store = std::make_shared<GatedTableStateStore>(storeExecutor(), std::string{kSortedByBDescending});
    TableHarness h{store};
    auto const screen = h.mount(kRowsTable);
    h.advance(1s);  // past the wait for the stored state: the rows come through as they are
    int const table = h.byTestId("Table", "t");
    REQUIRE(rowKeys(h, table) == std::vector<std::string>{"1", "2", "3"});
    SECTION("untouched") {
        store->release();
        h.settle();
        CHECK(rowKeys(h, table) == std::vector<std::string>{"3", "2", "1"});
        CHECK(elementValue(*screen, "t", "sort") == R"([{"column":"b","direction":"desc"}])");
    }
    SECTION("touched") {
        h.backend.clickHeader(table, 0);
        h.settle();
        REQUIRE(elementValue(*screen, "t", "sort") == R"([{"column":"a","direction":"asc"}])");
        store->release();
        h.settle();
        CHECK(elementValue(*screen, "t", "sort") == R"([{"column":"a","direction":"asc"}])");
    }
}

TEST_CASE("covi stored state: a table over rows takes a late state's layout, filters and view",
          "[covi][ui][table][state]") {
    auto const store = std::make_shared<GatedTableStateStore>(
        storeExecutor(), R"({"version":1,"filters":{"columns":{"a":{"include":[{"eq":"x"}]}}},)"
                         R"("layout":{"order":["b","a"],"hidden":[],"pinned":0,"widths":{}},"view":"mine"})");
    TableHarness h{store};
    auto const screen = h.mount(kRowsTable);
    h.advance(1s);
    int const table = h.byTestId("Table", "t");
    store->release();
    h.settle();
    CHECK(rowKeys(h, table) == std::vector<std::string>{"1", "3"});
    CHECK(elementValue(*screen, "t", "visibleColumns") == R"(["b","a"])");
}

TEST_CASE("covi stored state: a collection takes a state that arrives late unless it was touched",
          "[covi][ui][table][state]") {
    auto const store = std::make_shared<GatedTableStateStore>(
        storeExecutor(), R"({"version":1,"sort":[{"column":"name","direction":"desc"}],)"
                         R"("filters":{"columns":{"name":{"include":[{"contains":"a"}]}}},)"
                         R"("layout":{"order":["id","name","weight"],"hidden":["weight"],"pinned":0,"widths":{}},)"
                         R"("view":"mine"})");
    TableHarness h{store};
    h.extraViews.insert_or_assign("Lims_ListSamples", std::string{kMixedSamplesView});
    auto const screen = h.mount(kSamplesCollection);
    h.reply(h.dispatcher.last("Lims_ListSamples"), std::string{kFourSamples});
    h.advance(1s);
    int const collection = h.byTestId("Table", "collection_rows");
    REQUIRE(rowKeys(h, collection) == std::vector<std::string>{"1", "2", "3", "4"});
    SECTION("untouched") {
        store->release();
        h.settle();
        std::vector<std::string> const keys = rowKeys(h, collection);
        REQUIRE_FALSE(keys.empty());
        CHECK(keys.front() == "3");
        CHECK(keys.size() == 4);  // every name holds an `a`
        CHECK(h.backend.prop(collection, "sort") == "1:Descending");
    }
    SECTION("touched") {
        h.backend.clickHeader(collection, 1);
        h.settle();
        std::string const sorted = h.backend.prop(collection, "sort");
        REQUIRE(sorted == "1:Ascending");
        store->release();
        h.settle();
        CHECK(h.backend.prop(collection, "sort") == sorted);
    }
}

TEST_CASE("covi stored state: a collection whose store fails or holds no state leaves the rows as they come",
          "[covi][ui][table][state]") {
    SECTION("the load fails after the rows came") {
        auto const store = std::make_shared<GatedTableStateStore>(storeExecutor(), std::string{});
        TableHarness h{store};
        h.extraViews.insert_or_assign("Lims_ListSamples", std::string{kMixedSamplesView});
        auto const screen = h.mount(kSamplesCollection);
        h.reply(h.dispatcher.last("Lims_ListSamples"), std::string{kFourSamples});
        h.advance(1s);
        store->fail();
        h.settle();
        int const collection = h.byTestId("Table", "collection_rows");
        CHECK(rowKeys(h, collection) == std::vector<std::string>{"1", "2", "3", "4"});
        CHECK(morph::ui::testing::diagnosticsMentioning(h, "could not be loaded") == 1);
    }
    SECTION("nothing is stored") {
        auto const store = std::make_shared<GatedTableStateStore>(storeExecutor(), std::string{});
        TableHarness h{store};
        h.extraViews.insert_or_assign("Lims_ListSamples", std::string{kMixedSamplesView});
        auto const screen = h.mount(kSamplesCollection);
        h.reply(h.dispatcher.last("Lims_ListSamples"), std::string{kFourSamples});
        store->release();
        h.settle();
        int const collection = h.byTestId("Table", "collection_rows");
        CHECK(rowKeys(h, collection) == std::vector<std::string>{"1", "2", "3", "4"});
    }
}

TEST_CASE("covi stored state: a stored sort over many rows holds the rows back until the engine has ordered them",
          "[covi][ui][table][state]") {
    auto const store = std::make_shared<GatedTableStateStore>(
        storeExecutor(), R"({"version":1,"sort":[{"column":"name","direction":"desc"}]})");
    TableHarness h{store};
    auto const screen = h.mount(morph::ui::testing::rowsDoc(morph::ui::testing::idRows(6000)));
    int const table = h.byTestId("Table", "list");
    store->release();
    h.settle();
    for (int round = 0; round < 20 && rowKeys(h, table).empty(); ++round) {
        h.advance(20ms);
    }
    std::vector<std::string> const keys = rowKeys(h, table);
    INFO("children " << h.backend.children(table).size());
    REQUIRE_FALSE(keys.empty());
    CHECK(keys.front() == "999");  // `n999` is the greatest name of the 6000
    CHECK(elementValue(*screen, "list", "sort") == R"([{"column":"name","direction":"desc"}])");
}
