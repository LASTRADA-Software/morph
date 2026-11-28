// SPDX-License-Identifier: Apache-2.0
//
// A table's configuration as pure functions: sort and layout gestures, value filters, conditions, menus, copy text
// and the status line's messages. Each case names the mutation it was seen to fail on.

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <map>
#include <morph/table/data_source.hpp>
#include <morph/table/engine.hpp>
#include <morph/table/filter.hpp>
#include <morph/table/sort.hpp>
#include <morph/ui/backend.hpp>
#include <morph/ui/locale_table.hpp>
#include <morph/ui/table_config.hpp>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace ui = morph::ui;
namespace table = morph::table;

// The units `kg` and `g` of dimension mass, as a table's description lists them.
std::vector<table::UnitInfo> massUnits() {
    return {table::UnitInfo{.id = "kg",
                            .symbol = "kg",
                            .dimension = "mass",
                            .toBase = morph::math::Rational{1, morph::math::DecimalPlaces{0}},
                            .decimals = 3},
            table::UnitInfo{.id = "g",
                            .symbol = "g",
                            .dimension = "mass",
                            .toBase = morph::math::Rational{morph::math::Numerator{1}, morph::math::Denominator{1000},
                                                            morph::math::DecimalPlaces{3}},
                            .decimals = 0}};
}

// Mutation: Extend treated as Click (Shift ignored).
TEST_CASE("sort gesture: a click replaces the chain; Shift adds a key or flips one in place", "[ui][table][config]") {
    using table::SortDirection::Ascending;
    using table::SortDirection::Descending;
    ui::SortChain const two{{.column = "name", .dir = Ascending}, {.column = "mass", .dir = Descending}};
    CHECK(ui::applySortGesture(two, "name", ui::SortGesture::Click, 8).chain == ui::SortChain{{"name", Descending}});
    CHECK(ui::applySortGesture(two, "mass", ui::SortGesture::Click, 8).chain == ui::SortChain{{"mass", Ascending}});
    CHECK(ui::applySortGesture({{"name", Descending}}, "name", ui::SortGesture::Click, 8).chain ==
          ui::SortChain{{"name", Ascending}});
    CHECK(ui::applySortGesture(two, "site", ui::SortGesture::Extend, 8).chain ==
          ui::SortChain{{"name", Ascending}, {"mass", Descending}, {"site", Ascending}});
    CHECK(ui::applySortGesture(two, "name", ui::SortGesture::Extend, 8).chain ==
          ui::SortChain{{"name", Descending}, {"mass", Descending}});
    CHECK(ui::applySortGesture(two, "mass", ui::SortGesture::Remove, 8).chain == ui::SortChain{{"name", Ascending}});
    CHECK(ui::applySortGesture(two, "site", ui::SortGesture::Descending, 8).chain ==
          ui::SortChain{{"site", Descending}});
}

// Mutation: the bound checked before the "already in the chain" case (the flip at the bound is refused).
TEST_CASE("sort gesture: at the bound a new key is refused and a key in the chain still flips",
          "[ui][table][config]") {
    using table::SortDirection::Ascending;
    using table::SortDirection::Descending;
    ui::SortChain const full{{.column = "a", .dir = Ascending}, {.column = "b", .dir = Ascending}};
    ui::SortChange const added = ui::applySortGesture(full, "c", ui::SortGesture::Extend, 2);
    CHECK(added.chain == full);
    CHECK(added.refusal == "A table sorts by at most 2 columns");
    CHECK(ui::applySortGesture(full, "c", ui::SortGesture::Add, 2).refusal == "A table sorts by at most 2 columns");
    ui::SortChange const flipped = ui::applySortGesture(full, "b", ui::SortGesture::Extend, 2);
    CHECK(flipped.chain == ui::SortChain{{"a", Ascending}, {"b", Descending}});
    CHECK_FALSE(flipped.refusal);
}

