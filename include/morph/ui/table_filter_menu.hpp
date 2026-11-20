// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file ui/table_filter_menu.hpp
/// @brief A table's filter menu: what it edits, the state it holds while open, the filter it writes, and its view
///        tree.
///
/// The menu edits one column's filter, or one group's: a values list (ticks over the distinct values the table's
/// rows list), or up to two conditions by the column's kind, never both. Ticks apply when the user presses Apply;
/// conditions apply on their own, `FilterMenuHost::debounce` after the last edit. The lowering of a table node owns
/// one `FilterMenu`, gives it a host that reaches the table's rows and filters, and mounts `filterMenuDialog` beside
/// the table. See `docs/spec/table/table.md`, "Filters from the header" and "Column groups".

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <morph/core/callback_scope.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/scheduler.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/table/filter.hpp>
#include <morph/table/query.hpp>
#include <morph/ui/cell_format.hpp>
#include <morph/ui/expression.hpp>
#include <morph/ui/locale_table.hpp>
#include <morph/ui/table_config.hpp>
#include <morph/ui/table_rows.hpp>
#include <morph/ui/view.hpp>
#include <morph/util/datetime.hpp>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace morph::ui {

/// @brief What a filter menu edits: one column, or one group of columns.
struct FilterTarget {
    /// @brief The column's id; empty for a group.
    std::string column;
    /// @brief A declared group's id, by which its values are pooled (`DistinctRequest::group`) and its filter named;
    ///        empty for a column and for a group the user formed.
    std::string group;
    /// @brief A group's columns, in the group's order; empty for a column.
    std::vector<std::string> columns;
    /// @brief Whether the group is one the user formed: it has no values list, and its filter names its columns.
    bool formed = false;
    /// @brief The menu's title: the column's or the group's label.
    std::string label;
    /// @brief The column as it shows its values, with its kind; a group's first column.
    table::TableColumn shown;
    /// @brief What a `Measure` value is shown through; its registry is kept alive by `keep`.
    DisplayUnits units;
    /// @brief The table's units, which a `Measure` operand is read with.
    std::vector<table::UnitInfo> unitList;
    /// @brief The units a `Measure` condition offers, by id.
    std::vector<std::string> unitChoices;
    /// @brief Whether the rows can list the column's values: the menu then shows a values list.
    bool listed = true;
    /// @brief Holds what `units` points into.
    std::shared_ptr<void const> keep;
};

/// @brief A group's filter in a table's filters, as the menu finds it.
/// @param filters The table's filters.
/// @param target  The group.
/// @return Its index in `filters.groups`: by id for a declared group, by its columns for one the user formed.
[[nodiscard]] inline std::optional<std::size_t> groupFilterIndex(table::FilterSpec const& filters,
                                                                 FilterTarget const& target) {
    for (std::size_t at = 0; at < filters.groups.size(); ++at) {
        table::GroupFilter const& group = filters.groups[at];
        bool const same = target.formed ? group.group.empty() && group.columns == target.columns
                                        : !target.group.empty() && group.group == target.group;
        if (same) {
            return at;
        }
    }
    return std::nullopt;
}

/// @brief One condition as the menu shows it while the user edits it.
struct ConditionDraft {
    /// @brief The condition; none while the user has chosen none.
    std::optional<ConditionOp> op;
    /// @brief The operand as typed, or a date as the picker gives it (`YYYY-MM-DD`).
    std::string operand;
    /// @brief `between`'s upper end, as typed.
    std::string second;
    /// @brief A `Measure` operand's unit when the user typed none: one of `FilterTarget::unitChoices`.
    std::string unit;
    /// @brief Whether the operand, or the second, does not read as the column's kind.
    bool invalid = false;

    /// @brief Memberwise equality.
    /// @return Whether every member matches.
    bool operator==(ConditionDraft const&) const = default;
};

/// @brief One entry of a values list: a value, or a date bucket of the tree.
struct FilterListEntry {
    /// @brief The value as the reply gave it.
    table::DistinctValue value;
    /// @brief A date tree's bucket; `Value` for a flat list.
    table::DateBucket bucket = table::DateBucket::Value;
    /// @brief The entry's depth in a date tree: 0 for a year, 1 for a month, 2 for a day.
    std::size_t depth = 0;
    /// @brief Whether a date bucket is opened, its children listed after it.
    bool opened = false;

    /// @brief Memberwise equality.
    /// @return Whether every member matches.
    bool operator==(FilterListEntry const&) const = default;
};

/// @brief What a filter menu holds while it is open.
struct FilterMenuState {
    /// @brief Whether the menu is open.
    bool open = false;
    /// @brief Counts the menu's openings: a new opening builds its content again.
    std::uint64_t opening = 0;
    /// @brief The column or group it edits.
    FilterTarget target;
    /// @brief The search typed.
    std::string search;
    /// @brief The values listed: the pages asked for under `search`, or the date tree.
    std::vector<FilterListEntry> list;
    /// @brief The values the search matches, as the last reply counted them.
    std::int64_t distinct = 0;
    /// @brief Every value of the column, the search aside; -1 until a reply without a search counted them.
    std::int64_t total = -1;
    /// @brief Rows with an empty cell, as the last reply counted them.
    std::int64_t blanks = 0;
    /// @brief Invalid cells, which no box ticks.
    std::int64_t invalid = 0;
    /// @brief Whether rows were left out because a cell was not loaded.
    bool partial = false;
    /// @brief Whether a reply to the values list came.
    bool listedOnce = false;
    /// @brief The ticks of a flat list, as operations.
    ValueTicks ticks;
    /// @brief The ticks of a date tree: each ticked bucket's operand, `a..b`.
    std::vector<std::string> dates;
    /// @brief The two conditions.
    std::array<ConditionDraft, 2> conditions;
    /// @brief "match case", for a text column's conditions.
    bool matchCase = false;
    /// @brief How the two conditions join.
    table::Combine combine = table::Combine::Any;
    /// @brief A group's match line, and its N.
    GroupMatch match = GroupMatch::AnyColumn;
    /// @brief The N of "in at least N columns" and "in exactly N columns", as typed.
    std::string count = "1";
    /// @brief Why the last Apply applied nothing; empty when it applied.
    std::string refusal;

    /// @brief Memberwise equality.
    /// @return Whether every member matches.
    bool operator==(FilterMenuState const& other) const {
        return open == other.open && opening == other.opening && search == other.search && list == other.list &&
               distinct == other.distinct && total == other.total && blanks == other.blanks &&
               invalid == other.invalid && partial == other.partial && listedOnce == other.listedOnce &&
               ticks.ticked == other.ticks.ticked && ticks.blanks == other.ticks.blanks &&
               ticks.ops.size() == other.ticks.ops.size() &&
               std::ranges::equal(ticks.ops, other.ticks.ops,
                                  [](TickOp const& a, TickOp const& b) {
                                      return a.search == b.search && a.operand == b.operand && a.ticked == b.ticked;
                                  }) &&
               dates == other.dates && conditions == other.conditions && matchCase == other.matchCase &&
               combine == other.combine && match == other.match && count == other.count &&
               refusal == other.refusal && target.column == other.target.column &&
               target.group == other.target.group && target.columns == other.target.columns;
    }
};

// ---------------------------------------------------------------------------------------------------------------
// Pure parts
// ---------------------------------------------------------------------------------------------------------------

