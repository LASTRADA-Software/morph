// SPDX-License-Identifier: Apache-2.0

// The non-template definitions of <morph/ui/table_config.hpp>.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <iterator>
#include <map>
#include <morph/render/locale_format.hpp>
#include <morph/table/data_source.hpp>
#include <morph/table/engine.hpp>
#include <morph/table/filter.hpp>
#include <morph/table/sort.hpp>
#include <morph/ui/backend.hpp>
#include <morph/ui/locale_table.hpp>
#include <morph/ui/measure.hpp>
#include <morph/ui/table_config.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace morph::ui {

std::string groupedCount(std::int64_t n, std::string_view separator) {
    // The magnitude as unsigned, so the most negative count has one too.
    std::uint64_t magnitude = n < 0 ? std::uint64_t{0} - static_cast<std::uint64_t>(n) : static_cast<std::uint64_t>(n);
    std::string digits;
    for (bool more = true; more; more = magnitude != 0U) {
        digits.push_back(static_cast<char>('0' + (magnitude % 10U)));
        magnitude /= 10U;
    }
    std::string out = n < 0 ? "-" : "";
    for (std::size_t at = digits.size(); at-- > 0;) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): at < digits.size() by the loop's decrement test
        out.push_back(digits[at]);
        if (at != 0 && at % 3 == 0) {
            out.append(separator);
        }
    }
    return out;
}

}  // namespace morph::ui

namespace morph::ui::tableText {

std::string copied(std::size_t rows, std::size_t unloaded, std::string_view separator) {
    std::string out =
        "Copied " + groupedCount(static_cast<std::int64_t>(rows), separator) + (rows == 1 ? " row" : " rows");
    if (unloaded != 0) {
        out += ", " + groupedCount(static_cast<std::int64_t>(unloaded), separator) +
               (unloaded == 1 ? " cell was not loaded" : " cells were not loaded");
    }
    return out;
}

std::string notConfigurable(Configure what) {
    std::string_view verb = "moved";
    if (!allows(what, Configure::Reorder)) {
        if (allows(what, Configure::Hide)) {
            verb = "hidden";
        } else if (allows(what, Configure::Pin)) {
            verb = "pinned";
        } else if (allows(what, Configure::Resize)) {
            verb = "resized";
        }
    }
    return "This table's columns cannot be " + std::string{verb};
}

std::string unreadable(std::size_t cells, std::string_view separator) {
    return groupedCount(static_cast<std::int64_t>(cells), separator) +
           (cells == 1 ? " cell could not be read" : " cells could not be read");
}

}  // namespace morph::ui::tableText

namespace morph::ui {

SortChange applySortGesture(SortChain const& chain, std::string const& column, SortGesture gesture,
                            std::size_t maxSortKeys) {
    using table::SortDirection;
    auto const held = std::ranges::find(chain, column, &table::SortKey::column);
    auto const append = [&]() -> SortChange {
        if (chain.size() >= maxSortKeys) {
            return {.chain = chain, .refusal = tableText::sortBound(maxSortKeys)};
        }
        SortChain out = chain;
        out.push_back({.column = column, .dir = SortDirection::Ascending});
        return {.chain = std::move(out), .refusal = std::nullopt};
    };
    switch (gesture) {
        case SortGesture::Click: {
            bool const firstAscending =
                !chain.empty() && chain.front().column == column && chain.front().dir == SortDirection::Ascending;
            return {.chain = {{.column = column,
                               .dir = firstAscending ? SortDirection::Descending : SortDirection::Ascending}},
                    .refusal = std::nullopt};
        }
        case SortGesture::Ascending:
            return {.chain = {{.column = column, .dir = SortDirection::Ascending}}, .refusal = std::nullopt};
        case SortGesture::Descending:
            return {.chain = {{.column = column, .dir = SortDirection::Descending}}, .refusal = std::nullopt};
        case SortGesture::Extend: {
            if (held == chain.end()) {
                return append();
            }
            SortChain out = chain;
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): held points into chain, and out is a copy of chain
            auto& key = out[static_cast<std::size_t>(held - chain.begin())];
            key.dir = key.dir == SortDirection::Ascending ? SortDirection::Descending : SortDirection::Ascending;
            return {.chain = std::move(out), .refusal = std::nullopt};
        }
        case SortGesture::Add:
            return held == chain.end() ? append() : SortChange{.chain = chain, .refusal = std::nullopt};
        case SortGesture::Remove: {
            SortChain out = chain;
            std::erase_if(out, [&](table::SortKey const& key) { return key.column == column; });
            return {.chain = std::move(out), .refusal = std::nullopt};
        }
        default:
            break;
    }
    return {.chain = chain, .refusal = std::nullopt};
}

}  // namespace morph::ui