// Mutation: applyLayoutGesture ignoring `locked`.
TEST_CASE("layout gesture: moves, hides, pins and resets, refusing a locked column and the last visible one",
          "[ui][table][config]") {
    table::ColumnLayout layout{{"id", "name", "mass", "site"}};
    std::vector<ui::LayoutColumn> const columns{{.id = "id", .label = "ID", .locked = true},
                                                {.id = "name", .label = "Name"},
                                                {.id = "mass", .label = "Mass"},
                                                {.id = "site", .label = "Site"}};
    CHECK(ui::applyLayoutGesture(layout, columns, ui::Configure::All, ui::MoveColumn{"mass", -1}).changed);
    CHECK(std::ranges::equal(layout.order(), std::vector<std::string>{"id", "mass", "name", "site"}));
    CHECK(ui::applyLayoutGesture(layout, columns, ui::Configure::All, ui::MoveColumn{"id", 1}).refusal ==
          "ID is locked");
    CHECK(ui::applyLayoutGesture(layout, columns, ui::Configure::All, ui::PinColumn{"name"}).changed);
    CHECK(layout.pinned() == 1);
    CHECK(layout.visible().front() == "name");
    CHECK(ui::applyLayoutGesture(layout, columns, ui::Configure::All, ui::ResizeColumn{"mass", 14}).changed);
    CHECK(layout.widths().at("mass") == 14);
    for (std::string const id : {"name", "mass", "site"}) {
        CHECK(ui::applyLayoutGesture(layout, columns, ui::Configure::All, ui::HideColumn{id}).changed);
    }
    CHECK(layout.visible() == std::vector<std::string>{"id"});
    CHECK(ui::applyLayoutGesture(layout, columns, ui::Configure::All, ui::HideColumn{"id"}).refusal == "ID is locked");
    table::ColumnLayout lone{{"a", "b"}};
    std::vector<ui::LayoutColumn> const ab{{.id = "a", .label = "A"}, {.id = "b", .label = "B"}};
    CHECK(ui::applyLayoutGesture(lone, ab, ui::Configure::All, ui::HideColumn{"a"}).changed);
    CHECK(ui::applyLayoutGesture(lone, ab, ui::Configure::All, ui::HideColumn{"b"}).refusal ==
          "At least one column stays visible");
    CHECK(ui::applyLayoutGesture(lone, ab, ui::Configure::Hide, ui::MoveColumn{"b", -1}).refusal ==
          "This table's columns cannot be moved");
    CHECK(ui::applyLayoutGesture(layout, columns, ui::Configure::All, ui::ResetColumns{}).changed);
    CHECK(std::ranges::equal(layout.order(), std::vector<std::string>{"id", "name", "mass", "site"}));
    CHECK(layout.pinned() == 0);
    CHECK(layout.widths().empty());
}

// Mutation: UnpinColumn leaving the column in place (only the count shrinks).
TEST_CASE("layout gesture: unpinning moves the column after the pinned ones; drags and groups",
          "[ui][table][config]") {
    table::ColumnLayout layout{{"a", "b", "c", "d"}};
    std::vector<ui::LayoutColumn> const columns{
        {.id = "a", .label = "A"},
        {.id = "b", .label = "B"},
        {.id = "c", .label = "C"},
        {.id = "d", .label = "D", .locked = false, .kind = table::ColumnKind::Integer}};
    CHECK(ui::applyLayoutGesture(layout, columns, ui::Configure::All, ui::PinColumn{"c"}).changed);
    CHECK(ui::applyLayoutGesture(layout, columns, ui::Configure::All, ui::PinColumn{"d"}).changed);
    CHECK(layout.visible() == std::vector<std::string>{"c", "d", "a", "b"});
    CHECK(ui::applyLayoutGesture(layout, columns, ui::Configure::All, ui::UnpinColumn{"c"}).changed);
    CHECK(layout.visible() == std::vector<std::string>{"d", "c", "a", "b"});
    CHECK(layout.pinned() == 1);
    CHECK(ui::applyLayoutGesture(layout, columns, ui::Configure::All, ui::MoveColumnTo{"d", 9}).changed);
    CHECK(layout.visible() == std::vector<std::string>{"c", "a", "b", "d"});
    CHECK(layout.pinned() == 0);  // dropped past the pinned columns
    CHECK(ui::applyLayoutGesture(layout, columns, ui::Configure::Pin, ui::ResizeColumn{"a", 3}).refusal ==
          "This table's columns cannot be resized");
    CHECK(ui::applyLayoutGesture(layout, columns, ui::Configure::None, ui::ResetColumns{}).refusal);
    // Mutation: forming a group whatever the table allows.
    CHECK(ui::applyLayoutGesture(layout, columns, ui::Configure::Pin, ui::FormGroup{"P", {"a", "b"}}).refusal ==
          "This table's columns cannot be moved");
    CHECK(ui::applyLayoutGesture(layout, columns, ui::Configure::Hide, ui::FormGroup{"P", {"a", "d"}}).refusal ==
          "A group's columns hold one kind of value");
    CHECK(ui::applyLayoutGesture(layout, columns, ui::Configure::Hide, ui::FormGroup{"P", {"a"}}).refusal ==
          "A group has 2 to 16 columns");
    ui::LayoutResult const ghost = ui::applyLayoutGesture(layout, columns, ui::Configure::Hide,
                                                          ui::FormGroup{.label = "P", .columns = {"a", "ghost"}});
    CHECK_FALSE(ghost.changed);
    CHECK_FALSE(ghost.refusal);
    CHECK(ui::applyLayoutGesture(layout, columns, ui::Configure::Hide, ui::FormGroup{"P", {"a", "b"}}).changed);
    // Mutation: the already-grouped refusal passed through as the layout's developer text.
    CHECK(ui::applyLayoutGesture(layout, columns, ui::Configure::Hide, ui::FormGroup{"Q", {"c", "b"}}).refusal ==
          "B is already in P");
    CHECK(ui::applyLayoutGesture(layout, columns, ui::Configure::None, ui::SetDisplayUnit{"d", "mass", "g"}).changed);
    CHECK(ui::applyLayoutGesture(layout, columns, ui::Configure::Reorder, ui::RemoveGroup{"g1"}).changed);
    CHECK(layout.groups().empty());
}