namespace detail::filtermenu {

/// @brief Folds ASCII letters to lower case, as a search's text is compared locally.
/// @param text The text.
/// @return It, folded.
[[nodiscard]] inline std::string folded(std::string_view text) {
    std::string out{text};
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

/// @brief The day a `YYYY-MM-DD` date, or the date part of an ISO date-time, falls on.
/// @param text The text.
/// @return Days since 1970-01-01; nothing when it does not read.
[[nodiscard]] inline std::optional<std::int64_t> dayOf(std::string_view text) {
    if (text.size() < 10 || text[4] != '-' || text[7] != '-') {
        return std::nullopt;
    }
    int year = 0;
    unsigned month = 0;
    unsigned day = 0;
    char const* const at = text.data();
    if (std::from_chars(at, at + 4, year).ec != std::errc{} || std::from_chars(at + 5, at + 7, month).ec != std::errc{} ||
        std::from_chars(at + 8, at + 10, day).ec != std::errc{}) {
        return std::nullopt;
    }
    std::chrono::year_month_day const date{std::chrono::year{year}, std::chrono::month{month}, std::chrono::day{day}};
    if (!date.ok()) {
        return std::nullopt;
    }
    return std::chrono::sys_days{date}.time_since_epoch().count();
}

/// @brief A date bucket's range, as days.
/// @param operand The bucket's operand, `a..b`.
/// @return Its first and last day; nothing when it does not read.
[[nodiscard]] inline std::optional<std::pair<std::int64_t, std::int64_t>> rangeOf(std::string_view operand) {
    std::size_t const cut = operand.find("..");
    if (cut == std::string_view::npos) {
        return std::nullopt;
    }
    auto const first = dayOf(operand.substr(0, cut));
    auto const last = dayOf(operand.substr(cut + 2));
    if (!first || !last) {
        return std::nullopt;
    }
    return std::pair{*first, *last};
}

/// @brief A range of days as a date bucket's operand.
/// @param first    Its first day.
/// @param last     Its last day.
/// @param dateTime Whether the column holds date-times: its ends are `YYYY-MM-DDT00:00:00Z` and `…T23:59:59Z`.
/// @return `a..b`.
[[nodiscard]] inline std::string rangeText(std::int64_t first, std::int64_t last, bool dateTime) {
    return dateTime ? table::detail::isoDate(first) + "T00:00:00Z.." + table::detail::isoDate(last) + "T23:59:59Z"
                    : table::detail::isoDate(first) + ".." + table::detail::isoDate(last);
}

/// @brief Whether a ticked range covers a bucket.
/// @param ranges The ticked ranges' operands.
/// @param bucket The bucket's operand.
/// @return `{covered, own}`: whether a range covers it, and whether one of those is the bucket's own.
[[nodiscard]] inline std::pair<bool, bool> coverOf(std::span<std::string const> ranges, std::string const& bucket) {
    auto const range = rangeOf(bucket);
    bool covered = false;
    bool own = false;
    for (std::string const& ticked : ranges) {
        if (ticked == bucket) {
            covered = true;
            own = true;
            continue;
        }
        if (auto const held = rangeOf(ticked); held && range && held->first <= range->first && range->second <= held->second) {
            covered = true;
        }
    }
    return {covered, own};
}

/// @brief Whether a text column's condition, or a number's or a date's, takes an operand.
/// @param op The condition.
/// @return `false` for `is empty` and `is not empty`.
[[nodiscard]] inline bool takesOperand(ConditionOp op) {
    return op != ConditionOp::IsEmpty && op != ConditionOp::IsNotEmpty;
}

/// @brief The condition an entry is, with its operands, when it is one the menu offers.
/// @param entry The entry.
/// @return The draft; nothing for an entry the menu cannot show.
[[nodiscard]] inline std::optional<ConditionDraft> draftOf(table::FilterEntry const& entry) {
    if (::morph::ui::detail::operatorCount(entry) != 1) {
        return std::nullopt;
    }
    ConditionDraft draft;
    auto const take = [&](ConditionOp op, std::optional<std::string> const& operand) {
        if (operand) {
            draft.op = op;
            draft.operand = *operand;
        }
    };
    take(ConditionOp::Equals, entry.eq);
    take(ConditionOp::NotEquals, entry.ne);
    take(ConditionOp::Contains, entry.contains);
    take(ConditionOp::BeginsWith, entry.startsWith);
    take(ConditionOp::EndsWith, entry.endsWith);
    take(ConditionOp::Less, entry.lt);
    take(ConditionOp::LessOrEqual, entry.le);
    take(ConditionOp::Greater, entry.gt);
    take(ConditionOp::GreaterOrEqual, entry.ge);
    if (entry.between) {
        draft.op = ConditionOp::Between;
        draft.operand = (*entry.between)[0];
        draft.second = (*entry.between)[1];
    }
    if (entry.isEmpty == true) {
        draft.op = ConditionOp::IsEmpty;
    }
    if (entry.notEmpty == true) {
        draft.op = ConditionOp::IsNotEmpty;
    }
    return draft.op ? std::optional{draft} : std::nullopt;
}

}  // namespace detail::filtermenu

/// @brief Merges the ticked buckets of a date tree into the ranges a filter sends.
///
/// The ranges are sorted by their first end; one inside another adds nothing, and two that touch (the next begins
/// the day after the previous ends, or the second after on a date-time column) or overlap become one.
/// @param ticked   The ticked buckets' operands, `a..b`, in any order.
/// @param dateTime Whether the column holds date-times: its ends are `YYYY-MM-DDT00:00:00Z` and `…T23:59:59Z`.
/// @return The ranges, ascending, as `a..b` operands; an operand that does not read is left out.
[[nodiscard]] inline std::vector<std::string> mergeDateRanges(std::span<std::string const> ticked, bool dateTime) {
    std::vector<std::pair<std::int64_t, std::int64_t>> ranges;
    for (std::string const& operand : ticked) {
        if (auto const range = detail::filtermenu::rangeOf(operand)) {
            ranges.push_back(*range);
        }
    }
    std::ranges::sort(ranges);
    std::vector<std::pair<std::int64_t, std::int64_t>> merged;
    for (auto const& range : ranges) {
        if (!merged.empty() && range.first <= merged.back().second + 1) {
            merged.back().second = std::max(merged.back().second, range.second);
        } else {
            merged.push_back(range);
        }
    }
    std::vector<std::string> out;
    out.reserve(merged.size());
    for (auto const& [first, last] : merged) {
        out.push_back(detail::filtermenu::rangeText(first, last, dateTime));
    }
    return out;
}

/// @brief A values list's ticks as a stored values filter makes them.
/// @param filter The column's filter, of the values part (`classifyFilter`).
/// @return The ticks: an include side ticks its members and nothing else, an exclude side unticks its members.
[[nodiscard]] inline ValueTicks ticksOfFilter(table::ColumnFilter const& filter) {
    bool const include = !filter.include.empty();
    auto const& side = include ? filter.include : filter.exclude;
    ValueTicks ticks{.ticked = !include, .ops = {}, .blanks = !include};
    for (table::FilterEntry const& entry : side) {
        if (entry.oneOf) {
            for (std::string const& member : *entry.oneOf) {
                ticks.ops.push_back(TickOp{.search = {}, .operand = member, .ticked = include});
            }
        }
        if (entry.isEmpty == true) {
            ticks.blanks = include;
        }
    }
    return ticks;
}

/// @brief The conditions a stored filter's entries are, as the menu shows them.
/// @param entries The include entries.
/// @return Up to two drafts, the rest of the array empty; nothing when an entry is one the menu cannot show or
///         there are more than two.
[[nodiscard]] inline std::optional<std::array<ConditionDraft, 2>> draftsOf(std::span<table::FilterEntry const> entries) {
    if (entries.size() > 2) {
        return std::nullopt;
    }
    std::array<ConditionDraft, 2> out;
    for (std::size_t at = 0; at < entries.size(); ++at) {
        auto draft = detail::filtermenu::draftOf(entries[at]);
        if (!draft) {
            return std::nullopt;
        }
        out.at(at) = std::move(*draft);
    }
    return out;
}

/// @brief A group's match line as a filter's quantifier.
/// @param match   The match line's choice; `Together` is not a quantifier and reads as `AnyColumn`.
/// @param n       N, for `AtLeast` and `Exactly`.
/// @return The quantifier.
[[nodiscard]] inline table::ColumnCount columnCountOf(GroupMatch match, std::int64_t n) {
    switch (match) {
        case GroupMatch::EveryColumn:
            return {.kind = table::ColumnCount::Kind::All, .n = 1};
        case GroupMatch::AtLeast:
            return {.kind = table::ColumnCount::Kind::AtLeast, .n = n};
        case GroupMatch::Exactly:
            return {.kind = table::ColumnCount::Kind::Exactly, .n = n};
        case GroupMatch::AnyColumn:
        case GroupMatch::Together:
            break;
    }
    return {.kind = table::ColumnCount::Kind::Any, .n = 1};
}

/// @brief A match line's text.
/// @param match The choice.
/// @return `in any column`, `in every column`, `in at least N columns`, `in exactly N columns`, or
///         `all of them, each in a column`.
[[nodiscard]] inline std::string_view matchLabel(GroupMatch match) {
    switch (match) {
        case GroupMatch::AnyColumn:
            return "in any column";
        case GroupMatch::EveryColumn:
            return "in every column";
        case GroupMatch::AtLeast:
            return "in at least N columns";
        case GroupMatch::Exactly:
            return "in exactly N columns";
        case GroupMatch::Together:
            return "all of them, each in a column";
    }
    return {};
}

/// @brief One line of a values list as the view draws it.
struct FilterListRow {
    /// @brief What the line is.
    enum class Kind : std::uint8_t {
        Heading,  ///< A `Measure` column's dimension, before its first value.
        Value,    ///< A value or a date bucket, with its box.
    };
    /// @brief What the line is.
    Kind kind = Kind::Value;
    /// @brief The line's identity among the list's lines.
    std::string key;
    /// @brief A value's index in the list, the `<i>` of its test ids; the dimension of a heading.
    std::string id;
    /// @brief A value's box caption, `{text} ({count})`; a heading's dimension.
    std::string label;
    /// @brief Whether a value's box is ticked.
    bool checked = false;
    /// @brief Whether a value's box can be toggled: a bucket a ticked bucket above covers cannot.
    bool enabled = true;
    /// @brief Whether a date bucket opens to its months or days.
    bool opens = false;
    /// @brief The value's index in `FilterMenuState::list`.
    std::size_t index = 0;
    /// @brief The value's depth in a date tree.
    std::size_t depth = 0;

    /// @brief Memberwise equality.
    /// @return Whether every member matches.
    bool operator==(FilterListRow const&) const = default;
};

/// @brief What a filter menu reaches of its table.
struct FilterMenuHost {
    /// @brief The rows the values are asked of.
    std::function<TableRows*()> rows;
    /// @brief The filters the table holds now.
    std::function<table::FilterSpec()> filters;
    /// @brief Applies the user's filters: the rows', the held ones and `onFiltersChange`. Returns whether they were
    ///        applied; a refusal is the host's to show.
    std::function<bool(table::FilterSpec)> apply;
    /// @brief A gesture on the table: clears its status line's message.
    std::function<void()> touch;
    /// @brief The columns a values list reads while the menu is open; empty when it closes.
    std::function<void(std::vector<std::string> const&)> listing;
    /// @brief Waits out the debounce; null applies each edit at once.
    reactive::Scheduler* scheduler = nullptr;
    /// @brief How long a search and a condition wait after the last edit.
    std::chrono::milliseconds debounce{250};
    /// @brief The session's formatting: the values' texts, and the locale operands are typed in.
    FormatContext format;
    /// @brief The bound on one `oneOf` entry (`QueryLimits::maxSetValues`).
    std::size_t maxSetValues = 10'000;
    /// @brief The bound on one column's entries (`QueryLimits::maxEntries`).
    std::size_t maxEntries = 32;
    /// @brief The most values one request asks for while Apply names values (`QueryLimits::maxLimit`).
    std::int64_t maxLimit = 1000;
    /// @brief The values one page of the list shows (`DistinctRequest::limit`).
    std::int64_t pageSize = 200;
};

/// @brief A table's filter menu: its state while open, the requests it makes and the filters it applies.
///
/// Neither copied nor moved: its replies and timers hold it.
class FilterMenu {
public:
    /// @brief A closed menu.
    /// @param runtime The runtime its state belongs to.
    explicit FilterMenu(reactive::Runtime& runtime) : _state{runtime, FilterMenuState{}} {}