namespace morph::ui::detail {

GestureNeeds needsOf(LayoutGesture const& gesture) {
    return std::visit(
        [](auto const& alternative) -> GestureNeeds {
            using G = std::decay_t<decltype(alternative)>;
            if constexpr (std::is_same_v<G, MoveColumn> || std::is_same_v<G, MoveColumnTo>) {
                return {.capability = Configure::Reorder, .lockable = true, .column = alternative.id};
            } else if constexpr (std::is_same_v<G, HideColumn>) {
                return {.capability = Configure::Hide, .lockable = true, .column = alternative.id};
            } else if constexpr (std::is_same_v<G, ShowColumn>) {
                return {.capability = Configure::Hide, .lockable = false, .column = alternative.id};
            } else if constexpr (std::is_same_v<G, PinColumn> || std::is_same_v<G, UnpinColumn>) {
                return {.capability = Configure::Pin, .lockable = true, .column = alternative.id};
            } else if constexpr (std::is_same_v<G, ResizeColumn>) {
                return {.capability = Configure::Resize, .lockable = true, .column = alternative.id};
            } else {
                return {};
            }
        },
        gesture);
}

void placeBeside(table::ColumnLayout& layout, std::string const& columnId, std::string const& anchor, bool after) {
    if (anchor == columnId) {
        return;
    }
    std::vector<std::string> order(layout.order().begin(), layout.order().end());
    std::erase(order, columnId);
    auto const anchorIndex = static_cast<std::size_t>(std::ranges::find(order, anchor) - order.begin());
    layout.move(columnId, after ? anchorIndex + 1 : anchorIndex);
}

}  // namespace morph::ui::detail

