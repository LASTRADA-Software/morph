// SPDX-License-Identifier: Apache-2.0

// The edges of a table's configuration functions: layout gestures that are refused or change nothing, value-filter
// plans at their bounds, typed operands, group filters, condition entries, the menus and the status line's text.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
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
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace ui = morph::ui;
namespace table = morph::table;

std::vector<ui::LayoutColumn> columnsOf(std::initializer_list<std::string> ids,
                                        table::ColumnKind kind = table::ColumnKind::Text) {
    std::vector<ui::LayoutColumn> out;
    for (std::string const& id : ids) {
        out.push_back({.id = id, .label = "Label " + id, .locked = false, .kind = kind});
    }
    return out;
}

std::vector<std::string> visibleOf(table::ColumnLayout const& layout) { return layout.visible(); }

std::vector<std::string> labelsOf(std::vector<ui::TableMenuEntry> const& entries) {
    std::vector<std::string> out;
    out.reserve(entries.size());
    for (auto const& entry : entries) {
        out.push_back(entry.entry.separator ? "-" : entry.entry.label);
    }
    return out;
}

ui::TableMenuEntry const& entryNamed(std::vector<ui::TableMenuEntry> const& entries, std::string_view label) {
    for (auto const& entry : entries) {
        if (entry.entry.label == label) {
            return entry;
        }
    }
    FAIL("no menu entry " << label);
    return entries.front();
}
}  // namespace

TEST_CASE("cov config: a sort gesture value outside the enum changes nothing", "[ui][table][config][cov]") {
    ui::SortChain const chain{{.column = "a", .dir = table::SortDirection::Descending}};
    auto const change = ui::applySortGesture(chain, "b", static_cast<ui::SortGesture>(99), 8);
    CHECK(change.chain == chain);
    CHECK_FALSE(change.refusal.has_value());
    // Add on a column already in the chain keeps the chain as it is and does not refuse.
    auto const held = ui::applySortGesture(chain, "a", ui::SortGesture::Add, 1);
    CHECK(held.chain == chain);
    CHECK_FALSE(held.refusal.has_value());
    // Adding past the bound is refused with the bound's text.
    auto const bounded = ui::applySortGesture(chain, "b", ui::SortGesture::Add, 1);
    CHECK(bounded.chain == chain);
    CHECK(bounded.refusal == ui::tableText::sortBound(1));
}

TEST_CASE("cov config: the status line's counts and capability messages", "[ui][table][config][cov]") {
    CHECK(ui::groupedCount(0) == "0");
    CHECK(ui::groupedCount(-1234567, ".") == "-1.234.567");
    CHECK(ui::groupedCount(std::numeric_limits<std::int64_t>::min()) == "-9,223,372,036,854,775,808");
    CHECK(ui::tableText::copied(1, 0) == "Copied 1 row");
    CHECK(ui::tableText::copied(2, 1) == "Copied 2 rows, 1 cell was not loaded");
    CHECK(ui::tableText::copied(1234, 2000, ".") == "Copied 1.234 rows, 2.000 cells were not loaded");
    CHECK(ui::tableText::unreadable(1) == "1 cell could not be read");
    CHECK(ui::tableText::unreadable(3) == "3 cells could not be read");
    CHECK(ui::tableText::notConfigurable(ui::Configure::Reorder) == "This table's columns cannot be moved");
    CHECK(ui::tableText::notConfigurable(ui::Configure::Hide) == "This table's columns cannot be hidden");
    CHECK(ui::tableText::notConfigurable(ui::Configure::Pin) == "This table's columns cannot be pinned");
    CHECK(ui::tableText::notConfigurable(ui::Configure::Resize) == "This table's columns cannot be resized");
    CHECK(ui::tableText::notConfigurable(ui::Configure::None) == "This table's columns cannot be moved");
    CHECK(ui::tableText::notConfigurable(ui::Configure::All) == "This table's columns cannot be moved");
}

TEST_CASE("cov config: a gesture on an unknown column, a locked one or a forbidden capability is refused",
          "[ui][table][config][cov]") {
    table::ColumnLayout layout{{"a", "b", "c"}};
    auto columns = columnsOf({"a", "b", "c"});
    columns.at(1).locked = true;
    auto const unknown = ui::applyLayoutGesture(layout, columns, ui::Configure::All, ui::HideColumn{.id = "zz"});
    CHECK_FALSE(unknown.changed);
    CHECK_FALSE(unknown.refusal.has_value());
    auto const locked = ui::applyLayoutGesture(layout, columns, ui::Configure::All, ui::HideColumn{.id = "b"});
    CHECK_FALSE(locked.changed);
    CHECK(locked.refusal == "Label b is locked");
    // Showing a column is not lockable.
    CHECK(visibleOf(layout) == std::vector<std::string>{"a", "b", "c"});
    auto const forbidden = ui::applyLayoutGesture(layout, columns, ui::Configure::Reorder, ui::HideColumn{.id = "a"});
    CHECK(forbidden.refusal == ui::tableText::notConfigurable(ui::Configure::Hide));
    CHECK(visibleOf(layout).size() == 3);
    auto const reset = ui::applyLayoutGesture(layout, columns, ui::Configure::None, ui::ResetColumns{});
    CHECK(reset.refusal == ui::tableText::notConfigurable(ui::Configure::None));
    auto const grouped =
        ui::applyLayoutGesture(layout, columns, ui::Configure::Pin | ui::Configure::Resize, ui::FormGroup{});
    CHECK(grouped.refusal == ui::tableText::notConfigurable(ui::Configure::Reorder));
    auto const ungrouped = ui::applyLayoutGesture(layout, columns, ui::Configure::Resize, ui::RemoveGroup{.id = "g1"});
    CHECK(ungrouped.refusal == ui::tableText::notConfigurable(ui::Configure::Reorder));
    // Hide alone is enough to group.
    auto const none = ui::applyLayoutGesture(layout, columns, ui::Configure::Hide, ui::RemoveGroup{.id = "g1"});
    CHECK_FALSE(none.refusal.has_value());
    CHECK_FALSE(none.changed);
}