    FilterMenu(FilterMenu const&) = delete;
    FilterMenu& operator=(FilterMenu const&) = delete;
    FilterMenu(FilterMenu&&) = delete;
    FilterMenu& operator=(FilterMenu&&) = delete;
    /// @brief Stops its timers; replies still in flight are dropped.
    ~FilterMenu() = default;

    /// @brief Gives the menu what it reaches of its table: the lowering's, set again by each mount.
    /// @param host The host.
    void setHost(FilterMenuHost host) { _host = std::move(host); }

    /// @brief The menu's state. Tracked.
    /// @return It.
    [[nodiscard]] FilterMenuState const& state() const { return _state.get(); }

    /// @brief Opens the menu on a column or a group, showing the part its stored filter is, and asks for the first
    ///        page of its values.
    /// @param target The column or group.
    void open(FilterTarget target) {
        _searchTimer.cancel();
        _sendTimer.cancel();
        ++_serial;
        FilterMenuState next;
        next.open = true;
        next.opening = _state.peek().opening + 1;
        next.target = std::move(target);
        FilterTarget const& on = next.target;
        bool const isGroup = on.column.empty();
        table::ColumnKind const kind = on.shown.info.kind;
        table::FilterSpec const filters = _host.filters ? _host.filters() : table::FilterSpec{};
        if (isGroup) {
            next.ticks.ticked = false;
            if (auto const at = groupFilterIndex(filters, on)) {
                readGroup(next, filters.groups.at(*at));
            }
        } else if (auto const held = filters.columns.find(on.column); held != filters.columns.end()) {
            if (classifyFilter(held->second, kind) == FilterPart::Values && valuesListed(next)) {
                if (dates(next)) {
                    for (table::FilterEntry const& entry : held->second.include) {
                        if (entry.between) {
                            next.dates.push_back((*entry.between)[0] + ".." + (*entry.between)[1]);
                        }
                        if (entry.isEmpty == true) {
                            next.ticks.blanks = true;
                        }
                    }
                } else {
                    next.ticks = ticksOfFilter(held->second);
                }
            } else if (auto drafts = draftsOf(held->second.include); drafts && held->second.exclude.empty()) {
                next.conditions = std::move(*drafts);
                next.combine = held->second.combine;
                next.matchCase = std::ranges::any_of(held->second.include, [](table::FilterEntry const& entry) {
                    return entry.caseSensitive == true;
                });
            }
        }
        // A date tree is include-only: no bucket starts ticked, and neither does "(Blanks)" unless the stored
        // filter ticks it.
        if (dates(next)) {
            next.ticks.ticked = false;
            next.ticks.blanks = next.ticks.blanks.value_or(false);
        }
        for (ConditionDraft& draft : next.conditions) {
            if (draft.unit.empty() && !on.unitChoices.empty()) {
                draft.unit = on.unitChoices.front();
            }
        }
        _state.set(std::move(next));
        if (_host.listing) {
            _host.listing(columnsOf(_state.peek().target));
        }
        if (valuesListed(_state.peek())) {
            load(false);
        }
    }

    /// @brief Closes the menu; a pending condition is dropped with it.
    void close() {
        if (!_state.peek().open) {
            return;
        }
        _searchTimer.cancel();
        _sendTimer.cancel();
        ++_serial;
        _state.mutate([](FilterMenuState& state) {
            state.open = false;
            state.list.clear();
        });
        if (_host.listing) {
            _host.listing({});
        }
    }

    /// @brief The search typed: the list asks again, `debounce` after the last keystroke.
    /// @param text The search.
    void search(std::string text) {
        touch();
        _state.mutate([&](FilterMenuState& state) { state.search = std::move(text); });
        _searchTimer.cancel();
        later(_searchTimer, [this] { load(false); });
    }

    /// @brief Ticks or unticks a value, or a date bucket.
    /// @param index   The value's index in the list.
    /// @param checked Its new state.
    void tick(std::size_t index, bool checked) {
        touch();
        _sendTimer.cancel();
        _state.mutate([&](FilterMenuState& state) {
            if (index >= state.list.size()) {
                return;
            }
            clearConditions(state);
            FilterListEntry const& entry = state.list[index];
            if (dates(state)) {
                std::erase(state.dates, entry.value.operand);
                if (!checked) {
                    // A range that holds the bucket and more, as Apply merged it, keeps the rest: its days before
                    // and after the bucket.
                    auto const bucket = detail::filtermenu::rangeOf(entry.value.operand);
                    bool const dateTime = state.target.shown.info.kind == table::ColumnKind::DateTime;
                    std::vector<std::string> kept;
                    for (std::string const& held : state.dates) {
                        auto const range = detail::filtermenu::rangeOf(held);
                        if (!bucket || !range || range->first > bucket->first || bucket->second > range->second) {
                            kept.push_back(held);
                            continue;
                        }
                        if (range->first < bucket->first) {
                            kept.push_back(detail::filtermenu::rangeText(range->first, bucket->first - 1, dateTime));
                        }
                        if (bucket->second < range->second) {
                            kept.push_back(detail::filtermenu::rangeText(bucket->second + 1, range->second, dateTime));
                        }
                    }
                    state.dates = std::move(kept);
                }
                if (checked) {
                    // A ticked bucket covers those under it: their own ticks go.
                    auto const range = detail::filtermenu::rangeOf(entry.value.operand);
                    std::erase_if(state.dates, [&](std::string const& held) {
                        auto const inner = detail::filtermenu::rangeOf(held);
                        return range && inner && range->first <= inner->first && inner->second <= range->second;
                    });
                    state.dates.push_back(entry.value.operand);
                }
                return;
            }
            state.ticks.ops.push_back(TickOp{.search = {}, .operand = entry.value.operand, .ticked = checked});
            // "all of them, each in a column" is offered while 2 or more values are ticked: below that the line
            // falls back to its default.
            if (state.match == GroupMatch::Together &&
                std::ranges::count_if(state.list, [&](FilterListEntry const& listed) {
                    return tickedNow(state, listed.value);
                }) < 2) {
                state.match = GroupMatch::AnyColumn;
            }
        });
    }

    /// @brief Ticks or unticks "(Select all)": every value the search shows, every page of them.
    /// @param checked Its new state.
    void tickAll(bool checked) {
        touch();
        _sendTimer.cancel();
        _state.mutate([&](FilterMenuState& state) {
            clearConditions(state);
            state.ticks.ops.push_back(TickOp{.search = state.search, .operand = std::nullopt, .ticked = checked});
            if (state.search.empty() && state.ticks.blanks) {
                state.ticks.blanks = checked;
            }
        });
    }

    /// @brief Ticks or unticks "(Blanks)".
    /// @param checked Its new state.
    void tickBlanks(bool checked) {
        touch();
        _sendTimer.cancel();
        _state.mutate([&](FilterMenuState& state) {
            clearConditions(state);
            state.ticks.blanks = checked;
        });
    }

    /// @brief Asks for the next page of values.
    void more() {
        touch();
        load(true);
    }

    /// @brief Opens or closes a date bucket: an opened year lists its months, an opened month its days.
    /// @param index The bucket's index in the list.
    void openBucket(std::size_t index) {
        touch();
        FilterMenuState const& now = _state.peek();
        if (index >= now.list.size()) {
            return;
        }
        FilterListEntry const entry = now.list[index];
        if (entry.bucket != table::DateBucket::Year && entry.bucket != table::DateBucket::Month) {
            return;
        }
        if (entry.opened) {
            _state.mutate([&](FilterMenuState& state) {
                state.list[index].opened = false;
                std::size_t end = index + 1;
                while (end < state.list.size() && state.list[end].depth > entry.depth) {
                    ++end;
                }
                state.list.erase(state.list.begin() + static_cast<std::ptrdiff_t>(index) + 1,
                                 state.list.begin() + static_cast<std::ptrdiff_t>(end));
            });
            return;
        }
        table::DateBucket const bucket =
            entry.bucket == table::DateBucket::Year ? table::DateBucket::Month : table::DateBucket::Day;
        table::DistinctRequest request = requestOf(now);
        request.bucket = bucket;
        request.search = entry.value.text + "-";
        request.offset = 0;
        std::uint64_t const serial = _serial;
        std::string const operand = entry.value.operand;
        ask(request, [this, serial, operand, bucket, depth = entry.depth + 1](
                         std::expected<table::DistinctValues, table::TableError> const& reply) {
            if (serial != _serial || !reply) {
                return;
            }
            _state.mutate([&](FilterMenuState& state) {
                auto const at = std::ranges::find_if(state.list, [&](FilterListEntry const& held) {
                    return held.value.operand == operand && held.depth + 1 == depth;
                });
                if (at == state.list.end() || at->opened) {
                    return;
                }
                at->opened = true;
                std::vector<FilterListEntry> children;
                for (table::DistinctValue const& value : reply->values) {
                    children.push_back(FilterListEntry{.value = value, .bucket = bucket, .depth = depth, .opened = false});
                }
                state.list.insert(at + 1, children.begin(), children.end());
            });
        });
    }

