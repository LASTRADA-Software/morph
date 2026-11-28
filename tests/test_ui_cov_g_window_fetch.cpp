// SPDX-License-Identifier: Apache-2.0

// What a window reads of a reply to a fetch of rows by key: a reply that gives its rows no index is refused, a row
// shorter than the columns asked or a column the reply lacks leaves an empty cell, and a level the row has not loaded
// leaves the cell unloaded.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <expected>
#include <memory>
#include <morph/table/data_source.hpp>
#include <morph/ui/table_rows.hpp>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "ui_table_support.hpp"

namespace {

namespace table = morph::table;
namespace ui = morph::ui;
using morph::ui::testing::RowsFixture;
using morph::ui::testing::samplesDescription;
using morph::ui::testing::withLevel;

using Rows = std::expected<std::vector<ui::FetchedRow>, table::TableError>;

table::RowId id(std::int64_t key) { return table::RowId{key}; }

std::optional<Rows> fetched(RowsFixture& h, std::shared_ptr<ui::WindowRows> const& rows,
                            std::vector<std::string> columns, std::string const& page) {
    std::optional<Rows> got;
    rows->fetchRows({id(1)}, std::move(columns)).then([&got](Rows const& result) { got = result; });
    h.settle();
    h.replyPage(page);
    return got;
}

}  // namespace

TEST_CASE("covg window fetch: a keyed reply with no index for its rows is refused", "[covg][ui][table]") {
    RowsFixture h;
    auto const rows = h.windowRows("samples");
    auto const got = fetched(h, rows, {"id"},
                             R"({"revision":")" + h.described.revision +
                                 R"(","columns":["id"],"keys":[1],"rows":[[1]],"indices":[],"total":1})");
    REQUIRE(got.has_value());
    REQUIRE_FALSE(got->has_value());
    CHECK(got->error().message == "the keyed reply gives its rows no index");
}

TEST_CASE("covg window fetch: a column the reply lacks is an empty cell", "[covg][ui][table]") {
    RowsFixture h;
    auto const rows = h.windowRows("samples");
    auto const got = fetched(h, rows, {"id", "name", "c3"},
                             R"({"revision":")" + h.described.revision +
                                 R"(","columns":["id","name"],"keys":[1],"rows":[[1,"x"]],"indices":[0],"total":1})");
    REQUIRE(got.has_value());
    REQUIRE(got->has_value());
    REQUIRE((*got)->size() == 1);
    auto const& cells = (*got)->front().cells;
    REQUIRE(cells.size() == 3);
    CHECK(cells.at(0) == table::Cell{std::int64_t{1}});
    CHECK(cells.at(1) == table::Cell{std::string{"x"}});
    CHECK(std::holds_alternative<std::monostate>(cells.at(2)));  // the reply names no such column
}

TEST_CASE("covg window fetch: a level the row has not loaded leaves its cell unloaded", "[covg][ui][table]") {
    RowsFixture h;
    auto const rows = h.windowRows("samples", withLevel(samplesDescription(3), "name", "recipe"));
    auto const got = fetched(h, rows, {"id", "name"},
                             R"({"revision":")" + h.described.revision +
                                 R"(","columns":["id","name"],"keys":[1],"rows":[[1,"x"]],"indices":[0],)"
                                 R"("unloadedRows":[{"index":0,"levels":["recipe"]}],"total":1})");
    REQUIRE(got.has_value());
    REQUIRE(got->has_value());
    auto const& cells = (*got)->front().cells;
    REQUIRE(cells.size() == 2);
    CHECK(std::holds_alternative<table::Unloaded>(cells.at(1)));
    CHECK(cells.at(0) == table::Cell{std::int64_t{1}});
}

TEST_CASE("covg window fetch: a description of the same revision keeps the columns and a new one replaces them",
          "[covg][ui][table]") {
    RowsFixture h;
    auto const rows = h.windowRows("samples");
    REQUIRE(rows->description() != nullptr);
    CHECK(rows->description()->columns.size() == 3);
    rows->setDescription(samplesDescription(3));  // the same revision
    h.settle();
    CHECK(rows->description()->columns.size() == 3);
    rows->setDescription(samplesDescription(5));
    h.settle();
    CHECK(rows->description()->columns.size() == 5);
    CHECK(rows->description()->revision == samplesDescription(5).revision);
}

TEST_CASE("covg window fetch: rows that go while a fetch is asked answer it as gone", "[covg][ui][table]") {
    RowsFixture h;
    std::optional<Rows> got;
    {
        auto const rows = h.windowRows("samples");
        rows->fetchRows({id(1)}, {"id"}).then([&got](Rows const& result) { got = result; });
        h.settle();
        REQUIRE_FALSE(got.has_value());
    }
    h.settle();
    REQUIRE(got.has_value());
    REQUIRE_FALSE(got->has_value());
    CHECK(got->error().message == "the rows are gone");
}