TEST_CASE("cov config: moving columns stays on its side of the pin boundary", "[ui][table][config][cov]") {
    table::ColumnLayout layout{{"a", "b", "c", "d", "e"}};
    auto const columns = columnsOf({"a", "b", "c", "d", "e"});
    layout.setPinned(2);
    auto const apply = [&](ui::LayoutGesture const& gesture) {
        return ui::applyLayoutGesture(layout, columns, ui::Configure::All, gesture);
    };
    CHECK_FALSE(apply(ui::MoveColumn{.id = "a", .by = 0}).changed);
    // A pinned column moves inside the pinned columns and no further.
    CHECK(apply(ui::MoveColumn{.id = "a", .by = 5}).changed);
    CHECK(visibleOf(layout) == std::vector<std::string>{"b", "a", "c", "d", "e"});
    CHECK_FALSE(apply(ui::MoveColumn{.id = "a", .by = 5}).changed);
    CHECK(apply(ui::MoveColumn{.id = "a", .by = -5}).changed);
    CHECK(visibleOf(layout) == std::vector<std::string>{"a", "b", "c", "d", "e"});
    // An unpinned column moves inside the unpinned columns.
    CHECK_FALSE(apply(ui::MoveColumn{.id = "c", .by = -9}).changed);  // already first of the unpinned columns
    CHECK(visibleOf(layout) == std::vector<std::string>{"a", "b", "c", "d", "e"});
    CHECK(apply(ui::MoveColumn{.id = "c", .by = 2}).changed);
    CHECK(visibleOf(layout) == std::vector<std::string>{"a", "b", "d", "e", "c"});
    CHECK(layout.pinned() == 2);
    // A hidden column does not move.
    REQUIRE(layout.setVisible("e", false));
    CHECK_FALSE(apply(ui::MoveColumn{.id = "e", .by = 1}).changed);
    CHECK_FALSE(apply(ui::MoveColumnTo{.id = "e", .position = 0}).changed);
}

TEST_CASE("cov config: dropping a column among, past or at the edge of the pinned columns",
          "[ui][table][config][cov]") {
    table::ColumnLayout layout{{"a", "b", "c", "d"}};
    auto const columns = columnsOf({"a", "b", "c", "d"});
    layout.setPinned(2);
    auto const apply = [&](ui::LayoutGesture const& gesture) {
        return ui::applyLayoutGesture(layout, columns, ui::Configure::All, gesture);
    };
    // An unpinned column dropped before the pinned ones joins them.
    CHECK(apply(ui::MoveColumnTo{.id = "c", .position = 0}).changed);
    CHECK(visibleOf(layout) == std::vector<std::string>{"c", "a", "b", "d"});
    CHECK(layout.pinned() == 3);
    // A pinned column dropped past the pinned ones leaves them; a position past the end is the end.
    CHECK(apply(ui::MoveColumnTo{.id = "c", .position = 99}).changed);
    CHECK(visibleOf(layout) == std::vector<std::string>{"a", "b", "d", "c"});
    CHECK(layout.pinned() == 2);
    // A drop on the boundary keeps the column's state: unpinned stays unpinned.
    CHECK(apply(ui::MoveColumnTo{.id = "c", .position = 2}).changed);
    CHECK(visibleOf(layout) == std::vector<std::string>{"a", "b", "c", "d"});
    CHECK(layout.pinned() == 2);
    // A pinned column dropped on the boundary stays pinned.
    CHECK(apply(ui::MoveColumnTo{.id = "a", .position = 1}).changed);
    CHECK(visibleOf(layout) == std::vector<std::string>{"b", "a", "c", "d"});
    CHECK(layout.pinned() == 2);
    // The only visible column has nowhere to go.
    table::ColumnLayout single{{"a", "b"}};
    REQUIRE(single.setVisible("b", false));
    auto const result =
        ui::applyLayoutGesture(single, columns, ui::Configure::All, ui::MoveColumnTo{.id = "a", .position = 1});
    CHECK_FALSE(result.changed);
}

