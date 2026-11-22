// SPDX-License-Identifier: Apache-2.0

// Grids of a form: the focus chain, the cell, the walk, read-only as one rule, per-cell marks and submission, derived
// values, cell selection, paste, fill and clear, and the rows of a grid (docs/spec/table/editing.md, delivery steps 1
// to 3).

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <morph/core/action_refusal.hpp>
#include <morph/forms/derive.hpp>
#include <morph/forms/engine/form_session.hpp>
#include <morph/ui/document.hpp>
#include <morph/ui/grid.hpp>
#include <morph/ui/interpreter.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "grid_test_support.hpp"
#include "ui_table_support.hpp"

namespace {

using namespace morph;
using morph::test::grid::at;
using morph::test::grid::columnsOf;
using morph::test::grid::enabledOf;
using morph::test::grid::entry;
using morph::test::grid::GridFixture;
using morph::test::grid::keysOf;
using morph::test::grid::labelsOf;

// Mutation: FormGridSource::write parsing the number itself (std::stod after replacing ','), not through the field.
TEST_CASE("grid: a cell encodes as its field does", "[ui][grid]") {
    GridFixture f{3};
    f.grid->focusCell(at(1, "mass"));
    f.grid->type("12,5");
    CHECK(f.grid->press("Enter") == ui::KeyOutcome::Handled);
    CHECK(f.json("specimens[0].mass") == R"({"num":1250,"den":100,"dp":2})");
    CHECK(f.grid->focus() == at(2, "mass"));                 // enterMoves: down
    f.grid->type("1,234");
    f.grid->press("Enter");
    CHECK(f.form->field("specimens[1].mass").draft() == "1,234");   // kept as typed
    CHECK(f.form->field("specimens[1].mass").error().peek()->code == "too-precise");
    CHECK_FALSE(f.form->ready());
    CHECK(f.source->marks(ui::Key{std::int64_t{2}}, "mass").error);
    f.grid->focusCell(at(2, "mass"));
    CHECK(f.grid->message() == "Too many decimal places");    // the forms error, while the cell has focus
}

// Mutation: GridSource::readOnly testing spec().readOnly only (the readonlyWhen section fails).
TEST_CASE("grid: read-only is one rule", "[ui][grid]") {
    GridFixture f{2};
    SECTION("computed and derived members") {
        f.grid->focusCell(at(1, "volume"));
        f.grid->type("2,0");
        f.grid->press("Tab");
        CHECK(f.grid->focus() == at(1, "note"));             // density and limit skipped
        f.grid->press("Shift+Tab");
        CHECK(f.grid->focus() == at(1, "volume"));
        for (std::string const column : {"density", "limit"}) {
            f.grid->focusCell(at(1, column));                  // an arrow or a click reaches it
            f.grid->type("5");
            CHECK_FALSE(f.grid->editing().has_value());
            CHECK(f.grid->message() == "This cell is read-only");
            CHECK(f.source->marks(ui::Key{std::int64_t{1}}, column).readOnly);
        }
    }
    SECTION("a readonlyWhen rule") {
        f.h.type(*f.form, "reviewer", "ana");
        f.grid->focusCell(at(1, "mass"));
        f.grid->type("5");
        CHECK_FALSE(f.grid->editing().has_value());
        CHECK(f.source->marks(ui::Key{std::int64_t{1}}, "mass").readOnly);
        CHECK(f.grid->press("Tab") == ui::KeyOutcome::FocusNext);  // no editable cell is left
    }
}

// Mutation: Tab wrapping to the first editable cell, which never leaves the grid.
TEST_CASE("grid: the walk", "[ui][grid]") {
    SECTION("enterMoves down fills a column") {
        GridFixture f{3};
        f.grid->focusCell(at(1, "mass"));
        for (auto const* text : {"1", "2", "3"}) { f.grid->type(text); f.grid->press("Enter"); }
        CHECK(f.json("specimens[2].mass") == R"({"num":300,"den":100,"dp":2})");
        CHECK(f.grid->focus() == at(3, "mass"));             // no row below and no appendRows: it stays
    }
    SECTION("Tab on the last editable cell leaves") {
        GridFixture f{2};
        f.grid->focusCell(at(2, "note"));
        CHECK(f.grid->press("Tab") == ui::KeyOutcome::FocusNext);
        f.grid->focusCell(at(1, "label"));
        CHECK(f.grid->press("Shift+Tab") == ui::KeyOutcome::FocusPrevious);
    }
    SECTION("walk: columns goes down first") {
        GridFixture f{2, ui::GridOptions{.walk = ui::Walk::Columns}};
        f.grid->focusCell(at(1, "mass"));
        f.grid->press("Tab");
        CHECK(f.grid->focus() == at(2, "mass"));
        f.grid->press("Tab");
        CHECK(f.grid->focus() == at(1, "volume"));
    }
    SECTION("Escape restores the draft the cell had when the editor opened") {
        GridFixture f{1};
        f.grid->focusCell(at(1, "label"));
        f.grid->type("a");
        f.grid->press("Enter");
        f.grid->focusCell(at(1, "label"));
        f.grid->type("b");
        f.grid->press("Escape");
        CHECK(f.form->field("specimens[0].label").draft() == "a");
    }
}

// Mutation: an empty grid answering Tab with Handled.
TEST_CASE("grid: an empty grid lets Tab leave at once", "[ui][grid]") {
    GridFixture f{0};
    CHECK_FALSE(f.grid->focus().has_value());
    CHECK(f.grid->press("Tab") == ui::KeyOutcome::FocusNext);
    CHECK(f.grid->press("Shift+Tab") == ui::KeyOutcome::FocusPrevious);
}

// Mutation: FormRowsSource giving the engine's view order to the array (a source that reorders the field's rows).
TEST_CASE("grid: a sorted grid keeps its record", "[ui][grid]") {
    GridFixture f{3};
    for (auto const& [key, text] : {std::pair{1, "3"}, {2, "1"}, {3, "2"}}) {
        f.grid->focusCell(at(key, "mass")); f.grid->type(text); f.grid->press("Enter");
    }
    REQUIRE(f.source->view().setSort({table::SortKey{.column = "mass"}}).has_value());
    CHECK(keysOf(*f.source) == std::vector<ui::Key>{ui::Key{std::int64_t{2}}, ui::Key{std::int64_t{3}}, ui::Key{std::int64_t{1}}});
    f.grid->focusCell(at(3, "mass")); f.grid->type("9"); f.grid->press("Enter");
    CHECK(f.json("specimens[2].mass") == R"({"num":900,"den":100,"dp":2})");   // in place, entered order
    CHECK(f.json("specimens[0].mass") == R"({"num":300,"den":100,"dp":2})");
    std::uint64_t const added = f.form->field("specimens").addRow();
    f.grid->focusCell(at(static_cast<std::int64_t>(added), "mass")); f.grid->type("0"); f.grid->press("Enter");
    CHECK(keysOf(*f.source).back() == ui::Key{static_cast<std::int64_t>(added)});   // at the end until the sort changes
    REQUIRE(f.source->view().setSort({}).has_value());
    REQUIRE(f.source->view().setSort({table::SortKey{.column = "mass"}}).has_value());
    CHECK(keysOf(*f.source).front() == ui::Key{static_cast<std::int64_t>(added)});   // 0,00 sorts first
}

// Mutation: modified() comparing the whole ObjectArray's snapshot, which marks every cell.
TEST_CASE("grid: what changed", "[ui][grid]") {
    GridFixture f{2};
    f.grid->focusCell(at(1, "mass")); f.grid->type("4"); f.grid->press("Enter");
    CHECK(f.form->modified("specimens#1.mass"));
    CHECK_FALSE(f.form->modified("specimens#1.volume"));
    CHECK_FALSE(f.form->modified("specimens#2.mass"));
    CHECK(f.form->rowModified("specimens", 1));
    CHECK_FALSE(f.form->rowModified("specimens", 2));
    CHECK(f.source->modifiedCount() == 1);
    CHECK(f.source->modifiedRows() == 1);
    f.form->revert("specimens#1.mass");
    CHECK(f.form->field("specimens[0].mass").draft().empty());
    CHECK(f.source->modifiedCount() == 0);
    std::uint64_t const added = f.form->field("specimens").addRow();
    CHECK(f.form->rowModified("specimens", added));          // a row added since the load, as a whole
}

// Mutation: modified() false for every read-only member (the readonlyWhen section loses the typed cell's mark).
TEST_CASE("grid: only computed and derived members never draw a modified mark", "[ui][grid]") {
    GridFixture f{1};
    SECTION("a cell frozen by a rule after typing stays modified") {
        f.grid->focusCell(at(1, "mass")); f.grid->type("4"); f.grid->press("Enter");
        f.h.type(*f.form, "reviewer", "ana");                  // readonlyWhen now freezes the grid
        REQUIRE(f.form->field("specimens[0].mass").readonly().get());
        CHECK(f.form->modified("specimens#1.mass"));
        CHECK(f.source->marks(ui::Key{std::int64_t{1}}, "mass").modified);
    }
    SECTION("a computed member written programmatically is not modified") {
        f.form->assign("specimens[0].density", R"({"num":2,"den":1,"dp":3})");
        CHECK_FALSE(f.form->modified("specimens#1.density"));
        CHECK_FALSE(f.form->rowModified("specimens", 1));
    }
}

// Mutation: "Revert row" reverting the focused cell only (label keeps "x").
TEST_CASE("grid: revert cell and revert row from the cell's menu", "[ui][grid]") {
    GridFixture f{2};
    f.grid->focusCell(at(1, "label")); f.grid->type("x"); f.grid->press("Tab");
    f.grid->type("4"); f.grid->press("Enter");                 // (1, mass)
    auto entries = f.grid->cellEntries(ui::Key{std::int64_t{2}}, "mass");
    CHECK(labelsOf(entries) == std::vector<std::string>{"Revert cell", "Revert row", "", "Insert row above",
                                                        "Insert row below", "Delete row"});
    CHECK_FALSE(enabledOf(entry(entries, "Revert cell")));       // row 2 holds nothing typed
    entries = f.grid->cellEntries(ui::Key{std::int64_t{1}}, "mass");
    entry(entries, "Revert cell").onSelect();
    CHECK(f.form->field("specimens[0].mass").draft().empty());
    CHECK(f.form->field("specimens[0].label").draft() == "x");
    f.grid->focusCell(at(1, "mass")); f.grid->type("5"); f.grid->press("Enter");
    entries = f.grid->cellEntries(ui::Key{std::int64_t{1}}, "label");
    entry(entries, "Revert row").onSelect();
    CHECK(f.form->field("specimens[0].label").draft().empty());
    CHECK(f.form->field("specimens[0].mass").draft().empty());   // every cell of the row, not the menu's cell
    CHECK_FALSE(f.form->rowModified("specimens", 1));
}

// Mutation: the editor holding its text in a buffer of its own until commit (the draft is still "" while typing).
TEST_CASE("grid: the editor's text is the cell's draft, and a commit is one change", "[ui][grid]") {
    GridFixture f{2};
    std::size_t commits = 0;
    f.source->onCommitted([&] { ++commits; });               // test seam: called by committed()
    f.grid->focusCell(at(1, "label"));
    f.grid->type("a");
    CHECK(f.grid->editing() == at(1, "label"));
    CHECK(f.form->field("specimens[0].label").draft() == "a");   // live, before any commit
    CHECK(commits == 0);
    f.grid->type("ab");                                        // the editor's whole new text
    CHECK(f.form->field("specimens[0].label").draft() == "ab");
    f.grid->press("Enter");
    CHECK(commits == 1);
    CHECK_FALSE(f.grid->editing().has_value());
    f.grid->focusCell(at(1, "label")); f.grid->press("F2"); f.grid->press("Enter");   // opened, nothing changed
    CHECK(commits == 1);
    f.grid->focusCell(at(1, "label")); f.grid->type("zz"); f.grid->press("Escape");
    CHECK(f.form->field("specimens[0].label").draft() == "ab");
    CHECK(commits == 1);                                       // Escape is no change
}

// Mutation: FormSession::submit not calling the before-submit hooks (the editor stays open, no commit is counted).
TEST_CASE("grid: the form's submit ends an open edit first", "[ui][grid]") {
    GridFixture f{1};
    std::size_t commits = 0;
    f.source->onCommitted([&] { ++commits; });
    f.grid->focusCell(at(1, "mass")); f.grid->type("7,5");
    REQUIRE(f.grid->editing().has_value());
    f.form->submit(); f.h.settle();
    CHECK_FALSE(f.grid->editing().has_value());
    CHECK(commits == 1);
    REQUIRE(f.h.server.submits().size() == 1);
    CHECK(f.h.server.lastBody().find(R"("mass":{"num":750,"den":100,"dp":2})") != std::string::npos);
}

// Mutation: GridController::click only focusing the cell (the check box stays unset).
TEST_CASE("grid: a click on a Boolean cell toggles it", "[ui][grid]") {
    GridFixture f{2, {}, morph::test::grid::specimensWithChoices()};
    f.grid->click(at(1, "checked"));
    f.h.settle();
    CHECK(f.json("specimens[0].checked") == "true");
    CHECK(f.grid->focus() == at(1, "checked"));
    f.grid->click(at(1, "checked"));
    f.h.settle();
    CHECK(f.json("specimens[0].checked") == "false");
    f.grid->click(at(1, "note"));
    f.h.settle();
    CHECK_FALSE(f.grid->editing().has_value());
}

// Mutation: FormRowsSource::row building the whole snapshot on every read (the walk builds once per step).
TEST_CASE("grid: a walk over a large grid reads its rows without rebuilding them", "[ui][grid]") {
    GridFixture f{300};
    f.grid->focusCell(at(1, "label"));
    f.h.settle();
    ui::FormRowsSource const& rows = f.source->view().source();
    std::size_t const before = rows.builds();
    for (int step = 0; step < 200; ++step) {
        f.grid->press("Tab");
    }
    f.h.settle();
    CHECK(f.grid->focus().has_value());
    CHECK(f.grid->focus() != at(1, "label"));
    CHECK(rows.builds() == before);
}

// ── Mounted ─────────────────────────────────────────────────────────────────

// A grid of `GridFixture` mounted on a recording backend: its node over every member in schema order (label 0, mass 1,
// volume 2, density 3, limit 4, note 5).
struct MountedGrid {
    explicit MountedGrid(std::size_t rows, ui::TableOptions options = {}) : f{rows} {
        options.common.testId = "grid";
        view.emplace(f.h.rt, backend, f.grid->node(columnsOf(f), std::move(options)));
        f.h.settle();
        table = backend.find("Table", "testId", "grid").value_or(0);
        REQUIRE(table != 0);
    }
    MountedGrid(MountedGrid const&) = delete;
    MountedGrid& operator=(MountedGrid const&) = delete;
    MountedGrid(MountedGrid&&) = delete;
    MountedGrid& operator=(MountedGrid&&) = delete;
    ~MountedGrid() { view.reset(); }

