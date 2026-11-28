// SPDX-License-Identifier: Apache-2.0

// What a document binds a model table to, and what the table says when the bound value is no good: a filter that is
// not one, a sort over a column the table lacks, a layout, a selection and a persist name.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <memory>
#include <string>
#include <string_view>

#include "ui_table_support.hpp"

namespace {

using morph::ui::testing::elementValue;
using morph::ui::testing::kModelTable;
using morph::ui::testing::replyDescribe;
using morph::ui::testing::reportsOf;
using morph::ui::testing::TableHarness;
using morph::ui::testing::withTableKey;

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a test helper; the names say which text is which.
std::string bound(std::string_view state, std::string_view keys) {
    std::string text = withTableKey(kModelTable, keys);
    text.insert(text.find(R"("models":)"), R"("state":{)" + std::string{state} + "},");
    return text;
}

struct Binding {
    std::string_view name;
    std::string_view state;
    std::string_view keys;
    std::string_view report;
};

}  // namespace

TEST_CASE("covg model table binds: a bound filter that is no filter is said", "[covg][ui][table]") {
    auto const& c = GENERATE(values<Binding>({
        {"a filter that is a number", R"("f":{"init":5})", R"("filters":{"ref":"f"})", "`filters` is not a filter"},
        {"a filter that is null", R"("f":{"init":null})", R"("filters":{"ref":"f"})", ""},
    }));
    CAPTURE(c.name);
    TableHarness h;
    auto const screen = h.mount(bound(c.state, c.keys));
    replyDescribe(h);
    std::string joined;
    for (auto const& report : reportsOf(h)) {
        joined += report + "|";
    }
    if (c.report.empty()) {
        CHECK(joined.empty());
    } else {
        CHECK(joined.contains(c.report));
    }
}

TEST_CASE("covg model table binds: a layout and a literal sort that the document gives are the table's",
          "[covg][ui][table]") {
    TableHarness h;
    auto const screen =
        h.mount(bound(R"("lay":{"init":{"lit":{"order":["name","id","c3"],"hidden":["c3"],"pinned":0}}})",
                      R"("layout":{"ref":"lay"},)"
                      R"("sort":{"lit":[{"column":"name","direction":"desc"}]})"));
    replyDescribe(h);
    CHECK(elementValue(*screen, "list", "visibleColumns") == R"(["name","id"])");
    CHECK(elementValue(*screen, "list", "sort") == R"([{"column":"name","direction":"desc"}])");
}