TEST_CASE("cov config: hiding, showing, pinning and unpinning keep the pin count true", "[ui][table][config][cov]") {
    table::ColumnLayout layout{{"a", "b", "c", "d"}};
    auto const columns = columnsOf({"a", "b", "c", "d"});
    auto const apply = [&](ui::LayoutGesture const& gesture) {
        return ui::applyLayoutGesture(layout, columns, ui::Configure::All, gesture);
    };
    // Pin with none pinned goes first; pin again goes after the pinned ones; a pinned column is not pinned twice.
    CHECK(apply(ui::PinColumn{.id = "c"}).changed);
    CHECK(visibleOf(layout) == std::vector<std::string>{"c", "a", "b", "d"});
    CHECK(layout.pinned() == 1);
    CHECK(apply(ui::PinColumn{.id = "d"}).changed);
    CHECK(visibleOf(layout) == std::vector<std::string>{"c", "d", "a", "b"});
    CHECK(layout.pinned() == 2);
    CHECK_FALSE(apply(ui::PinColumn{.id = "d"}).changed);
    // Unpin from a pair puts the column right after the pinned one left; unpin the last one puts it first of the rest.
    CHECK(apply(ui::UnpinColumn{.id = "c"}).changed);
    CHECK(visibleOf(layout) == std::vector<std::string>{"d", "c", "a", "b"});
    CHECK(layout.pinned() == 1);
    CHECK(apply(ui::UnpinColumn{.id = "d"}).changed);
    CHECK(layout.pinned() == 0);
    CHECK(visibleOf(layout) == std::vector<std::string>{"d", "c", "a", "b"});
    CHECK_FALSE(apply(ui::UnpinColumn{.id = "a"}).changed);
    // Hiding a pinned column unpins it; showing a column that lands among the pinned ones pins it.
    layout.setPinned(2);
    CHECK(apply(ui::HideColumn{.id = "d"}).changed);
    CHECK(layout.pinned() == 1);
    CHECK_FALSE(apply(ui::ShowColumn{.id = "c"}).changed);
    CHECK(apply(ui::ShowColumn{.id = "d"}).changed);
    CHECK(visibleOf(layout) == std::vector<std::string>{"d", "c", "a", "b"});
    CHECK(layout.pinned() == 2);
    // Hiding every column but one is refused.
    REQUIRE(apply(ui::HideColumn{.id = "a"}).changed);
    REQUIRE(apply(ui::HideColumn{.id = "b"}).changed);
    REQUIRE(apply(ui::HideColumn{.id = "c"}).changed);
    auto const last = apply(ui::HideColumn{.id = "d"});
    CHECK_FALSE(last.changed);
    CHECK(last.refusal == ui::tableText::lastVisible());
    // Unpinning the only pinned column of a single-column layout moves nothing.
    table::ColumnLayout lone{{"a"}};
    lone.setPinned(1);
    auto const one = columnsOf({"a"});
    CHECK(ui::applyLayoutGesture(lone, one, ui::Configure::All, ui::UnpinColumn{.id = "a"}).changed);
    CHECK(lone.pinned() == 0);
}

TEST_CASE("cov config: resizing, resetting, units and groups through gestures", "[ui][table][config][cov]") {
    table::ColumnLayout layout{{"a", "b", "c"}};
    auto columns = columnsOf({"a", "b", "c"});
    auto const apply = [&](ui::LayoutGesture const& gesture) {
        return ui::applyLayoutGesture(layout, columns, ui::Configure::All, gesture);
    };
    CHECK(apply(ui::ResizeColumn{.id = "a", .units = 12}).changed);
    CHECK(layout.widths().at("a") == 12);
    CHECK(apply(ui::ResizeColumn{.id = "a", .units = std::nullopt}).changed);
    CHECK(layout.widths().empty());
    CHECK(apply(ui::SetDisplayUnit{.id = "a", .dimension = "mass", .unit = "g"}).changed);
    CHECK(layout.units().at("a").at("mass") == "g");
    CHECK(apply(ui::SetDisplayUnit{.id = "a", .dimension = "mass", .unit = std::nullopt}).changed);
    CHECK(layout.units().empty());
    CHECK(apply(ui::MoveColumn{.id = "a", .by = 1}).changed);
    CHECK(apply(ui::ResetColumns{}).changed);
    CHECK(visibleOf(layout) == std::vector<std::string>{"a", "b", "c"});

    // A group of unknown columns changes nothing and says nothing.
    auto const unknown = apply(ui::FormGroup{.label = "G", .columns = {"a", "zz"}});
    CHECK_FALSE(unknown.changed);
    CHECK_FALSE(unknown.refusal.has_value());
    // A group has two or more columns.
    CHECK(apply(ui::FormGroup{.label = "G", .columns = {"a"}}).refusal == ui::tableText::groupSize());
    // And at most the bound.
    std::vector<std::string> ids;
    for (std::size_t at = 0; at <= ui::kMaxGroupColumns; ++at) {
        ids.push_back("c" + std::to_string(at));
    }
    table::ColumnLayout wideLayout{ids};
    std::vector<ui::LayoutColumn> wideColumns;
    wideColumns.reserve(ids.size());
    for (auto const& id : ids) {
        wideColumns.push_back({.id = id, .label = id, .locked = false, .kind = table::ColumnKind::Text});
    }
    CHECK(ui::applyLayoutGesture(wideLayout, wideColumns, ui::Configure::All,
                                 ui::FormGroup{.label = "G", .columns = ids})
              .refusal == ui::tableText::groupSize());
    // A group's columns hold one kind of value.
    columns.at(1).kind = table::ColumnKind::Integer;
    CHECK(apply(ui::FormGroup{.label = "G", .columns = {"a", "b"}}).refusal == ui::tableText::groupKind());
    columns.at(1).kind = table::ColumnKind::Text;
    // A column in a group joins no other group.
    CHECK(apply(ui::FormGroup{.label = "G", .columns = {"a", "b"}}).changed);
    CHECK(apply(ui::FormGroup{.label = "H", .columns = {"b", "c"}}).refusal ==
          ui::tableText::alreadyGrouped("Label b", "G"));
    // A column named twice is the layout's own refusal.
    auto const twice = apply(ui::FormGroup{.label = "H", .columns = {"c", "c"}});
    CHECK_FALSE(twice.changed);
    REQUIRE(twice.refusal.has_value());
    CHECK(twice.refusal->contains("named twice"));
    CHECK(apply(ui::RemoveGroup{.id = "g1"}).changed);
    CHECK(layout.groups().empty());
}

TEST_CASE("cov config: the ticks of a values list resolve in order", "[ui][table][config][cov]") {
    auto const under = [](std::string const& search) { return std::vector<std::string>{search + "1", search + "2"}; };
    ui::ValueTicks ticks;
    ticks.ticked = true;
    ticks.blanks = true;
    ticks.ops = {{.search = {}, .operand = "x", .ticked = false},
                 {.search = "ab", .operand = std::nullopt, .ticked = false}};
    auto const resolved = ui::resolveTicks(ticks, under);
    CHECK(resolved.ticked);
    CHECK(resolved.blanks == true);
    CHECK(resolved.covered == std::map<std::string, bool>{{"x", false}, {"ab1", false}, {"ab2", false}});
    // A tick with no search and no operand sets every value and forgets what came before.
    ticks.ops.push_back({.search = {}, .operand = std::nullopt, .ticked = false});
    auto const cleared = ui::resolveTicks(ticks, under);
    CHECK_FALSE(cleared.ticked);
    CHECK(cleared.covered.empty());
}