    // The row widget showing a key.
    [[nodiscard]] int row(std::int64_t key) {
        std::optional<int> const found = backend.tableRow(table, ui::Key{key});
        REQUIRE(found.has_value());
        return *found;
    }

    GridFixture f;
    ui::testing::RecordingBackend backend;
    std::optional<ui::Mounted> view;
    int table = 0;
};

// Mutation: the marks binding reading the cell's row signal untracked (the cell rebound to show row 30 keeps row 1's
// "m").
TEST_CASE("grid: a cell's marks are drawn by its cell view and follow the row it shows", "[ui][grid][mount]") {
    MountedGrid m{40};
    m.backend.scrollTable(m.table, 0, 10);   // rows 0 to 19 realized
    m.f.h.settle();
    m.f.grid->focusCell(at(1, "mass")); m.f.grid->type("4"); m.f.grid->press("Enter");
    m.f.h.settle();
    CHECK(m.backend.prop(m.row(1), "marks:1") == "m");
    CHECK(m.backend.prop(m.row(1), "modified") == "true");
    CHECK(m.backend.prop(m.row(1), "marks:3").find('r') != std::string::npos);   // density, computed
    CHECK(m.backend.prop(m.row(2), "marks:1").empty());
    CHECK(m.backend.prop(m.row(2), "modified") == "false");
    int const shownOne = m.row(1);
    std::size_t const created = m.view->stats().cellsCreated;
    std::size_t const rebound = m.view->stats().cellsRebound;
    // The window moves to rows 29 to 39: the row of key 1 leaves it, and its widget shows key 30.
    m.backend.scrollTable(m.table, 39, 1);
    m.f.h.settle();
    CHECK(m.row(30) == shownOne);
    CHECK(m.backend.prop(m.row(30), "marks:1").empty());
    CHECK(m.view->stats().cellsRebound > rebound);
    CHECK(m.view->stats().cellsCreated == created);   // no cell view was built
    m.f.form->revert("specimens#1.mass");
    m.backend.scrollTable(m.table, 0, 10);
    m.f.h.settle();
    CHECK(m.backend.prop(m.row(1), "marks:1").empty());
    CHECK(m.backend.prop(m.row(1), "modified") == "false");
}

// Mutation: the mount passing a cell's place in the layout as `column` (with `label` hidden, mass reports 0).
TEST_CASE("grid: a cell's column is its index among every column, hidden ones included", "[ui][grid][mount]") {
    ui::TableOptions options;
    options.arrangement = ui::TableArrangement{.visible = {1, 2, 3, 4, 5}, .pinned = 0, .widths = {}};
    MountedGrid m{2, std::move(options)};
    m.f.grid->focusCell(at(1, "mass")); m.f.grid->type("4"); m.f.grid->press("Enter");
    m.f.h.settle();
    CHECK(m.backend.prop(m.row(1), "marks:1") == "m");
    CHECK_FALSE(m.backend.hasProp(m.row(1), "marks:0"));
    m.backend.clickCell(m.table, 0, 1, true);
    m.f.h.settle();
    CHECK(m.f.grid->editing() == at(1, "mass"));
    CHECK_FALSE(m.backend.prop(m.row(1), "editor:1").empty());
}

// Mutation: the row not held (FormGridSource::holdRow doing nothing: the edited row moves under its editor, which is
// built again, and leaves the view while the filter rejects its draft).
TEST_CASE("grid: the row an editor is open on holds its place until the edit ends", "[ui][grid][mount]") {
    MountedGrid m{3};
    for (std::size_t row = 0; row < 3; ++row) {
        m.f.form->field("specimens[" + std::to_string(row) + "].mass").text().set(std::to_string(row + 1));
    }
    m.f.h.settle();
    SECTION("sorted by mass") {
        REQUIRE(m.f.source->view().setSort({table::SortKey{.column = "mass", .dir = table::SortDirection::Ascending}}));
        m.f.h.settle();
        REQUIRE(keysOf(*m.f.source) == std::vector<ui::Key>{ui::Key{std::int64_t{1}}, ui::Key{std::int64_t{2}},
                                                               ui::Key{std::int64_t{3}}});
        m.f.grid->focusCell(at(1, "mass"));
        m.f.grid->type("9");
        m.f.h.settle();
        std::string const editor = m.backend.prop(m.row(1), "editor:1");
        REQUIRE_FALSE(editor.empty());
        m.backend.edit(std::stoi(editor), "95");
        m.f.h.settle();
        CHECK(m.f.grid->editing() == at(1, "mass"));
        CHECK(m.f.source->positionOf(ui::Key{std::int64_t{1}}) == std::optional<std::size_t>{0});
        CHECK(m.backend.prop(m.row(1), "editor:1") == editor);
        m.f.grid->press("Enter");
        m.f.h.settle();
        CHECK_FALSE(m.f.grid->editing().has_value());
        CHECK(keysOf(*m.f.source) == std::vector<ui::Key>{ui::Key{std::int64_t{2}}, ui::Key{std::int64_t{3}},
                                                             ui::Key{std::int64_t{1}}});
        CHECK(m.f.grid->focus() == at(2, "mass"));   // down from view row 0, as the rows stood at the commit
    }
    SECTION("filtered by mass") {
        table::FilterSpec filter;
        filter.columns["mass"].include = {table::FilterEntry{.lt = "5"}};
        REQUIRE(m.f.source->view().setFilter(filter));
        m.f.h.settle();
        m.f.grid->focusCell(at(2, "mass"));
        m.f.grid->type("9");
        m.f.h.settle();
        CHECK(m.f.grid->editing() == at(2, "mass"));
        CHECK(m.f.source->positionOf(ui::Key{std::int64_t{2}}) == std::optional<std::size_t>{1});
        CHECK_FALSE(m.backend.prop(m.row(2), "editor:1").empty());
        m.f.grid->press("Escape");
        m.f.h.settle();
        CHECK(m.f.source->positionOf(ui::Key{std::int64_t{2}}) == std::optional<std::size_t>{1});   // cancelled
        m.f.grid->type("9");
        m.f.h.settle();
        m.f.grid->press("Enter");
        m.f.h.settle();
        CHECK_FALSE(m.f.source->positionOf(ui::Key{std::int64_t{2}}).has_value());
        CHECK(keysOf(*m.f.source) == std::vector<ui::Key>{ui::Key{std::int64_t{1}}, ui::Key{std::int64_t{3}}});
    }
}

// Mutations, each with its own build: `GridController::leave` not letting go (focus leaving, and the form's submit,
// whose before-submit hook is `leave`, keep key 1 first); `focusCell` not letting go (a click on another cell keeps
// key 1 first); `GridRows::rebuild` keeping a held row the form no longer has (key 1 stays in the view).
TEST_CASE("grid: the held row is let go when focus leaves, on submit, on a click elsewhere, and when it is removed",
          "[ui][grid][mount]") {
    MountedGrid m{3};
    for (std::size_t row = 0; row < 3; ++row) {
        m.f.form->field("specimens[" + std::to_string(row) + "].mass").text().set(std::to_string(row + 1));
    }
    m.f.h.settle();
    REQUIRE(m.f.source->view().setSort({table::SortKey{.column = "mass", .dir = table::SortDirection::Ascending}}));
    m.f.grid->focusCell(at(1, "mass"));
    m.f.grid->type("9");
    m.f.h.settle();
    REQUIRE(m.f.grid->editing() == at(1, "mass"));
    REQUIRE(m.f.source->positionOf(ui::Key{std::int64_t{1}}) == std::optional<std::size_t>{0});
    std::vector<ui::Key> const moved{ui::Key{std::int64_t{2}}, ui::Key{std::int64_t{3}}, ui::Key{std::int64_t{1}}};
    SECTION("focus leaving") {
        m.backend.focusOut(m.table);
        m.f.h.settle();
        CHECK_FALSE(m.f.grid->editing().has_value());
        CHECK(keysOf(*m.f.source) == moved);
    }
    SECTION("submit") {
        m.f.form->submit();
        m.f.h.settle();
        CHECK_FALSE(m.f.grid->editing().has_value());
        CHECK(keysOf(*m.f.source) == moved);
    }
    SECTION("a click on another cell") {
        m.backend.clickCell(m.table, 2, 1);   // (3, mass), view row 2 at the click
        m.f.h.settle();
        CHECK_FALSE(m.f.grid->editing().has_value());
        CHECK(keysOf(*m.f.source) == moved);
        CHECK(m.f.grid->focus() == at(3, "mass"));
    }
    SECTION("removed from outside") {
        m.f.form->field("specimens").removeRow(1);
        m.f.h.settle();
        CHECK(keysOf(*m.f.source) == std::vector<ui::Key>{ui::Key{std::int64_t{2}}, ui::Key{std::int64_t{3}}});
        CHECK_FALSE(m.f.source->positionOf(ui::Key{std::int64_t{1}}).has_value());
    }
}

// ── Derived values ──────────────────────────────────────────────────────────

using namespace std::chrono_literals;
std::string const kNoValues = R"({"values":[]})";

// Mutations: loadDerived resolving `specimens[i]` against the current rows (the value lands on the inserted row);
// FormGridSource::deriveFailed reading `deriveState() == Failed` (the standing state is gone while the retry runs).
TEST_CASE("grid: derived values", "[ui][grid][derive]") {
    GridFixture f{3};
    f.enableDerive();
    f.grid->focusCell(at(1, "mass")); f.grid->type("4,00"); f.grid->press("Enter");
    f.grid->type("abc"); f.grid->press("Enter");                 // (2, mass): does not encode, left out
    f.scheduler.advance(149ms); f.h.settle();
    CHECK(f.evaluate.submits().empty());
    f.scheduler.advance(1ms); f.h.settle();
    REQUIRE(f.evaluate.submits().size() == 1);
    REQUIRE(f.derive.submits().size() == 1);
    CHECK(f.evaluate.submits().at(0).body == R"({"sampleId":1,"specimens":[{"mass":{"num":400,"den":100,"dp":2}},{},{}]})");
    CHECK(f.derive.submits().at(0).body == R"({"body":{"sampleId":1,"specimens":[{"mass":{"num":400,"den":100,"dp":2}},{},{}]}})");
    CHECK(f.form->deriveState() == forms::DeriveState::Running);
    CHECK(f.source->marks(ui::Key{std::int64_t{1}}, "limit").muted);
    CHECK_FALSE(f.source->marks(ui::Key{std::int64_t{1}}, "mass").muted);      // an entered cell is not recomputed

    f.form->field("specimens").insertRow(1);                                  // above rows 2 and 3
    f.grid->focusCell(at(2, "note")); f.grid->type("typed"); f.grid->press("Enter");   // a commit during the round
    f.derive.resolveSubmit(0, R"({"values":[{"path":"specimens[1].limit","value":{"num":7,"den":1,"dp":2}},
                                            {"path":"specimens[1].note","value":"server"}]})");
    f.evaluate.resolveSubmit(0, R"({"sampleId":1,"specimens":[{"mass":{"num":400,"den":100,"dp":2},
                                    "density":{"num":2,"den":1,"dp":3}},{},{}],"errors":{},"ready":true})");
    f.h.settle();
    CHECK(f.json("specimens[2].limit") == R"({"num":700,"den":100,"dp":2})");   // row key 2, now at position 2
    CHECK(f.json("specimens[1].limit").empty());                                // the inserted row is untouched
    CHECK(f.json("specimens[0].density") == R"({"num":2000,"den":1000,"dp":3})");
    CHECK(f.form->field("specimens#2.note").draft() == "typed");               // not a derived member: refused
    CHECK(f.form->modified("specimens#2.note"));
    CHECK(f.evaluate.submits().size() == 2);                                    // the stale round: one more, at once
    CHECK(f.derive.submits().size() == 2);
    f.evaluate.resolveSubmit(1, R"({"errors":{},"ready":false})");
    f.derive.resolveSubmit(1, kNoValues);
    f.h.settle();
    CHECK(f.form->deriveState() == forms::DeriveState::Current);

    // A failed round keeps the values, marks them stale, and Recalculate runs one more.
    f.grid->focusCell(at(1, "mass")); f.grid->type("5"); f.grid->press("Enter");
    f.scheduler.advance(150ms); f.h.settle();
    f.evaluate.rejectSubmit(2, "unreachable");
    f.derive.resolveSubmit(2, kNoValues);
    f.h.settle();
    CHECK(f.form->deriveState() == forms::DeriveState::Failed);
    CHECK(f.source->marks(ui::Key{std::int64_t{2}}, "limit").stale);
    CHECK_FALSE(f.source->marks(ui::Key{std::int64_t{2}}, "mass").stale);
    CHECK(f.json("specimens[2].limit") == R"({"num":700,"den":100,"dp":2})");
    CHECK(f.grid->standing() == "Derived values could not be recalculated");
    CHECK(f.grid->standingEntries() == std::vector<ui::StatusEntry>{{"recalculate", "Recalculate"}});
    f.grid->chooseEntry("recalculate"); f.h.settle();
    CHECK(f.evaluate.submits().size() == 4);
    // The failure stands, and the values stay stale, while the retry runs, until a round succeeds.
    CHECK(f.grid->standing() == "Derived values could not be recalculated");
    CHECK(f.source->marks(ui::Key{std::int64_t{2}}, "limit").stale);
    f.evaluate.resolveSubmit(3, R"({"errors":{},"ready":false})");
    f.derive.resolveSubmit(3, kNoValues);
    f.h.settle();
    CHECK(f.grid->standing().empty());
    CHECK(f.grid->standingEntries().empty());
    CHECK_FALSE(f.source->marks(ui::Key{std::int64_t{2}}, "limit").stale);
}

