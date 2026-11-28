// SPDX-License-Identifier: Apache-2.0

// A grid source asked about a row or a column it does not have: every question has a benign answer, and nothing is
// written.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <morph/ui/grid.hpp>
#include <string>
#include <vector>

#include "grid_test_support.hpp"

namespace {

using morph::test::grid::fieldCell;
using morph::test::grid::FixedGridFixture;
using morph::test::grid::GridFixture;
using morph::test::grid::textCell;
using namespace morph;

// What a source says of a cell it does not have.
void expectNoSuchCell(ui::GridSource& source, ui::Key const& key, std::string const& column, bool refuses = false) {
    INFO("column " << column);
    CHECK(source.readOnly(key, column));
    CHECK_FALSE(source.toggles(key, column));
    CHECK_FALSE(source.covered(key, column));
    CHECK(source.draft(key, column).empty());
    CHECK(source.text(key, column).empty());
    CHECK_FALSE(source.error(key, column).has_value());
    ui::CellMarks const marks = source.marks(key, column);
    CHECK_FALSE(marks.modified);
    CHECK_FALSE(marks.error);
    CHECK_FALSE(source.rowModified(key));
    CHECK(source.refusal(key, column).has_value() == refuses);  // a model's grid says why a row is not loaded
    CHECK_FALSE(source.resends(key, column));
    CHECK(source.spans(key).empty());
    source.write(key, column, "ignored");
    source.revertCell(key, column);
    source.editorOpened(key, column);
    source.committed();
}

}  // namespace

TEST_CASE("cov unknown: a form grid answers for a row or a column it lacks without changing anything",
          "[r2][ui][grid][cov]") {
    GridFixture f{2};
    for (ui::Key const& key : {ui::Key{std::int64_t{99}}, ui::Key{std::string{"zz"}}, ui::Key{std::int64_t{0}},
                               ui::Key{std::int64_t{-3}}}) {
        expectNoSuchCell(*f.source, key, "label");
        CHECK_FALSE(f.source->positionOf(key).has_value());
    }
    expectNoSuchCell(*f.source, ui::Key{std::int64_t{1}}, "nope");
    CHECK_FALSE(f.source->keyAt(2).has_value());
    CHECK_FALSE(f.source->keyOf(ui::Value{}).has_value());
    CHECK_FALSE(f.source->keyOf(ui::Value{std::string{"x"}}).has_value());
    CHECK_FALSE(f.source->editor(ui::Key{std::int64_t{99}}, "label") == nullptr);  // an empty cell stands in
    CHECK(f.form->field("specimens").rows().size() == 2);
    CHECK(f.form->field("specimens[0].label").draft().empty());
    CHECK(f.form->field("specimens[1].label").draft().empty());
}

TEST_CASE("cov unknown: a fixed grid answers for a row or a column it lacks without changing anything",
          "[r2][ui][grid][fixed][cov]") {
    std::vector<std::variant<ui::FixedRow, ui::FixedBlock>> rows;
    rows.emplace_back(ui::FixedRow{.key = "density",
                                   .label = "density",
                                   .cells = {{"min", fieldCell("limits.density.min")}, {"max", textCell("x")}}});
    std::vector<ui::GridColumn> columns;
    for (std::string const id : {"min", "max"}) {
        columns.push_back(ui::GridColumn{.id = id, .label = id, .field = {}, .value = {}});
    }
    FixedGridFixture f{R"({"limits":{}})", std::move(rows), std::move(columns)};
    for (ui::Key const& key : {ui::Key{std::string{"zz"}}, ui::Key{std::int64_t{1}}}) {
        expectNoSuchCell(*f.source, key, "min");
        CHECK_FALSE(f.source->positionOf(key).has_value());
    }
    expectNoSuchCell(*f.source, ui::Key{std::string{"density"}}, "nope");
    CHECK_FALSE(f.source->keyAt(1).has_value());
    CHECK_FALSE(f.source->keyOf(ui::Value{}).has_value());
    CHECK(f.form->field("limits.density.min").draft().empty());
    CHECK(f.source->modifiedCount() == 0);
}

TEST_CASE("cov unknown: a model grid answers for a row or a column it lacks without sending anything",
          "[r2][ui][grid][cov]") {
    morph::test::grid::ModelGridFixture f{{1, 2}};
    for (ui::Key const& key : {ui::Key{std::int64_t{99}}, ui::Key{std::string{"zz"}}}) {
        expectNoSuchCell(*f.source, key, "label", true);
    }
    expectNoSuchCell(*f.source, ui::Key{std::int64_t{1}}, "nope", true);
    CHECK(f.commits.sent.empty());
}