// Mutation: HideColumn leaving the pinned count as it was (the next column becomes pinned).
TEST_CASE("layout gesture: hiding a pinned column unpins no other and pins none", "[ui][table][config]") {
    table::ColumnLayout layout{{"a", "b", "c", "d"}};
    std::vector<ui::LayoutColumn> const columns{
        {.id = "a", .label = "A"}, {.id = "b", .label = "B"}, {.id = "c", .label = "C"}, {.id = "d", .label = "D"}};
    REQUIRE(ui::applyLayoutGesture(layout, columns, ui::Configure::All, ui::PinColumn{"a"}).changed);
    // Mutation: a column placed beside itself (pinning the first column with none pinned moves it last).
    CHECK(layout.visible() == std::vector<std::string>{"a", "b", "c", "d"});
    REQUIRE(ui::applyLayoutGesture(layout, columns, ui::Configure::All, ui::PinColumn{"b"}).changed);
    CHECK(ui::applyLayoutGesture(layout, columns, ui::Configure::All, ui::HideColumn{"a"}).changed);
    CHECK(layout.visible() == std::vector<std::string>{"b", "c", "d"});
    CHECK(layout.pinned() == 1);
    CHECK(ui::applyLayoutGesture(layout, columns, ui::Configure::All, ui::HideColumn{"d"}).changed);
    CHECK(layout.pinned() == 1);
}

// Mutation: ShowColumn leaving the pinned count as it was (the last pinned column is pushed out).
TEST_CASE("layout gesture: a column shown among the pinned columns joins them", "[ui][table][config]") {
    table::ColumnLayout layout{{"a", "b", "c", "d"}};
    std::vector<ui::LayoutColumn> const columns{
        {.id = "a", .label = "A"}, {.id = "b", .label = "B"}, {.id = "c", .label = "C"}, {.id = "d", .label = "D"}};
    REQUIRE(ui::applyLayoutGesture(layout, columns, ui::Configure::All, ui::PinColumn{"a"}).changed);
    REQUIRE(ui::applyLayoutGesture(layout, columns, ui::Configure::All, ui::PinColumn{"b"}).changed);
    REQUIRE(ui::applyLayoutGesture(layout, columns, ui::Configure::All, ui::PinColumn{"c"}).changed);
    REQUIRE(ui::applyLayoutGesture(layout, columns, ui::Configure::All, ui::HideColumn{"b"}).changed);
    REQUIRE(layout.pinned() == 2);
    CHECK(ui::applyLayoutGesture(layout, columns, ui::Configure::All, ui::ShowColumn{"b"}).changed);
    CHECK(layout.visible() == std::vector<std::string>{"a", "b", "c", "d"});
    CHECK(layout.pinned() == 3);
    CHECK_FALSE(ui::applyLayoutGesture(layout, columns, ui::Configure::All, ui::ShowColumn{"b"}).changed);
    CHECK(layout.pinned() == 3);
}