// Mutation: noteCommit keeping the timer of the first commit (`if (!_debounce.active())`), which sends 150 ms after
// it rather than after the last commit.
TEST_CASE("grid: a commit restarts the debounce", "[ui][grid][derive]") {
    GridFixture f{2};
    f.enableDerive();
    f.grid->focusCell(at(1, "mass")); f.grid->type("1"); f.grid->press("Enter");
    f.scheduler.advance(100ms); f.h.settle();
    f.grid->type("2"); f.grid->press("Enter");                  // (2, mass), 100 ms after the first commit
    f.scheduler.advance(50ms); f.h.settle();
    CHECK(f.evaluate.submits().empty());                        // 150 ms after the first, 50 after the last
    f.scheduler.advance(100ms); f.h.settle();
    CHECK(f.evaluate.submits().size() == 1);
}

// Mutation: loadDerived(round, evaluatedBody) without its sequence check (the older body overwrites 9 with 8).
TEST_CASE("grid: an older evaluation is ignored", "[ui][grid][derive]") {
    GridFixture f{1};
    auto const older = f.form->captureRound();
    auto const newer = f.form->captureRound();
    auto const body = [](std::string_view density) {
        return std::string{R"({"sampleId":1,"specimens":[{"density":)"} + std::string{density} + "}]}";
    };
    CHECK(f.form->loadDerived(newer, std::string_view{body(R"({"num":9,"den":1,"dp":3})")}).loaded == 1);
    CHECK(f.form->loadDerived(older, std::string_view{body(R"({"num":8,"den":1,"dp":3})")}).loaded == 0);
    CHECK(f.json("specimens[0].density") == R"({"num":9000,"den":1000,"dp":3})");
}

// Mutation: loadDerived without the sequence check (the late older reply overwrites 9 with 8).
TEST_CASE("grid: an older derive reply is ignored", "[ui][grid][derive]") {
    GridFixture f{2};
    auto const older = f.form->captureRound();
    auto const newer = f.form->captureRound();
    auto const value = [](std::string json) { return forms::Derived{.values = {{"specimens[1].limit", glz::raw_json{std::move(json)}}}}; };
    CHECK(f.form->loadDerived(newer, value(R"({"num":9,"den":1,"dp":2})")).loaded == 1);
    CHECK(f.form->loadDerived(older, value(R"({"num":8,"den":1,"dp":2})")).loaded == 0);
    CHECK(f.json("specimens[1].limit") == R"({"num":900,"den":100,"dp":2})");
}

// Mutation: loadDerived resolving `specimens[i]` against the current rows. The first row is the one removed, so the
// mutation writes row 1's value onto row 2, now at position 0, and drops row 2's (a removed last row would pass for
// the wrong reason: its path falls past the end either way).
TEST_CASE("grid: a value for a row removed since the call is dropped", "[ui][grid][derive]") {
    GridFixture f{2};
    auto const round = f.form->captureRound();                 // rows 1, 2 at sending
    f.form->field("specimens").removeRow(1);                   // then row 1 goes; row 2 is at position 0
    auto const load = f.form->loadDerived(round, forms::Derived{.values = {
        {"specimens[0].limit", glz::raw_json{R"({"num":1,"den":1,"dp":2})"}},
        {"specimens[1].limit", glz::raw_json{R"({"num":3,"den":1,"dp":2})"}}}});
    CHECK(load.loaded == 1);
    CHECK(load.refused.empty());                                 // a removed row is dropped, not refused
    CHECK(f.json("specimens[0].limit") == R"({"num":300,"den":100,"dp":2})");   // row 2's own value
    CHECK(f.form->field("specimens").rows().size() == 1);
}

// Mutation: a derive reply that moves the loaded snapshot of the members it writes.
TEST_CASE("grid: a derive reply moves no snapshot", "[ui][grid][derive]") {
    GridFixture f{1};
    f.grid->focusCell(at(1, "mass")); f.grid->type("4"); f.grid->press("Enter");
    auto const round = f.form->captureRound();
    auto const load = f.form->loadDerived(round, forms::Derived{.values = {{"specimens[0].limit", glz::raw_json{R"({"num":1,"den":1,"dp":2})"}},
                                                                           {"specimens[0].mass",  glz::raw_json{R"({"num":5,"den":1,"dp":2})"}}}});
    CHECK(load.loaded == 1);
    CHECK(load.refused == std::vector<std::string>{"specimens[0].mass"});
    CHECK(f.form->modified("specimens#1.mass"));
    CHECK(f.form->field("specimens[0].mass").draft() == "4");
    CHECK_FALSE(f.form->modified("specimens#1.limit"));        // a derived member draws no modified mark
}

// Mutation: the column's field resolved once for every row.
TEST_CASE("grid: kind per row", "[ui][grid][derive]") {
    GridFixture f{morph::test::grid::GridSpec{.schema = std::string{morph::test::grid::kKindPerRow},
                                              .body = R"({"rows":[{"isChoice":true},{"isChoice":false}]})",
                                              .array = "rows",
                                              .columns = morph::test::grid::kindPerRowColumns(),
                                              .options = {}}};
    CHECK(f.source->field(ui::Key{std::int64_t{1}}, "value")->spec().kind == forms::FieldKind::Enum);
    CHECK(f.source->field(ui::Key{std::int64_t{2}}, "value")->spec().kind == forms::FieldKind::Decimal);
    f.grid->focusCell(at(2, "value")); f.grid->type("3,5"); f.grid->press("Enter");
    CHECK(f.json("rows[1].amount") == R"({"num":35,"den":10,"dp":1})");
    f.grid->focusCell(at(1, "mass")); f.grid->type("1,25"); f.grid->press("Enter");
    REQUIRE(f.form->field("rows[0].mass").error().peek().has_value());
    CHECK(f.form->field("rows[0].mass").error().peek()->code == "too-precise");
    f.grid->focusCell(at(2, "mass")); f.grid->type("1,255"); f.grid->press("Enter");
    CHECK(f.json("rows[1].mass") == R"({"num":1255,"den":1000,"dp":3})");
}

