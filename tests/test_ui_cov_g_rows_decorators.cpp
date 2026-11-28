// SPDX-License-Identifier: Apache-2.0

// The row sources a grid and a summarised table are drawn from, asked what lies outside them: an index or a range
// off the rows, a summary line with no data row beside it, and the changes a source tells before anyone listens.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <expected>
#include <memory>
#include <morph/table/view_change.hpp>
#include <morph/testing/step_executor.hpp>
#include <morph/ui/grid.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "grid_test_support.hpp"
#include "ui_fake_rows.hpp"

namespace {

using morph::test::grid::GridFixture;
using morph::testing::FakeRows;
using morph::testing::StepExecutor;
using morph::ui::Key;
using morph::ui::SummaryRows;
using morph::ui::SummarySpec;
namespace table = morph::table;
namespace ui = morph::ui;

Key summaryKey(std::string const& key) { return Key{"\x1Fsummary:" + key}; }

struct Over {
    StepExecutor owner;
    std::shared_ptr<FakeRows> fake;
    std::shared_ptr<SummaryRows> rows;

    Over(std::int64_t count, std::vector<SummarySpec> specs, std::vector<SummarySpec> perRun = {},
         SummaryRows::Runs runs = {})
        : fake{std::make_shared<FakeRows>(count, 2, owner)},
          rows{std::make_shared<SummaryRows>(fake, std::move(specs), std::move(perRun), std::move(runs))} {}
};

}  // namespace

TEST_CASE("covg summary rows: the data rows must be given", "[covg][ui][grid][summary]") {
    CHECK_THROWS_AS(SummaryRows(nullptr, {}, {}, {}), std::invalid_argument);
}

TEST_CASE("covg summary rows: an index off the mounted rows names nothing", "[covg][ui][grid][summary]") {
    Over const o{3, {{.key = "t", .top = true}, {.key = "b", .top = false}}};
    CHECK(o.rows->mountedTotal().rows == 5);
    CHECK_FALSE(o.rows->row(-1).has_value());
    CHECK_FALSE(o.rows->row(5).has_value());
    REQUIRE(o.rows->row(0).has_value());
    CHECK(o.rows->row(0)->key == summaryKey("t"));
    REQUIRE(o.rows->row(1).has_value());
    CHECK(o.rows->row(1)->key == table::RowId{std::int64_t{0}});
    CHECK(o.rows->row(4)->key == summaryKey("b"));
    CHECK_FALSE(o.rows->mounted(-1).has_value());
    CHECK_FALSE(o.rows->mounted(3).has_value());
    CHECK(o.rows->mounted(0) == std::optional<std::int64_t>{1});
    CHECK(o.rows->mounted(2) == std::optional<std::int64_t>{3});
    CHECK_FALSE(o.rows->data(-1).has_value());
    CHECK_FALSE(o.rows->data(5).has_value());
    CHECK(o.rows->data(0) == std::optional<std::int64_t>{0});  // a summary line takes the data row beside it
    CHECK(o.rows->data(4) == std::optional<std::int64_t>{2});
    CHECK(o.rows->data(2) == std::optional<std::int64_t>{1});
    CHECK_FALSE(o.rows->summary(-1));
    CHECK(o.rows->summary(0));
    CHECK_FALSE(o.rows->summary(1));
    CHECK(o.rows->summary(4));
    CHECK_FALSE(o.rows->summary(5));
}

TEST_CASE("covg summary rows: with no data row a summary line has none beside it", "[covg][ui][grid][summary]") {
    Over const o{0, {{.key = "t", .top = true}, {.key = "b", .top = false}}};
    CHECK(o.rows->mountedTotal().rows == 2);
    CHECK_FALSE(o.rows->data(0).has_value());
    CHECK_FALSE(o.rows->data(1).has_value());
    CHECK_FALSE(o.rows->mounted(0).has_value());
}

TEST_CASE("covg summary rows: a window of mounted rows is the data rows in it", "[covg][ui][grid][summary]") {
    struct Window {
        std::int64_t first;
        std::int64_t count;
        std::int64_t from;
        std::int64_t rows;
    };
    Over const o{3, {{.key = "t", .top = true}, {.key = "b", .top = false}}};
    for (Window const& w :
         {Window{.first = 1, .count = 2, .from = 0, .rows = 2}, Window{.first = 0, .count = 1, .from = 0, .rows = 0},
          Window{.first = 4, .count = 1, .from = 3, .rows = 0}, Window{.first = -3, .count = 2, .from = 0, .rows = 0},
          Window{.first = 2, .count = -5, .from = 1, .rows = 0},
          Window{.first = 1, .count = 40, .from = 0, .rows = 3}}) {
        CAPTURE(w.first, w.count);
        o.rows->visible(w.first, w.count, {});
        CHECK(o.fake->lastVisible.first == w.from);
        CHECK(o.fake->lastVisible.count == w.rows);
    }
}

TEST_CASE("covg summary rows: a row located among the data rows is told at its mounted index",
          "[covg][ui][grid][summary]") {
    Over o{3, {{.key = "t", .top = true}}};
    std::vector<ui::RowLocation> answers;
    for (std::int64_t const key : {1, 99}) {
        o.rows->locateRow(table::RowId{key}, {},
                          [&answers](ui::RowLocation const& found) { answers.push_back(found); });
    }
    o.owner.runAll();
    REQUIRE(answers.size() == 2);
    REQUIRE(answers.at(0).has_value());
    CHECK(*answers.at(0) == std::optional<std::int64_t>{2});  // data row 1 sits below the top summary
    REQUIRE(answers.at(1).has_value());
    CHECK_FALSE(answers.at(1)->has_value());
}