// Mutation: MoveColumn clamping only to the visible ends (Move left on the first unpinned column pins it).
TEST_CASE("layout gesture: a move stays on its side of the pin boundary", "[ui][table][config]") {
    table::ColumnLayout layout{{"a", "b", "c", "d", "e"}};
    std::vector<ui::LayoutColumn> const columns{{.id = "a", .label = "A"},
                                                {.id = "b", .label = "B"},
                                                {.id = "c", .label = "C"},
                                                {.id = "d", .label = "D"},
                                                {.id = "e", .label = "E"}};
    layout.setPinned(2);
    CHECK_FALSE(ui::applyLayoutGesture(layout, columns, ui::Configure::All, ui::MoveColumn{"c", -1}).changed);
    CHECK_FALSE(ui::applyLayoutGesture(layout, columns, ui::Configure::All, ui::MoveColumn{"b", 1}).changed);
    CHECK(ui::applyLayoutGesture(layout, columns, ui::Configure::All, ui::MoveColumn{"e", -9}).changed);
    CHECK(layout.visible() == std::vector<std::string>{"a", "b", "e", "c", "d"});
    CHECK(ui::applyLayoutGesture(layout, columns, ui::Configure::All, ui::MoveColumn{"a", 9}).changed);
    CHECK(layout.visible() == std::vector<std::string>{"b", "a", "e", "c", "d"});
    CHECK(layout.pinned() == 2);
}

// Mutation: MoveColumnTo leaving the pinned count as it was.
TEST_CASE("layout gesture: a drag pins a column dropped among the pinned ones and unpins one dropped past them",
          "[ui][table][config]") {
    table::ColumnLayout layout{{"a", "b", "c", "d"}};
    std::vector<ui::LayoutColumn> const columns{
        {.id = "a", .label = "A"}, {.id = "b", .label = "B"}, {.id = "c", .label = "C"}, {.id = "d", .label = "D"}};
    layout.setPinned(2);
    CHECK(ui::applyLayoutGesture(layout, columns, ui::Configure::All, ui::MoveColumnTo{"d", 0}).changed);
    CHECK(layout.visible() == std::vector<std::string>{"d", "a", "b", "c"});
    CHECK(layout.pinned() == 3);
    CHECK(ui::applyLayoutGesture(layout, columns, ui::Configure::All, ui::MoveColumnTo{"a", 3}).changed);
    CHECK(layout.visible() == std::vector<std::string>{"d", "b", "c", "a"});
    CHECK(layout.pinned() == 2);
    CHECK(ui::applyLayoutGesture(layout, columns, ui::Configure::All, ui::MoveColumnTo{"c", 2}).changed == false);
    CHECK(ui::applyLayoutGesture(layout, columns, ui::Configure::All, ui::MoveColumnTo{"b", 2}).changed);
    CHECK(layout.visible() == std::vector<std::string>{"d", "c", "b", "a"});
    CHECK(layout.pinned() == 1);  // b dropped past the pinned columns; c, which it passed, stays unpinned
}

