// SPDX-License-Identifier: Apache-2.0

// The filter menu's dialog over a mounted table: the date pickers of a condition, the join between two conditions,
// the group match line and the controls a column of custom values does not get.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <morph/table/description.hpp>
#include <morph/util/datetime.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ui_table_model_support.hpp"
#include "ui_table_support.hpp"

namespace {

namespace table = morph::table;
using namespace table_model_support;
using namespace std::chrono_literals;
using morph::ui::testing::clickFilter;
using morph::ui::testing::clickGroupFilter;
using morph::ui::testing::readJson;
using morph::ui::testing::TableHarness;

morph::time::Timestamp instant(std::int64_t days, std::int64_t seconds) {
    return morph::time::Timestamp{morph::time::DateTime{
        std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::sys_days{std::chrono::days{days}}} +
        std::chrono::seconds{seconds}}};
}

void pick(TableHarness& h, std::string_view testId, std::optional<morph::time::Timestamp> value) {
    h.backend.setDateTime(h.byTestId("DateTimeInput", testId), value);
    h.settle();
}

table::TableDescription withDateTime() {
    table::TableDescription d = kindsDescription();
    for (auto& column : d.columns) {
        if (column.info.id == "d") {
            column.info.kind = table::ColumnKind::DateTime;
        }
    }
    return table::describe(std::move(d));
}
}  // namespace

