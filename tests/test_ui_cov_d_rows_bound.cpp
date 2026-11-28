// SPDX-License-Identifier: Apache-2.0

// A table over rows whose sort and selection a document binds: the keys of a sort that name a sortable column order
// the rows, every other shape is left out, and a selection that is no list is reported once.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <string>
#include <string_view>

#include "ui_table_support.hpp"

namespace {

using morph::ui::testing::TableHarness;

constexpr std::string_view kBound = R"({"vocab":"ui/1","id":"d",
  "state":{"items":{"init":{"lit":[{"id":1,"a":"b"},{"id":2,"a":"c"},{"id":3,"a":"a"}]}},
           "srt":{"init":null},"sel":{"init":null}},
  "view":{"kind":"column","children":[
    {"kind":"table","id":"t","testId":"t","rows":{"ref":"items"},"key":{"row":"id"},"selectionMode":"multiple",
     "sort":{"ref":"srt"},"selection":{"ref":"sel"},
     "onSelectionChange":[{"set":"sel","value":{"ref":"event"}}],
     "columns":[{"id":"id","label":"Id"},{"id":"a","label":"A","sortable":true}]},
    {"kind":"button","testId":"go","label":"go",
     "onClick":[{"set":"srt","value":SORT},{"set":"sel","value":SELECTION}]}]}})";

struct Mounted {
    TableHarness h;
    std::unique_ptr<morph::ui::Screen> screen;

    Mounted(std::string_view sort, std::string_view selection) {
        std::string text{kBound};
        text.replace(text.find("SORT"), 4, sort);
        text.replace(text.find("SELECTION"), 9, selection);
        screen = h.mount(text);
        h.backend.click(h.byTestId("Button", "go"));
        h.settle();
    }

    [[nodiscard]] bool reported(std::string_view text) const {
        auto const lines = morph::ui::testing::reportsOf(h);
        return std::ranges::any_of(lines, [text](std::string const& line) { return line.contains(text); });
    }
};

}  // namespace

TEST_CASE(
    "covd rows bound: the sort a document holds orders the rows when every key names a column and a direction, and is "
    "reported otherwise",
    "[ui][table][covd]") {
    struct Case {
        std::string_view sort;
        std::string_view order;
        bool reported;
    };
    auto const [sort, order, reported] = GENERATE(
        Case{R"({"lit":[{"column":"a","direction":"asc"}]})", "3 1 2", false},
        Case{R"({"lit":[{"column":"a","direction":"desc"}]})", "2 1 3", false},
        Case{R"({"lit":[{"column":"a"}]})", "1 2 3", true},
        Case{R"({"lit":[{"column":"a","direction":7}]})", "1 2 3", true},
        Case{R"({"lit":[{"direction":"desc"}]})", "1 2 3", true}, Case{R"({"lit":[{"column":5}]})", "1 2 3", true},
        Case{R"({"lit":[{"column":"id","direction":"asc"}]})", "1 2 3", true}, Case{R"({"lit":"a"})", "1 2 3", true},
        Case{R"({"lit":null})", "1 2 3", false}, Case{R"({"lit":[]})", "1 2 3", false});
    CAPTURE(sort);
    Mounted const m{sort, R"({"lit":[]})"};
    CHECK(morph::ui::testing::rowOrder(m.h) == order);
    CHECK(m.reported("a table's sort is a list of") == reported);
}

TEST_CASE("covd rows bound: a selection the document holds is a list of keys, and anything else is reported once",
          "[ui][table][covd]") {
    struct Case {
        std::string_view selection;
        std::string_view value;
        bool reported;
    };
    auto const [selection, value, reported] = GENERATE(
        Case{R"({"lit":[1,3]})", "[1,3]", false}, Case{R"({"lit":[]})", "[]", false},
        Case{R"({"lit":null})", "[]", false}, Case{R"({"lit":7})", "[]", true}, Case{R"({"lit":"a"})", "[]", true});
    CAPTURE(selection);
    Mounted const m{R"({"lit":[]})", selection};
    CHECK(morph::ui::testing::elementValue(m.h, "t", "selection") == morph::ui::testing::value(value));
    CHECK(m.reported("a table's selection is a list of keys") == reported);
}

TEST_CASE("covd rows bound: a user's selection reaches the document's state", "[ui][table][covd]") {
    Mounted m{R"({"lit":[]})", R"({"lit":[1]})"};
    int const table = m.h.byTestId("Table", "t");
    m.h.backend.selectRows(table, {morph::ui::Key{std::int64_t{2}}, morph::ui::Key{std::int64_t{3}}});
    m.h.settle();
    CHECK(morph::ui::testing::elementValue(m.h, "t", "selection") == morph::ui::testing::value("[2,3]"));
}