    /// @brief Chooses a condition: conditions apply `debounce` after the last edit.
    /// @param which The condition, 0 or 1.
    /// @param op    The condition chosen.
    void setOp(std::size_t which, ConditionOp op) {
        editCondition([&](FilterMenuState& state) { state.conditions.at(which).op = op; });
    }

    /// @brief Types a condition's operand.
    /// @param which The condition, 0 or 1.
    /// @param text  The operand.
    void setOperand(std::size_t which, std::string text) {
        editCondition([&](FilterMenuState& state) { state.conditions.at(which).operand = std::move(text); });
    }

    /// @brief Types `between`'s upper end.
    /// @param which The condition, 0 or 1.
    /// @param text  The upper end.
    void setSecond(std::size_t which, std::string text) {
        editCondition([&](FilterMenuState& state) { state.conditions.at(which).second = std::move(text); });
    }

    /// @brief Chooses a `Measure` condition's unit.
    /// @param which The condition, 0 or 1.
    /// @param unit  The unit's id.
    void setUnit(std::size_t which, std::string unit) {
        editCondition([&](FilterMenuState& state) { state.conditions.at(which).unit = std::move(unit); });
    }

    /// @brief Sets "match case".
    /// @param on Whether text conditions match case.
    void setMatchCase(bool on) {
        editCondition([&](FilterMenuState& state) { state.matchCase = on; });
    }

    /// @brief Sets how the two conditions join.
    /// @param combine `All` for "and", `Any` for "or".
    void setCombine(table::Combine combine) {
        editCondition([&](FilterMenuState& state) { state.combine = combine; });
    }

    /// @brief Chooses a group's match line; a group's conditions apply it as they apply an edit.
    /// @param match The choice.
    void setMatch(GroupMatch match) {
        touch();
        _state.mutate([&](FilterMenuState& state) { state.match = match; });
        if (hasConditions(_state.peek())) {
            scheduleSend();
        }
    }

    /// @brief Types the N of a group's match line.
    /// @param text N, as typed.
    void setCount(std::string text) {
        touch();
        _state.mutate([&](FilterMenuState& state) { state.count = std::move(text); });
        if (hasConditions(_state.peek())) {
            scheduleSend();
        }
    }

    /// @brief Applies the menu: a condition waiting for its debounce goes at once; otherwise the ticks are resolved,
    ///        every page of the values they reach asked for, and the side `planValueFilter` chooses is set. The menu
    ///        closes once the filter is applied, and says in its note why when nothing is.
    void apply() {
        touch();
        FilterMenuState const& now = _state.peek();
        if (hasConditions(now)) {
            _sendTimer.cancel();
            if (sendConditions()) {
                close();
            }
            return;
        }
        if (now.target.column.empty() && now.target.formed) {
            return;
        }
        if (dates(now)) {
            applyDates();
        } else if (now.target.column.empty()) {
            applyGroupValues();
        } else {
            applyValues();
        }
    }

    /// @brief "Clear filter": removes the column's or the group's filter, and the menu's ticks and conditions.
    void clearFilter() {
        touch();
        _sendTimer.cancel();
        FilterMenuState const& now = _state.peek();
        table::FilterSpec spec = _host.filters ? _host.filters() : table::FilterSpec{};
        if (now.target.column.empty()) {
            if (auto const at = groupFilterIndex(spec, now.target)) {
                spec.groups.erase(spec.groups.begin() + static_cast<std::ptrdiff_t>(*at));
            }
        } else {
            spec.columns.erase(now.target.column);
        }
        if (_host.apply) {
            static_cast<void>(_host.apply(std::move(spec)));
        }
        _state.mutate([](FilterMenuState& state) {
            clearConditions(state);
            state.ticks = ValueTicks{.ticked = !dates(state) && !state.target.column.empty(), .ops = {},
                                     .blanks = state.blanks > 0 ? std::optional{!dates(state)} : std::nullopt};
            state.dates.clear();
            state.refusal.clear();
        });
    }

    /// @brief The list's lines as the view draws them: a `Measure` column's dimension heading before its first
    ///        value, each value's caption by the column's display and its count, ticked as the ticks say. Tracked.
    /// @return The lines.
    [[nodiscard]] std::vector<FilterListRow> rows() const {
        FilterMenuState const& state = _state.get();
        std::vector<FilterListRow> out;
        if (!state.open) {
            return out;
        }
        std::vector<table::DistinctValue> values;
        values.reserve(state.list.size());
        for (FilterListEntry const& entry : state.list) {
            values.push_back(entry.value);
        }
        std::vector<std::string> const texts =
            distinctTexts(values, state.target.shown, _host.format, state.target.units);
        bool const tree = dates(state);
        std::string dimension;
        for (std::size_t at = 0; at < state.list.size(); ++at) {
            FilterListEntry const& entry = state.list[at];
            if (!entry.value.dimension.empty() && entry.value.dimension != dimension) {
                dimension = entry.value.dimension;
                out.push_back(FilterListRow{.kind = FilterListRow::Kind::Heading,
                                            .key = "heading:" + dimension,
                                            .id = dimension,
                                            .label = dimension});
            }
            FilterListRow row{.kind = FilterListRow::Kind::Value,
                              .key = "value:" + std::to_string(at) + ":" + entry.value.operand,
                              .id = std::to_string(at),
                              .label = texts.at(at) + " (" +
                                       groupedCount(entry.value.count, _host.format.numeric.groupSeparator) + ")",
                              .checked = false,
                              .enabled = true,
                              .opens = tree && entry.bucket != table::DateBucket::Day,
                              .index = at,
                              .depth = entry.depth};
            if (tree) {
                // A bucket under a ticked bucket of the tree is ticked with it and cannot be toggled alone; one a
                // range covers that no listed bucket is (a stored or merged range) is ticked and can be unticked.
                auto const [covered, own] = detail::filtermenu::coverOf(state.dates, entry.value.operand);
                row.checked = covered;
                row.enabled = !covered || own || !underTicked(state, at);
            } else {
                row.checked = tickedNow(state, entry.value);
            }
            out.push_back(std::move(row));
        }
        return out;
    }

    /// @brief Whether "(Select all)" shows ticked: every value listed is. Tracked.
    /// @return It.
    [[nodiscard]] bool allTicked() const {
        FilterMenuState const& state = _state.get();
        return !state.list.empty() && std::ranges::all_of(state.list, [&](FilterListEntry const& entry) {
            return tickedNow(state, entry.value);
        });
    }

    /// @brief Whether Apply can apply something: a condition, or a values list with a box ticked. A date tree's
    ///        Apply with nothing ticked clears its filter, so it is always enabled. Tracked.
    /// @return It.
    [[nodiscard]] bool canApply() const {
        FilterMenuState const& state = _state.get();
        if (hasConditions(state) || dates(state)) {
            return true;
        }
        if (state.target.formed) {
            return false;
        }
        if (state.ticks.blanks == true) {
            return true;
        }
        // Every value's state after the last operation that set them all, then any operation since that ticks.
        auto const last = std::ranges::find_if(state.ticks.ops.rbegin(), state.ticks.ops.rend(), [](TickOp const& op) {
            return !op.operand && op.search.empty();
        });
        bool const base = last == state.ticks.ops.rend() ? state.ticks.ticked : last->ticked;
        return base || std::any_of(state.ticks.ops.rbegin(), last, [](TickOp const& op) { return op.ticked; });
    }

    /// @brief The note under the list: that it may grow, how many cells could not be read, and why the last Apply
    ///        applied nothing, one a line. Tracked.
    /// @return It.
    [[nodiscard]] std::string note() const {
        FilterMenuState const& state = _state.get();
        std::vector<std::string> lines;
        if (state.partial) {
            lines.push_back(tableText::mayGrow());
        }
        if (state.invalid > 0) {
            lines.push_back(
                tableText::unreadable(static_cast<std::size_t>(state.invalid), _host.format.numeric.groupSeparator));
        }
        if (!state.refusal.empty()) {
            lines.push_back(state.refusal);
        }
        std::string out;
        for (std::string const& line : lines) {
            out += (out.empty() ? "" : "\n") + line;
        }
        return out;
    }

    /// @brief Whether the open target shows a values list.
    /// @param state The menu's state.
    /// @return `false` for a group the user formed, a `Custom` column, and rows that list no values.
    [[nodiscard]] static bool valuesListed(FilterMenuState const& state) {
        return !state.target.formed && state.target.listed && state.target.shown.info.kind != table::ColumnKind::Custom;
    }

    /// @brief Whether the open target's list is a date tree.
    /// @param state The menu's state.
    /// @return `true` on a `Date` or `DateTime` column.
    [[nodiscard]] static bool dates(FilterMenuState const& state) {
        table::ColumnKind const kind = state.target.shown.info.kind;
        return !state.target.column.empty() && (kind == table::ColumnKind::Date || kind == table::ColumnKind::DateTime);
    }

private:
    using Reply = std::expected<table::DistinctValues, table::TableError>;

    static void clearConditions(FilterMenuState& state) {
        for (ConditionDraft& draft : state.conditions) {
            std::string const unit = draft.unit;
            draft = ConditionDraft{};
            draft.unit = unit;
        }
        state.matchCase = false;
        state.combine = table::Combine::Any;
    }