TEST_CASE("cov filter dialog: a date condition takes the day a picker names", "[ui][table][config][cov]") {
    TableHarness h;
    auto const screen = mountDescribed(h, kindsDescription());
    int const table = h.backend.all("Table").front();
    clickFilter(h, table, 4);
    chooseOption(h, "table_filter_op_1", "between");
    pick(h, "table_filter_operand_1", instant(20'000, 0));
    h.advance(250ms);
    CHECK(readJson(*screen, R"({"ref":"t.filters.columns.d"})") == "null");  // no upper end yet
    pick(h, "table_filter_second_1", instant(20'010, 0));
    h.advance(250ms);
    CHECK(readJson(*screen, R"({"ref":"t.filters.columns.d.include"})") ==
          R"([{"between":["2024-10-04","2024-10-14"]}])");
    // The picker shows what the operand holds, and clearing it empties the operand.
    CHECK(propOf(h, "DateTimeInput", "table_filter_operand_1", "value").contains("2024"));
    pick(h, "table_filter_operand_1", std::nullopt);
    h.advance(250ms);
    CHECK(propOf(h, "DateTimeInput", "table_filter_operand_1", "value") !=
          propOf(h, "DateTimeInput", "table_filter_second_1", "value"));
}

TEST_CASE("cov filter dialog: a date-time condition keeps the time of day", "[ui][table][config][cov]") {
    TableHarness h;
    auto const screen = mountDescribed(h, withDateTime());
    int const table = h.backend.all("Table").front();
    clickFilter(h, table, 4);
    CHECK(propOf(h, "DateTimeInput", "table_filter_operand_1", "mode") == "DateTime");
    chooseOption(h, "table_filter_op_1", ">");
    pick(h, "table_filter_operand_1", instant(20'000, (8 * 3600) + (30 * 60) + 15));
    h.advance(250ms);
    CHECK(readJson(*screen, R"({"ref":"t.filters.columns.d.include"})") == R"([{"gt":"2024-10-04T08:30:15Z"}])");
    // A stored condition reopens in the picker with its time.
    dismissMenu(h);
    clickFilter(h, table, 4);
    CHECK(propOf(h, "DateTimeInput", "table_filter_operand_1", "value").contains("08:30"));
}

TEST_CASE("cov filter dialog: two conditions join by and or or", "[ui][table][config][cov]") {
    TableHarness h;
    auto const screen = mountDescribed(h, kindsDescription());
    int const table = h.backend.all("Table").front();
    clickFilter(h, table, 1);
    chooseOption(h, "table_filter_op_1", ">");
    typeInto(h, "table_filter_operand_1", "2");
    chooseOption(h, "table_filter_op_2", "<");
    typeInto(h, "table_filter_operand_2", "9");
    h.advance(250ms);
    CHECK(readJson(*screen, R"({"ref":"t.filters.columns.n.combine"})") == R"("any")");
    chooseOption(h, "table_filter_combine", "and");
    h.advance(250ms);
    CHECK(readJson(*screen, R"({"ref":"t.filters.columns.n.combine"})") == R"("all")");
    CHECK(propOf(h, "Select", "table_filter_combine", "selected").contains("and"));
    chooseOption(h, "table_filter_combine", "or");
    h.advance(250ms);
    CHECK(readJson(*screen, R"({"ref":"t.filters.columns.n.combine"})") == R"("any")");
}

TEST_CASE("cov filter dialog: a column of custom values offers conditions only", "[ui][table][config][cov]") {
    TableHarness h;
    auto const screen = mountDescribed(h, kindsDescription());
    int const table = h.backend.all("Table").front();
    clickFilter(h, table, 3);
    CHECK_FALSE(mounted(h, "TextInput", "table_filter_search"));
    CHECK_FALSE(mounted(h, "Checkbox", "table_filter_all"));
    CHECK_FALSE(mounted(h, "Button", "table_filter_more"));
    chooseOption(h, "table_filter_op_1", "is not empty");
    h.advance(250ms);
    CHECK(readJson(*screen, R"({"ref":"t.filters.columns.code.include"})") == R"([{"notEmpty":true}])");
    // The operand inputs stay hidden for an operator that takes none.
    CHECK(propOf(h, "TextInput", "table_filter_operand_1", "visible") == "false");
}

TEST_CASE("cov filter dialog: a measure's unit starts unchosen and each choice is typed in",
          "[ui][table][config][cov]") {
    TableHarness h;
    auto const screen = mountDescribed(h, kindsDescription());
    int const table = h.backend.all("Table").front();
    clickFilter(h, table, 6);
    CHECK(propOf(h, "Select", "table_filter_unit_1", "selected") != "");
    chooseOption(h, "table_filter_op_1", "=");
    typeInto(h, "table_filter_operand_1", "2");
    chooseOption(h, "table_filter_unit_1", "kg");
    h.advance(250ms);
    CHECK(readJson(*screen, R"({"ref":"t.filters.columns.weight.include"})") == R"([{"eq":"2 kg"}])");
}

TEST_CASE("cov filter dialog: a group's match line offers every quantifier and shows N for two of them",
          "[ui][table][config][cov]") {
    constexpr std::string_view kCompanies = R"({
      "vocab": "ui/1", "id": "companies",
      "state": { "items": { "init": { "lit": [
        { "id": 1, "a": "F", "b": "S" }, { "id": 2, "a": "F", "b": "F" }, { "id": 3, "a": "S", "b": "F" } ] } } },
      "view": { "kind": "table", "id": "t", "rows": { "ref": "items" },
                "columns": [ { "id": "a" }, { "id": "b" } ],
                "groups": [ { "id": "pair", "label": "Pair", "columns": { "lit": ["a", "b"] } } ] } })";
    TableHarness h;
    auto const screen = h.mount(kCompanies);
    int const table = h.backend.all("Table").front();
    clickGroupFilter(h, table, 0);
    CHECK(propOf(h, "TextInput", "table_filter_count", "visible") == "false");
    toggleBox(h, "table_filter_value_0");
    chooseOption(h, "table_filter_match", "in every column");
    CHECK(propOf(h, "TextInput", "table_filter_count", "visible") == "false");
    chooseOption(h, "table_filter_match", "in exactly N columns");
    CHECK(propOf(h, "TextInput", "table_filter_count", "visible") == "true");
    typeInto(h, "table_filter_count", "1");
    pressButton(h, "table_filter_apply");
    CHECK(readJson(*screen, R"({"ref":"t.filters.groups"})").contains(R"("match":{"exactly":1})"));
    clickGroupFilter(h, table, 0);
    chooseOption(h, "table_filter_match", "in at least N columns");
    CHECK(propOf(h, "TextInput", "table_filter_count", "visible") == "true");
    chooseOption(h, "table_filter_match", "in any column");
    CHECK(propOf(h, "TextInput", "table_filter_count", "visible") == "false");
}