namespace morph::ui {

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one visit over every gesture kind.
LayoutResult applyLayoutGesture(table::ColumnLayout& layout, std::span<LayoutColumn const> columns, Configure allowed,
                                LayoutGesture const& gesture) {
    auto const find = [&](std::string_view columnId) -> LayoutColumn const* {
        auto const found = std::ranges::find(columns, columnId, &LayoutColumn::id);
        return found == columns.end() ? nullptr : &*found;
    };
    detail::GestureNeeds const needs = detail::needsOf(gesture);
    LayoutColumn const* const column = needs.column.empty() ? nullptr : find(needs.column);
    if (needs.capability != Configure::None && column == nullptr) {
        return {};
    }
    if (needs.lockable && column->locked) {
        return {.changed = false, .refusal = tableText::locked(column->label)};
    }
    if (needs.capability != Configure::None && !allows(allowed, needs.capability)) {
        return {.changed = false, .refusal = tableText::notConfigurable(needs.capability)};
    }
    if (std::holds_alternative<ResetColumns>(gesture) && allowed == Configure::None) {
        return {.changed = false, .refusal = tableText::notConfigurable(Configure::None)};
    }
    bool const grouping = std::holds_alternative<FormGroup>(gesture) || std::holds_alternative<RemoveGroup>(gesture);
    if (grouping && (allowed & (Configure::Reorder | Configure::Hide)) == Configure::None) {
        return {.changed = false, .refusal = tableText::notConfigurable(Configure::Reorder)};
    }

    table::ColumnLayoutValue const before = layout.value();
    std::optional<std::string> refusal;
    std::visit(
        // NOLINTNEXTLINE(readability-function-cognitive-complexity): the visitor's arms are the same decision table.
        [&](auto const& change) {
            using G = std::decay_t<decltype(change)>;
            std::vector<std::string> const visible = layout.visible();
            [[maybe_unused]] std::size_t const pinned = layout.pinned();
            if constexpr (std::is_same_v<G, MoveColumn>) {
                std::size_t const from = detail::visibleIndex(visible, change.id);
                if (from == visible.size() || change.by == 0) {
                    return;
                }
                // A move stays on its side of the pin boundary; crossing it is Pin's and Unpin's.
                bool const isPinned = from < pinned;
                auto const first = static_cast<std::ptrdiff_t>(isPinned ? 0 : pinned);
                auto const last = static_cast<std::ptrdiff_t>(isPinned ? pinned : visible.size()) - 1;
                auto const target = std::clamp(static_cast<std::ptrdiff_t>(from) + change.by, first, last);
                if (static_cast<std::size_t>(target) != from) {
                    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): target is clamped to the visible columns' range
                    detail::placeBeside(layout, change.id, visible[static_cast<std::size_t>(target)], change.by > 0);
                }
            } else if constexpr (std::is_same_v<G, MoveColumnTo>) {
                std::size_t const from = detail::visibleIndex(visible, change.id);
                if (from == visible.size()) {
                    return;
                }
                std::vector<std::string> others = visible;
                std::erase(others, change.id);
                if (others.empty()) {
                    return;
                }
                std::size_t const target = std::min(change.position, others.size());
                if (target == others.size()) {
                    detail::placeBeside(layout, change.id, others.back(), true);
                } else {
                    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): target < others.size() by the branch above
                    detail::placeBeside(layout, change.id, others[target], false);
                }
                // The other pinned columns stay pinned: a drop among them pins the column, a drop past them unpins
                // it, and a drop on the boundary keeps its state.
                bool const wasPinned = from < pinned;
                std::size_t const otherPinned = pinned - (wasPinned ? 1 : 0);
                bool const nowPinned = target < otherPinned || (target == otherPinned && wasPinned);
                layout.setPinned(otherPinned + (nowPinned ? 1 : 0));
            } else if constexpr (std::is_same_v<G, HideColumn>) {
                bool const wasPinned = detail::visibleIndex(visible, change.id) < pinned;
                if (!layout.setVisible(change.id, false)) {
                    refusal = tableText::lastVisible();
                } else if (wasPinned) {
                    layout.setPinned(pinned - 1);
                }
            } else if constexpr (std::is_same_v<G, ShowColumn>) {
                if (detail::visibleIndex(visible, change.id) != visible.size() ||
                    !layout.setVisible(change.id, true)) {
                    return;
                }
                // A column shown among the pinned columns joins them, so none of them is pushed out.
                if (detail::visibleIndex(layout.visible(), change.id) < pinned) {
                    layout.setPinned(pinned + 1);
                }
            } else if constexpr (std::is_same_v<G, PinColumn>) {
                std::size_t const index = detail::visibleIndex(visible, change.id);
                if (index == visible.size() || index < pinned) {
                    return;
                }
                if (pinned == 0) {
                    detail::placeBeside(layout, change.id, visible.front(), false);
                } else {
                    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): pinned is at most the visible count, and nonzero here
                    detail::placeBeside(layout, change.id, visible[pinned - 1], true);
                }
                layout.setPinned(pinned + 1);
            } else if constexpr (std::is_same_v<G, UnpinColumn>) {
                std::size_t const index = detail::visibleIndex(visible, change.id);
                if (index >= pinned) {
                    return;
                }
                std::vector<std::string> others = visible;
                std::erase(others, change.id);
                if (pinned >= 2) {
                    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): pinned is at most the visible count, and others lacks at most the moved column
                    detail::placeBeside(layout, change.id, others[pinned - 2], true);
                } else if (!others.empty()) {
                    detail::placeBeside(layout, change.id, others.front(), false);
                }
                layout.setPinned(pinned - 1);
            } else if constexpr (std::is_same_v<G, ResizeColumn>) {
                static_cast<void>(layout.setWidth(change.id, change.units));
            } else if constexpr (std::is_same_v<G, ResetColumns>) {
                layout.reset();
            } else if constexpr (std::is_same_v<G, SetDisplayUnit>) {
                static_cast<void>(layout.setUnit(change.id, change.dimension, change.unit));
            } else if constexpr (std::is_same_v<G, FormGroup>) {
                if (std::ranges::any_of(change.columns,
                                        [&](std::string const& columnId) { return find(columnId) == nullptr; })) {
                    return;
                }
                if (change.columns.size() < 2 || change.columns.size() > kMaxGroupColumns) {
                    refusal = tableText::groupSize();
                    return;
                }
                LayoutColumn const& first = *find(change.columns.front());
                for (std::string const& columnId : change.columns) {
                    LayoutColumn const& member = *find(columnId);
                    if (member.kind != first.kind) {
                        refusal = tableText::groupKind();
                        return;
                    }
                    for (table::ColumnGroupInfo const& group : layout.groups()) {
                        if (std::ranges::find(group.columns, columnId) != group.columns.end()) {
                            refusal = tableText::alreadyGrouped(member.label, group.label);
                            return;
                        }
                    }
                }
                if (auto formed = layout.formGroup(change.label, change.columns); !formed) {
                    refusal = std::move(formed.error().message);
                }
            } else if constexpr (std::is_same_v<G, RemoveGroup>) {
                static_cast<void>(layout.removeGroup(change.id));
            }
        },
        gesture);
    if (refusal) {
        return {.changed = false, .refusal = std::move(refusal)};
    }
    return {.changed = layout.value() != before, .refusal = std::nullopt};
}

