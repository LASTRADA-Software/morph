// SPDX-License-Identifier: Apache-2.0

// A form grid's spans read back for a row that has them, and a fixed grid's request for one more round of derived
// values, which reaches its form and does nothing without one.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <morph/forms/derive.hpp>
#include <morph/forms/engine/form_session.hpp>
#include <morph/reactive/testing/manual_scheduler.hpp>
#include <morph/table/query.hpp>
#include <morph/ui/grid.hpp>
#include <morph/ui/locale_table.hpp>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "grid_test_support.hpp"

namespace {

namespace ui = morph::ui;
using morph::test::grid::fieldCell;
using morph::test::grid::GridFixture;

std::vector<ui::GridColumn> minMax() {
    std::vector<ui::GridColumn> out;
    for (std::string const id : {"min", "max"}) {
        out.push_back(ui::GridColumn{.id = id, .label = id, .field = {}, .value = {}});
    }
    return out;
}

std::vector<std::variant<ui::FixedRow, ui::FixedBlock>> oneRow() {
    std::vector<std::variant<ui::FixedRow, ui::FixedBlock>> parts;
    parts.emplace_back(
        ui::FixedRow{.key = "density",
                     .label = "Density",
                     .cells = {{"min", fieldCell("limits.density.min")}, {"max", fieldCell("limits.density.max")}}});
    return parts;
}

}  // namespace

TEST_CASE("covj form grid source: a row's spans read back as given, and another row has none", "[covj][ui][grid]") {
    GridFixture f{3};
    f.source->setSpans([](morph::forms::RowState const& row) {
        std::vector<ui::CellSpan> spans;
        if (row.key() == 1) {
            spans.push_back({.column = "mass", .rows = 2, .columns = 1});
        }
        return spans;
    });
    std::vector<ui::CellSpan> const given = f.source->spans(ui::Key{std::int64_t{1}});
    REQUIRE(given.size() == 1);
    CHECK(given.front().column == "mass");
    CHECK(given.front().rows == 2);
    CHECK(f.source->spans(ui::Key{std::int64_t{3}}).empty());
}

TEST_CASE("covj fixed grid source: recalculate makes one more round of derived values", "[covj][ui][grid]") {
    morph::test::formsengine::Harness h;
    morph::reactive::testing::ManualScheduler scheduler;
    morph::test::formsengine::FakeServer evaluate{h.owner};
    morph::test::formsengine::FakeServer derive{h.owner};
    auto form = h.session("Lims_SetLimits", morph::test::grid::kSetLimits,
                          morph::forms::FormSessionOptions{.locale = ui::localeFacts("de").numeric, .bcp47 = "de"});
    form->prefill(R"({"limits":{}})");
    h.settle();
    form->deriveWith(morph::forms::DeriveOptions{
        .evaluate = [&](std::string body) { return evaluate.submitter()("Lims_SetLimits", std::move(body)); },
        .derive = [&](std::string body) { return derive.submitter()("Lims_DeriveLimits", std::move(body)); },
        .scheduler = &scheduler,
        .debounce = std::chrono::milliseconds{150},
        .refused = {}});
    ui::FixedGridSource source{
        h.rt, form.get(), oneRow(), minMax(),
        ui::FormatContext{.locale = "de", .numeric = ui::withoutGrouping(ui::localeFacts("de").numeric)}};
    REQUIRE(evaluate.submits().empty());
    source.recalculate();
    h.settle();
    CHECK(evaluate.submits().size() == 1);
}

TEST_CASE("covj fixed grid source: recalculate over no form does nothing", "[covj][ui][grid]") {
    morph::test::formsengine::Harness h;
    std::vector<std::variant<ui::FixedRow, ui::FixedBlock>> rows;
    rows.emplace_back(ui::FixedRow{
        .key = "density",
        .label = "Density",
        .cells = {{"min", morph::test::grid::textCell("lo")}, {"max", morph::test::grid::textCell("hi")}}});
    ui::FixedGridSource source{
        h.rt, nullptr, std::move(rows), minMax(),
        ui::FormatContext{.locale = "de", .numeric = ui::withoutGrouping(ui::localeFacts("de").numeric)}};
    source.recalculate();
    source.committed();
    CHECK(source.text(ui::Key{std::string{"density"}}, "min") == "lo");
}

TEST_CASE("covj form grid source: a span that fits nowhere covers nothing and is not given", "[covj][ui][grid]") {
    GridFixture f{3};
    using Spans = std::vector<ui::CellSpan>;
    auto const [name, spans] = GENERATE(
        std::pair<std::string, Spans>{"no rows", Spans{{.column = "mass", .rows = 0, .columns = 1}}},
        std::pair<std::string, Spans>{"a column the grid lacks", Spans{{.column = "nosuch", .rows = 2, .columns = 1}}},
        std::pair<std::string, Spans>{"rows past the last", Spans{{.column = "mass", .rows = 4, .columns = 1}}});
    CAPTURE(name);
    f.source->setSpans([spans](morph::forms::RowState const& row) { return row.key() == 1 ? spans : Spans{}; });
    CHECK(f.source->spans(ui::Key{std::int64_t{1}}).empty());
    CHECK_FALSE(f.source->covered(ui::Key{std::int64_t{2}}, "mass"));
    CHECK_FALSE(f.source->covered(ui::Key{std::int64_t{1}}, "volume"));
}

TEST_CASE("covj form grid source: a sorted grid gives and covers no spans", "[covj][ui][grid]") {
    GridFixture f{3};
    f.source->setSpans([](morph::forms::RowState const& row) {
        std::vector<ui::CellSpan> spans;
        if (row.key() == 1) {
            spans.push_back({.column = "mass", .rows = 2, .columns = 1});
        }
        return spans;
    });
    REQUIRE(f.source->covered(ui::Key{std::int64_t{2}}, "mass"));
    REQUIRE(f.source->view().setSort({morph::table::SortKey{.column = "mass"}}).has_value());
    CHECK(f.source->spans(ui::Key{std::int64_t{1}}).empty());
    CHECK_FALSE(f.source->covered(ui::Key{std::int64_t{2}}, "mass"));
    REQUIRE(f.source->view().setSort({}).has_value());
    CHECK(f.source->covered(ui::Key{std::int64_t{2}}, "mass"));
}

TEST_CASE("covj form grid source: derived cells are muted while a round runs, and the typed ones are not",
          "[covj][ui][grid]") {
    using namespace std::chrono_literals;
    GridFixture f{2};
    f.enableDerive();
    f.grid->focusCell(morph::test::grid::at(1, "mass"));
    f.grid->type("5");
    f.grid->press("Enter");
    f.scheduler.advance(150ms);
    f.h.settle();
    REQUIRE(f.form->deriveState() == morph::forms::DeriveState::Running);
    CHECK(f.source->marks(ui::Key{std::int64_t{1}}, "limit").muted);
    CHECK_FALSE(f.source->marks(ui::Key{std::int64_t{1}}, "limit").stale);
    CHECK_FALSE(f.source->marks(ui::Key{std::int64_t{1}}, "mass").muted);
    CHECK_FALSE(f.source->marks(ui::Key{std::int64_t{1}}, "note").muted);
}