TEST_CASE("covg summary rows: a change told before anyone listens is taken in without telling",
          "[covg][ui][grid][summary]") {
    Over const o{3, {{.key = "t", .top = true}}};
    o.fake->setTotal(5, table::TotalKind::Exact);
    CHECK(o.rows->mountedTotal().rows == 6);
}

TEST_CASE("covg summary rows: a change that keeps the layout is told in mounted indices and one that does not resets",
          "[covg][ui][grid][summary]") {
    std::vector<std::pair<std::int64_t, std::int64_t>> runs{{0, 2}, {2, 1}};
    Over const o{3, {{.key = "t", .top = true}}, {{.key = "r", .top = false}}, [&runs] { return runs; }};
    // top summary, run 0 (data 0, 1) and its summary, run 1 (data 2) and its summary.
    CHECK(o.rows->mountedTotal().rows == 6);
    std::vector<table::ViewChange> told;
    o.rows->onChange([&told](table::ViewChange const& change) { told.push_back(change); });
    using Kind = table::ViewOp::Kind;
    o.fake->announce({table::ViewOp{.kind = Kind::Changed, .first = 1, .count = 2, .to = 0}});
    REQUIRE(told.size() == 1);
    // The changed data rows 1 and 2 sit at mounted rows 2 and 4; then the summary rows 0, 3 and 5 change.
    std::vector<std::size_t> changed;
    for (auto const& op : told.back().ops) {
        CHECK(op.kind == Kind::Changed);
        changed.push_back(op.first);
    }
    CHECK(changed == std::vector<std::size_t>{2, 4, 0, 3, 5});
    told.clear();
    o.fake->announce({table::ViewOp{.kind = Kind::Inserted, .first = 3, .count = 1, .to = 0}});
    REQUIRE(told.size() == 1);
    REQUIRE(told.back().ops.size() == 1);
    CHECK(told.back().ops.front().kind == Kind::Reset);
    told.clear();
    runs = {{0, 3}};  // the runs themselves changed
    o.fake->announce({table::ViewOp{.kind = Kind::Changed, .first = 0, .count = 1, .to = 0}});
    REQUIRE(told.size() == 1);
    CHECK(told.back().ops.front().kind == Kind::Reset);
    told.clear();
    o.fake->announce({table::ViewOp{.kind = Kind::Reset, .first = 0, .count = 0, .to = 0}});
    REQUIRE(told.size() == 1);
    CHECK(told.back().ops.front().kind == Kind::Reset);
}

TEST_CASE("covg summary rows: an ungrouped change moves down by the summaries drawn above",
          "[covg][ui][grid][summary]") {
    Over const o{3, {{.key = "t", .top = true}, {.key = "b", .top = false}}};
    std::vector<table::ViewChange> told;
    o.rows->onChange([&told](table::ViewChange const& change) { told.push_back(change); });
    using Kind = table::ViewOp::Kind;
    o.fake->announce({table::ViewOp{.kind = Kind::Moved, .first = 0, .count = 1, .to = 2},
                      table::ViewOp{.kind = Kind::Changed, .first = 1, .count = 1, .to = 0}});
    REQUIRE(told.size() == 1);
    auto const& ops = told.back().ops;
    REQUIRE(ops.size() == 4);
    CHECK(ops.at(0).kind == Kind::Moved);
    CHECK(ops.at(0).first == 1);
    CHECK(ops.at(0).to == 3);
    CHECK(ops.at(1).first == 2);
    CHECK(ops.at(2).first == 0);  // the summary rows change with the data
    CHECK(ops.at(3).first == 4);
}

TEST_CASE("covg grid rows: an index or a range off the rows names nothing", "[covg][ui][grid]") {
    GridFixture f{3};
    ui::TableRows& rows = f.source->view();
    CHECK_FALSE(rows.row(-1).has_value());
    CHECK_FALSE(rows.row(3).has_value());
    CHECK(rows.row(2).has_value());
    struct Range {
        std::int64_t first;
        std::int64_t count;
        bool ok;
    };
    for (Range const& r : {Range{.first = -1, .count = 1, .ok = false}, Range{.first = 0, .count = -1, .ok = false},
                           Range{.first = 2, .count = 5, .ok = false}, Range{.first = 1, .count = 2, .ok = true}}) {
        CAPTURE(r.first, r.count);
        std::optional<std::expected<std::vector<table::RowId>, table::TableError>> got;
        rows.keysOf(r.first, r.count).then([&got](auto const& keys) { got = keys; });
        f.h.settle();
        REQUIRE(got.has_value());
        CHECK(got->has_value() == r.ok);
        if (!r.ok) {
            CHECK(got->error().message == "a range outside the view");
        }
    }
}

TEST_CASE("covg grid rows: a row is located by its key and an absent key is nowhere", "[covg][ui][grid]") {
    GridFixture f{3};
    ui::TableRows& rows = f.source->view();
    std::vector<ui::RowLocation> answers;
    for (std::int64_t const key : {2, 99}) {
        rows.locateRow(table::RowId{key}, {}, [&answers](ui::RowLocation const& found) { answers.push_back(found); });
    }
    f.h.settle();
    REQUIRE(answers.size() == 2);
    REQUIRE(answers.at(0).has_value());
    CHECK(*answers.at(0) == std::optional<std::int64_t>{1});
    REQUIRE(answers.at(1).has_value());
    CHECK_FALSE(answers.at(1)->has_value());
}
