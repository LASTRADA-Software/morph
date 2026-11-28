// SPDX-License-Identifier: Apache-2.0
//
// Cell edits of a model's table: drafts, what a settled commit leaves behind, and what a row's disappearance or
// a key change does to the edits held for it.

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
using Ok = std::expected<void, std::vector<table::FieldError>>;

}  // namespace

TEST_CASE("clearing a draft keeps the cell a commit modified and drops the rest", "[covc][table][edits]") {
    RowCommits c;
    table::CellEdits edits{c.commit()};
    edits.setDraft(k1, "a", "x", "bad");
    edits.setDraft(k1, "b", "y", std::nullopt);
    edits.clearDraft(k1, "a");
    CHECK_FALSE(edits.draft(k1, "a"));
    CHECK_FALSE(edits.error(k1, "a"));
    CHECK(edits.draft(k1, "b") == "y");
    CHECK(edits.rowEdited(k1));
    edits.clearDraft(k1, "unknown");
    edits.clearDraft(k2, "a");
    CHECK(edits.rowEdited(k1));
    edits.clearDraft(k1, "b");
    CHECK_FALSE(edits.rowEdited(k1));

    edits.setDraft(k1, "a", "1", std::nullopt);
    edits.commitRow({.row = k1, .cells = {{.column = "a", .json = "1"}}});
    c.done.at(0)(Ok{});
    CHECK(edits.modified(k1, "a"));
    edits.clearDraft(k1, "a");
    CHECK_FALSE(edits.draft(k1, "a"));
    CHECK(edits.modified(k1, "a"));
    CHECK(edits.rowEdited(k1));
    CHECK(edits.modifiedCount() == 1);
}

TEST_CASE("a commit with no draft settles to a modified cell that a fetch clears", "[covc][table][edits]") {
    RowCommits c;
    table::CellEdits edits{c.commit()};
    std::vector<std::pair<table::RowId, bool>> heard;
    edits.onRowSettled([&heard](table::RowId const& row, bool refused) { heard.emplace_back(row, refused); });
    edits.commitRow({.row = k1, .cells = {{.column = "a", .json = "1"}}});
    c.done.at(0)(Ok{});
    REQUIRE(heard.size() == 1);
    CHECK_FALSE(heard[0].second);
    CHECK(edits.modified(k1, "a"));
    CHECK_FALSE(edits.draft(k1, "a"));
    edits.rowFetched(table::RowId{std::int64_t{99}});
    edits.rowFetched(k1);
    CHECK_FALSE(edits.modified(k1, "a"));
    CHECK_FALSE(edits.rowEdited(k1));

    edits.commitRow({.row = k2, .cells = {{.column = "a", .json = "1"}}});
    c.done.at(1)(std::unexpected(std::vector<table::FieldError>{{.column = "a", .message = "no"}}));
    REQUIRE(heard.size() == 2);
    CHECK(heard[1].second);
}

TEST_CASE("a fetch keeps an unmodified draft and a cell with an edit in flight", "[covc][table][edits]") {
    RowCommits c;
    table::CellEdits edits{c.commit()};
    edits.setDraft(k1, "a", "typed", std::nullopt);
    edits.rowFetched(k1);
    CHECK(edits.draft(k1, "a") == "typed");
    edits.commitRow({.row = k1, .cells = {{.column = "b", .json = "1"}}});
    edits.commitRow({.row = k1, .cells = {{.column = "c", .json = "2"}}});
    CHECK(edits.stale(k1, "b"));
    CHECK(edits.stale(k1, "c"));
    CHECK_FALSE(edits.stale(k1, "a"));
    c.done.at(0)(Ok{});
    CHECK_FALSE(edits.stale(k1, "b"));
    CHECK(edits.stale(k1, "c"));
}

TEST_CASE("a row that is absent settles its flight without marking anything", "[covc][table][edits]") {
    RowCommits c;
    table::CellEdits edits{c.commit()};
    std::vector<bool> heard;
    edits.onRowSettled([&heard](table::RowId const&, bool refused) { heard.push_back(refused); });
    edits.commitRow({.row = k1, .cells = {{.column = "a", .json = "1"}}});
    edits.rowAbsent(k1);
    edits.rowAbsent(k2);
    c.done.at(0)(Ok{});
    CHECK(heard.empty());
    CHECK(edits.modifiedCount() == 0);
    CHECK_FALSE(edits.stale(k1, "a"));
}

TEST_CASE("moving a row onto its own key changes nothing, and kept columns keep their rows", "[covc][table][edits]") {
    RowCommits c;
    table::CellEdits edits{c.commit()};
    edits.setDraft(k1, "a", "1", "bad");
    edits.setDraft(k1, "b", "2", "worse");
    edits.moveRow(k1, k1);
    CHECK(edits.draft(k1, "a") == "1");
    CHECK(edits.errorCount() == 2);
    std::vector<std::string> const kept{"b"};
    edits.keepColumns(kept);
    CHECK_FALSE(edits.draft(k1, "a"));
    CHECK(edits.draft(k1, "b") == "2");
    CHECK(edits.errorCount() == 1);
    CHECK(edits.error(k1, "b") == "worse");
}

TEST_CASE("of several refusals with no column, the first lands on every cell", "[covc][table][edits]") {
    RowCommits c;
    table::CellEdits edits{c.commit()};
    edits.commitRow({.row = k1, .cells = {{.column = "a", .json = "1"}, {.column = "b", .json = "2"}}});
    c.done.at(0)(std::unexpected(
        std::vector<table::FieldError>{{.column = "", .message = "first"}, {.column = "", .message = "second"}}));
    CHECK(edits.error(k1, "a") == "first");
    CHECK(edits.error(k1, "b") == "first");
    edits.commitRow({.row = k1, .cells = {{.column = "a", .json = "1"}, {.column = "b", .json = "2"}}});
    c.done.at(1)(std::unexpected(std::vector<table::FieldError>{{.column = "a", .message = "only a"}}));
    CHECK(edits.error(k1, "a") == "only a");
    CHECK_FALSE(edits.error(k1, "b"));
}