// Mutation: planValueFilter always sending the side the operations reached.
TEST_CASE("value filter: the smaller side is sent, and both sides over the bound disable the list",
          "[ui][table][config]") {
    using Side = ui::ValueFilterPlan::Side;
    auto const covering = [](int count, bool ticked) {
        std::map<std::string, bool> covered;
        for (int i = 0; i < count; ++i) {
            covered.emplace("v" + std::to_string(i), ticked);
        }
        return covered;
    };
    auto const one =
        ui::planValueFilter(50'000, ui::ResolvedTicks{.ticked = true, .covered = {{"v00007", false}}}, 10'000);
    CHECK(one.side == Side::Exclude);
    CHECK(one.members == 1);
    CHECK_FALSE(one.needsComplement);
    CHECK(ui::planValueFilter(25'000, ui::ResolvedTicks{.ticked = true}, 10'000).side == Side::All);
    CHECK(ui::planValueFilter(25'000, ui::ResolvedTicks{.ticked = false, .covered = covering(12'000, true)}, 10'000)
              .side == Side::Disabled);
    auto const complement =
        ui::planValueFilter(25'000, ui::ResolvedTicks{.ticked = true, .covered = covering(20'000, false)}, 10'000);
    CHECK(complement.side == Side::Include);
    CHECK(complement.members == 5'000);
    CHECK(complement.needsComplement);
    CHECK(ui::planValueFilter(3, ui::ResolvedTicks{.ticked = false, .blanks = false}, 10'000).side == Side::Nothing);
    // Mutation: the tie decided as Include (`T <= U`): one of two values unticked sends the other as an include.
    auto const tie = ui::planValueFilter(
        2, ui::ResolvedTicks{.ticked = true, .covered = {{"Open", false}}, .blanks = true}, 10'000);
    CHECK(tie.side == Side::Exclude);
    CHECK(tie.members == 1);
    // Mutation: an empty `oneOf` written beside `isEmpty` when only "(Blanks)" is ticked.
    auto const blanksOnly = ui::planValueFilter(2, ui::ResolvedTicks{.ticked = false, .blanks = true}, 10'000);
    CHECK(blanksOnly.side == Side::Include);
    CHECK(blanksOnly.members == 0);
    auto const onlyBlanks = ui::valueFilter(Side::Include, {}, true);
    REQUIRE(onlyBlanks);
    REQUIRE(onlyBlanks->include.size() == 1);
    CHECK(onlyBlanks->include.at(0).isEmpty == true);
    CHECK_FALSE(ui::valueFilter(Side::Exclude, {}, std::nullopt));
    auto const include = ui::valueFilter(Side::Include, {"F", "S"}, true);
    REQUIRE(include);
    CHECK(include->combine == table::Combine::Any);
    REQUIRE(include->include.size() == 2);
    CHECK(include->include.at(0).oneOf == std::vector<std::string>{"F", "S"});
    CHECK(include->include.at(1).isEmpty == true);
    auto const exclude = ui::valueFilter(Side::Exclude, {"X"}, false);
    REQUIRE(exclude);
    REQUIRE(exclude->exclude.size() == 2);
    CHECK(exclude->exclude.at(1).isEmpty == true);
    auto const ranges = ui::valueFilter(Side::Include, {"2026-03-01..2026-03-31", "2026-05-14..2026-05-14"},
                                        std::nullopt, ui::ValueForm::Ranges);
    REQUIRE(ranges);
    REQUIRE(ranges->include.size() == 2);
    CHECK(ranges->include.at(0).between == std::array<std::string, 2>{"2026-03-01", "2026-03-31"});
}

// Mutation: resolveTicks applying the operations in reverse order.
TEST_CASE("value ticks: operations under searches resolve in order, the last one winning", "[ui][table][config]") {
    std::map<std::string, std::vector<std::string>> const shows{{"ab", {"ab1", "xab"}}, {"x", {"xab", "xy"}}};
    auto const under = [&](std::string const& search) { return shows.at(search); };
    ui::ValueTicks const ticks{.ticked = true,
                               .ops = {{.search = "", .operand = std::nullopt, .ticked = false},
                                       {.search = "ab", .operand = std::nullopt, .ticked = true},
                                       {.search = "x", .operand = std::nullopt, .ticked = false},
                                       {.search = "", .operand = "xab", .ticked = true}}};
    ui::ResolvedTicks const resolved = ui::resolveTicks(ticks, under);
    CHECK_FALSE(resolved.ticked);
    CHECK(resolved.covered == std::map<std::string, bool>{{"ab1", true}, {"xab", true}, {"xy", false}});
}

// Mutation: classifyFilter reading a Custom column's lone isEmpty as "(Blanks)".
TEST_CASE("filter parts: a column's filter is values or conditions, never both", "[ui][table][config]") {
    using table::ColumnKind;
    auto const entry = [](auto set) {
        table::FilterEntry e;
        set(e);
        return e;
    };
    table::ColumnFilter const values{.combine = table::Combine::Any,
                                     .include = {entry([](auto& e) { e.oneOf = std::vector<std::string>{"F"}; }),
                                                 entry([](auto& e) { e.isEmpty = true; })}};
    CHECK(ui::classifyFilter(values, ColumnKind::Text) == ui::FilterPart::Values);
    table::ColumnFilter const blanks{.combine = table::Combine::Any,
                                     .include = {entry([](auto& e) { e.isEmpty = true; })}};
    CHECK(ui::classifyFilter(blanks, ColumnKind::Text) == ui::FilterPart::Values);
    CHECK(ui::classifyFilter(blanks, ColumnKind::Custom) == ui::FilterPart::Conditions);
    table::ColumnFilter const conditions{
        .combine = table::Combine::All,
        .include = {entry([](auto& e) { e.gt = "2"; }), entry([](auto& e) { e.lt = "5"; })}};
    CHECK(ui::classifyFilter(conditions, ColumnKind::Integer) == ui::FilterPart::Conditions);
    CHECK(ui::classifyFilter({}, ColumnKind::Integer) == ui::FilterPart::None);
    // Mutation: a `between` read as a date bucket on any kind.
    auto const range = ui::valueFilter(ui::ValueFilterPlan::Side::Include, {"2026-03-01..2026-03-31"}, std::nullopt,
                                       ui::ValueForm::Ranges);
    REQUIRE(range);
    CHECK(ui::classifyFilter(*range, ColumnKind::Date) == ui::FilterPart::Values);
    CHECK(ui::classifyFilter(*range, ColumnKind::Integer) == ui::FilterPart::Conditions);
}

// Mutation: canonicalOperand passing typed text through unchanged.
TEST_CASE("typed operands: numbers reach the wire with a point and no grouping", "[ui][table][config]") {
    ui::LocaleFacts const& de = ui::localeFacts("de");
    CHECK(ui::canonicalOperand("1.234,5", table::ColumnKind::Decimal, de) == "1234.5");
    CHECK(ui::canonicalOperand("12,5", table::ColumnKind::Integer, de) == std::nullopt);
    CHECK(ui::canonicalOperand("-7", table::ColumnKind::Integer, de) == "-7");
    CHECK(ui::canonicalOperand("1,5", table::ColumnKind::Decimal, ui::localeFacts("en")) == std::nullopt);
    CHECK(ui::canonicalOperand("abc", table::ColumnKind::Text, de) == "abc");
    CHECK(ui::canonicalOperand("2026-05-14", table::ColumnKind::Date, de) == "2026-05-14");
    // Mutation: the Measure branch passing the typed text through ("0,3 kg" reaches the wire unchanged).
    CHECK(ui::canonicalOperand("0,3 kg", table::ColumnKind::Measure, de, massUnits()) == "0.3 kg");
    CHECK(ui::canonicalOperand("250", table::ColumnKind::Measure, de, massUnits(), "g") == "250 g");
}

// Mutation: Together built as one oneOf entry (the row F, F, X would pass).
TEST_CASE("group filter from ticks: the match line chooses the quantifier, together takes one eq per value",
          "[ui][table][config]") {
    auto const atLeast = ui::groupValueFilter("company", 3, {"F", "S"}, std::nullopt, ui::GroupMatch::AtLeast, 2);
    REQUIRE(atLeast);
    CHECK(atLeast->match.kind == table::ColumnCount::Kind::AtLeast);
    CHECK(atLeast->match.n == 2);
    CHECK(atLeast->include.at(0).oneOf == std::vector<std::string>{"F", "S"});
    auto const together = ui::groupValueFilter("company", 3, {"F", "S"}, std::nullopt, ui::GroupMatch::Together, 0);
    REQUIRE(together);
    CHECK(together->mode == table::GroupMode::Together);
    REQUIRE(together->include.size() == 2);
    CHECK(together->include.at(0).eq == "F");
    CHECK(together->include.at(1).eq == "S");
    CHECK_FALSE(ui::groupValueFilter("company", 3, {"F"}, std::nullopt, ui::GroupMatch::Together, 0));
    CHECK_FALSE(ui::groupValueFilter("company", 3, {"F", "S", "T", "U"}, std::nullopt, ui::GroupMatch::Together, 0));
    auto const blanks = ui::groupValueFilter("company", 3, {"F"}, true, ui::GroupMatch::AnyColumn, 0);
    REQUIRE(blanks);
    CHECK(blanks->include.at(1).isEmpty == true);
    CHECK_FALSE(ui::groupValueFilter("company", 3, {"F"}, std::nullopt, ui::GroupMatch::AtLeast, 4));
}

// Mutation: conditionOps returning the numeric list for every kind.
TEST_CASE("conditions follow the column's kind", "[ui][table][config]") {
    using enum ui::ConditionOp;
    CHECK(ui::conditionOps(table::ColumnKind::Text) ==
          std::vector<ui::ConditionOp>{Equals, NotEquals, Contains, BeginsWith, EndsWith});
    auto const numeric =
        std::vector<ui::ConditionOp>{Equals, NotEquals, Less, LessOrEqual, Greater, GreaterOrEqual, Between};
    for (auto const kind : {table::ColumnKind::Integer, table::ColumnKind::Key, table::ColumnKind::Decimal,
                            table::ColumnKind::Quantity, table::ColumnKind::Number, table::ColumnKind::Date,
                            table::ColumnKind::DateTime, table::ColumnKind::Measure}) {
        CHECK(ui::conditionOps(kind) == numeric);
    }
    CHECK(ui::conditionOps(table::ColumnKind::Bool).empty());
    CHECK(ui::conditionOps(table::ColumnKind::Custom) == std::vector<ui::ConditionOp>{IsEmpty, IsNotEmpty});
    CHECK(ui::conditionLabel(NotEquals, table::ColumnKind::Text) == "does not equal");
    CHECK(ui::conditionLabel(NotEquals, table::ColumnKind::Integer) == "≠");
    table::FilterEntry const between = ui::conditionEntry(Between, "1", "5", false);
    CHECK(between.between == std::array<std::string, 2>{"1", "5"});
    CHECK(ui::conditionEntry(Contains, "ab", {}, true).caseSensitive == true);
}

// Mutation: headerMenu leaving "Add to sort" enabled for a column already in the chain.
TEST_CASE("header menu: entries, order and enablement", "[ui][table][config]") {
    ui::HeaderMenuInput const input{.column = {.id = "name", .label = "Name"},
                                    .sortable = true,
                                    .filterable = true,
                                    .sortPlace = 0,
                                    .chainSize = 1,
                                    .maxSortKeys = 8,
                                    .visibleIndex = 1,
                                    .visibleCount = 3,
                                    .pinned = 0,
                                    .dimensions = {},
                                    .groups = {{.id = "company", .label = "Company", .columns = 3}},
                                    .allowed = ui::Configure::All};
    std::vector<ui::MenuEntry> const entries = ui::menuEntries(ui::headerMenu(input));
    std::vector<std::string> labels;
    for (ui::MenuEntry const& entry : entries) {
        // NOLINTNEXTLINE(performance-inefficient-vector-operation): `labels` is a fixture list built once; sizing it first would not change what the case checks
        labels.push_back(entry.separator ? "-" : entry.label);
    }
    CHECK(labels == std::vector<std::string>{"Sort ascending", "Sort descending", "Add to sort", "Remove from sort",
                                             "-", "Filter…", "Filter Company, 3 columns…", "-", "Move left",
                                             "Move right", "Pin column", "Hide column"});
    CHECK_FALSE(entries.at(2).enabled);
    CHECK(entries.at(3).enabled);
    CHECK(entries.at(8).keys == "Ctrl+Shift+Left");
    ui::HeaderMenuInput locked = input;
    locked.column.locked = true;
    std::vector<ui::MenuEntry> const lockedEntries = ui::menuEntries(ui::headerMenu(locked));
    CHECK_FALSE(lockedEntries.at(8).enabled);
    CHECK_FALSE(lockedEntries.at(11).enabled);
    ui::HeaderMenuInput hideOnly = input;
    hideOnly.allowed = ui::Configure::Hide;
    CHECK(ui::menuEntries(ui::headerMenu(hideOnly)).back().label == "Hide column");
    CHECK(ui::menuEntries(ui::headerMenu(hideOnly)).size() == 9);
    // Mutation: Move left and Move right enabled across the pin boundary.
    ui::HeaderMenuInput firstUnpinned = input;  // visible index 1, one pinned column before it
    firstUnpinned.pinned = 1;
    CHECK_FALSE(ui::menuEntries(ui::headerMenu(firstUnpinned)).at(8).enabled);
    CHECK(ui::menuEntries(ui::headerMenu(firstUnpinned)).at(9).enabled);
    ui::HeaderMenuInput lastPinned = input;  // visible index 1 of two pinned columns
    lastPinned.pinned = 2;
    std::vector<ui::MenuEntry> const lastPinnedEntries = ui::menuEntries(ui::headerMenu(lastPinned));
    CHECK(lastPinnedEntries.at(8).enabled);
    CHECK_FALSE(lastPinnedEntries.at(9).enabled);
    CHECK(lastPinnedEntries.at(10).label == "Unpin");
    ui::HeaderMenuInput ends = input;
    ends.visibleIndex = 0;
    ends.visibleCount = 1;
    ends.sortPlace = std::nullopt;
    ends.chainSize = 8;
    std::vector<ui::MenuEntry> const endEntries = ui::menuEntries(ui::headerMenu(ends));
    CHECK_FALSE(endEntries.at(2).enabled);  // the chain is at its bound
    CHECK_FALSE(endEntries.at(8).enabled);
    CHECK_FALSE(endEntries.at(9).enabled);
    CHECK_FALSE(endEntries.at(11).enabled);  // the only visible column
    // Mutation: the Display unit submenu drawn with no check on the chosen unit.
    ui::HeaderMenuInput measure = input;
    measure.column = {.id = "weight", .label = "Weight", .locked = false, .kind = table::ColumnKind::Measure};
    measure.dimensions = {{.dimension = "mass", .units = {"g", "kg"}, .chosen = "g"}};
    std::vector<ui::TableMenuEntry> const measureMenu = ui::headerMenu(measure);
    REQUIRE(measureMenu.back().entry.label == "Display unit");
    REQUIRE(measureMenu.back().items.size() == 2);
    CHECK(measureMenu.back().items.at(0).entry.label == "mass: g");
    CHECK(measureMenu.back().items.at(0).entry.checked == true);
    CHECK(measureMenu.back().items.at(1).entry.checked == false);
    CHECK(measureMenu.back().items.at(1).command == ui::MenuCommand::DisplayUnit);
    CHECK(measureMenu.back().items.at(1).argument == "mass\x1Fkg");
    std::vector<std::size_t> const path{measureMenu.size() - 1, 1};
    ui::TableMenuEntry const* const kg = ui::chosen(measureMenu, path);
    REQUIRE(kg != nullptr);
    CHECK(kg->argument == "mass\x1Fkg");
    CHECK(ui::chosen(measureMenu, std::vector<std::size_t>{4}) == nullptr);  // a separator
    std::vector<ui::MenuEntry> const table =
        ui::menuEntries(ui::tableMenu({.anyFilter = false, .groups = {}, .allowed = ui::Configure::All}));
    std::vector<std::string> tableLabels;
    for (ui::MenuEntry const& entry : table) {
        // NOLINTNEXTLINE(performance-inefficient-vector-operation): `tableLabels` is a fixture list built once; sizing it first would not change what the case checks
        tableLabels.push_back(entry.separator ? "-" : entry.label);
    }
    CHECK(tableLabels == std::vector<std::string>{"Columns…", "Sort…", "Clear all filters", "Reset columns", "-",
                                                  "Copy", "Copy with headers"});
    CHECK_FALSE(table.at(2).enabled);
    CHECK(table.at(5).keys == "Ctrl+C");
    std::vector<ui::MenuEntry> const fixed =
        ui::menuEntries(ui::tableMenu({.anyFilter = true,
                                       .groups = {{.id = "company", .label = "Company", .columns = 3}},
                                       .allowed = ui::Configure::None}));
    std::vector<std::string> fixedLabels;
    for (ui::MenuEntry const& entry : fixed) {
        // NOLINTNEXTLINE(performance-inefficient-vector-operation): `fixedLabels` is a fixture list built once; sizing it first would not change what the case checks
        fixedLabels.push_back(entry.separator ? "-" : entry.label);
    }
    CHECK(fixedLabels == std::vector<std::string>{"Sort…", "Filter Company, 3 columns…", "Clear all filters", "-",
                                                  "Copy", "Copy with headers"});
    CHECK(fixed.at(2).enabled);
}

// Mutation: copyText replacing "\r\n" with two spaces.
TEST_CASE("copy text: tabs and line breaks become one space; unloaded cells are empty and counted",
          "[ui][table][config]") {
    std::vector<std::string> const labels{"Na\tme", "N"};
    std::vector<std::vector<std::optional<std::string>>> const rows{{"a\tb\r\nc", "1"}, {std::nullopt, "x\ry\nz"}};
    ui::CopiedText const plain = ui::copyText({}, rows);
    CHECK(plain.text == "a b c\t1\n\tx y z");
    CHECK(plain.unloaded == 1);
    CHECK(ui::copyText(labels, rows).text == "Na me\tN\na b c\t1\n\tx y z");
}

// Mutations: groupedCount writing the digits ungrouped; `copied` using the plural for one row or one cell.
TEST_CASE("status copy: counts are grouped and messages are exact", "[ui][table][config]") {
    CHECK(ui::tableText::tooManyDates() == "Too many dates to list; filter with a condition");
    CHECK(ui::groupedCount(999) == "999");
    CHECK(ui::groupedCount(10'000) == "10,000");
    CHECK(ui::groupedCount(-1'234'567) == "-1,234,567");
    CHECK(ui::groupedCount(10'000, ".") == "10.000");
    CHECK(ui::tableText::sortBound(32) == "A table sorts by at most 32 columns");
    CHECK(ui::tableText::rangeBound(10'000) == "A range selects at most 10,000 rows");
    CHECK(ui::tableText::copyBound(10'000) == "A copy holds at most 10,000 rows");
    CHECK(ui::tableText::copied(200, 12) == "Copied 200 rows, 12 cells were not loaded");
    CHECK(ui::tableText::copied(1, 1) == "Copied 1 row, 1 cell was not loaded");
    CHECK(ui::tableText::copied(3, 0) == "Copied 3 rows");
    CHECK(ui::tableText::unreadable(1) == "1 cell could not be read");
    CHECK(ui::tableText::notConfigurable(ui::Configure::Pin) == "This table's columns cannot be pinned");
}

}  // namespace

// Mutation: sortDescription capitalised ("Sorted ascending, …"), or the count written as the place.
TEST_CASE("sort marks: the header's mark and accessible description", "[ui][table][config]") {
    CHECK(ui::sortDescription(ui::SortDirection::Ascending, 2, 3) == "sorted ascending, key 2 of 3");
    CHECK(ui::sortDescription(ui::SortDirection::Descending, 1, 1) == "sorted descending");
    CHECK(ui::sortMarkText(ui::SortDirection::Ascending, 2, 3) == " \u25B22");
    CHECK(ui::sortMarkText(ui::SortDirection::Descending, 1, 1) == " \u25BC");
}