ResolvedTicks resolveTicks(ValueTicks const& ticks,
                           std::function<std::vector<std::string>(std::string const& search)> const& valuesUnder) {
    ResolvedTicks out{.ticked = ticks.ticked, .covered = {}, .blanks = ticks.blanks};
    for (TickOp const& op : ticks.ops) {
        if (op.operand) {
            out.covered.insert_or_assign(*op.operand, op.ticked);
        } else if (op.search.empty()) {
            out.covered.clear();
            out.ticked = op.ticked;
        } else {
            for (std::string const& operand : valuesUnder(op.search)) {
                out.covered.insert_or_assign(operand, op.ticked);
            }
        }
    }
    return out;
}

ValueFilterPlan planValueFilter(std::int64_t distinct, ResolvedTicks const& ticks, std::size_t maxSetValues) {
    using PlanSide = ValueFilterPlan::Side;
    std::size_t const total = distinct < 0 ? 0 : static_cast<std::size_t>(distinct);
    std::size_t const reached = std::min(ticks.covered.size(), total);
    auto const tickedCovered =
        static_cast<std::size_t>(std::ranges::count_if(ticks.covered, [](auto const& entry) { return entry.second; }));
    std::size_t const ticked = std::min(total, (ticks.ticked ? total - reached : 0) + tickedCovered);
    std::size_t const unticked = total - ticked;
    if (ticked == total && ticks.blanks != false) {
        return {.side = PlanSide::All, .members = 0, .needsComplement = false};
    }
    if (ticked == 0 && ticks.blanks != true) {
        return {.side = PlanSide::Nothing, .members = 0, .needsComplement = false};
    }
    bool const include = ticked < unticked;
    std::size_t const members = include ? ticked : unticked;
    if (members > maxSetValues) {
        return {.side = PlanSide::Disabled, .members = members, .needsComplement = false};
    }
    // The values outside C are on the side `ticks.ticked` puts them.
    bool const outsideOnSide = total > reached && (include == ticks.ticked);
    return {
        .side = include ? PlanSide::Include : PlanSide::Exclude, .members = members, .needsComplement = outsideOnSide};
}

std::optional<table::ColumnFilter> valueFilter(ValueFilterPlan::Side side, std::vector<std::string> members,
                                               std::optional<bool> blanks, ValueForm form) {
    using PlanSide = ValueFilterPlan::Side;
    if (side != PlanSide::Include && side != PlanSide::Exclude) {
        return std::nullopt;
    }
    std::vector<table::FilterEntry> entries;
    if (!members.empty()) {
        if (form == ValueForm::Set) {
            table::FilterEntry entry;
            entry.oneOf = std::move(members);
            entries.push_back(std::move(entry));
        } else {
            for (std::string const& member : members) {
                std::size_t const cut = member.find("..");
                table::FilterEntry entry;
                entry.between = std::array<std::string, 2>{member.substr(0, cut),
                                                           cut == std::string::npos ? member : member.substr(cut + 2)};
                entries.push_back(std::move(entry));
            }
        }
    }
    if (blanks == (side == PlanSide::Include)) {
        table::FilterEntry entry;
        entry.isEmpty = true;
        entries.push_back(std::move(entry));
    }
    if (entries.empty()) {
        return std::nullopt;
    }
    table::ColumnFilter filter{.combine = table::Combine::Any, .include = {}, .exclude = {}};
    (side == PlanSide::Include ? filter.include : filter.exclude) = std::move(entries);
    return filter;
}

}  // namespace morph::ui

namespace morph::ui::detail {

std::size_t operatorCount(table::FilterEntry const& entry) {
    return static_cast<std::size_t>(entry.eq.has_value()) + static_cast<std::size_t>(entry.ne.has_value()) +
           static_cast<std::size_t>(entry.lt.has_value()) + static_cast<std::size_t>(entry.le.has_value()) +
           static_cast<std::size_t>(entry.gt.has_value()) + static_cast<std::size_t>(entry.ge.has_value()) +
           static_cast<std::size_t>(entry.between.has_value()) + static_cast<std::size_t>(entry.contains.has_value()) +
           static_cast<std::size_t>(entry.startsWith.has_value()) +
           static_cast<std::size_t>(entry.endsWith.has_value()) + static_cast<std::size_t>(entry.isEmpty.has_value()) +
           static_cast<std::size_t>(entry.notEmpty.has_value()) + static_cast<std::size_t>(entry.oneOf.has_value());
}

bool valuesShape(std::span<table::FilterEntry const> entries, table::ColumnKind kind) {
    bool const dates = kind == table::ColumnKind::Date || kind == table::ColumnKind::DateTime;
    std::size_t oneOf = 0;
    std::size_t between = 0;
    std::size_t blanks = 0;
    for (table::FilterEntry const& entry : entries) {
        if (operatorCount(entry) != 1 || entry.caseSensitive.has_value()) {
            return false;
        }
        if (entry.oneOf) {
            ++oneOf;
        } else if (entry.between && dates) {
            ++between;
        } else if (entry.isEmpty == true) {
            ++blanks;
        } else {
            return false;
        }
    }
    if (oneOf > 1 || blanks > 1 || (oneOf == 1 && between != 0)) {
        return false;
    }
    if (oneOf == 0 && between == 0) {
        return blanks == 1 && kind != table::ColumnKind::Custom;
    }
    return true;
}

}  // namespace morph::ui::detail