TEST_CASE("cov config: a value filter's plan picks the smaller side and gives up past the bound",
          "[ui][table][config][cov]") {
    using Side = ui::ValueFilterPlan::Side;
    ui::ResolvedTicks all{.ticked = true, .covered = {}, .blanks = std::nullopt};
    CHECK(ui::planValueFilter(10, all, 5).side == Side::All);
    all.blanks = false;
    CHECK(ui::planValueFilter(10, all, 5).side == Side::Exclude);  // blanks unticked: every value and no blank
    all.blanks = true;
    CHECK(ui::planValueFilter(10, all, 5).side == Side::All);
    ui::ResolvedTicks none{.ticked = false, .covered = {}, .blanks = std::nullopt};
    CHECK(ui::planValueFilter(10, none, 5).side == Side::Nothing);
    none.blanks = true;
    CHECK(ui::planValueFilter(10, none, 5).side == Side::Include);  // only the blanks
    CHECK(ui::planValueFilter(-3, none, 5).side == Side::All);      // no values at all, blanks ticked
    ui::ResolvedTicks const few{.ticked = false, .covered = {{"x", true}}, .blanks = std::nullopt};
    auto const include = ui::planValueFilter(10, few, 5);
    CHECK(include.side == Side::Include);
    CHECK(include.members == 1);
    CHECK_FALSE(include.needsComplement);
    ui::ResolvedTicks const most{.ticked = true, .covered = {{"x", false}}, .blanks = std::nullopt};
    auto const exclude = ui::planValueFilter(10, most, 5);
    CHECK(exclude.side == Side::Exclude);
    CHECK(exclude.members == 1);
    CHECK_FALSE(exclude.needsComplement);
    // Both sides over the bound: the list is disabled.
    ui::ResolvedTicks const both{
        .ticked = false, .covered = {{"a", true}, {"b", true}, {"c", true}, {"d", true}}, .blanks = std::nullopt};
    auto const over = ui::planValueFilter(100, both, 3);
    CHECK(over.side == Side::Disabled);
    CHECK(over.members == 4);
    // The values the ticks do not name sit on the filtered side when that side holds the unnamed ones.
    ui::ResolvedTicks const complement{
        .ticked = true, .covered = {{"a", false}, {"b", false}, {"c", false}}, .blanks = std::nullopt};
    auto const needs = ui::planValueFilter(4, complement, 5);
    CHECK(needs.side == Side::Include);
    CHECK(needs.members == 1);
    CHECK(needs.needsComplement);
}

TEST_CASE("cov config: valueFilter builds the entries of a plan", "[ui][table][config][cov]") {
    using Side = ui::ValueFilterPlan::Side;
    CHECK_FALSE(ui::valueFilter(Side::All, {"a"}, std::nullopt).has_value());
    CHECK_FALSE(ui::valueFilter(Side::Nothing, {"a"}, std::nullopt).has_value());
    CHECK_FALSE(ui::valueFilter(Side::Disabled, {"a"}, std::nullopt).has_value());
    CHECK_FALSE(ui::valueFilter(Side::Include, {}, std::nullopt).has_value());
    auto const include = ui::valueFilter(Side::Include, {"a", "b"}, true);
    REQUIRE(include.has_value());
    REQUIRE(include->include.size() == 2);
    CHECK(include->include.front().oneOf == std::vector<std::string>{"a", "b"});
    CHECK(include->include.back().isEmpty == true);
    CHECK(include->exclude.empty());
    auto const exclude = ui::valueFilter(Side::Exclude, {"a"}, false);
    REQUIRE(exclude.has_value());
    REQUIRE(exclude->exclude.size() == 2);
    CHECK(exclude->exclude.back().isEmpty == true);
    CHECK(exclude->include.empty());
    auto const blanksOnly = ui::valueFilter(Side::Include, {}, true);
    REQUIRE(blanksOnly.has_value());
    REQUIRE(blanksOnly->include.size() == 1);
    CHECK(blanksOnly->include.front().isEmpty == true);
    // A date bucket is one between entry; a member with no range is both bounds.
    auto const ranges =
        ui::valueFilter(Side::Include, {"2024-01-01..2024-01-31", "2024-03-05"}, std::nullopt, ui::ValueForm::Ranges);
    REQUIRE(ranges.has_value());
    REQUIRE(ranges->include.size() == 2);
    CHECK(ranges->include.front().between == std::array<std::string, 2>{"2024-01-01", "2024-01-31"});
    CHECK(ranges->include.back().between == std::array<std::string, 2>{"2024-03-05", "2024-03-05"});
}