// Rows whose `grade` options depend on the row's `material` and the form's `site`.
constexpr std::string_view kChoicesPerRow = R"({"type":"object","title":"Grades","properties":{
  "site":{"type":["string","null"],"title":"Site","x-order":0},
  "rows":{"type":"array","title":"Rows","x-order":1,"items":{"type":"object","properties":{
    "material":{"type":["string","null"],"title":"Material","x-order":0},
    "grade":{"type":["string","null"],"title":"Grade","x-optionsAction":"Lims_ListGrades",
             "x-optionsDependsOn":["material","site"],"x-order":1}}}}}})";

// Rows whose `mass` is a quantity in kg, also entered in g.
constexpr std::string_view kMassUnits = R"({"type":"object","title":"Masses","properties":{
  "rows":{"type":"array","title":"Rows","x-order":0,"items":{"type":"object","properties":{
    "mass":{"type":["object","null"],"title":"Mass","x-decimalPlaces":3,"ExtUnits":{"unitAscii":"kg"},
            "x-unitAlternatives":[{"id":"g","decimals":0,"num":1,"den":1000}],"x-order":0},
    "note":{"type":["string","null"],"title":"Note","x-order":1}}}}}})";

// Mutation: the unit overlay re-applied each commit (`changed` always true in FormGridSource::applyUnit): the note's
// commit puts the row back in kg.
TEST_CASE("grid: a unit the user picks stays under the unit overlay", "[ui][grid][derive]") {
    std::vector<ui::GridColumn> columns;
    columns.push_back(ui::GridColumn{.id = "mass", .label = "Mass", .field = "mass", .value = {}, .fieldOf = {},
                                     .decimals = {},
                                     .unit = [](forms::RowState const& row) {
                                         // Read as a document's overlay reads its row: every commit reruns it.
                                         static_cast<void>(row.field("note")->encoded().get());
                                         return std::optional<std::string>{"kg"};
                                     }});
    columns.push_back(ui::GridColumn{.id = "note", .label = "Note", .field = "note", .value = {}});
    GridFixture f{morph::test::grid::GridSpec{.schema = std::string{kMassUnits},
                                              .body = R"({"rows":[{}]})",
                                              .array = "rows",
                                              .columns = std::move(columns),
                                              .options = {}}};
    forms::FieldState& mass = f.form->field("rows[0].mass");
    CHECK(mass.unit().peek() == 0);
    f.grid->focusCell(at(1, "mass")); f.grid->press("F2");
    mass.switchUnit(1);                                          // the editor's unit select: g
    f.grid->type("2000"); f.grid->press("Enter"); f.h.settle();
    f.grid->focusCell(at(1, "note")); f.grid->type("x"); f.grid->press("Enter"); f.h.settle();
    CHECK(mass.unit().peek() == 1);
    CHECK(mass.draft() == "2000");
    CHECK(f.json("rows[0].mass") == R"({"num":2000,"den":1000,"dp":3})");   // 2 kg
}

// Mutation: the overlay applied after the loaded value (`state.switchUnit(index)` in place of
// `FormSession::changeAsLoaded` on the first apply): the loaded row shows modified, and a revert returns to kg.
TEST_CASE("grid: a unit overlay's first unit is part of the loaded row", "[ui][grid][derive]") {
    std::vector<ui::GridColumn> columns;
    columns.push_back(ui::GridColumn{.id = "mass", .label = "Mass", .field = "mass", .value = {}, .fieldOf = {},
                                     .decimals = {},
                                     .unit = [](forms::RowState const&) { return std::optional<std::string>{"g"}; }});
    columns.push_back(ui::GridColumn{.id = "note", .label = "Note", .field = "note", .value = {}});
    GridFixture f{morph::test::grid::GridSpec{.schema = std::string{kMassUnits},
                                              .body = R"({"rows":[{"mass":{"num":2,"den":1,"dp":3}}]})",
                                              .array = "rows",
                                              .columns = std::move(columns),
                                              .options = {}}};
    f.h.settle();
    forms::FieldState& mass = f.form->field("rows[0].mass");
    CHECK(mass.unit().peek() == 1);                                // shown in g, converted as the unit select does
    CHECK(mass.draft() == "2.000");
    CHECK_FALSE(f.form->modified("rows#1.mass"));
    CHECK_FALSE(f.source->rowModified(ui::Key{std::int64_t{1}}));
    CHECK_FALSE(enabledOf(entry(f.grid->cellEntries(ui::Key{std::int64_t{1}}, "mass"), "Revert cell")));
    mass.switchUnit(0);                                            // the user enters it in kg
    CHECK(f.form->modified("rows#1.mass"));
    f.form->revert("rows#1.mass");
    CHECK(mass.unit().peek() == 1);                                // back to the overlay's g
    CHECK(mass.draft() == "2.000");
    CHECK_FALSE(f.form->modified("rows#1.mass"));
}

// Mutation: a remembered "no unit" counting as a unit given (`first` only when nothing is remembered): the g the
// overlay gives once the note asks for it makes the loaded mass modified.
TEST_CASE("grid: a unit overlay that first gives no unit applies its first unit as loaded", "[ui][grid][derive]") {
    std::vector<ui::GridColumn> columns;
    columns.push_back(ui::GridColumn{.id = "mass", .label = "Mass", .field = "mass", .value = {}, .fieldOf = {},
                                     .decimals = {},
                                     .unit = [](forms::RowState const& row) {
                                         return row.field("note")->draft() == "g" ? std::optional<std::string>{"g"}
                                                                                  : std::optional<std::string>{};
                                     }});
    columns.push_back(ui::GridColumn{.id = "note", .label = "Note", .field = "note", .value = {}});
    GridFixture f{morph::test::grid::GridSpec{.schema = std::string{kMassUnits},
                                              .body = R"({"rows":[{"mass":{"num":2,"den":1,"dp":3}}]})",
                                              .array = "rows",
                                              .columns = std::move(columns),
                                              .options = {}}};
    f.h.settle();
    forms::FieldState& mass = f.form->field("rows[0].mass");
    CHECK(mass.unit().peek() == 0);                                // no unit given: kg
    f.grid->focusCell(at(1, "note")); f.grid->type("g"); f.grid->press("Enter"); f.h.settle();
    CHECK(mass.unit().peek() == 1);
    CHECK_FALSE(f.form->modified("rows#1.mass"));
    f.form->revert("rows#1.mass");
    CHECK(mass.unit().peek() == 1);
}

// The specimens form, submitted explicitly.
std::string explicitSpecimens() {
    std::string schema{morph::test::grid::kSpecimens};
    std::string_view const title = R"("title":"Specimens",)";
    schema.replace(schema.find(title), title.size(), std::string{title} + R"("x-submitMode":"explicit",)");
    return schema;
}

// Mutation: a snapshot that counts computed and derived members (the round's limit makes the collection differ from
// what the save sent: the edited mass stays modified after the save, and the refetch skips the collection).
TEST_CASE("grid: a save that a derive round overlaps ends the edit", "[ui][grid][derive]") {
    for (bool const inFlight : {false, true}) {
        DYNAMIC_SECTION((inFlight ? "a submit while the round is in flight" : "a submit inside the debounce")) {
            GridFixture f{morph::test::grid::GridSpec{.schema = explicitSpecimens(),
                                                      .body = morph::test::grid::blankSpecimens(1),
                                                      .array = "specimens",
                                                      .columns = {},
                                                      .options = {}}};
            f.enableDerive();
            f.h.server.replyWith([](morph::test::formsengine::Call const&) { return std::optional<std::string>{}; });
            f.grid->focusCell(at(1, "mass")); f.grid->type("4"); f.grid->press("Enter"); f.h.settle();
            if (inFlight) {
                f.scheduler.advance(150ms); f.h.settle();
            }
            f.form->submit(); f.h.settle();
            if (!inFlight) {
                f.scheduler.advance(150ms); f.h.settle();
            }
            REQUIRE(f.derive.submits().size() == 1);
            REQUIRE(f.h.server.submits().size() == 1);
            f.derive.resolveSubmit(0, R"({"values":[{"path":"specimens[0].limit","value":{"num":7,"den":1,"dp":2}}]})");
            f.evaluate.resolveSubmit(0, R"({"errors":{},"ready":true})");
            f.h.settle();
            CHECK(f.json("specimens[0].limit") == R"({"num":700,"den":100,"dp":2})");
            f.h.server.resolveSubmit(0, R"({"ok":true})");
            f.h.settle();
            CHECK_FALSE(f.form->modified("specimens#1.mass"));
            CHECK(f.source->modifiedCount() == 0);
            f.form->refill(R"({"sampleId":1,"specimens":[{"mass":{"num":9,"den":1,"dp":2}}]})");
            f.h.settle();
            CHECK(f.json("specimens[0].mass") == R"({"num":900,"den":100,"dp":2})");
        }
    }
}

// Mutation: as above (a snapshot that counts computed and derived members: once the row is reverted the collection
// still differs from its loaded snapshot by the round's limit, so the refetch skips it).
TEST_CASE("grid: a derive round in automatic mode leaves the collection loaded", "[ui][grid][derive]") {
    GridFixture f{1};                                              // automatic: the commit saves at once
    f.enableDerive();
    f.grid->focusCell(at(1, "mass")); f.grid->type("4"); f.grid->press("Enter"); f.h.settle();
    REQUIRE(f.h.server.submits().size() == 1);
    f.scheduler.advance(150ms); f.h.settle();
    f.derive.resolveSubmit(0, R"({"values":[{"path":"specimens[0].limit","value":{"num":7,"den":1,"dp":2}}]})");
    f.evaluate.resolveSubmit(0, R"({"errors":{},"ready":true})");
    f.h.settle();
    // An automatic form's saved collection stays an edit (forms/engine.md): the typed mass is still modified.
    CHECK(f.form->modified("specimens#1.mass"));
    CHECK_FALSE(f.form->modified("specimens#1.limit"));
    f.form->revert("specimens#1");
    f.h.settle();
    CHECK(f.source->modifiedCount() == 0);
    CHECK(f.json("specimens[0].limit") == R"({"num":700,"den":100,"dp":2})");   // loaded with the round's value
    f.form->refill(R"({"sampleId":1,"specimens":[{"mass":{"num":9,"den":1,"dp":2}}]})");
    f.h.settle();
    CHECK(f.json("specimens[0].mass") == R"({"num":900,"den":100,"dp":2})");
}

// Mutation: a fetch per row (no shared fetch).
TEST_CASE("grid: choices per row", "[ui][grid][derive]") {
    GridFixture f{morph::test::grid::GridSpec{
        .schema = std::string{kChoicesPerRow},
        .body = R"({"site":"north","rows":[{"material":"A"},{"material":"A"},{"material":"B"},{"material":"B"},)"
                R"({"material":"C"},{"material":"A"}]})",
        .array = "rows",
        .columns = {},
        .options = {}}};
    f.h.settle();
    CHECK(f.h.server.fetches().empty());                     // nothing is fetched before an editor opens
    for (std::int64_t key = 1; key <= 6; ++key) {
        f.grid->focusCell(at(key, "grade")); f.grid->press("F2"); f.h.settle(); f.grid->press("Escape");
    }
    REQUIRE(f.h.server.fetches().size() == 3);
    CHECK(f.h.server.fetches().at(0).body == R"({"material":"A","site":"north"})");
    CHECK(f.h.server.fetches().at(1).body == R"({"material":"B","site":"north"})");
    CHECK(f.h.server.fetches().at(2).body == R"({"material":"C","site":"north"})");
}

// ── Selecting cells, paste and fill, rows of a grid ─────────────────────────

