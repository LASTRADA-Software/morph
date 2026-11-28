// SPDX-License-Identifier: Apache-2.0

// A table over rows that keeps its arrangement in a store: what a stored state that fits, one that names what the
// table lacks, one that is not a state and a store that fails each leave of the table and what they report.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ui_table_support.hpp"

namespace {

using morph::ui::testing::GatedTableStateStore;
using morph::ui::testing::storeExecutor;
using morph::ui::testing::TableHarness;

constexpr std::string_view kRowsTable = R"({"vocab":"ui/1","id":"d",
  "state":{"items":{"init":{"lit":[{"id":1,"a":"x","b":"p"},{"id":2,"a":"y","b":"q"},{"id":3,"a":"x","b":"r"}]}}},
  "view":{"kind":"table","id":"t","testId":"t","rows":{"ref":"items"},"key":{"row":"id"},
    "columns":[{"id":"a","label":"A","sortable":true},{"id":"b","label":"B","sortable":true}]}})";

struct Mounted {
    std::shared_ptr<GatedTableStateStore> store;
    TableHarness h;
    std::unique_ptr<morph::ui::Screen> screen;

    explicit Mounted(std::string stored, bool fail = false)
        : store{std::make_shared<GatedTableStateStore>(storeExecutor(), std::move(stored))}, h{store} {
        screen = h.mount(kRowsTable);
        if (fail) {
            store->fail();
        } else {
            store->release();
        }
        h.settle();
    }

    [[nodiscard]] bool reported(std::string_view text) const {
        for (std::string const& line : morph::ui::testing::reportsOf(h)) {
            if (line.contains(text)) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] std::string order() const { return morph::ui::testing::rowOrder(h); }
};

}  // namespace

TEST_CASE("covd rows state: a stored state that fits arranges the table", "[ui][table][state][covd]") {
    struct Case {
        std::string_view stored;
        std::string_view order;
        std::string_view visible;
    };
    auto const [stored, order, visible] =
        GENERATE(Case{R"({"version":1,"sort":[{"column":"b","direction":"desc"}]})", "3 2 1", R"(["a","b"])"},
                 Case{R"({"version":1,"filters":{"columns":{"a":{"include":[{"eq":"x"}]}}}})", "1 3", R"(["a","b"])"},
                 Case{R"({"version":1,"layout":{"order":["b","a"],"hidden":[],"pinned":0,"widths":{}}})", "1 2 3",
                      R"(["b","a"])"},
                 Case{R"({"version":1,"layout":{"order":["a","b"],"hidden":["a"],"pinned":0,"widths":{}}})", "1 2 3",
                      R"(["b"])"},
                 Case{R"({"version":1,"view":"mine"})", "1 2 3", R"(["a","b"])"}, Case{"", "1 2 3", R"(["a","b"])"});
    CAPTURE(stored);
    Mounted const m{std::string{stored}};
    CHECK(m.order() == order);
    CHECK(morph::ui::testing::elementValue(m.h, "t", "visibleColumns") == morph::ui::testing::value(visible));
    CHECK_FALSE(m.reported("could not be loaded"));
    CHECK_FALSE(m.reported("is not used"));
}

TEST_CASE(
    "covd rows state: a stored state that is not a state, and a store that fails, are reported and leave the table as "
    "it is",
    "[ui][table][state][covd]") {
    {
        Mounted const m{"{not json"};
        CHECK(m.reported("the stored state of table `t` is not used"));
        CHECK(m.order() == "1 2 3");
    }
    {
        Mounted const m{"", true};
        CHECK(m.reported("the stored state of table `t` could not be loaded"));
        CHECK(m.order() == "1 2 3");
    }
}

TEST_CASE("covd rows state: a stored sort and filter on a column the table lacks are dropped",
          "[ui][table][state][covd]") {
    Mounted const m{R"({"version":1,"sort":[{"column":"gone","direction":"asc"}],)"
                    R"("filters":{"columns":{"gone":{"include":[{"eq":"x"}]}}}})"};
    CHECK(m.order() == "1 2 3");
}