TEST_CASE("cov config: classifyFilter tells a values list from a condition", "[ui][table][config][cov]") {
    using table::ColumnKind;
    using ui::FilterPart;
    auto const entryWith = [](auto set) {
        table::FilterEntry entry;
        set(entry);
        return entry;
    };
    auto const oneOf = entryWith([](table::FilterEntry& e) { e.oneOf = std::vector<std::string>{"a"}; });
    auto const blank = entryWith([](table::FilterEntry& e) { e.isEmpty = true; });
    auto const between = entryWith([](table::FilterEntry& e) { e.between = std::array<std::string, 2>{"a", "b"}; });
    auto const eq = entryWith([](table::FilterEntry& e) { e.eq = "a"; });
    table::ColumnFilter filter;
    CHECK(ui::classifyFilter(filter, ColumnKind::Text) == FilterPart::None);
    filter.include = {oneOf, blank};
    CHECK(ui::classifyFilter(filter, ColumnKind::Text) == FilterPart::Values);
    filter.combine = table::Combine::All;
    CHECK(ui::classifyFilter(filter, ColumnKind::Text) == FilterPart::Conditions);
    filter.combine = table::Combine::Any;
    filter.exclude = {blank};
    CHECK(ui::classifyFilter(filter, ColumnKind::Text) == FilterPart::Conditions);
    filter.exclude.clear();
    filter.include = {oneOf, oneOf};
    CHECK(ui::classifyFilter(filter, ColumnKind::Text) == FilterPart::Conditions);
    filter.include = {blank, blank};
    CHECK(ui::classifyFilter(filter, ColumnKind::Text) == FilterPart::Conditions);
    filter.include = {between};
    CHECK(ui::classifyFilter(filter, ColumnKind::Date) == FilterPart::Values);
    CHECK(ui::classifyFilter(filter, ColumnKind::Text) == FilterPart::Conditions);
    filter.include = {oneOf, between};
    CHECK(ui::classifyFilter(filter, ColumnKind::Date) == FilterPart::Conditions);
    filter.include = {eq};
    CHECK(ui::classifyFilter(filter, ColumnKind::Text) == FilterPart::Conditions);
    auto sensitive = oneOf;
    sensitive.caseSensitive = true;
    filter.include = {sensitive};
    CHECK(ui::classifyFilter(filter, ColumnKind::Text) == FilterPart::Conditions);
    auto twoOperators = oneOf;
    twoOperators.eq = "a";
    filter.include = {twoOperators};
    CHECK(ui::classifyFilter(filter, ColumnKind::Text) == FilterPart::Conditions);
    auto notBlank = blank;
    notBlank.isEmpty = false;
    filter.include = {notBlank};
    CHECK(ui::classifyFilter(filter, ColumnKind::Text) == FilterPart::Conditions);
    // Blanks alone is a values list, except on a column of custom values.
    filter.include = {blank};
    CHECK(ui::classifyFilter(filter, ColumnKind::Text) == FilterPart::Values);
    CHECK(ui::classifyFilter(filter, ColumnKind::Custom) == FilterPart::Conditions);
    filter.include = {};
    filter.exclude = {oneOf};
    CHECK(ui::classifyFilter(filter, ColumnKind::Text) == FilterPart::Values);
}

TEST_CASE("cov config: canonicalOperand reads what was typed by the column's kind", "[ui][table][config][cov]") {
    using table::ColumnKind;
    auto const& en = ui::localeFacts("en");
    auto const& de = ui::localeFacts("de");
    CHECK(ui::canonicalOperand("12", ColumnKind::Integer, en) == "12");
    CHECK(ui::canonicalOperand("  -12  ", ColumnKind::Key, en) == "-12");
    CHECK_FALSE(ui::canonicalOperand("", ColumnKind::Integer, en).has_value());
    CHECK_FALSE(ui::canonicalOperand("   ", ColumnKind::Integer, en).has_value());
    CHECK_FALSE(ui::canonicalOperand("1.5", ColumnKind::Integer, en).has_value());
    CHECK_FALSE(ui::canonicalOperand("abc", ColumnKind::Integer, en).has_value());
    CHECK_FALSE(ui::canonicalOperand("-", ColumnKind::Decimal, en).has_value());
    CHECK(ui::canonicalOperand("1.5", ColumnKind::Decimal, en) == "1.5");
    CHECK(ui::canonicalOperand("1,5", ColumnKind::Number, de) == "1.5");
    CHECK(ui::canonicalOperand("-2", ColumnKind::Quantity, en) == "-2");
    CHECK_FALSE(ui::canonicalOperand("1.", ColumnKind::Decimal, en).has_value());
    CHECK_FALSE(ui::canonicalOperand(".5", ColumnKind::Decimal, en).has_value());
    for (auto const kind :
         {ColumnKind::Text, ColumnKind::Date, ColumnKind::DateTime, ColumnKind::Bool, ColumnKind::Custom}) {
        CHECK(ui::canonicalOperand(" as typed ", kind, en) == " as typed ");
    }
    std::vector<table::UnitInfo> const units{
        table::UnitInfo{.id = "kg",
                        .symbol = "kg",
                        .dimension = "mass",
                        .toBase = morph::math::Rational{1, morph::math::DecimalPlaces{0}},
                        .decimals = 3}};
    CHECK(ui::canonicalOperand("2.5 kg", ColumnKind::Measure, en, units) == "2.5 kg");
    CHECK(ui::canonicalOperand("2.5", ColumnKind::Measure, en, units, "kg") == "2.5 kg");
    CHECK_FALSE(ui::canonicalOperand("2.5 lb", ColumnKind::Measure, en, units).has_value());
    CHECK(ui::canonicalOperand("x", static_cast<ColumnKind>(99), en) == "x");
}