using morph::test::grid::descriptionOf;
using morph::test::grid::kSpecimens;

// Mutation: CellRange stored as view positions (the sort moves the corners onto other rows).
TEST_CASE("grid: cell selection", "[ui][grid][cells]") {
    GridFixture f{3};
    for (auto const& [key, text] : {std::pair{1, "3"}, {2, "1"}, {3, "2"}}) {
        f.grid->focusCell(at(key, "mass")); f.grid->type(text); f.grid->press("Enter");
    }
    f.grid->focusCell(at(1, "mass"));
    f.grid->press("Shift+Down"); f.grid->press("Shift+Right");
    CHECK(f.grid->selection() == ui::CellRange{at(1, "mass"), at(2, "volume")});
    REQUIRE(f.source->view().setSort({table::SortKey{.column = "mass"}}).has_value());
    CHECK(f.grid->selection() == ui::CellRange{at(1, "mass"), at(2, "volume")});   // corners kept by key
    CHECK(f.grid->copy() == "1,00\t\n2,00\t\n3,00\t");    // rows 2, 3, 1 now lie between the corners' positions
}

// Mutation: the loader accepting selectionMode "cells" on a model-backed table (loadDocumentText succeeds).
TEST_CASE("grid: cells on a model's table are refused when the document loads", "[ui][grid][cells]") {
    ui::testing::TableHarness h;
    std::string const text = ui::testing::withTableKey(ui::testing::kModelTable, R"("selectionMode":"cells")");
    auto const loaded = ui::loadDocumentText(text, ui::LoadOptions{.catalog = h.session.options().catalog});
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().reason.find("selectionMode") != std::string::npos);
}

// Mutation: a paste that shifts a value past a read-only cell into the next editable one.
TEST_CASE("grid: paste, fill, clear", "[ui][grid][cells]") {
    std::string schema{kSpecimens};
    schema.replace(schema.find(R"("maxItems":5)"), 12, R"("maxItems":2)");
    GridFixture f{1, ui::GridOptions{.appendRows = true}, schema};
    f.enableDerive();
    f.grid->focusCell(at(1, "volume"));                       // columns: volume, density (read-only), limit (read-only), note
    auto const result = f.grid->paste("1\t2\t3\ta\n4\t5\t6\tb\n7\t8\t9\tc");   // 3 rows × 4 columns
    CHECK(result.written == 4);                                // volume and note of two rows
    CHECK(result.readOnly == 4);
    CHECK(result.pastRows == 4);
    CHECK(f.grid->message() == "Pasted 4 values; 4 on read-only cells and 4 past the last row were dropped");
    CHECK(f.form->field("specimens[0].note").draft() == "a");  // the block keeps its shape
    CHECK(f.form->field("specimens[1].volume").draft() == "4");
    CHECK(f.form->field("specimens").rows().size() == 2);     // one row added, to maxItems
    f.scheduler.advance(150ms); f.h.settle();
    CHECK(f.evaluate.submits().size() == 1);                   // one paste, one round
    f.grid->select(ui::CellRange{at(1, "volume"), at(2, "note")});
    f.grid->fillDown();
    CHECK(f.form->field("specimens[1].volume").draft() == "1");
    CHECK(f.form->field("specimens[1].note").draft() == "a");
    f.grid->clear();
    CHECK(f.form->field("specimens[0].volume").draft().empty());
    CHECK(f.form->field("specimens[1].note").draft().empty());
    CHECK(f.form->field("specimens[0].density").draft().empty());   // read-only: never written
}

// Review Focus 1, paste half. Mutation: an empty grid's paste starting nowhere and appending nothing.
TEST_CASE("grid: paste into an empty grid", "[ui][grid][cells]") {
    SECTION("with appendRows") {
        GridFixture f{0, ui::GridOptions{.appendRows = true}};
        auto const result = f.grid->paste("x\t1\ny\t2\nz\t3");
        CHECK(result.written == 6);
        CHECK(f.form->field("specimens").rows().size() == 3);
        CHECK(f.form->field("specimens[2].label").draft() == "z");
        CHECK(f.form->field("specimens[2].mass").draft() == "3");
    }
    SECTION("without appendRows") {
        GridFixture f{0};
        auto const result = f.grid->paste("x\t1\ny\t2");
        CHECK(result.written == 0);
        CHECK(result.pastRows == 4);
        CHECK(f.form->field("specimens").rows().empty());
        CHECK(f.grid->message() == "Pasted 0 values; 4 past the last row were dropped");
    }
}

// Review Focus 3. Mutation: lines split on "\n" only, keeping "\r" and the trailing empty line.
TEST_CASE("grid: spreadsheet clipboard text pastes as typed values", "[ui][grid][cells]") {
    GridFixture f{2, ui::GridOptions{.appendRows = true}};
    f.grid->focusCell(at(1, "mass"));
    auto const result = f.grid->paste(" 12,5 \t1.234,5\r\n3\t4\r\n");
    CHECK(result.written == 4);
    CHECK(result.pastRows == 0);
    CHECK(f.form->field("specimens").rows().size() == 2);    // no third row from the trailing line break
    CHECK(f.json("specimens[0].mass") == R"({"num":1250,"den":100,"dp":2})");
    CHECK(f.json("specimens[0].volume") == R"({"num":12345,"den":10,"dp":1})");
    CHECK(f.form->field("specimens[1].volume").draft() == "4");
}

// Mutation: the maxPasteCells check left out (the block is written).
TEST_CASE("grid: a paste over maxPasteCells is refused whole", "[ui][grid][cells]") {
    GridFixture f{1, ui::GridOptions{.maxPasteCells = 3}};
    f.grid->focusCell(at(1, "label"));
    auto const result = f.grid->paste("a\tb\nc\td");
    CHECK(result.written == 0);
    CHECK(f.form->field("specimens[0].label").draft().empty());
    CHECK(f.grid->message() == "A paste holds at most 3 values");
    CHECK(ui::GridOptions{}.maxPasteCells == 10'000);
}

// Mutation: an insertion not checking maxItems (Insert stays offered at the bound).
TEST_CASE("grid: rows of a grid", "[ui][grid][rows]") {
    std::string schema{kSpecimens};
    schema.replace(schema.find(R"("minItems":0)"), 12, R"("minItems":1)");
    schema.replace(schema.find(R"("maxItems":5)"), 12, R"("maxItems":2)");
    GridFixture f{1, {}, schema};
    f.grid->focusCell(at(1, "label"));
    CHECK_FALSE(f.grid->rowOps().remove);
    CHECK(f.grid->rowOps().why == "A grid holds at least 1 rows");
    f.grid->press("Ctrl+Shift+=");
    CHECK(f.form->field("specimens").rows().size() == 2);
    CHECK_FALSE(f.grid->rowOps().insert);
    CHECK(f.grid->rowOps().why == "A grid holds at most 2 rows");
    f.grid->type("kept"); f.grid->press("Enter");             // (1, label)
    f.grid->focusCell(at(1, "label"));
    f.grid->press("Ctrl+-");
    CHECK(f.form->field("specimens").rows().size() == 1);
    CHECK(f.grid->message() == "Deleted 1 row");
    CHECK(f.grid->messageEntries() == std::vector<ui::StatusEntry>{{"undo", "Undo"}});
    f.grid->chooseEntry("undo");
    CHECK(f.form->field("specimens[0].label").draft() == "kept");
    SECTION("rowsFixed offers none") {
        GridFixture fixed{2, ui::GridOptions{.rowsFixed = true}};
        CHECK_FALSE(fixed.grid->rowOps().insert);
        CHECK_FALSE(fixed.grid->rowOps().remove);
        fixed.grid->focusCell(at(2, "note"));
        CHECK(fixed.grid->press("Tab") == ui::KeyOutcome::FocusNext);
    }
    SECTION("appendRows adds a row on Tab past the end") {
        GridFixture grow{1, ui::GridOptions{.appendRows = true}};
        grow.grid->focusCell(at(1, "note"));
        CHECK(grow.grid->press("Tab") == ui::KeyOutcome::Handled);
        CHECK(grow.form->field("specimens").rows().size() == 2);
        CHECK(grow.grid->focus() == at(2, "label"));
    }
}

// Mutation: FormGridSource::undoRemove restoring after a change (the row edited since comes back beside its copy).
TEST_CASE("grid: undo does nothing once a change followed the deletion", "[ui][grid][rows]") {
    GridFixture f{2};
    f.grid->focusCell(at(1, "label")); f.grid->type("a"); f.grid->press("Enter");
    f.grid->focusCell(at(1, "label"));
    f.grid->press("Ctrl+-");
    REQUIRE(f.grid->messageEntries() == std::vector<ui::StatusEntry>{{"undo", "Undo"}});
    f.grid->focusCell(at(2, "label")); f.grid->type("b"); f.grid->press("Enter");
    CHECK(f.grid->messageEntries().empty());                   // the next change ends the offer
    f.grid->chooseEntry("undo");
    CHECK(f.form->field("specimens").rows().size() == 1);
    CHECK(f.form->field("specimens[0].label").draft() == "b");
}

// Mutation: the row entries offered with no bound check (Insert enabled at maxItems).
TEST_CASE("grid: row entries in the cell's menu and Add row", "[ui][grid][rows]") {
    std::string schema{kSpecimens};
    schema.replace(schema.find(R"("maxItems":5)"), 12, R"("maxItems":3)");
    GridFixture f{2, ui::GridOptions{.appendRows = true}, schema};
    auto entries = f.grid->cellEntries(ui::Key{std::int64_t{1}}, "label");
    CHECK(labelsOf(entries) == std::vector<std::string>{"Revert cell", "Revert row", "", "Insert row above",
                                                        "Insert row below", "Delete row"});   // "" the separator
    entry(entries, "Insert row above").onSelect();
    CHECK(keysOf(*f.source).size() == 3);
    CHECK(keysOf(*f.source).at(1) == ui::Key{std::int64_t{1}});   // the new row stands above row 1
    entries = f.grid->cellEntries(ui::Key{std::int64_t{1}}, "label");
    CHECK_FALSE(enabledOf(entry(entries, "Insert row below")));
    CHECK(descriptionOf(entry(entries, "Insert row below")) == "A grid holds at most 3 rows");
    f.grid->select(ui::CellRange{at(1, "label"), at(2, "mass")});
    entries = f.grid->cellEntries(ui::Key{std::int64_t{1}}, "label");
    entry(entries, "Delete rows").onSelect();
    CHECK(keysOf(*f.source).size() == 1);
    SECTION("rowsFixed offers no row entry") {
        GridFixture fixed{2, ui::GridOptions{.rowsFixed = true}};
        CHECK(labelsOf(fixed.grid->cellEntries(ui::Key{std::int64_t{1}}, "label")) ==
              std::vector<std::string>{"Revert cell", "Revert row"});
    }
}

// Mutation: insertRow calling committed() once per column (several changes for one insertion; the debounce folds
// their rounds into one, so the changes are counted at FormGridSource::onCommitted).
TEST_CASE("grid: each row operation is one change", "[ui][grid][rows]") {
    GridFixture f{3};
    std::size_t changes = 0;
    f.source->onCommitted([&changes] { ++changes; });
    f.enableDerive();
    f.grid->focusCell(at(1, "label"));
    f.grid->insertRow(true);
    CHECK(changes == 1);
    f.scheduler.advance(150ms); f.h.settle();
    CHECK(f.evaluate.submits().size() == 1);
    f.evaluate.resolveSubmit(0, R"({"errors":{},"ready":false})"); f.derive.resolveSubmit(0, kNoValues); f.h.settle();
    f.grid->focusCell(at(1, "label")); f.grid->type("x"); f.grid->press("Enter");
    f.scheduler.advance(150ms); f.h.settle();
    f.evaluate.resolveSubmit(1, R"({"errors":{},"ready":false})"); f.derive.resolveSubmit(1, kNoValues); f.h.settle();
    f.grid->select(ui::CellRange{at(1, "label"), at(4, "label")});   // view [1, 4, 2, 3]: rows 1 and 4
    f.grid->deleteRows();                                      // two rows, one holding a value
    CHECK(changes == 3);
    f.scheduler.advance(150ms); f.h.settle();
    CHECK(f.evaluate.submits().size() == 3);
    f.evaluate.resolveSubmit(2, R"({"errors":{},"ready":false})"); f.derive.resolveSubmit(2, kNoValues); f.h.settle();
    f.grid->chooseEntry("undo");
    CHECK(changes == 4);
    f.scheduler.advance(150ms); f.h.settle();
    CHECK(f.evaluate.submits().size() == 4);
}

// ── In a document ───────────────────────────────────────────────────────────

constexpr std::string_view kGridDocument = R"({"vocab":"ui/1","id":"specimens",
  "models":{"sample":{"type":"Lims_SampleModel"}},
  "view":{"kind":"column","children":[
    {"kind":"form","id":"rec","model":"sample","action":"Lims_RecordSpecimens","submit":"explicit"},
    {"kind":"textInput","id":"before","testId":"before"},
    {"kind":"table","id":"grid","testId":"grid","form":"rec","field":"specimens",
     "columns":[{"id":"mass","field":"mass"},{"id":"note","field":"note"}]},
    {"kind":"textInput","id":"after","testId":"after"}]}})";