namespace morph::ui {

FilterPart classifyFilter(table::ColumnFilter const& filter, table::ColumnKind kind) {
    if (filter.include.empty() && filter.exclude.empty()) {
        return FilterPart::None;
    }
    if (filter.combine != table::Combine::Any || (!filter.include.empty() && !filter.exclude.empty())) {
        return FilterPart::Conditions;
    }
    auto const& side = filter.include.empty() ? filter.exclude : filter.include;
    return detail::valuesShape(side, kind) ? FilterPart::Values : FilterPart::Conditions;
}

std::optional<std::string> canonicalOperand(std::string_view typed, table::ColumnKind kind, LocaleFacts const& locale,
                                            std::span<table::UnitInfo const> units, std::string_view chosenUnit) {
    using table::ColumnKind;
    auto const number = [&](bool fraction) -> std::optional<std::string> {
        std::size_t const first = typed.find_first_not_of(' ');
        std::size_t const last = typed.find_last_not_of(' ');
        if (first == std::string_view::npos) {
            return std::nullopt;
        }
        auto canonical = render::normalizeLocaleNumber(typed.substr(first, last - first + 1), locale.numeric);
        if (!canonical) {
            return std::nullopt;
        }
        std::string_view digits = *canonical;
        if (digits.starts_with('-')) {
            digits.remove_prefix(1);
        }
        std::size_t const point = digits.find('.');
        std::string_view const whole = digits.substr(0, point);
        std::string_view const part = point == std::string_view::npos ? std::string_view{} : digits.substr(point + 1);
        if (whole.empty() || (point != std::string_view::npos && (!fraction || part.empty()))) {
            return std::nullopt;
        }
        return canonical;
    };
    switch (kind) {
        case ColumnKind::Integer:
        case ColumnKind::Key:
            return number(false);
        case ColumnKind::Decimal:
        case ColumnKind::Quantity:
        case ColumnKind::Number:
            return number(true);
        case ColumnKind::Measure: {
            auto const measure = parseMeasure(typed, units, locale, chosenUnit);
            if (!measure) {
                return std::nullopt;
            }
            return measureOperand(*measure);
        }
        case ColumnKind::Text:
        case ColumnKind::Date:
        case ColumnKind::DateTime:
        case ColumnKind::Bool:
        case ColumnKind::Custom:
            return std::string{typed};
        default:
            break;
    }
    return std::string{typed};
}

std::expected<table::GroupFilter, std::string> groupValueFilter(std::string group, std::size_t groupColumns,
                                                                std::vector<std::string> ticked,
                                                                std::optional<bool> blanks, GroupMatch match,
                                                                std::int64_t n) {
    table::GroupFilter filter{.group = std::move(group)};
    if (match == GroupMatch::Together) {
        if (ticked.size() < 2 || ticked.size() > groupColumns) {
            return std::unexpected("All of them, each in a column, takes 2 to " + std::to_string(groupColumns) +
                                   " values");
        }
        filter.mode = table::GroupMode::Together;
        for (std::string& value : ticked) {
            table::FilterEntry entry;
            entry.eq = std::move(value);
            filter.include.push_back(std::move(entry));
        }
        return filter;
    }
    auto const columns = static_cast<std::int64_t>(groupColumns);
    switch (match) {
        case GroupMatch::AnyColumn:
            filter.match = {.kind = table::ColumnCount::Kind::Any, .n = 1};
            break;
        case GroupMatch::EveryColumn:
            filter.match = {.kind = table::ColumnCount::Kind::All, .n = 1};
            break;
        case GroupMatch::AtLeast:
        case GroupMatch::Exactly:
            if (n < (match == GroupMatch::AtLeast ? 1 : 0) || n > columns) {
                return std::unexpected("N is " + std::string{match == GroupMatch::AtLeast ? "1" : "0"} + " to " +
                                       std::to_string(groupColumns));
            }
            filter.match = {.kind = match == GroupMatch::AtLeast ? table::ColumnCount::Kind::AtLeast
                                                                 : table::ColumnCount::Kind::Exactly,
                            .n = n};
            break;
        case GroupMatch::Together:
        default:
            break;
    }
    bool const withBlanks = match == GroupMatch::AnyColumn && blanks == true;
    if (ticked.empty() && !withBlanks) {
        return std::unexpected(std::string{"No value is ticked"});
    }
    if (!ticked.empty()) {
        table::FilterEntry entry;
        entry.oneOf = std::move(ticked);
        filter.include.push_back(std::move(entry));
    }
    if (withBlanks) {
        table::FilterEntry entry;
        entry.isEmpty = true;
        filter.include.push_back(std::move(entry));
    }
    return filter;
}

std::vector<ConditionOp> conditionOps(table::ColumnKind kind) {
    using enum ConditionOp;
    using table::ColumnKind;
    switch (kind) {
        case ColumnKind::Text:
            return {Equals, NotEquals, Contains, BeginsWith, EndsWith};
        case ColumnKind::Integer:
        case ColumnKind::Key:
        case ColumnKind::Decimal:
        case ColumnKind::Quantity:
        case ColumnKind::Number:
        case ColumnKind::Date:
        case ColumnKind::DateTime:
        case ColumnKind::Measure:
            return {Equals, NotEquals, Less, LessOrEqual, Greater, GreaterOrEqual, Between};
        case ColumnKind::Bool:
            return {};
        case ColumnKind::Custom:
            return {IsEmpty, IsNotEmpty};
        default:
            break;
    }
    return {};
}

std::string_view conditionLabel(ConditionOp op, table::ColumnKind kind) {
    bool const words = kind == table::ColumnKind::Text;
    switch (op) {
        case ConditionOp::Equals:
            return words ? "equals" : "=";
        case ConditionOp::NotEquals:
            return words ? "does not equal" : "≠";
        case ConditionOp::Contains:
            return "contains";
        case ConditionOp::BeginsWith:
            return "begins with";
        case ConditionOp::EndsWith:
            return "ends with";
        case ConditionOp::Less:
            return "<";
        case ConditionOp::LessOrEqual:
            return "≤";
        case ConditionOp::Greater:
            return ">";
        case ConditionOp::GreaterOrEqual:
            return "≥";
        case ConditionOp::Between:
            return "between";
        case ConditionOp::IsEmpty:
            return "is empty";
        case ConditionOp::IsNotEmpty:
            return "is not empty";
        default:
            break;
    }
    return {};
}

table::FilterEntry conditionEntry(ConditionOp op, std::string operand, std::string second, bool matchCase) {
    table::FilterEntry entry;
    bool text = false;
    switch (op) {
        case ConditionOp::Equals:
            entry.eq = std::move(operand);
            text = true;
            break;
        case ConditionOp::NotEquals:
            entry.ne = std::move(operand);
            text = true;
            break;
        case ConditionOp::Contains:
            entry.contains = std::move(operand);
            text = true;
            break;
        case ConditionOp::BeginsWith:
            entry.startsWith = std::move(operand);
            text = true;
            break;
        case ConditionOp::EndsWith:
            entry.endsWith = std::move(operand);
            text = true;
            break;
        case ConditionOp::Less:
            entry.lt = std::move(operand);
            break;
        case ConditionOp::LessOrEqual:
            entry.le = std::move(operand);
            break;
        case ConditionOp::Greater:
            entry.gt = std::move(operand);
            break;
        case ConditionOp::GreaterOrEqual:
            entry.ge = std::move(operand);
            break;
        case ConditionOp::Between:
            entry.between = std::array<std::string, 2>{std::move(operand), std::move(second)};
            break;
        case ConditionOp::IsEmpty:
            entry.isEmpty = true;
            break;
        case ConditionOp::IsNotEmpty:
            entry.notEmpty = true;
            break;
        default:
            break;
    }
    if (text && matchCase) {
        entry.caseSensitive = true;
    }
    return entry;
}

}  // namespace morph::ui