TEST_CASE("cov config: groupValueFilter checks the match against the group's size", "[ui][table][config][cov]") {
    using ui::GroupMatch;
    auto const together = ui::groupValueFilter("g1", 3, {"a", "b"}, std::nullopt, GroupMatch::Together, 0);
    REQUIRE(together.has_value());
    CHECK(together->mode == table::GroupMode::Together);
    REQUIRE(together->include.size() == 2);
    CHECK(together->include.front().eq == "a");
    CHECK_FALSE(ui::groupValueFilter("g1", 3, {"a"}, std::nullopt, GroupMatch::Together, 0).has_value());
    auto const tooMany = ui::groupValueFilter("g1", 2, {"a", "b", "c"}, std::nullopt, GroupMatch::Together, 0);
    REQUIRE_FALSE(tooMany.has_value());
    CHECK(tooMany.error() == "All of them, each in a column, takes 2 to 2 values");

    auto const any = ui::groupValueFilter("g1", 3, {"a"}, true, GroupMatch::AnyColumn, 0);
    REQUIRE(any.has_value());
    CHECK(any->match.kind == table::ColumnCount::Kind::Any);
    REQUIRE(any->include.size() == 2);
    CHECK(any->include.back().isEmpty == true);
    auto const blanksOnly = ui::groupValueFilter("g1", 3, {}, true, GroupMatch::AnyColumn, 0);
    REQUIRE(blanksOnly.has_value());
    REQUIRE(blanksOnly->include.size() == 1);
    CHECK(blanksOnly->include.front().isEmpty == true);
    // Blanks count only for "in any column".
    CHECK_FALSE(ui::groupValueFilter("g1", 3, {}, true, GroupMatch::EveryColumn, 0).has_value());
    CHECK(ui::groupValueFilter("g1", 3, {}, true, GroupMatch::EveryColumn, 0).error() == "No value is ticked");
    auto const every = ui::groupValueFilter("g1", 3, {"a"}, true, GroupMatch::EveryColumn, 0);
    REQUIRE(every.has_value());
    CHECK(every->match.kind == table::ColumnCount::Kind::All);
    CHECK(every->include.size() == 1);

    auto const atLeast = ui::groupValueFilter("g1", 3, {"a"}, std::nullopt, GroupMatch::AtLeast, 2);
    REQUIRE(atLeast.has_value());
    CHECK(atLeast->match.kind == table::ColumnCount::Kind::AtLeast);
    CHECK(atLeast->match.n == 2);
    CHECK(ui::groupValueFilter("g1", 3, {"a"}, std::nullopt, GroupMatch::AtLeast, 0).error() == "N is 1 to 3");
    CHECK(ui::groupValueFilter("g1", 3, {"a"}, std::nullopt, GroupMatch::AtLeast, 4).error() == "N is 1 to 3");
    auto const exactly = ui::groupValueFilter("g1", 3, {"a"}, std::nullopt, GroupMatch::Exactly, 0);
    REQUIRE(exactly.has_value());
    CHECK(exactly->match.kind == table::ColumnCount::Kind::Exactly);
    CHECK(exactly->match.n == 0);
    CHECK(ui::groupValueFilter("g1", 3, {"a"}, std::nullopt, GroupMatch::Exactly, -1).error() == "N is 0 to 3");
    CHECK(ui::groupValueFilter("g1", 3, {"a"}, std::nullopt, GroupMatch::Exactly, 4).error() == "N is 0 to 3");
    CHECK(ui::groupValueFilter("g1", 3, {"a"}, std::nullopt, GroupMatch::Exactly, 3).has_value());
}

TEST_CASE("cov config: conditions by column kind, their labels and their entries", "[ui][table][config][cov]") {
    using table::ColumnKind;
    using ui::ConditionOp;
    CHECK(ui::conditionOps(ColumnKind::Text) == std::vector{ConditionOp::Equals, ConditionOp::NotEquals,
                                                            ConditionOp::Contains, ConditionOp::BeginsWith,
                                                            ConditionOp::EndsWith});
    CHECK(ui::conditionOps(ColumnKind::Date).size() == 7);
    CHECK(ui::conditionOps(ColumnKind::Measure).back() == ConditionOp::Between);
    CHECK(ui::conditionOps(ColumnKind::Bool).empty());
    CHECK(ui::conditionOps(ColumnKind::Custom) == std::vector{ConditionOp::IsEmpty, ConditionOp::IsNotEmpty});
    CHECK(ui::conditionOps(static_cast<ColumnKind>(99)).empty());

    CHECK(ui::conditionLabel(ConditionOp::Equals, ColumnKind::Text) == "equals");
    CHECK(ui::conditionLabel(ConditionOp::Equals, ColumnKind::Integer) == "=");
    CHECK(ui::conditionLabel(ConditionOp::NotEquals, ColumnKind::Text) == "does not equal");
    CHECK(ui::conditionLabel(ConditionOp::NotEquals, ColumnKind::Integer) == "≠");
    CHECK(ui::conditionLabel(ConditionOp::Contains, ColumnKind::Text) == "contains");
    CHECK(ui::conditionLabel(ConditionOp::BeginsWith, ColumnKind::Text) == "begins with");
    CHECK(ui::conditionLabel(ConditionOp::EndsWith, ColumnKind::Text) == "ends with");
    CHECK(ui::conditionLabel(ConditionOp::Less, ColumnKind::Integer) == "<");
    CHECK(ui::conditionLabel(ConditionOp::LessOrEqual, ColumnKind::Integer) == "≤");
    CHECK(ui::conditionLabel(ConditionOp::Greater, ColumnKind::Integer) == ">");
    CHECK(ui::conditionLabel(ConditionOp::GreaterOrEqual, ColumnKind::Integer) == "≥");
    CHECK(ui::conditionLabel(ConditionOp::Between, ColumnKind::Integer) == "between");
    CHECK(ui::conditionLabel(ConditionOp::IsEmpty, ColumnKind::Custom) == "is empty");
    CHECK(ui::conditionLabel(ConditionOp::IsNotEmpty, ColumnKind::Custom) == "is not empty");
    CHECK(ui::conditionLabel(static_cast<ConditionOp>(99), ColumnKind::Text).empty());

    CHECK(ui::conditionEntry(ConditionOp::Equals, "a", "", true).eq == "a");
    CHECK(ui::conditionEntry(ConditionOp::Equals, "a", "", true).caseSensitive == true);
    CHECK_FALSE(ui::conditionEntry(ConditionOp::Equals, "a", "", false).caseSensitive.has_value());
    CHECK(ui::conditionEntry(ConditionOp::NotEquals, "a", "", false).ne == "a");
    CHECK(ui::conditionEntry(ConditionOp::Contains, "a", "", true).contains == "a");
    CHECK(ui::conditionEntry(ConditionOp::BeginsWith, "a", "", true).startsWith == "a");
    CHECK(ui::conditionEntry(ConditionOp::EndsWith, "a", "", true).endsWith == "a");
    CHECK(ui::conditionEntry(ConditionOp::Less, "1", "", true).lt == "1");
    CHECK(ui::conditionEntry(ConditionOp::LessOrEqual, "1", "", true).le == "1");
    CHECK(ui::conditionEntry(ConditionOp::Greater, "1", "", true).gt == "1");
    CHECK(ui::conditionEntry(ConditionOp::GreaterOrEqual, "1", "", true).ge == "1");
    // Case matters only for text operators.
    CHECK_FALSE(ui::conditionEntry(ConditionOp::Less, "1", "", true).caseSensitive.has_value());
    CHECK(ui::conditionEntry(ConditionOp::Between, "1", "9", true).between == std::array<std::string, 2>{"1", "9"});
    CHECK(ui::conditionEntry(ConditionOp::IsEmpty, "", "", false).isEmpty == true);
    CHECK(ui::conditionEntry(ConditionOp::IsNotEmpty, "", "", false).notEmpty == true);
    CHECK(ui::detail::operatorCount(ui::conditionEntry(static_cast<ConditionOp>(99), "a", "b", true)) == 0);
}

