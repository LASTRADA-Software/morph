// SPDX-License-Identifier: Apache-2.0

// A model table's column parts: the properties a part sets over the description's, what a tone or tooltip of its own
// shows, and the reports a property that does not fit is given.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "ui_table_model_support.hpp"
#include "ui_table_support.hpp"

namespace {

using namespace table_model_support;
using morph::ui::testing::cellAccent;
using morph::ui::testing::describeJson;
using morph::ui::testing::replyDescribe;
using morph::ui::testing::replyPage;
using morph::ui::testing::TableHarness;

constexpr std::string_view kColumns =
    R"({"kind":"table","id":"t","testId":"t","model":"sample","table":"samples",
        "selectColumn":true,"selectionMode":"multiple","selectAllLabel":{"lit":"Everything"},"virtualizeOverscan":{"lit":3},
        "columns":[
          {"id":"id","label":{"lit":"Ident"},"width":{"lit":{"fixed":8}},"role":{"lit":"muted"},"align":"end",
           "valign":"start","wrap":false},
          {"id":"mass","shrink":true,"tooltip":{"lit":"fixed tip"},"tone":{"lit":"ok"}},
          {"id":"name","role":{"fn":"format","args":["{0}","nonsense"]},"width":{"lit":{"stretch":2}},"shrink":true,
           "tooltip":{"row":"massTip"}},
          {"id":"count"}]})";
}  // namespace

TEST_CASE("cov model columns: a part's label, width, role and place win over the description's",
          "[ui][table][display][cov]") {
    TableHarness h;
    auto const screen = h.mount(doc(kColumns));
    replyDescribe(h, describeJson(companionDescription()));
    replyPage(h, companionPage(h));
    int const table = h.backend.all("Table").front();
    auto const columns = h.backend.tableColumns(table);
    REQUIRE(columns.size() == 4);
    CHECK(columns.at(0).label == "Ident");
    CHECK(columns.at(0).role == morph::ui::TextRole::Muted);
    CHECK(columns.at(0).align == morph::ui::Align::End);
    CHECK(columns.at(0).valign == morph::ui::Align::Start);
    CHECK_FALSE(columns.at(0).wrap);
    CHECK(columns.at(0).width.kind == morph::ui::Sizing::Kind::Fixed);
    // A part with no label keeps the description's.
    CHECK(columns.at(1).label == "Mass");
    // `shrink` takes effect only on a column that stretches.
    CHECK_FALSE(columns.at(1).shrink);  // this one is sized by its content
    CHECK(columns.at(2).width.kind == morph::ui::Sizing::Kind::Stretch);
    CHECK(columns.at(2).shrink);  // this one stretches
    // A role that is no name is reported and drawn as normal text.
    CHECK(columns.at(2).role == morph::ui::TextRole::Normal);
    bool reported = false;
    for (auto const& report : h.diagnostics) {
        reported = reported || report.reason.contains("role is");
    }
    CHECK(reported);
}

TEST_CASE("cov model columns: a column's own tone and tooltip replace the companions", "[ui][table][display][cov]") {
    TableHarness h;
    auto const screen = h.mount(doc(kColumns));
    replyDescribe(h, describeJson(companionDescription()));
    replyPage(h, companionPage(h));
    auto const own = cellAccent(h, 1, "mass");
    CHECK(own.tone == morph::ui::Tone::Ok);
    CHECK(own.tooltip == "fixed tip");
    // A tooltip read from the row, over a column the description gives none.
    CHECK(cellAccent(h, 1, "name").tooltip == "Above limit");
    // A cell that cannot be read says so.
    CHECK(cellAccent(h, 1, "count").tooltip == "The value could not be read");
    CHECK_FALSE(cellAccent(h, 1, "id").tone.has_value());
}

TEST_CASE("cov model columns: a tooltip that is null or not text shows none", "[ui][table][display][cov]") {
    TableHarness h;
    auto const screen = h.mount(doc(
        R"({"kind":"table","id":"t","testId":"t","model":"sample","table":"samples",
            "columns":[{"id":"id"},{"id":"mass","tooltip":{"row":"missing"}},
                       {"id":"name","tooltip":{"row":"count"}}]})"));
    replyDescribe(h, describeJson(companionDescription()));
    replyPage(h, companionPage(h));
    CHECK(cellAccent(h, 1, "mass").tooltip.empty());
    CHECK(cellAccent(h, 1, "name").tooltip == "abc");  // `count` holds text here: a text tooltip shows as it is
}