// The document above, mounted with sampleId 4 and two blank specimens (keys 1 and 2). Its locale is `en`; the grid's
// columns by index into `Table::columns` are mass 0 and note 1.
struct GridScreen {
    GridScreen() {
        ui::testing::addAction(h, "Lims_RecordSpecimens", std::string{morph::test::grid::kSpecimens},
                               ui::ActionInfo{.modelType = "Lims_SampleModel"});
        screen = h.mount(kGridDocument);
        ui::testing::elementOf(*screen, "rec").form->prefill(R"({"sampleId":4,"specimens":[{},{}]})");
        h.settle();
        table = h.byTestId("Table", "grid");
    }

    [[nodiscard]] ui::Value value(std::string_view name) const { return ui::testing::elementValue(h, "grid", name); }

    [[nodiscard]] int row(std::int64_t key) {
        std::optional<int> const found = h.backend.tableRow(table, ui::Key{key});
        REQUIRE(found.has_value());
        return *found;
    }

    [[nodiscard]] forms::FormSession& form() { return *ui::testing::elementOf(*screen, "rec").form; }

    ui::testing::TableHarness h;
    std::unique_ptr<ui::Screen> screen;
    int table = 0;
};

// Mutations: FormSession::submit not running its before-submit hooks (the editor is still open after the click:
// `editing` is not null); FormSession reading no field errors from the reply's `errors` (errorCount is 0).
TEST_CASE("grid: submission commits the open editor and places the reply's field errors", "[ui][grid][interpreter]") {
    GridScreen g;
    g.h.backend.clickCell(g.table, 0, 0);
    g.h.settle();
    g.h.backend.typeInto(g.table, "7.5");
    g.h.settle();
    REQUIRE(g.value("editing") == ui::testing::value(R"({"key":1,"column":"mass"})"));
    g.h.backend.click(g.h.byTestId("Button", "form_submit"));
    g.h.settle();
    CHECK(g.value("editing") == ui::Value{});
    CHECK(g.h.backend.prop(g.row(1), "editor:0").empty());
    auto& call = g.h.dispatcher.calls.at(g.h.dispatcher.last("Lims_RecordSpecimens"));
    CHECK(call.body.find(R"("mass":{"num":750,"den":100,"dp":2})") != std::string::npos);
    g.h.dispatcher.reply(g.h.dispatcher.last("Lims_RecordSpecimens"),
                         R"({"errors":{"specimens[1].mass":"must be positive","nosuch":"ignored"}})");
    g.h.settle();
    CHECK(g.value("errorCount") == ui::Value{std::int64_t{1}});
    CHECK(g.h.backend.cellAccent(g.table, ui::Key{std::int64_t{2}}, 0).tooltip == "must be positive");
    CHECK(g.h.backend.cellAccent(g.table, ui::Key{std::int64_t{1}}, 0).tooltip.empty());
    // A reply that names field errors did not save: the cell sent stays an edit.
    CHECK(g.value("modifiedCount") == ui::Value{std::int64_t{1}});
}

// Mutation: FormSession taking a refusal's sentence arguments as field errors by their names (errorCount is 1 and the
// cell's tooltip is the argument's value).
TEST_CASE("grid: a refusal's argument named like a field is not a field error", "[ui][grid][interpreter]") {
    GridScreen g;
    g.h.backend.click(g.h.byTestId("Button", "form_submit"));
    g.h.settle();
    auto& call = g.h.dispatcher.calls.at(g.h.dispatcher.last("Lims_RecordSpecimens"));
    call.answered = true;
    call.promise.reject(std::make_exception_ptr(model::ActionRefusal{
        "lims.specimens.invalid", {model::RefusalArg{.name = "specimens[1].mass", .value = "must be positive"}},
        "refused"}));
    g.h.settle();
    CHECK(g.value("errorCount") == ui::Value{std::int64_t{0}});
    CHECK(g.h.backend.cellAccent(g.table, ui::Key{std::int64_t{2}}, 0).tooltip.empty());
}

// Mutations: the lowering leaving out the cell menu's built-in entries (the menu starts at "Revert cell"); the grid's
// columns given no filter target (the filter entries are disabled, and row 1 stays in view); the grid's filters not
// held (the table menu's "Clear all filters" stays disabled, and row 1 stays out of view); the grid's element values
// not read from its configuring (`filters` is null).
TEST_CASE("grid: the cell menu offers copy and filter before the grid's own entries", "[ui][grid][interpreter]") {
    GridScreen g;
    g.form().field("specimens[0].note").text().set("a");
    g.form().field("specimens[1].note").text().set("b");
    g.h.settle();
    std::vector<ui::MenuEntry> const entries = g.h.backend.rightClickCell(g.table, ui::Key{std::int64_t{1}}, 1);
    std::vector<std::string> labels;
    for (ui::MenuEntry const& shown : entries) {
        labels.push_back(shown.label);
    }
    CHECK(labels == std::vector<std::string>{"Copy", "Copy with headers", "Filter by this value", "Exclude this value",
                                             "Revert cell", "Revert row", "", "Insert row above", "Insert row below",
                                             "Delete row"});
    g.h.backend.chooseCellMenu(g.table, ui::Key{std::int64_t{1}}, 1, {"Copy with headers"});
    g.h.settle();
    CHECK(g.h.backend.clipboardText() == "note\na");
    g.h.backend.chooseCellMenu(g.table, ui::Key{std::int64_t{1}}, 1, {"Exclude this value"});
    g.h.settle();
    CHECK_FALSE(g.h.backend.tableRow(g.table, ui::Key{std::int64_t{1}}).has_value());
    CHECK(g.h.backend.tableRow(g.table, ui::Key{std::int64_t{2}}).has_value());
    // The filter shows on its column's header, and the table menu clears it.
    CHECK(g.h.backend.prop(g.table, "filtered") == "[1]");
    CHECK(g.value("filters").field("columns") != nullptr);
    CHECK(g.value("filters").field("columns")->field("note") != nullptr);
    CHECK(g.h.backend.prop(g.table, "columns").find("filterable=true") != std::string::npos);
    g.h.backend.chooseMenu(g.table, std::nullopt, {"Clear all filters"});
    g.h.settle();
    CHECK(g.h.backend.tableRow(g.table, ui::Key{std::int64_t{1}}).has_value());
    CHECK(g.h.backend.prop(g.table, "filtered") == "[]");
}

constexpr std::string_view kMenuGridDocument = R"({"vocab":"ui/1","id":"specimens",
  "models":{"sample":{"type":"Lims_SampleModel"}},
  "state":{"picked":{"init":null}},
  "view":{"kind":"column","children":[
    {"kind":"form","id":"rec","model":"sample","action":"Lims_RecordSpecimens","submit":"explicit"},
    {"kind":"table","id":"grid","testId":"grid","form":"rec","field":"specimens",
     "columns":[{"id":"mass","field":"mass"},{"id":"note","field":"note","sortable":true}],
     "cellMenu":[{"label":"Pick","onClick":[{"set":"picked","value":{"ref":"column"}}]}]},
    {"kind":"text","testId":"picked","text":{"fn":"coalesce","args":[{"ref":"picked"},"-"]}}]}})";

// Mutations: the cell menu leaving out the document's `cellMenu` parts (no separator and no "Pick"); the grid's sort
// not given to its rows (after the header click, view row 0 still holds key 1).
TEST_CASE("grid: the document's cell menu entries follow a separator after the grid's, and its header sorts",
          "[ui][grid][interpreter]") {
    ui::testing::TableHarness h;
    ui::testing::addAction(h, "Lims_RecordSpecimens", std::string{morph::test::grid::kSpecimens},
                           ui::ActionInfo{.modelType = "Lims_SampleModel"});
    auto const screen = h.mount(kMenuGridDocument);
    ui::testing::elementOf(*screen, "rec").form->prefill(R"({"sampleId":4,"specimens":[{},{}]})");
    h.settle();
    int const table = h.byTestId("Table", "grid");
    std::vector<std::string> labels;
    for (ui::MenuEntry const& shown : h.backend.rightClickCell(table, ui::Key{std::int64_t{1}}, 1)) {
        labels.push_back(shown.separator ? std::string{"--"} : shown.label);
    }
    CHECK(labels == std::vector<std::string>{"Copy", "Copy with headers", "Filter by this value", "Exclude this value",
                                             "Revert cell", "Revert row", "--", "Insert row above", "Insert row below",
                                             "Delete row", "--", "Pick"});
    h.backend.chooseCellMenu(table, ui::Key{std::int64_t{1}}, 1, {"Pick"});
    h.settle();
    CHECK(h.text("picked") == "note");
    forms::FormSession& form = *ui::testing::elementOf(*screen, "rec").form;
    form.field("specimens[0].note").text().set("b");
    form.field("specimens[1].note").text().set("a");
    h.settle();
    h.backend.clickHeader(table, 1);
    h.settle();
    h.backend.clickCell(table, 0, 1);
    h.settle();
    h.backend.typeInto(table, "c");
    h.settle();
    CHECK(ui::testing::elementValue(h, "grid", "editing") == ui::testing::value(R"({"key":2,"column":"note"})"));
}

constexpr std::string_view kDisplayGridDocument = R"({"vocab":"ui/1","id":"specimens",
  "models":{"sample":{"type":"Lims_SampleModel"}},
  "view":{"kind":"column","children":[
    {"kind":"form","id":"rec","model":"sample","action":"Lims_RecordSpecimens","submit":"explicit"},
    {"kind":"table","id":"grid","testId":"grid","form":"rec","field":"specimens",
     "layout":{"lit":{"order":["mass","label","shown","note"],"hidden":["label"],"pinned":0,"widths":{}}},
     "columns":[{"id":"mass","field":"mass"},{"id":"label","field":"label"},
                {"id":"shown","label":"Shown","value":{"fn":"concat","args":["n:",{"ref":"row.note"}]}},
                {"id":"note","field":"note"}]}]}})";