    [[nodiscard]] static bool hasConditions(FilterMenuState const& state) {
        return std::ranges::any_of(state.conditions, [](ConditionDraft const& draft) { return draft.op.has_value(); });
    }

    [[nodiscard]] static std::vector<std::string> columnsOf(FilterTarget const& target) {
        return target.column.empty() ? target.columns : std::vector<std::string>{target.column};
    }

    // Whether a date bucket lies under a listed bucket that is ticked itself.
    [[nodiscard]] static bool underTicked(FilterMenuState const& state, std::size_t index) {
        auto const inner = detail::filtermenu::rangeOf(state.list.at(index).value.operand);
        for (std::size_t at = 0; at < state.list.size(); ++at) {
            FilterListEntry const& above = state.list.at(at);
            auto const outer = detail::filtermenu::rangeOf(above.value.operand);
            if (at == index || above.depth >= state.list.at(index).depth || !outer || !inner ||
                outer->first > inner->first || inner->second > outer->second) {
                continue;
            }
            if (std::ranges::contains(state.dates, above.value.operand)) {
                return true;
            }
        }
        return false;
    }

    // Whether a value's box shows ticked: the ticks' first state, then each operation that reaches it in order. A
    // search reaches the values whose text contains it, folded.
    [[nodiscard]] static bool tickedNow(FilterMenuState const& state, table::DistinctValue const& value) {
        bool ticked = state.ticks.ticked;
        for (TickOp const& op : state.ticks.ops) {
            if (op.operand) {
                if (*op.operand == value.operand) {
                    ticked = op.ticked;
                }
            } else if (op.search.empty() ||
                       detail::filtermenu::folded(value.text).contains(detail::filtermenu::folded(op.search))) {
                ticked = op.ticked;
            }
        }
        return ticked;
    }

    void readGroup(FilterMenuState& next, table::GroupFilter const& group) const {
        bool const values = !next.target.formed && group.exclude.empty() &&
                            (group.mode == table::GroupMode::Together
                                 ? std::ranges::all_of(group.include,
                                                       [](table::FilterEntry const& entry) {
                                                           return entry.eq.has_value() &&
                                                                  ::morph::ui::detail::operatorCount(entry) == 1;
                                                       })
                                 : ::morph::ui::detail::valuesShape(group.include, next.target.shown.info.kind) &&
                                       std::ranges::any_of(group.include, [](table::FilterEntry const& entry) {
                                           return entry.oneOf.has_value();
                                       }));
        if (values) {
            for (table::FilterEntry const& entry : group.include) {
                if (entry.oneOf) {
                    for (std::string const& member : *entry.oneOf) {
                        next.ticks.ops.push_back(TickOp{.search = {}, .operand = member, .ticked = true});
                    }
                }
                if (entry.eq) {
                    next.ticks.ops.push_back(TickOp{.search = {}, .operand = *entry.eq, .ticked = true});
                }
                if (entry.isEmpty == true) {
                    next.ticks.blanks = true;
                }
            }
        } else if (auto drafts = draftsOf(group.include); drafts && group.exclude.empty()) {
            next.conditions = std::move(*drafts);
            next.combine = group.combine;
        }
        if (group.mode == table::GroupMode::Together) {
            next.match = GroupMatch::Together;
            return;
        }
        switch (group.match.kind) {
            case table::ColumnCount::Kind::Any:
                next.match = GroupMatch::AnyColumn;
                break;
            case table::ColumnCount::Kind::All:
                next.match = GroupMatch::EveryColumn;
                break;
            case table::ColumnCount::Kind::AtLeast:
                next.match = GroupMatch::AtLeast;
                next.count = std::to_string(group.match.n);
                break;
            case table::ColumnCount::Kind::Exactly:
                next.match = GroupMatch::Exactly;
                next.count = std::to_string(group.match.n);
                break;
        }
    }

    void touch() const {
        if (_host.touch) {
            _host.touch();
        }
    }

    // Runs @p fn after the debounce, replacing what @p timer held; at once without a scheduler.
    void later(reactive::TimerHandle& timer, std::function<void()> fn) {
        if (_host.scheduler == nullptr || _host.debounce.count() <= 0) {
            fn();
            return;
        }
        timer = _host.scheduler->after(_host.debounce, _scope.token().guard(std::move(fn)));
    }

    template <typename F>
    void editCondition(F&& edit) {
        touch();
        _state.mutate([&](FilterMenuState& state) {
            std::forward<F>(edit)(state);
            // Editing a condition leaves the values part: its ticks start again.
            state.ticks = ValueTicks{.ticked = !dates(state) && !state.target.column.empty(), .ops = {},
                                     .blanks = state.blanks > 0 ? std::optional{!dates(state)} : std::nullopt};
            state.dates.clear();
            state.refusal.clear();
        });
        scheduleSend();
    }

    void scheduleSend() {
        _sendTimer.cancel();
        later(_sendTimer, [this] { static_cast<void>(sendConditions()); });
    }

    [[nodiscard]] table::DistinctRequest requestOf(FilterMenuState const& state) const {
        table::DistinctRequest request;
        if (state.target.column.empty()) {
            request.group = state.target.group;
        } else {
            request.column = state.target.column;
        }
        request.search = state.search;
        request.bucket = dates(state) ? table::DateBucket::Year : table::DateBucket::Value;
        request.limit = _host.pageSize;
        return request;
    }

    void ask(table::DistinctRequest request, std::function<void(Reply const&)> done) {
        TableRows* const rows = _host.rows ? _host.rows() : nullptr;
        if (rows == nullptr) {
            done(Reply{std::unexpected(table::TableError{
                .code = table::TableErrorCode::Unavailable, .column = {}, .message = "the table has no rows"})});
            return;
        }
        rows->distinctValues(std::move(request)).then(_scope, std::move(done));
    }

    // Asks for the list's first page under the search, or with @p next the page after those listed.
    void load(bool next) {
        FilterMenuState const& now = _state.peek();
        if (!now.open || !valuesListed(now)) {
            return;
        }
        table::DistinctRequest request = requestOf(now);
        request.offset = next ? static_cast<std::int64_t>(std::ranges::count_if(
                                    now.list, [](FilterListEntry const& entry) { return entry.depth == 0; }))
                              : 0;
        std::uint64_t const serial = ++_serial;
        bool const searched = !request.search.empty();
        ask(request, [this, serial, next, searched](Reply const& reply) {
            if (serial != _serial) {
                return;
            }
            _state.mutate([&](FilterMenuState& state) {
                state.listedOnce = true;
                if (!reply) {
                    // Rows that list no values: the conditions alone show.
                    if (reply.error().code == table::TableErrorCode::Unavailable) {
                        state.target.listed = false;
                    }
                    state.list.clear();
                    return;
                }
                if (!next) {
                    state.list.clear();
                }
                for (table::DistinctValue const& value : reply->values) {
                    state.list.push_back(FilterListEntry{
                        .value = value,
                        .bucket = dates(state) ? table::DateBucket::Year : table::DateBucket::Value,
                        .depth = 0,
                        .opened = false});
                }
                state.distinct = reply->distinct;
                if (!searched) {
                    state.total = reply->distinct;
                }
                state.blanks = reply->blanks;
                state.invalid = reply->invalid;
                state.partial = reply->partial;
                // "(Blanks)" is a box only while the column has empty cells; with none it is on no side.
                if (reply->blanks == 0 && !searched && !dates(state)) {
                    state.ticks.blanks.reset();
                } else if (reply->blanks > 0 && !state.ticks.blanks) {
                    state.ticks.blanks = !dates(state) && state.ticks.ticked;
                }
            });
        });
    }

    // Every value a search shows, every page of them, asked at `maxLimit` a request.
    void pageAll(std::string search, std::function<void(std::expected<std::vector<table::DistinctValue>, std::string>,
                                                        std::int64_t distinct)>
                                         done) {
        struct Paging {
            table::DistinctRequest request;
            std::vector<table::DistinctValue> values;
            std::function<void(std::expected<std::vector<table::DistinctValue>, std::string>, std::int64_t)> done;
        };
        auto paging = std::make_shared<Paging>();
        paging->request = requestOf(_state.peek());
        paging->request.search = std::move(search);
        paging->request.limit = _host.maxLimit;
        paging->request.offset = 0;
        paging->done = std::move(done);
        auto step = std::make_shared<std::function<void()>>();
        std::weak_ptr<std::function<void()>> const weakStep = step;
        *step = [this, paging, weakStep] {
            ask(paging->request, [paging, keep = weakStep.lock()](Reply const& reply) {
                if (!reply) {
                    paging->done(std::unexpected(reply.error().message), 0);
                    return;
                }
                paging->values.insert(paging->values.end(), reply->values.begin(), reply->values.end());
                paging->request.offset += static_cast<std::int64_t>(reply->values.size());
                if (reply->values.empty() || paging->request.offset >= reply->distinct) {
                    paging->done(std::move(paging->values), reply->distinct);
                    return;
                }
                if (keep) {
                    (*keep)();
                }
            });
        };
        (*step)();
    }

