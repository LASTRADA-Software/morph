// SPDX-License-Identifier: Apache-2.0
//
// Row edits held for a table: what a fetch leaves of a cell that has an edit in flight, which columns survive a
// change of columns, and how a settled flight is found among the rows'.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <expected>
#include <morph/table/edits.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "table/row_commits.hpp"

namespace table = morph::table;
using morph::test::RowCommits;

namespace {

table::RowId const k1{std::int64_t{1}};
table::RowId const k2{std::int64_t{2}};
table::RowId const k3{std::int64_t{3}};
using Ok = std::expected<void, std::vector<table::FieldError>>;

}  // namespace

TEST_CASE("a fetch leaves a modified cell alone while a later edit of it is in flight", "[covf][table][edits]") {
    RowCommits c;
    table::CellEdits edits{c.commit()};
    edits.commitRow({.row = k1, .cells = {{.column = "a", .json = "1"}}});
    c.done.at(0)(Ok{});
    REQUIRE(edits.modified(k1, "a"));
    edits.commitRow({.row = k1, .cells = {{.column = "a", .json = "2"}}});
    REQUIRE(edits.stale(k1, "a"));
    edits.rowFetched(k1);
    CHECK(edits.modified(k1, "a"));
    c.done.at(1)(Ok{});
    edits.rowFetched(k1);
    CHECK_FALSE(edits.modified(k1, "a"));
}

TEST_CASE("a fetch clears the draft of a cell whose settled edit it shows", "[covf][table][edits]") {
    RowCommits c;
    table::CellEdits edits{c.commit()};
    edits.setDraft(k1, "a", "1", std::nullopt);
    edits.commitRow({.row = k1, .cells = {{.column = "a", .json = "1"}}});
    c.done.at(0)(Ok{});
    CHECK(edits.draft(k1, "a") == "1");
    edits.rowFetched(k1);
    CHECK_FALSE(edits.draft(k1, "a"));
    CHECK_FALSE(edits.rowEdited(k1));
    CHECK(edits.modifiedCount() == 0);
}

TEST_CASE("a fetch keeps the draft typed after the edit that settled", "[covf][table][edits]") {
    RowCommits c;
    table::CellEdits edits{c.commit()};
    edits.setDraft(k1, "a", "1", std::nullopt);
    edits.commitRow({.row = k1, .cells = {{.column = "a", .json = "1"}}});
    edits.setDraft(k1, "a", "12", std::nullopt);
    c.done.at(0)(Ok{});
    edits.rowFetched(k1);
    CHECK(edits.draft(k1, "a") == "12");
    CHECK_FALSE(edits.modified(k1, "a"));
}

TEST_CASE("changing the columns drops a row whose every column is gone and keeps the others", "[covf][table][edits]") {
    RowCommits c;
    table::CellEdits edits{c.commit()};
    edits.setDraft(k1, "a", "1", "bad");
    edits.setDraft(k2, "b", "2", "worse");
    std::vector<std::string> const kept{"b"};
    edits.keepColumns(kept);
    CHECK_FALSE(edits.draft(k1, "a"));
    CHECK_FALSE(edits.error(k1, "a"));
    CHECK_FALSE(edits.rowEdited(k1));
    CHECK(edits.draft(k2, "b") == "2");
    CHECK(edits.error(k2, "b") == "worse");
    CHECK(edits.errorCount() == 1);
}

TEST_CASE("a flight is settled among those of other rows, and once", "[covf][table][edits]") {
    RowCommits c;
    table::CellEdits edits{c.commit()};
    std::vector<std::pair<table::RowId, bool>> heard;
    edits.onRowSettled([&heard](table::RowId const& row, bool refused) { heard.emplace_back(row, refused); });
    edits.commitRow({.row = k1, .cells = {{.column = "a", .json = "1"}}});
    edits.commitRow({.row = k2, .cells = {{.column = "a", .json = "2"}}});
    edits.commitRow({.row = k3, .cells = {{.column = "a", .json = "3"}}});
    REQUIRE(c.done.size() == 3);
    c.done.at(2)(std::unexpected(std::vector<table::FieldError>{{.column = "a", .message = "no"}}));
    REQUIRE(heard.size() == 1);
    CHECK(heard[0].first == k3);
    CHECK(heard[0].second);
    CHECK(edits.error(k3, "a") == "no");
    c.done.at(1)(Ok{});
    REQUIRE(heard.size() == 2);
    CHECK(heard[1].first == k2);
    CHECK_FALSE(heard[1].second);
    // A completion called again finds no flight and changes nothing.
    c.done.at(1)(std::unexpected(std::vector<table::FieldError>{{.column = "a", .message = "late"}}));
    CHECK(heard.size() == 2);
    CHECK_FALSE(edits.error(k2, "a"));
}

TEST_CASE("a row moved while its edit is in flight settles under its new key", "[covf][table][edits]") {
    RowCommits c;
    table::CellEdits edits{c.commit()};
    std::vector<table::RowId> heard;
    edits.onRowSettled([&heard](table::RowId const& row, bool) { heard.push_back(row); });
    edits.setDraft(k1, "a", "1", "bad");
    edits.commitRow({.row = k1, .cells = {{.column = "a", .json = "1"}}});
    edits.moveRow(k1, k2);
    CHECK(edits.draft(k2, "a") == "1");
    CHECK(edits.error(k2, "a") == "bad");
    CHECK_FALSE(edits.draft(k1, "a"));
    CHECK(edits.stale(k2, "a"));
    c.done.at(0)(Ok{});
    REQUIRE(heard.size() == 1);
    CHECK(heard[0] == k2);
    CHECK(edits.modified(k2, "a"));
}