namespace morph::ui::detail {

TableMenuEntry command(std::string label, MenuCommand command, bool enabled, std::string keys, std::string argument) {
    MenuEntry entry;
    entry.label = std::move(label);
    entry.keys = std::move(keys);
    entry.enabled = enabled;
    return {.entry = std::move(entry), .command = command, .argument = std::move(argument), .items = {}};
}

std::vector<TableMenuEntry> joinSections(std::vector<std::vector<TableMenuEntry>> sections) {
    std::vector<TableMenuEntry> out;
    for (auto& section : sections) {
        if (section.empty()) {
            continue;
        }
        if (!out.empty()) {
            MenuEntry line;
            line.separator = true;
            out.push_back(
                {.entry = std::move(line), .command = MenuCommand::SortAscending, .argument = {}, .items = {}});
        }
        std::ranges::move(section, std::back_inserter(out));
    }
    return out;
}

TableMenuEntry heading(std::string label) {
    MenuEntry entry;
    entry.label = std::move(label);
    entry.heading = true;
    entry.enabled = false;
    return {.entry = std::move(entry), .command = MenuCommand::None, .argument = {}, .items = {}};
}

TableMenuEntry viewEntry(std::string const& name, MenuCommand command, bool current, bool modified) {
    TableMenuEntry entry = detail::command(current && modified ? name + " (modified)" : name, command, true, {}, name);
    entry.entry.checked = current;
    return entry;
}

}  // namespace morph::ui::detail