    // Resolves the ticks: the values each search's "(Select all)" reached, one search after another, and the count
    // of every value.
    void resolve(std::function<void(std::expected<std::pair<ResolvedTicks, std::int64_t>, std::string>)> done) {
        FilterMenuState const& now = _state.peek();
        auto searches = std::make_shared<std::vector<std::string>>();
        for (TickOp const& op : now.ticks.ops) {
            if (!op.operand && !op.search.empty() && !std::ranges::contains(*searches, op.search)) {
                searches->push_back(op.search);
            }
        }
        auto reached = std::make_shared<std::map<std::string, std::vector<std::string>, std::less<>>>();
        auto finish = [reached, ticks = now.ticks, done](std::int64_t distinct) {
            ResolvedTicks resolved = resolveTicks(ticks, [&](std::string const& search) { return reached->at(search); });
            done(std::pair{std::move(resolved), distinct});
        };
        auto counted = [this, total = now.total, finish, done] {
            if (total >= 0) {
                finish(total);
                return;
            }
            table::DistinctRequest request = requestOf(_state.peek());
            request.search.clear();
            request.offset = 0;
            request.limit = 1;
            ask(request, [finish, done](Reply const& reply) {
                if (!reply) {
                    done(std::unexpected(reply.error().message));
                    return;
                }
                finish(reply->distinct);
            });
        };
        auto step = std::make_shared<std::function<void(std::size_t)>>();
        std::weak_ptr<std::function<void(std::size_t)>> const weakStep = step;
        *step = [this, searches, reached, counted, done, weakStep](std::size_t at) {
            if (at >= searches->size()) {
                counted();
                return;
            }
            pageAll(searches->at(at), [reached, searches, at, done, self = weakStep.lock()](
                                          std::expected<std::vector<table::DistinctValue>, std::string> values,
                                          std::int64_t) {
                if (!values) {
                    done(std::unexpected(values.error()));
                    return;
                }
                std::vector<std::string>& operands = (*reached)[searches->at(at)];
                for (table::DistinctValue const& value : *values) {
                    operands.push_back(value.operand);
                }
                if (self) {
                    (*self)(at + 1);
                }
            });
        };
        (*step)(0);
    }

    void refuse(std::string why) {
        _state.mutate([&](FilterMenuState& state) { state.refusal = std::move(why); });
    }

    // Sets @p spec and closes the menu once it is applied.
    void commit(table::FilterSpec spec) {
        bool const applied = _host.apply ? _host.apply(std::move(spec)) : false;
        if (applied) {
            refuse({});
            close();
        }
    }

    void applyValues() {
        std::uint64_t const serial = _serial;
        resolve([this, serial](std::expected<std::pair<ResolvedTicks, std::int64_t>, std::string> resolved) {
            if (serial != _serial) {
                return;
            }
            if (!resolved) {
                refuse(std::move(resolved.error()));
                return;
            }
            auto const& [ticks, distinct] = *resolved;
            ValueFilterPlan const plan = planValueFilter(distinct, ticks, _host.maxSetValues);
            std::string const column = _state.peek().target.column;
            using Side = ValueFilterPlan::Side;
            if (plan.side == Side::Disabled) {
                refuse(tableText::tooManyValues());
                return;
            }
            if (plan.side == Side::Nothing) {
                return;
            }
            if (plan.side == Side::All) {
                table::FilterSpec spec = _host.filters ? _host.filters() : table::FilterSpec{};
                spec.columns.erase(column);
                commit(std::move(spec));
                return;
            }
            bool const include = plan.side == Side::Include;
            auto write = [this, column, include, side = plan.side, blanks = ticks.blanks](std::vector<std::string> members) {
                table::FilterSpec spec = _host.filters ? _host.filters() : table::FilterSpec{};
                if (auto filter = valueFilter(side, std::move(members), blanks)) {
                    spec.columns.insert_or_assign(column, std::move(*filter));
                } else {
                    spec.columns.erase(column);
                }
                commit(std::move(spec));
            };
            if (!plan.needsComplement) {
                std::vector<std::string> members;
                for (auto const& [operand, ticked] : ticks.covered) {
                    if (ticked == include) {
                        members.push_back(operand);
                    }
                }
                write(std::move(members));
                return;
            }
            // The side names values no operation reached: every value is paged to name them.
            pageAll({}, [this, serial, ticks, include, write](
                            std::expected<std::vector<table::DistinctValue>, std::string> values, std::int64_t) {
                if (serial != _serial) {
                    return;
                }
                if (!values) {
                    refuse(std::move(values.error()));
                    return;
                }
                std::vector<std::string> members;
                for (table::DistinctValue const& value : *values) {
                    auto const held = ticks.covered.find(value.operand);
                    bool const ticked = held != ticks.covered.end() ? held->second : ticks.ticked;
                    if (ticked == include) {
                        members.push_back(value.operand);
                    }
                }
                write(std::move(members));
            });
        });
    }

    void applyGroupValues() {
        std::uint64_t const serial = _serial;
        resolve([this, serial](std::expected<std::pair<ResolvedTicks, std::int64_t>, std::string> resolved) {
            if (serial != _serial) {
                return;
            }
            if (!resolved) {
                refuse(std::move(resolved.error()));
                return;
            }
            ResolvedTicks const ticks = resolved->first;
            auto write = [this, ticks](std::vector<std::string> ticked) {
                FilterMenuState const& now = _state.peek();
                std::int64_t n = 0;
                static_cast<void>(std::from_chars(now.count.data(), now.count.data() + now.count.size(), n));
                auto filter = groupValueFilter(now.target.group, now.target.columns.size(), std::move(ticked),
                                               ticks.blanks, now.match, n);
                if (!filter) {
                    refuse(std::move(filter.error()));
                    return;
                }
                table::FilterSpec spec = _host.filters ? _host.filters() : table::FilterSpec{};
                if (auto const at = groupFilterIndex(spec, now.target)) {
                    spec.groups.at(*at) = std::move(*filter);
                } else {
                    spec.groups.push_back(std::move(*filter));
                }
                commit(std::move(spec));
            };
            if (!ticks.ticked) {
                std::vector<std::string> ticked;
                for (auto const& [operand, on] : ticks.covered) {
                    if (on) {
                        ticked.push_back(operand);
                    }
                }
                // In the order the list shows them.
                std::vector<std::string> ordered;
                for (FilterListEntry const& entry : _state.peek().list) {
                    if (std::ranges::contains(ticked, entry.value.operand)) {
                        ordered.push_back(entry.value.operand);
                    }
                }
                for (std::string const& operand : ticked) {
                    if (!std::ranges::contains(ordered, operand)) {
                        ordered.push_back(operand);
                    }
                }
                write(std::move(ordered));
                return;
            }
            pageAll({}, [this, serial, ticks, write](std::expected<std::vector<table::DistinctValue>, std::string> values,
                                                     std::int64_t) {
                if (serial != _serial) {
                    return;
                }
                if (!values) {
                    refuse(std::move(values.error()));
                    return;
                }
                std::vector<std::string> ticked;
                for (table::DistinctValue const& value : *values) {
                    auto const held = ticks.covered.find(value.operand);
                    if (held != ticks.covered.end() ? held->second : ticks.ticked) {
                        ticked.push_back(value.operand);
                    }
                }
                if (ticked.size() > _host.maxSetValues) {
                    refuse(tableText::tooManyValues());
                    return;
                }
                write(std::move(ticked));
            });
        });
    }

    void applyDates() {
        FilterMenuState const& now = _state.peek();
        bool const dateTime = now.target.shown.info.kind == table::ColumnKind::DateTime;
        std::vector<std::string> const ranges = mergeDateRanges(now.dates, dateTime);
        bool const blanks = now.ticks.blanks == true;
        table::FilterSpec spec = _host.filters ? _host.filters() : table::FilterSpec{};
        if (ranges.empty() && !blanks) {
            spec.columns.erase(now.target.column);
            commit(std::move(spec));
            return;
        }
        if (ranges.size() + (blanks ? 1U : 0U) > _host.maxEntries) {
            refuse(tableText::tooManyDates());
            return;
        }
        if (auto filter = valueFilter(ValueFilterPlan::Side::Include, ranges, now.ticks.blanks, ValueForm::Ranges)) {
            spec.columns.insert_or_assign(now.target.column, std::move(*filter));
        }
        commit(std::move(spec));
    }