TEST_CASE("cov model columns: unit and decimals on a measure column are reported and ignored",
          "[ui][table][display][cov]") {
    table::TableDescription d = companionDescription();
    d.columns.at(1).info.kind = table::ColumnKind::Measure;
    d.units = {table::UnitInfo{
        .id = "kg", .symbol = "kg", .dimension = "mass", .toBase = *table::parseDecimal("1"), .decimals = 3}};
    d = table::describe(std::move(d));
    TableHarness h;
    auto const screen = h.mount(doc(
        R"({"kind":"table","id":"t","testId":"t","model":"sample","table":"samples",
            "columns":[{"id":"id"},{"id":"mass","decimals":1,"unit":"kg"}]})"));
    replyDescribe(h, describeJson(d));
    bool reported = false;
    for (auto const& report : h.diagnostics) {
        reported = reported || report.reason.contains("is a measure column");
    }
    CHECK(reported);
    int const table = h.backend.all("Table").front();
    CHECK(h.backend.tableColumns(table).at(1).unit.empty());
    CHECK(h.backend.tableColumns(table).at(1).align == morph::ui::Align::End);
}

TEST_CASE("cov model columns: a column the description lacks, and a companion, are reported",
          "[ui][table][display][cov]") {
    TableHarness h;
    auto const screen = h.mount(doc(
        R"({"kind":"table","id":"t","testId":"t","model":"sample","table":"samples",
            "columns":[{"id":"id"},{"id":"nope"},{"id":"massTone"},{"id":"name"}]})"));
    replyDescribe(h, describeJson(companionDescription()));
    int const table = h.backend.all("Table").front();
    CHECK(h.backend.tableColumns(table).size() == 2);
    bool missing = false;
    bool companion = false;
    for (auto const& report : h.diagnostics) {
        missing = missing || report.reason.contains("has no column `nope`");
        companion = companion || report.reason.contains("is a companion column");
    }
    CHECK(missing);
    CHECK(companion);
}

TEST_CASE("cov model columns: a row's tone and label follow its record, and a locked row says why",
          "[ui][table][display][cov]") {
    TableHarness h;
    auto const screen = h.mount(doc(
        R"({"kind":"table","id":"t","testId":"t","model":"sample","table":"samples",
            "rowTone":{"row":"massTone"},"rowLabel":{"row":"name"},"showHeader":false})"));
    replyDescribe(h, describeJson(companionDescription()));
    CompanionRow locked = companionRow(1);
    locked.insert_or_assign("lockedBy", table::Cell{std::string{"Ann"}});
    replyPage(h, companionPage(h, {locked, companionRow(2)}));
    int const table = h.backend.all("Table").front();
    auto const first = h.backend.tableRow(table, morph::ui::Key{std::int64_t{1}});
    auto const second = h.backend.tableRow(table, morph::ui::Key{std::int64_t{2}});
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(h.backend.prop(table, "showHeader") != "true");
    auto const marks = h.backend.rowMarks(table, morph::ui::Key{std::int64_t{1}});
    REQUIRE(marks.has_value());
    CHECK(marks->locked);
    CHECK(marks->tooltip == "Ann");
    auto const open = h.backend.rowMarks(table, morph::ui::Key{std::int64_t{2}});
    REQUIRE(open.has_value());
    CHECK_FALSE(open->locked);
    CHECK(open->tooltip.empty());
}

TEST_CASE("cov model columns: bands over the description's columns draw, and a stray band is reported and dropped",
          "[ui][table][display][cov]") {
    TableHarness h;
    auto const screen = h.mount(doc(
        R"({"kind":"table","id":"t","testId":"t","model":"sample","table":"samples",
            "bands":[{"label":"Values","columns":{"lit":["mass","count"]}}]})"));
    replyDescribe(h, describeJson(companionDescription()));
    int const table = h.backend.all("Table").front();
    CHECK(h.backend.prop(table, "bands").contains("Values"));
    TableHarness stray;
    auto const strayScreen = stray.mount(doc(
        R"({"kind":"table","id":"t","testId":"t","model":"sample","table":"samples",
            "bands":[{"label":"Ghost","columns":{"lit":["ghost"]}}]})"));
    replyDescribe(stray, describeJson(companionDescription()));
    int const strayTable = stray.backend.all("Table").front();
    CHECK_FALSE(stray.backend.prop(strayTable, "bands").contains("Ghost"));
    bool reported = false;
    for (auto const& report : stray.diagnostics) {
        reported = reported || report.reason.contains("Ghost");
    }
    CHECK(reported);
}