TEST_CASE("cov config: the header menu offers what the column and the table allow", "[ui][table][config][cov]") {
    ui::HeaderMenuInput input;
    input.column = {.id = "a", .label = "A", .locked = false, .kind = table::ColumnKind::Text};
    input.sortable = true;
    input.filterable = true;
    input.visibleIndex = 1;
    input.visibleCount = 3;
    input.dimensions = {{.dimension = "mass", .units = {"kg", "g"}, .chosen = "g"}};
    auto const full = ui::headerMenu(input);
    CHECK(labelsOf(full) == std::vector<std::string>{"Sort ascending", "Sort descending", "Add to sort",
                                                     "Remove from sort", "-", "Filter…", "-", "Move left",
                                                     "Move right", "Pin column", "Hide column", "-", "Display unit"});
    CHECK(entryNamed(full, "Add to sort").entry.enabled);
    CHECK_FALSE(entryNamed(full, "Remove from sort").entry.enabled);
    auto const& display = entryNamed(full, "Display unit");
    REQUIRE(display.items.size() == 2);
    CHECK(display.items.front().entry.label == "mass: kg");
    CHECK(display.items.front().entry.checked != true);
    CHECK(display.items.back().entry.checked == true);
    CHECK(display.items.back().argument == std::string{"mass\x1F"} + "g");

    // A column already sorted on can be removed from the sort, not added to it.
    input.sortPlace = 0;
    input.chainSize = 1;
    auto const held = ui::headerMenu(input);
    CHECK_FALSE(entryNamed(held, "Add to sort").entry.enabled);
    CHECK(entryNamed(held, "Remove from sort").entry.enabled);
    // A full chain refuses another key.
    input.sortPlace = std::nullopt;
    input.chainSize = input.maxSortKeys;
    CHECK_FALSE(entryNamed(ui::headerMenu(input), "Add to sort").entry.enabled);
    input.chainSize = 0;

    // The edges of the pinned block: nothing moves across the boundary and the first pinned column offers Unpin.
    input.pinned = 1;
    input.visibleIndex = 1;
    auto const boundary = ui::headerMenu(input);
    CHECK_FALSE(entryNamed(boundary, "Move left").entry.enabled);
    CHECK(entryNamed(boundary, "Move right").entry.enabled);
    input.visibleIndex = 0;
    auto const pinnedFirst = ui::headerMenu(input);
    CHECK_FALSE(entryNamed(pinnedFirst, "Move left").entry.enabled);
    CHECK_FALSE(entryNamed(pinnedFirst, "Move right").entry.enabled);
    CHECK(entryNamed(pinnedFirst, "Unpin").command == ui::MenuCommand::UnpinColumn);
    input.pinned = 0;
    input.visibleIndex = 2;
    CHECK_FALSE(entryNamed(ui::headerMenu(input), "Move right").entry.enabled);

    // A locked column and a lone visible column disable the arranging commands.
    input.column.locked = true;
    CHECK_FALSE(entryNamed(ui::headerMenu(input), "Hide column").entry.enabled);
    input.column.locked = false;
    input.visibleCount = 1;
    input.visibleIndex = 0;
    CHECK_FALSE(entryNamed(ui::headerMenu(input), "Hide column").entry.enabled);

    // Only the capabilities the table allows are listed; groups add their filter.
    input.allowed = ui::Configure::None;
    input.sortable = false;
    input.filterable = false;
    input.dimensions.clear();
    input.groups = {{.id = "g1", .label = "Dims", .columns = 3}};
    auto const bare = ui::headerMenu(input);
    CHECK(labelsOf(bare) == std::vector<std::string>{"Filter Dims, 3 columns…"});
    CHECK(bare.front().command == ui::MenuCommand::FilterGroup);
    CHECK(bare.front().argument == "g1");
    input.allowed = ui::Configure::Reorder;
    CHECK(labelsOf(ui::headerMenu(input)) ==
          std::vector<std::string>{"Filter Dims, 3 columns…", "-", "Move left", "Move right"});
}