// Mutations: the lowering never feeding the layout to FormGridSource::setLayout (Tab from mass lands on the hidden
// label); the grid's element values not read from its configuring (`visibleColumns` is null); a display column's value read with no `row` bound (the cell is empty).
TEST_CASE("grid: a display column shows its value, and the walk skips it and hidden columns",
          "[ui][grid][interpreter]") {
    ui::testing::TableHarness h;
    ui::testing::addAction(h, "Lims_RecordSpecimens", std::string{morph::test::grid::kSpecimens},
                           ui::ActionInfo{.modelType = "Lims_SampleModel"});
    auto const screen = h.mount(kDisplayGridDocument);
    forms::FormSession& form = *ui::testing::elementOf(*screen, "rec").form;
    form.prefill(R"({"sampleId":4,"specimens":[{"note":"x"},{}]})");
    h.settle();
    int const table = h.byTestId("Table", "grid");
    std::optional<int> const first = h.backend.tableRow(table, ui::Key{std::int64_t{1}});
    REQUIRE(first.has_value());
    std::string shown;
    for (int const cell : h.backend.children(*first)) {
        if (h.backend.prop(cell, "column") == "2") {
            shown = h.backend.prop(cell, "text");
        }
    }
    CHECK(shown == "n:x");
    h.backend.clickCell(table, 0, 0);   // (1, mass)
    h.settle();
    h.backend.press(table, "Tab");
    h.settle();
    CHECK(ui::testing::elementValue(h, "grid", "editing") == ui::Value{});
    CHECK(h.backend.focusCell(table) == std::pair{std::size_t{0}, std::size_t{3}});   // (1, note)
    CHECK(ui::testing::elementValue(h, "grid", "visibleColumns") == ui::testing::value(R"(["mass","shown","note"])"));
    CHECK(ui::testing::elementValue(h, "grid", "hiddenCount") == ui::Value{std::int64_t{1}});
}

// Mutation: the loader taking a grid column with neither `field` nor `value` (the document loads).
TEST_CASE("grid: a column with neither a field nor a value is refused when the document loads",
          "[ui][grid][interpreter]") {
    ui::testing::TableHarness h;
    ui::testing::addAction(h, "Lims_RecordSpecimens", std::string{morph::test::grid::kSpecimens},
                           ui::ActionInfo{.modelType = "Lims_SampleModel"});
    std::string text{kGridDocument};
    text.replace(text.find(R"({"id":"note","field":"note"})"), std::string_view{R"({"id":"note","field":"note"})"}.size(),
                 R"({"id":"note"})");
    auto const loaded = ui::loadDocumentText(text, ui::LoadOptions{.catalog = h.session.options().catalog});
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().reason.find("`field`") != std::string::npos);
}

constexpr std::string_view kOverlayGridDocument = R"({"vocab":"ui/1","id":"specimens",
  "models":{"sample":{"type":"Lims_SampleModel"}},
  "state":{"picked":{"init":null}},
  "view":{"kind":"column","children":[
    {"kind":"form","id":"rec","model":"sample","action":"Lims_RecordSpecimens","submit":"explicit",
     "fields":{"specimens":{"widget":"grid","grid":{"walk":"columns",
       "columns":[{"id":"mass","label":"Mass","field":"mass"},{"id":"density","label":"Density","field":"density"},
                  {"id":"note","label":"Note","field":"note"}],
       "layout":{"lit":{"order":["mass","density","note"],"hidden":["note"],"pinned":0,"widths":{}}},
       "cellMenu":[{"label":"Pick","onClick":[{"set":"picked","value":{"ref":"column"}}]}],
       "onActivate":[{"set":"picked","value":{"ref":"event"}}]}}}},
    {"kind":"text","testId":"picked","text":{"fn":"coalesce","args":[{"ref":"picked"},"-"]}}]}})";

// Mutations: the form's lowering ignoring a field overlay's `grid` part (the grid shows every member); the grid's
// `onActivate` not lowered (a double click on the read-only density leaves `picked` empty); the overlay's grid drawn
// without its table's configuring (no arrangement hides `note`, and the cell menu has no "Pick").
TEST_CASE("grid: a field overlay's grid part gives the grid its columns, its onActivate and a table's keys",
          "[ui][grid][interpreter]") {
    ui::testing::TableHarness h;
    ui::testing::addAction(h, "Lims_RecordSpecimens", std::string{morph::test::grid::kSpecimens},
                           ui::ActionInfo{.modelType = "Lims_SampleModel"});
    auto const screen = h.mount(kOverlayGridDocument);
    ui::testing::elementOf(*screen, "rec").form->prefill(R"({"sampleId":4,"specimens":[{},{}]})");
    h.settle();
    std::vector<int> const tables = h.backend.all("Table");
    REQUIRE(tables.size() == 1);
    int const table = tables.front();
    CHECK(h.backend.headerText(table, 0) == "Mass");
    CHECK(h.backend.headerText(table, 1) == "Density");
    CHECK(h.backend.headerText(table, 2) == "Note");
    CHECK(h.backend.headerText(table, 3).empty());
    h.backend.clickCell(table, 1, 1, true);   // (2, density), read-only
    h.settle();
    CHECK(h.text("picked") == "2");
    // Its layout hides `note`.
    CHECK(h.backend.headerShown(table, 1));
    CHECK_FALSE(h.backend.headerShown(table, 2));
    std::vector<std::string> labels;
    for (ui::MenuEntry const& shown : h.backend.rightClickCell(table, ui::Key{std::int64_t{1}}, 0)) {
        labels.push_back(shown.separator ? std::string{"--"} : shown.label);
    }
    CHECK(labels.back() == "Pick");
    CHECK(std::ranges::contains(labels, std::string{"Copy"}));
    h.backend.chooseCellMenu(table, ui::Key{std::int64_t{1}}, 0, {"Pick"});
    h.settle();
    CHECK(h.text("picked") == "mass");
}

// Mutation: the loader taking a field overlay's `grid` part on a field that is not drawn as a grid (it loads).
TEST_CASE("grid: a field overlay's grid part needs the grid widget", "[ui][grid][interpreter]") {
    ui::testing::TableHarness h;
    ui::testing::addAction(h, "Lims_RecordSpecimens", std::string{morph::test::grid::kSpecimens},
                           ui::ActionInfo{.modelType = "Lims_SampleModel"});
    std::string text{kOverlayGridDocument};
    text.replace(text.find(R"("widget":"grid",)"), std::string_view{R"("widget":"grid",)"}.size(), "");
    auto const loaded = ui::loadDocumentText(text, ui::LoadOptions{.catalog = h.session.options().catalog});
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().reason.find("widget") != std::string::npos);
}

constexpr std::string_view kCellsGridDocument = R"({"vocab":"ui/1","id":"specimens",
  "models":{"sample":{"type":"Lims_SampleModel"}},
  "state":{"picked":{"init":null}},
  "view":{"kind":"column","children":[
    {"kind":"form","id":"rec","model":"sample","action":"Lims_RecordSpecimens","submit":"explicit"},
    {"kind":"table","id":"grid","testId":"grid","form":"rec","field":"specimens","selectionMode":"cells",
     "appendRows":true,"onSelectionChange":[{"set":"picked","value":{"ref":"event"}}],
     "columns":[{"id":"mass","field":"mass"},{"id":"note","field":"note"}]},
    {"kind":"text","testId":"picked","text":{"fn":"coalesce","args":[{"ref":"picked.focus.column"},"-"]}}]}})";

// Mutation: the lowering not handing the grid the clipboard (`Ctrl+V` writes nothing).
TEST_CASE("grid: a document's grid selects cells, copies and pastes them, and adds and deletes rows",
          "[ui][grid][interpreter][cells]") {
    ui::testing::TableHarness h;
    ui::testing::addAction(h, "Lims_RecordSpecimens", std::string{morph::test::grid::kSpecimens},
                           ui::ActionInfo{.modelType = "Lims_SampleModel"});
    auto const screen = h.mount(kCellsGridDocument);
    forms::FormSession& form = *ui::testing::elementOf(*screen, "rec").form;
    form.prefill(R"({"sampleId":4,"specimens":[{"note":"a"},{"note":"b"}]})");
    h.settle();
    int const table = h.byTestId("Table", "grid");
    h.backend.selectCells(table, 0, 0, 1, 1);
    h.settle();
    CHECK(ui::testing::elementValue(h, "grid", "selection") ==
          ui::testing::value(R"({"anchor":{"key":1,"column":"mass"},"focus":{"key":2,"column":"note"}})"));
    CHECK(h.backend.prop(table, "cellSelection") == "r0+2,c0+2");
    CHECK(h.text("picked") == "note");
    h.backend.press(table, "Ctrl+C");
    h.settle();
    CHECK(h.backend.clipboardText() == "\ta\n\tb");
    h.backend.clickCell(table, 0, 0);   // (1, mass), inside the selection, which stays
    h.settle();
    REQUIRE(h.backend.copyText("5\ty"));
    h.backend.press(table, "Ctrl+V");
    h.settle();
    CHECK(form.field("specimens[0].mass").draft() == "5");
    CHECK(form.field("specimens[0].note").draft() == "y");
    h.backend.click(h.byTestId("Button", "grid_addRow"));
    h.settle();
    CHECK(form.field("specimens").rows().size() == 3);
    h.backend.clickCell(table, 0, 0);
    h.settle();
    h.backend.chooseMenu(table, std::nullopt, {"Delete row"});
    h.settle();
    CHECK(form.field("specimens").rows().size() == 2);
}

// Mutation: the loader taking `appendRows` on a table over rows (it loads).
TEST_CASE("grid: the loader keeps cells and row operations to grids", "[ui][grid][interpreter][cells]") {
    ui::testing::TableHarness h;
    ui::testing::addAction(h, "Lims_RecordSpecimens", std::string{morph::test::grid::kSpecimens},
                           ui::ActionInfo{.modelType = "Lims_SampleModel"});
    auto const refusal = [&h](std::string const& text) {
        auto const loaded = ui::loadDocumentText(text, ui::LoadOptions{.catalog = h.session.options().catalog});
        return loaded.has_value() ? std::string{} : loaded.error().reason;
    };
    CHECK(refusal(std::string{kCellsGridDocument}).empty());
    std::string single{kCellsGridDocument};
    single.replace(single.find(R"("selectionMode":"cells")"), std::string_view{R"("selectionMode":"cells")"}.size(),
                   R"("selectionMode":"single")");
    CHECK(refusal(single).find("selects cells") != std::string::npos);
    std::string both{kCellsGridDocument};
    both.replace(both.find(R"("appendRows":true)"), std::string_view{R"("appendRows":true)"}.size(),
                 R"("appendRows":true,"rowsFixed":true)");
    CHECK(refusal(both).find("rowsFixed") != std::string::npos);
    constexpr std::string_view kRowsTable = R"({"vocab":"ui/1","id":"plain","state":{"items":{"init":{"lit":[]}}},
      "view":{"kind":"table","id":"t","rows":{"ref":"items"},"appendRows":true,"columns":[{"id":"a"}]}})";
    CHECK(refusal(std::string{kRowsTable}).find("`appendRows` is a grid's") != std::string::npos);
}

// Mutation: the mount ignoring KeyOutcome::FocusNext.
TEST_CASE("grid: Tab on the last editable cell moves focus to the next control", "[ui][grid][interpreter]") {
    GridScreen g;
    g.h.backend.clickCell(g.table, 1, 1);   // (2, note)
    g.h.settle();
    g.h.backend.press(g.table, "Tab");
    g.h.settle();
    CHECK(g.h.backend.focused() == g.h.byTestId("TextInput", "after"));
    g.h.backend.clickCell(g.table, 0, 0);   // (1, mass)
    g.h.settle();
    g.h.backend.press(g.table, "Shift+Tab");
    g.h.settle();
    CHECK(g.h.backend.focused() == g.h.byTestId("TextInput", "before"));
}