namespace morph::ui {

TableMenuEntry viewsMenu(ViewsMenuInput const& input) {
    using detail::command;
    bool const onOwn =
        input.current.has_value() && !input.currentShared && std::ranges::contains(input.own, *input.current);
    bool const onShared =
        input.current.has_value() && input.currentShared && std::ranges::contains(input.shared, *input.current);
    std::vector<TableMenuEntry> own;
    own.reserve(input.own.size());
    for (std::string const& name : input.own) {
        own.push_back(
            detail::viewEntry(name, MenuCommand::ApplyView, onOwn && name == *input.current, input.modified));
    }
    std::vector<TableMenuEntry> manage;
    manage.push_back(command("Save view…", MenuCommand::SaveView));
    manage.push_back(command("Rename view…", MenuCommand::RenameView, onOwn));
    manage.push_back(command("Remove view", MenuCommand::RemoveView, onOwn));
    manage.push_back(command("Set as default", MenuCommand::SetDefaultView, onOwn));
    TableMenuEntry start = command("Start from the default view", MenuCommand::DefaultOnMount);
    start.entry.checked = input.defaultOnMount;
    manage.push_back(std::move(start));
    std::vector<TableMenuEntry> shared;
    if (!input.shared.empty()) {
        shared.push_back(detail::heading("Shared views"));
        for (std::string const& name : input.shared) {
            shared.push_back(detail::viewEntry(name, MenuCommand::ApplySharedView, onShared && name == *input.current,
                                               input.modified));
        }
    }
    std::vector<TableMenuEntry> sharing;
    if (onShared) {
        sharing.push_back(command("Save as my view", MenuCommand::SaveAsMyView, true, {}, *input.current));
    }
    if (input.shares) {
        sharing.push_back(command("Publish view…", MenuCommand::PublishView));
    }
    TableMenuEntry views = command("Views", MenuCommand::None);
    views.items = detail::joinSections({std::move(own), std::move(manage), std::move(shared), std::move(sharing)});
    return views;
}

std::vector<TableMenuEntry> headerMenu(HeaderMenuInput const& input) {
    using detail::command;
    std::vector<TableMenuEntry> sort;
    if (input.sortable) {
        bool const held = input.sortPlace.has_value();
        sort.push_back(command("Sort ascending", MenuCommand::SortAscending));
        sort.push_back(command("Sort descending", MenuCommand::SortDescending));
        sort.push_back(command("Add to sort", MenuCommand::AddToSort, !held && input.chainSize < input.maxSortKeys));
        sort.push_back(command("Remove from sort", MenuCommand::RemoveFromSort, held));
    }
    std::vector<TableMenuEntry> filter;
    if (input.filterable) {
        filter.push_back(command("Filter…", MenuCommand::Filter));
    }
    for (MenuGroup const& group : input.groups) {
        filter.push_back(command(detail::groupLabel(group), MenuCommand::FilterGroup, true, {}, group.id));
    }
    std::vector<TableMenuEntry> arrange;
    bool const free = !input.column.locked;
    if (allows(input.allowed, Configure::Reorder)) {
        arrange.push_back(command("Move left", MenuCommand::MoveLeft,
                                  free && input.visibleIndex > 0 && input.visibleIndex != input.pinned,
                                  "Ctrl+Shift+Left"));
        arrange.push_back(
            command("Move right", MenuCommand::MoveRight,
                    free && input.visibleIndex + 1 < input.visibleCount && input.visibleIndex + 1 != input.pinned,
                    "Ctrl+Shift+Right"));
    }
    if (allows(input.allowed, Configure::Pin)) {
        bool const pinned = input.visibleIndex < input.pinned;
        arrange.push_back(pinned ? command("Unpin", MenuCommand::UnpinColumn, free)
                                 : command("Pin column", MenuCommand::PinColumn, free));
    }
    if (allows(input.allowed, Configure::Hide)) {
        arrange.push_back(command("Hide column", MenuCommand::HideColumn, free && input.visibleCount > 1));
    }
    std::vector<TableMenuEntry> units;
    if (!input.dimensions.empty()) {
        TableMenuEntry display = command("Display unit", MenuCommand::DisplayUnit);
        for (MenuDimension const& dimension : input.dimensions) {
            for (std::string const& unit : dimension.units) {
                TableMenuEntry item = command(dimension.dimension + ": " + unit, MenuCommand::DisplayUnit, true, {},
                                              dimension.dimension + '\x1F' + unit);
                item.entry.checked = unit == dimension.chosen;
                display.items.push_back(std::move(item));
            }
        }
        units.push_back(std::move(display));
    }
    return detail::joinSections({std::move(sort), std::move(filter), std::move(arrange), std::move(units)});
}

std::vector<TableMenuEntry> tableMenu(TableMenuInput const& input) {
    using detail::command;
    bool const arranging = input.allowed != Configure::None;
    std::vector<TableMenuEntry> configure;
    if (arranging) {
        configure.push_back(command("Columns…", MenuCommand::Columns));
    }
    configure.push_back(command("Sort…", MenuCommand::SortDialog));
    for (MenuGroup const& group : input.groups) {
        configure.push_back(command(detail::groupLabel(group), MenuCommand::FilterGroup, true, {}, group.id));
    }
    configure.push_back(command("Clear all filters", MenuCommand::ClearAllFilters, input.anyFilter));
    if (arranging) {
        configure.push_back(command("Reset columns", MenuCommand::ResetColumns));
    }
    std::vector<TableMenuEntry> views;
    if (input.views) {
        views.push_back(viewsMenu(*input.views));
    }
    std::vector<TableMenuEntry> copy;
    copy.push_back(command("Copy", MenuCommand::Copy, true, "Ctrl+C"));
    copy.push_back(command("Copy with headers", MenuCommand::CopyWithHeaders));
    return detail::joinSections({std::move(configure), std::move(views), std::move(copy)});
}

// NOLINTNEXTLINE(misc-no-recursion): one call per submenu level, bounded by the menu's depth.
std::vector<MenuEntry> menuEntries(std::span<TableMenuEntry const> entries) {
    std::vector<MenuEntry> out;
    out.reserve(entries.size());
    for (TableMenuEntry const& entry : entries) {
        MenuEntry shown = entry.entry;
        shown.items = menuEntries(entry.items);
        out.push_back(std::move(shown));
    }
    return out;
}

TableMenuEntry const* chosen(std::span<TableMenuEntry const> entries, std::span<std::size_t const> path) {
    TableMenuEntry const* found = nullptr;
    for (std::size_t const index : path) {
        if (index >= entries.size()) {
            return nullptr;
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): index < entries.size() by the check above
        found = &entries[index];
        if (found->entry.separator || found->entry.heading || !found->entry.enabled) {
            return nullptr;
        }
        entries = found->items;
    }
    return found != nullptr && found->items.empty() ? found : nullptr;
}

}  // namespace morph::ui