TEST_CASE("cov config: the table menu and the views menu list what is available", "[ui][table][config][cov]") {
    ui::TableMenuInput input;
    input.allowed = ui::Configure::None;
    input.groups = {{.id = "g1", .label = "Dims", .columns = 2}};
    auto const bare = ui::tableMenu(input);
    CHECK(labelsOf(bare) == std::vector<std::string>{"Sort…", "Filter Dims, 2 columns…", "Clear all filters", "-",
                                                     "Copy", "Copy with headers"});
    CHECK_FALSE(entryNamed(bare, "Clear all filters").entry.enabled);
    input.anyFilter = true;
    input.allowed = ui::Configure::All;
    input.views = ui::ViewsMenuInput{};
    auto const full = ui::tableMenu(input);
    CHECK(entryNamed(full, "Clear all filters").entry.enabled);
    CHECK(entryNamed(full, "Columns…").command == ui::MenuCommand::Columns);
    CHECK(entryNamed(full, "Reset columns").command == ui::MenuCommand::ResetColumns);
    CHECK(entryNamed(full, "Views").command == ui::MenuCommand::None);
    CHECK(entryNamed(full, "Copy").entry.keys == "Ctrl+C");

    ui::ViewsMenuInput views;
    views.own = {"mine", "other"};
    views.shared = {"team"};
    views.current = "mine";
    views.modified = true;
    views.defaultOnMount = true;
    views.shares = true;
    auto const own = ui::viewsMenu(views);
    CHECK(labelsOf(own.items) == std::vector<std::string>{"mine (modified)", "other", "-", "Save view…",
                                                          "Rename view…", "Remove view", "Set as default",
                                                          "Start from the default view", "-", "Shared views", "team",
                                                          "-", "Publish view…"});
    CHECK(entryNamed(own.items, "mine (modified)").entry.checked == true);
    CHECK(entryNamed(own.items, "other").entry.checked != true);
    CHECK(entryNamed(own.items, "mine (modified)").argument == "mine");
    CHECK(entryNamed(own.items, "Rename view…").entry.enabled);
    CHECK(entryNamed(own.items, "Start from the default view").entry.checked == true);
    CHECK(entryNamed(own.items, "team").entry.checked != true);
    CHECK(entryNamed(own.items, "Shared views").entry.heading);
    CHECK(entryNamed(own.items, "team").command == ui::MenuCommand::ApplySharedView);

    // On a shared view the user's own views are not current, and it can be saved as the user's own.
    views.currentShared = true;
    views.current = "team";
    views.shares = false;
    views.modified = false;
    auto const shared = ui::viewsMenu(views);
    CHECK_FALSE(entryNamed(shared.items, "Rename view…").entry.enabled);
    CHECK(entryNamed(shared.items, "mine").entry.checked != true);
    CHECK(entryNamed(shared.items, "team").entry.checked == true);
    auto const& saveAs = entryNamed(shared.items, "Save as my view");
    CHECK(saveAs.argument == "team");
    CHECK(saveAs.command == ui::MenuCommand::SaveAsMyView);
    // A current name the user does not have is on none of them.
    views.current = "gone";
    views.currentShared = false;
    CHECK_FALSE(entryNamed(ui::viewsMenu(views).items, "Remove view").entry.enabled);
    views.current = std::nullopt;
    views.shared.clear();
    CHECK(labelsOf(ui::viewsMenu(views).items) ==
          std::vector<std::string>{"mine", "other", "-", "Save view…", "Rename view…", "Remove view", "Set as default",
                                   "Start from the default view"});
}

TEST_CASE("cov config: a menu choice is a leaf the user can act on", "[ui][table][config][cov]") {
    ui::HeaderMenuInput input;
    input.column = {.id = "a", .label = "A", .locked = false, .kind = table::ColumnKind::Text};
    input.sortable = true;
    input.visibleCount = 2;
    input.dimensions = {{.dimension = "mass", .units = {"kg"}, .chosen = "kg"}};
    auto const entries = ui::headerMenu(input);
    // [0..3] sort, [4] separator, [5] move left (index 0: disabled), [6] move right, ... display unit last.
    std::vector<std::size_t> const sortAscending{0};
    REQUIRE(ui::chosen(entries, sortAscending) != nullptr);
    CHECK(ui::chosen(entries, sortAscending)->command == ui::MenuCommand::SortAscending);
    CHECK(ui::chosen(entries, std::vector<std::size_t>{}) == nullptr);
    CHECK(ui::chosen(entries, std::vector<std::size_t>{99}) == nullptr);
    CHECK(ui::chosen(entries, std::vector<std::size_t>{4}) == nullptr);  // the separator
    CHECK(ui::chosen(entries, std::vector<std::size_t>{5}) == nullptr);  // Move left, disabled at index 0
    CHECK(ui::chosen(entries, std::vector<std::size_t>{entries.size() - 1}) == nullptr);  // a submenu, not a leaf
    auto const* const leaf = ui::chosen(entries, std::vector<std::size_t>{entries.size() - 1, 0});
    REQUIRE(leaf != nullptr);
    CHECK(leaf->entry.label == "mass: kg");
    CHECK(ui::chosen(entries, std::vector<std::size_t>{entries.size() - 1, 5}) == nullptr);

    ui::ViewsMenuInput views;
    views.shared = {"team"};
    auto const menu = std::vector<ui::TableMenuEntry>{ui::viewsMenu(views)};
    auto const shown = ui::menuEntries(menu);
    REQUIRE(shown.size() == 1);
    CHECK(shown.front().label == "Views");
    REQUIRE(shown.front().items.size() == menu.front().items.size());
    CHECK(shown.front().items.back().label == "team");
    // A heading is not a choice.
    CHECK(ui::chosen(menu.front().items, std::vector<std::size_t>{std::size_t{6}}) == nullptr);
}

TEST_CASE("cov config: copied fields flatten line breaks and tabs to spaces", "[ui][table][config][cov]") {
    std::string out;
    ui::detail::appendField(out, "a\tb\nc\rd\r\ne");
    CHECK(out == "a b c d e");
    std::string tail;
    ui::detail::appendField(tail, "x\r");
    CHECK(tail == "x ");
}