    // Sends the conditions when every operand reads and every `between` has both ends; marks those that do not
    // read. Returns whether a filter was sent.
    bool sendConditions() {
        FilterMenuState const& now = _state.peek();
        if (!now.open || !hasConditions(now)) {
            return false;
        }
        FilterTarget const& target = now.target;
        table::ColumnKind const kind = target.shown.info.kind;
        LocaleFacts const locale{.tag = _host.format.locale,
                                 .numeric = _host.format.numeric,
                                 .currency = _host.format.currency,
                                 .datePattern = _host.format.datePattern,
                                 .dateTimePattern = _host.format.dateTimePattern};
        std::vector<table::FilterEntry> entries;
        std::array<bool, 2> invalid{false, false};
        bool incomplete = false;
        for (std::size_t at = 0; at < now.conditions.size(); ++at) {
            ConditionDraft const& draft = now.conditions.at(at);
            if (!draft.op) {
                continue;
            }
            std::string operand;
            std::string second;
            if (detail::filtermenu::takesOperand(*draft.op)) {
                if (draft.operand.empty() || (*draft.op == ConditionOp::Between && draft.second.empty())) {
                    incomplete = true;
                    continue;
                }
                auto const read = canonicalOperand(draft.operand, kind, locale, target.unitList, draft.unit);
                std::optional<std::string> const upper =
                    *draft.op == ConditionOp::Between
                        ? canonicalOperand(draft.second, kind, locale, target.unitList, draft.unit)
                        : std::optional<std::string>{std::string{}};
                if (!read || !upper) {
                    invalid.at(at) = true;
                    continue;
                }
                operand = *read;
                second = *upper;
            }
            entries.push_back(conditionEntry(*draft.op, std::move(operand), std::move(second),
                                             now.matchCase && kind == table::ColumnKind::Text));
        }
        _state.mutate([&](FilterMenuState& state) {
            for (std::size_t at = 0; at < state.conditions.size(); ++at) {
                state.conditions.at(at).invalid = invalid.at(at);
            }
        });
        if (incomplete || invalid[0] || invalid[1]) {
            return false;
        }
        table::FilterSpec spec = _host.filters ? _host.filters() : table::FilterSpec{};
        if (target.column.empty()) {
            std::int64_t n = 1;
            static_cast<void>(std::from_chars(now.count.data(), now.count.data() + now.count.size(), n));
            auto const at = groupFilterIndex(spec, target);
            if (entries.empty()) {
                if (at) {
                    spec.groups.erase(spec.groups.begin() + static_cast<std::ptrdiff_t>(*at));
                }
            } else {
                table::GroupFilter filter{.group = target.formed ? std::string{} : target.group,
                                          .columns = target.formed ? target.columns : std::vector<std::string>{},
                                          .mode = table::GroupMode::Columns,
                                          .match = columnCountOf(now.match, n),
                                          .combine = now.combine,
                                          .include = std::move(entries),
                                          .exclude = {}};
                if (at) {
                    spec.groups.at(*at) = std::move(filter);
                } else {
                    spec.groups.push_back(std::move(filter));
                }
            }
        } else if (entries.empty()) {
            spec.columns.erase(target.column);
        } else {
            spec.columns.insert_or_assign(target.column, table::ColumnFilter{.combine = now.combine,
                                                                              .include = std::move(entries),
                                                                              .exclude = {}});
        }
        if (_host.filters && spec == _host.filters()) {
            return true;
        }
        return _host.apply ? _host.apply(std::move(spec)) : false;
    }