namespace morph::ui::detail {

void appendField(std::string& out, std::string_view text) {
    for (std::size_t at = 0; at < text.size(); ++at) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): at < text.size() by the loop; at + 1 by the condition
        char const character = text[at];
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): at < text.size() by the loop; at + 1 by the condition
        if (character == '\r' && at + 1 < text.size() && text[at + 1] == '\n') {
            ++at;
            out.push_back(' ');
        } else if (character == '\t' || character == '\r' || character == '\n') {
            out.push_back(' ');
        } else {
            out.push_back(character);
        }
    }
}

}  // namespace morph::ui::detail

namespace morph::ui {

CopiedText copyText(std::span<std::string const> labels,
                    std::span<std::vector<std::optional<std::string>> const> rows) {
    CopiedText out;
    auto const line = [&](auto const& cells, auto const& textOf) {
        bool first = true;
        for (auto const& cell : cells) {
            if (!first) {
                out.text.push_back('\t');
            }
            first = false;
            detail::appendField(out.text, textOf(cell));
        }
    };
    bool any = false;
    if (!labels.empty()) {
        line(labels, [](std::string const& label) -> std::string_view { return label; });
        any = true;
    }
    for (auto const& row : rows) {
        if (any) {
            out.text.push_back('\n');
        }
        any = true;
        line(row, [&](std::optional<std::string> const& cell) -> std::string_view {
            if (!cell) {
                ++out.unloaded;
                return {};
            }
            return *cell;
        });
    }
    return out;
}

}  // namespace morph::ui