// Mutation: the loader checking a column's `field` only when the grid mounts (loadDocumentText succeeds).
TEST_CASE("grid: a column naming no member is refused when the document loads", "[ui][grid][interpreter]") {
    ui::testing::TableHarness h;
    ui::testing::addAction(h, "Lims_RecordSpecimens", std::string{morph::test::grid::kSpecimens},
                           ui::ActionInfo{.modelType = "Lims_SampleModel"});
    std::string text{kGridDocument};
    text.replace(text.find(R"({"id":"note","field":"note"})"), std::string_view{R"({"id":"note","field":"note"})"}.size(),
                 R"({"id":"x","field":"nosuch"})");
    auto const loaded = ui::loadDocumentText(text, ui::LoadOptions{.catalog = h.session.options().catalog});
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error().reason.find("nosuch") != std::string::npos);
}

// Mutation: the mount not wiring setOnFocusOut to GridController::leave (the editor stays open: `editing` is not null).
TEST_CASE("grid: a click focuses a cell, a double click opens its editor, focus leaving commits",
          "[ui][grid][interpreter]") {
    GridScreen g;
    g.h.backend.clickCell(g.table, 0, 0);
    g.h.settle();
    CHECK(g.value("editing") == ui::Value{});
    CHECK(g.h.backend.focusCell(g.table) == std::pair{std::size_t{0}, std::size_t{0}});
    g.h.backend.clickCell(g.table, 1, 0, true);
    g.h.settle();
    CHECK(g.value("editing") == ui::testing::value(R"({"key":2,"column":"mass"})"));
    std::string const shown = g.h.backend.prop(g.row(2), "editor:0");
    REQUIRE_FALSE(shown.empty());
    int const editor = std::stoi(shown);
    CHECK(g.h.backend.kindOf(editor) == "TextInput");
    CHECK(g.h.backend.focused() == editor);
    g.h.backend.edit(editor, "3");
    g.h.backend.focusOut(g.table);
    g.h.settle();
    CHECK(g.form().field("specimens[1].mass").draft() == "3");
    CHECK(g.value("editing") == ui::Value{});
    CHECK(g.h.backend.prop(g.row(2), "editor:0").empty());
    g.h.backend.clickCell(g.table, 0, 1);   // (1, note)
    g.h.settle();
    g.h.backend.typeInto(g.table, "8");
    g.h.settle();
    CHECK(g.value("editing") == ui::testing::value(R"({"key":1,"column":"note"})"));
    std::string const opened = g.h.backend.prop(g.row(1), "editor:1");
    REQUIRE_FALSE(opened.empty());
    CHECK(g.h.backend.prop(std::stoi(opened), "text") == "8");
    CHECK(g.h.backend.prop(g.table, "keys") == "Enter,Tab,Shift+Tab,Escape,Ctrl+Enter");
}

// A form with a derive action and a debounce of 40 ms, and a grid whose `value` column edits `volume` in a row whose
// note is "fine" and `label` in any other, whose `mass` takes three places in such a row and two in any other, and
// whose `note` is read-only in a row labelled "locked". Its columns by index are mass 0, value 1, limit 2, note 3.
constexpr std::string_view kDeriveDocument = R"({"vocab":"ui/1","id":"specimens",
  "models":{"sample":{"type":"Lims_SampleModel"}},
  "view":{"kind":"column","children":[
    {"kind":"form","id":"rec","model":"sample","action":"Lims_RecordSpecimens","submit":"explicit",
     "derive":"Lims_DeriveSpecimens","deriveDebounce":40},
    {"kind":"table","id":"grid","testId":"t","form":"rec","field":"specimens",
     "columns":[{"id":"mass","field":"mass","decimals":{"if":[{"fn":"eq","args":[{"fn":"coalesce","args":[{"ref":"row.note"},""]},"fine"]},3,2]}},
                {"id":"value","field":{"if":[{"fn":"eq","args":[{"fn":"coalesce","args":[{"ref":"row.note"},""]},"fine"]},"note","label"]}},
                {"id":"limit","field":"limit"},
                {"id":"note","field":"note","readonly":{"fn":"eq","args":[{"fn":"coalesce","args":[{"ref":"row.label"},""]},"locked"]},
                 "placeholder":"Remark"}]}]}})";

void addDeriveActions(ui::testing::TableHarness& h) {
    ui::testing::addAction(h, "Lims_RecordSpecimens", std::string{morph::test::grid::kSpecimens},
                           ui::ActionInfo{.modelType = "Lims_SampleModel"});
    ui::testing::addAction(h, "Lims_DeriveSpecimens", R"({"type":"object","properties":{"body":{"type":"object"}}})",
                           ui::ActionInfo{.modelType = "Lims_SampleModel"});
}

// Mutations: the lowering not reading `deriveDebounce` (no derive call at 40 ms); a column's member resolved once for
// every row (row 2's `value` cell shows its label, row 1's member, not its note).
TEST_CASE("grid: a form's derive action and debounce and a column's member and overlays per row",
          "[ui][grid][interpreter][derive]") {
    using namespace std::chrono_literals;
    ui::testing::TableHarness h;
    addDeriveActions(h);
    auto const screen = h.mount(kDeriveDocument);
    forms::FormSession& form = *ui::testing::elementOf(*screen, "rec").form;
    form.prefill(R"({"sampleId":4,"specimens":[{},{"label":"locked","note":"fine"}]})");
    h.settle();
    int const table = h.byTestId("Table", "t");
    CHECK(form.field("specimens[0].mass").places().peek() == std::optional<std::uint32_t>{2});
    CHECK(form.field("specimens[1].mass").places().peek() == std::optional<std::uint32_t>{3});
    CHECK(ui::testing::cellText(h, 2, "value") == "fine");     // row 2 edits its note
    CHECK(ui::testing::cellText(h, 1, "value").empty());       // row 1 its label, blank
    h.backend.clickCell(table, 1, 3);                          // (2, note): read-only in a row labelled "locked"
    h.settle();
    h.backend.typeInto(table, "z");
    h.settle();
    CHECK(ui::testing::elementValue(h, "grid", "editing") == ui::Value{});
    h.backend.clickCell(table, 0, 0);                          // (1, mass)
    h.settle();
    h.backend.typeInto(table, "1.5");
    h.settle();
    h.backend.press(table, "Enter");
    h.settle();
    h.advance(39ms);
    CHECK(h.dispatcher.count("Lims_DeriveSpecimens") == 0);
    h.advance(1ms);
    REQUIRE(h.dispatcher.count("Lims_DeriveSpecimens") == 1);
    std::size_t const derive = h.dispatcher.last("Lims_DeriveSpecimens");
    CHECK(h.dispatcher.calls.at(derive).body ==
          R"({"body":{"sampleId":4,"specimens":[{"mass":{"num":150,"den":100,"dp":2}},{"label":"locked","note":"fine"}]}})");
    std::size_t const evaluation = h.dispatcher.last("Lims_RecordSpecimens");
    CHECK(h.dispatcher.calls.at(evaluation).options.evaluate);   // density is computed: the action is evaluated
    h.dispatcher.reply(derive, R"({"values":[{"path":"specimens[1].limit","value":{"num":5,"den":1,"dp":2}},
                                             {"path":"specimens[0].note","value":"x"}]})");
    h.dispatcher.reply(evaluation, R"({"sampleId":4,"specimens":[{},{}],"errors":{},"ready":false})");
    h.settle();
    CHECK(ui::testing::cellText(h, 2, "limit") == "5.00");
    CHECK(form.field("specimens[0].note").draft().empty());
    CHECK(ui::testing::diagnosticsMentioning(h, "names `specimens[0].note`, which is not a derived member") == 1);
}

// Mutation: deriveForm reading `deriveDebounce` only when the form names `derive` (the evaluation goes at 150 ms).
TEST_CASE("grid: the debounce times the evaluation of a form with no derive action", "[ui][grid][interpreter][derive]") {
    using namespace std::chrono_literals;
    ui::testing::TableHarness h;
    addDeriveActions(h);
    std::string text{kDeriveDocument};
    text.replace(text.find(R"("derive":"Lims_DeriveSpecimens",)"),
                 std::string_view{R"("derive":"Lims_DeriveSpecimens",)"}.size(), "");
    auto const screen = h.mount(text);
    ui::testing::elementOf(*screen, "rec").form->prefill(R"({"sampleId":4,"specimens":[{},{}]})");
    h.settle();
    auto const evaluations = [&h] {
        return std::ranges::count_if(h.dispatcher.calls, [](auto const& call) {
            return call.action == "Lims_RecordSpecimens" && call.options.evaluate;
        });
    };
    int const table = h.byTestId("Table", "t");
    h.backend.clickCell(table, 0, 0);
    h.settle();
    h.backend.typeInto(table, "1.5");
    h.settle();
    h.backend.press(table, "Enter");
    h.settle();
    h.advance(39ms);
    CHECK(evaluations() == 0);
    h.advance(1ms);
    CHECK(evaluations() == 1);                                 // density is computed: rounds evaluate, at 40 ms
    CHECK(h.dispatcher.count("Lims_DeriveSpecimens") == 0);
}

// Mutation: the loader taking any `deriveDebounce` (the computed one loads).
TEST_CASE("grid: the loader checks a form's derive keys and keeps a grid column's keys to grids",
          "[ui][grid][interpreter][derive]") {
    ui::testing::TableHarness h;
    addDeriveActions(h);
    auto const refusal = [&h](std::string text) {
        auto const loaded = ui::loadDocumentText(text, ui::LoadOptions{.catalog = h.session.options().catalog});
        return loaded.has_value() ? std::string{} : loaded.error().reason;
    };
    std::string text{kDeriveDocument};
    CHECK(refusal(text).empty());
    std::string computed = text;
    computed.replace(computed.find(R"("deriveDebounce":40)"), std::string_view{R"("deriveDebounce":40)"}.size(),
                     R"("deriveDebounce":{"ref":"rec.ready"})");
    CHECK(refusal(computed).find("`deriveDebounce` is a literal count of milliseconds") != std::string::npos);
    // The debounce times the evaluation too, so a form whose rows are only computed takes it without `derive`.
    std::string unnamed = text;
    unnamed.replace(unnamed.find(R"("derive":"Lims_DeriveSpecimens",)"),
                    std::string_view{R"("derive":"Lims_DeriveSpecimens",)"}.size(), "");
    CHECK(refusal(unnamed).empty());
    std::string foreign = text;
    foreign.replace(foreign.find("Lims_DeriveSpecimens"), std::string_view{"Lims_DeriveSpecimens"}.size(),
                    "Lims_ClientDerive");   // an action of another model
    std::string const foreignReason = refusal(foreign);
    INFO(foreignReason);
    CHECK(foreignReason.find("belongs to `Lims_ClientModel`") != std::string::npos);
    constexpr std::string_view kRowsTable = R"({"vocab":"ui/1","id":"plain","state":{"items":{"init":{"lit":[]}}},
      "view":{"kind":"table","rows":{"ref":"items"},"columns":[{"id":"a","placeholder":"x"}]}})";
    std::string const plainReason = refusal(std::string{kRowsTable});
    INFO(plainReason);
    CHECK(plainReason.find("is a grid column's") != std::string::npos);
}

}  // namespace