    reactive::Signal<FilterMenuState> _state;
    FilterMenuHost _host;
    reactive::TimerHandle _searchTimer;
    reactive::TimerHandle _sendTimer;
    std::uint64_t _serial = 0;
    /// Last, so it is the first to go: no reply or timer reaches a half-destroyed menu.
    async::CallbackScope _scope;
};

// ---------------------------------------------------------------------------------------------------------------
// The view
// ---------------------------------------------------------------------------------------------------------------

namespace detail::filtermenu {

/// @brief A date as the picker shows it.
/// @param text `YYYY-MM-DD`, or an ISO date-time's date part.
/// @return The instant at its start, UTC; nothing for text that does not read.
[[nodiscard]] inline std::optional<time::Timestamp> instantOf(std::string_view text) {
    auto const day = dayOf(text);
    if (!day) {
        return std::nullopt;
    }
    std::chrono::sys_days const days{std::chrono::days{*day}};
    std::int64_t seconds = 0;
    if (text.size() >= 19 && text[10] == 'T') {
        int hour = 0;
        int minute = 0;
        int second = 0;
        char const* const at = text.data();
        static_cast<void>(std::from_chars(at + 11, at + 13, hour));
        static_cast<void>(std::from_chars(at + 14, at + 16, minute));
        static_cast<void>(std::from_chars(at + 17, at + 19, second));
        seconds = (hour * 3600) + (minute * 60) + second;
    }
    return time::Timestamp{time::DateTime{std::chrono::sys_time<std::chrono::milliseconds>{days} +
                                          std::chrono::seconds{seconds}}};
}

/// @brief The operand a picked instant is.
/// @param instant  The instant, UTC.
/// @param dateTime Whether the column holds date-times.
/// @return `YYYY-MM-DD`, or `YYYY-MM-DDTHH:MM:SSZ`.
[[nodiscard]] inline std::string operandOfInstant(time::DateTime const& instant, bool dateTime) {
    std::int64_t const seconds =
        std::chrono::duration_cast<std::chrono::seconds>(instant.value.time_since_epoch()).count();
    return dateTime ? table::detail::isoDateTime(seconds) : table::detail::isoDate(table::detail::dayOfSeconds(seconds));
}

/// @brief One condition's line: the condition select, the operand (a text input, or the date picker), `between`'s
///        second end and a `Measure` column's unit.
/// @param menu  The menu.
/// @param which The condition, 0 or 1.
/// @param target The column or group the menu edits.
/// @return The line.
[[nodiscard]] inline Node conditionLine(FilterMenu& menu, std::size_t which, FilterTarget const& target) {
    table::ColumnKind const kind = target.shown.info.kind;
    std::string const at = std::to_string(which + 1);
    std::vector<SelectOption> options;
    for (ConditionOp const op : conditionOps(kind)) {
        std::string const label{conditionLabel(op, kind)};
        options.push_back(SelectOption{.key = Key{label}, .label = label});
    }
    Select ops{.options = options,
                  .selected = [&menu, which, kind]() -> std::optional<Key> {
                      ConditionDraft const& draft = menu.state().conditions.at(which);
                      if (!draft.op) {
                          return std::nullopt;
                      }
                      return Key{std::string{conditionLabel(*draft.op, kind)}};
                  },
                  .onSelect = [&menu, which, kind](Key const& key) {
                      std::string const* const label = std::get_if<std::string>(&key);
                      for (ConditionOp const op : conditionOps(kind)) {
                          if (label != nullptr && conditionLabel(op, kind) == *label) {
                              menu.setOp(which, op);
                          }
                      }
                  }};
    ops.label = std::string{"Condition"};
    ops.common.testId = "table_filter_op_" + at;
    auto const takes = [&menu, which] {
        ConditionDraft const& draft = menu.state().conditions.at(which);
        return draft.op && takesOperand(*draft.op);
    };
    auto const between = [&menu, which] { return menu.state().conditions.at(which).op == ConditionOp::Between; };
    auto const errors = [&menu, which] {
        return menu.state().conditions.at(which).invalid ? std::vector<std::string>{"Not a value of this column"}
                                                         : std::vector<std::string>{};
    };
    std::vector<Node> children{select(std::move(ops))};
    bool const dated = kind == table::ColumnKind::Date || kind == table::ColumnKind::DateTime;
    for (bool const upper : {false, true}) {
        std::string const testId = std::string{upper ? "table_filter_second_" : "table_filter_operand_"} + at;
        auto const text = [&menu, which, upper] {
            ConditionDraft const& draft = menu.state().conditions.at(which);
            return upper ? draft.second : draft.operand;
        };
        auto const set = [&menu, which, upper](std::string value) {
            if (upper) {
                menu.setSecond(which, std::move(value));
            } else {
                menu.setOperand(which, std::move(value));
            }
        };
        if (dated) {
            bool const dateTime = kind == table::ColumnKind::DateTime;
            DateTimeInput picker{.value = [text] { return instantOf(text()); },
                                 .onChange = [set, dateTime](std::optional<time::Timestamp> const& picked) {
                                     set(picked && picked->hasValue() ? operandOfInstant(**picked, dateTime)
                                                                      : std::string{});
                                 },
                                 .mode = dateTime ? DateMode::DateTime : DateMode::Date};
            picker.common.testId = testId;
            picker.common.visible = upper ? Prop<bool>{between} : Prop<bool>{takes};
            picker.field.errors = errors;
            children.push_back(dateTimeInput(std::move(picker)));
        } else {
            TextInput input{.value = text, .onChange = set};
            input.common.testId = testId;
            input.common.visible = upper ? Prop<bool>{between} : Prop<bool>{takes};
            input.field.errors = errors;
            children.push_back(textInput(std::move(input)));
        }
    }
    if (kind == table::ColumnKind::Measure && !target.unitChoices.empty()) {
        std::vector<SelectOption> units;
        for (std::string const& unit : target.unitChoices) {
            units.push_back(SelectOption{.key = Key{unit}, .label = unit});
        }
        Select unit{.options = units,
                    .selected = [&menu, which]() -> std::optional<Key> {
                        std::string const& chosen = menu.state().conditions.at(which).unit;
                        return chosen.empty() ? std::nullopt : std::optional<Key>{Key{chosen}};
                    },
                    .onSelect = [&menu, which](Key const& key) {
                        if (std::string const* const id = std::get_if<std::string>(&key)) {
                            menu.setUnit(which, *id);
                        }
                    }};
        unit.label = std::string{"Unit"};
        unit.common.testId = "table_filter_unit_" + at;
        children.push_back(select(std::move(unit)));
    }
    return ui::row(Row{.children = std::move(children), .gap = 1});
}

/// @brief The menu's content for one opening: what it shows depends on what it edits, so each opening builds it.
/// @param menu   The menu.
/// @param target What the opening edits.
/// @return The content.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): one widget per part the spec lists.
[[nodiscard]] inline Node menuContent(FilterMenu& menu, FilterTarget const& target) {
    std::vector<Node> parts;
    table::ColumnKind const kind = target.shown.info.kind;
    bool const isGroup = target.column.empty();
    bool const tree = !isGroup && (kind == table::ColumnKind::Date || kind == table::ColumnKind::DateTime);
    bool const listed = !target.formed && target.listed && kind != table::ColumnKind::Custom;
    if (listed) {
        auto const shows = [&menu] { return FilterMenu::valuesListed(menu.state()); };
        if (!tree) {
            TextInput search{.value = [&menu] { return menu.state().search; },
                             .onChange = [&menu](std::string text) { menu.search(std::move(text)); },
                             .placeholder = std::string{"Search"}};
            search.label = std::string{"Search"};
            search.common.testId = "table_filter_search";
            search.common.visible = shows;
            parts.push_back(textInput(std::move(search)));
            Checkbox all{.label = std::string{"(Select all)"},
                         .checked = [&menu] { return menu.allTicked(); },
                         .onToggle = [&menu](bool checked) { menu.tickAll(checked); }};
            all.common.testId = "table_filter_all";
            all.common.visible = shows;
            parts.push_back(checkbox(std::move(all)));
        }
        Checkbox blanks{.label = std::string{"(Blanks)"},
                        .checked = [&menu] { return menu.state().ticks.blanks == true; },
                        .onToggle = [&menu](bool checked) { menu.tickBlanks(checked); }};
        blanks.common.testId = "table_filter_blanks";
        blanks.common.visible = [&menu] { return FilterMenu::valuesListed(menu.state()) && menu.state().blanks > 0; };
        parts.push_back(checkbox(std::move(blanks)));
        parts.push_back(forEach<FilterListRow>(
            [&menu] { return menu.rows(); }, [](FilterListRow const& row) -> Key { return Key{row.key}; },
            [&menu](reactive::Signal<FilterListRow> const& row) -> Node {
                FilterListRow const first = row.peek();
                if (first.kind == FilterListRow::Kind::Heading) {
                    Text heading{.text = first.label, .role = TextRole::Muted};
                    heading.common.testId = "table_filter_heading_" + first.id;
                    return ui::text(std::move(heading));
                }
                std::size_t const index = first.index;
                Checkbox box{.label = [&row] { return row.get().label; },
                             .checked = [&row] { return row.get().checked; },
                             .onToggle = [&menu, index](bool checked) { menu.tick(index, checked); }};
                box.common.testId = "table_filter_value_" + first.id;
                box.common.enabled = [&row] { return row.get().enabled; };
                std::vector<Node> line;
                if (first.depth > 0) {
                    Spacer indent;
                    indent.common.layout.width = Sizing::fixed(static_cast<int>(2 * first.depth));
                    line.push_back(spacer(std::move(indent)));
                }
                line.push_back(checkbox(std::move(box)));
                if (first.opens) {
                    Button open{.label = std::string{"▸"}, .onClick = [&menu, index] { menu.openBucket(index); }};
                    open.common.testId = "table_filter_open_" + first.id;
                    line.push_back(button(std::move(open)));
                }
                return ui::row(Row{.children = std::move(line), .gap = 1});
            },
            Axis::Vertical, 0));
        Button more{.label = std::string{"More values"}, .onClick = [&menu] { menu.more(); }};
        more.common.testId = "table_filter_more";
        more.common.visible = [&menu] {
            FilterMenuState const& state = menu.state();
            auto const top = std::ranges::count_if(state.list, [](FilterListEntry const& entry) { return entry.depth == 0; });
            return FilterMenu::valuesListed(state) && top < state.distinct;
        };
        parts.push_back(button(std::move(more)));
    }
    Text note{.text = [&menu] { return menu.note(); }, .role = TextRole::Muted};
    note.common.testId = "table_filter_note";
    parts.push_back(ui::text(std::move(note)));
    if (!conditionOps(kind).empty()) {
        parts.push_back(conditionLine(menu, 0, target));
        Select combine{
            .options = std::vector<SelectOption>{{.key = Key{std::string{"and"}}, .label = "and"},
                                                 {.key = Key{std::string{"or"}}, .label = "or"}},
            .selected = [&menu] {
                return std::optional<Key>{Key{std::string{menu.state().combine == table::Combine::All ? "and" : "or"}}};
            },
            .onSelect = [&menu](Key const& key) {
                std::string const* const name = std::get_if<std::string>(&key);
                if (name != nullptr) {
                    menu.setCombine(*name == "and" ? table::Combine::All : table::Combine::Any);
                }
            }};
        combine.label = std::string{"Join"};
        combine.common.testId = "table_filter_combine";
        parts.push_back(select(std::move(combine)));
        parts.push_back(conditionLine(menu, 1, target));
        if (kind == table::ColumnKind::Text) {
            Checkbox matchCase{.label = std::string{"match case"},
                               .checked = [&menu] { return menu.state().matchCase; },
                               .onToggle = [&menu](bool on) { menu.setMatchCase(on); }};
            matchCase.common.testId = "table_filter_case";
            parts.push_back(checkbox(std::move(matchCase)));
        }
    }
    if (isGroup) {
        std::size_t const columns = target.columns.size();
        bool const formed = target.formed;
        Select match{.options = [&menu, columns, formed] {
                         std::vector<SelectOption> out;
                         for (GroupMatch const choice : {GroupMatch::AnyColumn, GroupMatch::EveryColumn,
                                                         GroupMatch::AtLeast, GroupMatch::Exactly}) {
                             out.push_back(SelectOption{.key = Key{std::string{matchLabel(choice)}},
                                                        .label = std::string{matchLabel(choice)}});
                         }
                         // "all of them, each in a column" takes ticked values: 2 to k of them.
                         std::size_t ticked = 0;
                         for (FilterListRow const& row : menu.rows()) {
                             ticked += row.kind == FilterListRow::Kind::Value && row.checked ? 1U : 0U;
                         }
                         if (!formed && ticked >= 2 && ticked <= columns) {
                             std::string const label{matchLabel(GroupMatch::Together)};
                             out.push_back(SelectOption{.key = Key{label}, .label = label});
                         }
                         return out;
                     },
                     .selected = [&menu] { return std::optional<Key>{Key{std::string{matchLabel(menu.state().match)}}}; },
                     .onSelect = [&menu](Key const& key) {
                         std::string const* const label = std::get_if<std::string>(&key);
                         for (GroupMatch const choice : {GroupMatch::AnyColumn, GroupMatch::EveryColumn,
                                                         GroupMatch::AtLeast, GroupMatch::Exactly, GroupMatch::Together}) {
                             if (label != nullptr && matchLabel(choice) == *label) {
                                 menu.setMatch(choice);
                             }
                         }
                     }};
        match.label = std::string{"Match"};
        match.common.testId = "table_filter_match";
        parts.push_back(select(std::move(match)));
        TextInput count{.value = [&menu] { return menu.state().count; },
                        .onChange = [&menu](std::string text) { menu.setCount(std::move(text)); }};
        count.label = std::string{"N"};
        count.common.testId = "table_filter_count";
        count.common.visible = [&menu] {
            GroupMatch const match = menu.state().match;
            return match == GroupMatch::AtLeast || match == GroupMatch::Exactly;
        };
        parts.push_back(textInput(std::move(count)));
    }
    Button apply{.label = std::string{"Apply"}, .emphasis = ButtonEmphasis::Primary, .onClick = [&menu] { menu.apply(); }};
    apply.common.testId = "table_filter_apply";
    apply.common.enabled = [&menu] { return menu.canApply(); };
    Button clear{.label = std::string{"Clear filter"}, .onClick = [&menu] { menu.clearFilter(); }};
    clear.common.testId = "table_filter_clear";
    parts.push_back(ui::row(Row{.children = {button(std::move(apply)), button(std::move(clear))}, .gap = 1}));
    return ui::column(Column{.children = std::move(parts), .gap = 1});
}

}  // namespace detail::filtermenu

/// @brief The filter menu: a dialog, test id `table_filter`, titled with the column's or the group's label.
///
/// Its content is built for each opening, by what the menu edits. A values list: a search `table_filter_search`, a
/// box "(Select all)" `table_filter_all` and one "(Blanks)" `table_filter_blanks` while the column has empty cells,
/// then a box `table_filter_value_<i>` per value (`{text} ({count})`, in the reply's order, `i` from 0), a
/// `Measure` column's dimension heading `table_filter_heading_<dimension>` before its first value, a button
/// `table_filter_open_<i>` on a date bucket that opens to its months or days, and "More values"
/// (`table_filter_more`). A date tree has no search and no "(Select all)". A note `table_filter_note`. The
/// conditions, by the column's kind: `table_filter_op_1` and `_2`, `table_filter_operand_1` and `_2` (a date
/// picker on a date column), `table_filter_second_1` and `_2` for `between`, `table_filter_unit_1` and `_2` on a
/// `Measure` column, `table_filter_combine`, and "match case" (`table_filter_case`) on a text column. A group's
/// match line `table_filter_match` and its N `table_filter_count`. Then Apply (`table_filter_apply`) and "Clear
/// filter" (`table_filter_clear`). A dismissal closes it.
/// @param menu The menu; it must outlive the node.
/// @return The dialog's node.
[[nodiscard]] inline Node filterMenuDialog(FilterMenu& menu) {
    Node const content = forEach<std::uint64_t>(
        [&menu] {
            FilterMenuState const& state = menu.state();
            return state.open ? std::vector<std::uint64_t>{state.opening} : std::vector<std::uint64_t>{};
        },
        [](std::uint64_t const& opening) -> Key { return Key{static_cast<std::int64_t>(opening)}; },
        [&menu](reactive::Signal<std::uint64_t> const& opening) {
            static_cast<void>(opening);
            FilterTarget const target = menu.state().target;
            return detail::filtermenu::menuContent(menu, target);
        },
        Axis::Vertical, 0);
    Dialog dialog{.open = [&menu] { return menu.state().open; },
                  .title = [&menu] { return menu.state().target.label; },
                  .child = content,
                  .onDismiss = [&menu] { menu.close(); }};
    dialog.common.testId = "table_filter";
    return ui::dialog(std::move(dialog));
}

}  // namespace morph::ui
