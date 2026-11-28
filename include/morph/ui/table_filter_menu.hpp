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
#include <morph/export.hpp>
#include <morph/ui/cell_format.hpp>
#include <morph/ui/table_config.hpp>
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
[[nodiscard]] MORPH_API std::optional<std::size_t> groupFilterIndex(table::FilterSpec const& filters,
                                                                    FilterTarget const& target);

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
struct MORPH_API FilterMenuState {
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
    /// @param other The state to compare with.
    /// @return Whether every member matches.
    bool operator==(FilterMenuState const& other) const;
};

// ---------------------------------------------------------------------------------------------------------------
// Pure parts
// ---------------------------------------------------------------------------------------------------------------

namespace detail::filtermenu {

/// @brief Folds ASCII letters to lower case, as a search's text is compared locally.
/// @param text The text.
/// @return It, folded.
[[nodiscard]] MORPH_API std::string folded(std::string_view text);

/// @brief Reads a number from a slice of a text.
/// @tparam Number The integer type read.
/// @param text  The text.
/// @param from  The slice's first character.
/// @param count The slice's length; the slice ends where the text does when it is shorter.
/// @param out   The number read.
/// @return Whether the slice starts with a number.
template <typename Number>
[[nodiscard]] inline bool readNumber(std::string_view text, std::size_t from, std::size_t count, Number& out) {
    std::string_view const slice = text.substr(from, count);
    return std::from_chars(std::to_address(slice.begin()), std::to_address(slice.end()), out).ec == std::errc{};
}

/// @brief The day a `YYYY-MM-DD` date, or the date part of an ISO date-time, falls on.
/// @param text The text.
/// @return Days since 1970-01-01; nothing when it does not read.
[[nodiscard]] MORPH_API std::optional<std::int64_t> dayOf(std::string_view text);

/// @brief A date bucket's range, as days.
/// @param operand The bucket's operand, `a..b`.
/// @return Its first and last day; nothing when it does not read.
[[nodiscard]] MORPH_API std::optional<std::pair<std::int64_t, std::int64_t>> rangeOf(std::string_view operand);

/// @brief A range of days as a date bucket's operand.
/// @param first    Its first day.
/// @param last     Its last day.
/// @param dateTime Whether the column holds date-times: its ends are `YYYY-MM-DDT00:00:00Z` and `…T23:59:59Z`.
/// @return `a..b`.
[[nodiscard]] MORPH_API std::string rangeText(std::int64_t first, std::int64_t last, bool dateTime);

/// @brief Whether a ticked range covers a bucket.
/// @param ranges The ticked ranges' operands.
/// @param bucket The bucket's operand.
/// @return `{covered, own}`: whether a range covers it, and whether one of those is the bucket's own.
[[nodiscard]] MORPH_API std::pair<bool, bool> coverOf(std::span<std::string const> ranges, std::string const& bucket);

/// @brief Whether a text column's condition, or a number's or a date's, takes an operand.
/// @param op The condition.
/// @return `false` for `is empty` and `is not empty`.
[[nodiscard]] inline bool takesOperand(ConditionOp op) {
    return op != ConditionOp::IsEmpty && op != ConditionOp::IsNotEmpty;
}

/// @brief The condition an entry is, with its operands, when it is one the menu offers.
/// @param entry The entry.
/// @return The draft; nothing for an entry the menu cannot show.
[[nodiscard]] MORPH_API std::optional<ConditionDraft> draftOf(table::FilterEntry const& entry);

}  // namespace detail::filtermenu

/// @brief Merges the ticked buckets of a date tree into the ranges a filter sends.
///
/// The ranges are sorted by their first end; one inside another adds nothing, and two that touch (the next begins
/// the day after the previous ends, or the second after on a date-time column) or overlap become one.
/// @param ticked   The ticked buckets' operands, `a..b`, in any order.
/// @param dateTime Whether the column holds date-times: its ends are `YYYY-MM-DDT00:00:00Z` and `…T23:59:59Z`.
/// @return The ranges, ascending, as `a..b` operands; an operand that does not read is left out.
[[nodiscard]] MORPH_API std::vector<std::string> mergeDateRanges(std::span<std::string const> ticked, bool dateTime);

/// @brief A values list's ticks as a stored values filter makes them.
/// @param filter The column's filter, of the values part (`classifyFilter`).
/// @return The ticks: an include side ticks its members and nothing else, an exclude side unticks its members.
[[nodiscard]] MORPH_API ValueTicks ticksOfFilter(table::ColumnFilter const& filter);

/// @brief The conditions a stored filter's entries are, as the menu shows them.
/// @param entries The include entries.
/// @return Up to two drafts, the rest of the array empty; nothing when an entry is one the menu cannot show or
///         there are more than two.
[[nodiscard]] MORPH_API std::optional<std::array<ConditionDraft, 2>> draftsOf(
    std::span<table::FilterEntry const> entries);

/// @brief A group's match line as a filter's quantifier.
/// @param match   The match line's choice; `Together` is not a quantifier and reads as `AnyColumn`.
/// @param n       N, for `AtLeast` and `Exactly`.
/// @return The quantifier.
[[nodiscard]] MORPH_API table::ColumnCount columnCountOf(GroupMatch match, std::int64_t n);

/// @brief A match line's text.
/// @param match The choice.
/// @return `in any column`, `in every column`, `in at least N columns`, `in exactly N columns`, or
///         `all of them, each in a column`.
[[nodiscard]] MORPH_API std::string_view matchLabel(GroupMatch match);

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
    /// @brief A value's index in the list, the `{i}` of its test ids; the dimension of a heading.
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
class MORPH_API FilterMenu {
public:
    /// @brief A closed menu.
    /// @param runtime The runtime its state belongs to.
    explicit FilterMenu(reactive::Runtime& runtime) : _state{runtime} {}

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
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): one ordered setup sharing `next`.
    void open(FilterTarget target);

    /// @brief Closes the menu; a pending condition is dropped with it.
    void close();

    /// @brief The search typed: the list asks again, `debounce` after the last keystroke.
    /// @param text The search.
    void search(std::string text);

    /// @brief Ticks or unticks a value, or a date bucket.
    /// @param index   The value's index in the list.
    /// @param checked Its new state.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): one tick, by the kind of row it lands on.
    void tick(std::size_t index, bool checked);

    /// @brief Ticks or unticks "(Select all)": every value the search shows, every page of them.
    /// @param checked Its new state.
    void tickAll(bool checked);

    /// @brief Ticks or unticks "(Blanks)".
    /// @param checked Its new state.
    void tickBlanks(bool checked);

    /// @brief Asks for the next page of values.
    void more();

    /// @brief Opens or closes a date bucket: an opened year lists its months, an opened month its days.
    /// @param index The bucket's index in the list.
    void openBucket(std::size_t index);

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
    /// @param enabled Whether text conditions match case.
    void setMatchCase(bool enabled) {
        editCondition([&](FilterMenuState& state) { state.matchCase = enabled; });
    }

    /// @brief Sets how the two conditions join.
    /// @param combine `All` for "and", `Any` for "or".
    void setCombine(table::Combine combine) {
        editCondition([&](FilterMenuState& state) { state.combine = combine; });
    }

    /// @brief Chooses a group's match line; a group's conditions apply it as they apply an edit.
    /// @param match The choice.
    void setMatch(GroupMatch match);

    /// @brief Types the N of a group's match line.
    /// @param text N, as typed.
    void setCount(std::string text);

    /// @brief Applies the menu: a condition waiting for its debounce goes at once; otherwise the ticks are resolved,
    ///        every page of the values they reach asked for, and the side `planValueFilter` chooses is set. The menu
    ///        closes once the filter is applied, and says in its note why when nothing is.
    void apply();

    /// @brief "Clear filter": removes the column's or the group's filter, and the menu's ticks and conditions.
    void clearFilter();

    /// @brief The list's lines as the view draws them: a `Measure` column's dimension heading before its first
    ///        value, each value's caption by the column's display and its count, ticked as the ticks say. Tracked.
    /// @return The lines.
    [[nodiscard]] std::vector<FilterListRow> rows() const;

    /// @brief Whether "(Select all)" shows ticked: every value listed is. Tracked.
    /// @return It.
    [[nodiscard]] bool allTicked() const;

    /// @brief Whether Apply can apply something: a condition, or a values list with a box ticked. A date tree's
    ///        Apply with nothing ticked clears its filter, so it is always enabled. Tracked.
    /// @return It.
    [[nodiscard]] bool canApply() const;

    /// @brief The note under the list: that it may grow, how many cells could not be read, and why the last Apply
    ///        applied nothing, one a line. Tracked.
    /// @return It.
    [[nodiscard]] std::string note() const;

    /// @brief Whether the open target shows a values list.
    /// @param state The menu's state.
    /// @return `false` for a group the user formed, a `Custom` column, and rows that list no values.
    [[nodiscard]] static bool valuesListed(FilterMenuState const& state);

    /// @brief Whether the open target's list is a date tree.
    /// @param state The menu's state.
    /// @return `true` on a `Date` or `DateTime` column.
    [[nodiscard]] static bool dates(FilterMenuState const& state);

private:
    using Reply = std::expected<table::DistinctValues, table::TableError>;

    static void clearConditions(FilterMenuState& state);

    [[nodiscard]] static bool hasConditions(FilterMenuState const& state) {
        return std::ranges::any_of(state.conditions, [](ConditionDraft const& draft) { return draft.op.has_value(); });
    }

    [[nodiscard]] static std::vector<std::string> columnsOf(FilterTarget const& target) {
        return target.column.empty() ? target.columns : std::vector<std::string>{target.column};
    }

    // "all of them, each in a column" is offered while 2 or more values are ticked: below that the match line falls
    // back to its default.
    static void leaveTogether(FilterMenuState& state);

    // Whether a date bucket lies under a listed bucket that is ticked itself.
    [[nodiscard]] static bool underTicked(FilterMenuState const& state, std::size_t index);

    // Whether a value's box shows ticked: the ticks' first state, then each operation that reaches it in order. A
    // search reaches the values whose text contains it, folded.
    [[nodiscard]] static bool tickedNow(FilterMenuState const& state, table::DistinctValue const& value);

    static void readGroup(FilterMenuState& next, table::GroupFilter const& group);

    void touch() const;

    // Runs @p fn after the debounce, replacing what @p timer held; at once without a scheduler.
    void later(reactive::TimerHandle& timer, std::function<void()> fn);

    template <typename F>
    void editCondition(F&& edit) {
        touch();
        _state.mutate([&](FilterMenuState& state) {
            std::forward<F>(edit)(state);
            // Editing a condition leaves the values part: its ticks start again.
            state.ticks = ValueTicks{.ticked = !dates(state) && !state.target.column.empty(),
                                     .ops = {},
                                     .blanks = state.blanks > 0 ? std::optional{!dates(state)} : std::nullopt};
            state.dates.clear();
            state.refusal.clear();
        });
        scheduleSend();
    }

    void scheduleSend();

    [[nodiscard]] table::DistinctRequest requestOf(FilterMenuState const& state) const;

    void ask(table::DistinctRequest request, std::function<void(Reply const&)> done);

    // Asks for the list's first page under the search, or with @p next the page after those listed.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): the request and its reply share one edit.
    void load(
        bool
            next);  // NOLINT(clang-analyzer-cplusplus.NewDeleteLeaks): the reply's continuations are owned by the scope.

    // Every value a search shows, every page of them, asked at `maxLimit` a request.
    void pageAll(
        std::string search,
        std::function<void(std::expected<std::vector<table::DistinctValue>, std::string>, std::int64_t distinct)>
            done);

    // Resolves the ticks: the values each search's "(Select all)" reached, one search after another, and the count
    // of every value.
    void resolve(std::function<void(std::expected<std::pair<ResolvedTicks, std::int64_t>, std::string>)> const& done);

    void refuse(std::string why) {
        _state.mutate([&](FilterMenuState& state) { state.refusal = std::move(why); });
    }

    // Sets @p spec and closes the menu once it is applied.
    void commit(table::FilterSpec spec);

    // NOLINTNEXTLINE(readability-function-cognitive-complexity): one resolve-then-write sequence.
    void applyValues();

    // NOLINTNEXTLINE(readability-function-cognitive-complexity): one resolve-then-write sequence.
    void applyGroupValues();

    void applyDates();

    // Sends the conditions when every operand reads and every `between` has both ends; marks those that do not
    // read. Returns whether a filter was sent.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): reads both condition lines into one filter.
    bool sendConditions();

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
[[nodiscard]] MORPH_API std::optional<time::Timestamp> instantOf(std::string_view text);

/// @brief The operand a picked instant is.
/// @param instant  The instant, UTC.
/// @param dateTime Whether the column holds date-times.
/// @return `YYYY-MM-DD`, or `YYYY-MM-DDTHH:MM:SSZ`.
[[nodiscard]] MORPH_API std::string operandOfInstant(time::DateTime const& instant, bool dateTime);

/// @brief One condition's line: the condition select, the operand (a text input, or the date picker), `between`'s
///        second end and a `Measure` column's unit.
/// @param menu  The menu.
/// @param which The condition, 0 or 1.
/// @param target The column or group the menu edits.
/// @return The line.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): one line's controls, by condition kind.
[[nodiscard]] MORPH_API Node conditionLine(FilterMenu& menu, std::size_t which, FilterTarget const& target);

/// @brief The menu's content for one opening: what it shows depends on what it edits, so each opening builds it.
/// @param menu   The menu.
/// @param target What the opening edits.
/// @return The content.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): one widget per part the spec lists.
[[nodiscard]] MORPH_API Node menuContent(FilterMenu& menu, FilterTarget const& target);

}  // namespace detail::filtermenu

/// @brief The filter menu: a dialog, test id `table_filter`, titled with the column's or the group's label.
///
/// Its content is built for each opening, by what the menu edits. A values list: a search `table_filter_search`, a
/// box "(Select all)" `table_filter_all` and one "(Blanks)" `table_filter_blanks` while the column has empty cells,
/// then a box `table_filter_value_{i}` per value (`{text} ({count})`, in the reply's order, `i` from 0), a
/// `Measure` column's dimension heading `table_filter_heading_<dimension>` before its first value, a button
/// `table_filter_open_{i}` on a date bucket that opens to its months or days, and "More values"
/// (`table_filter_more`). A date tree has no search and no "(Select all)". A note `table_filter_note`. The
/// conditions, by the column's kind: `table_filter_op_1` and `_2`, `table_filter_operand_1` and `_2` (a date
/// picker on a date column), `table_filter_second_1` and `_2` for `between`, `table_filter_unit_1` and `_2` on a
/// `Measure` column, `table_filter_combine`, and "match case" (`table_filter_case`) on a text column. A group's
/// match line `table_filter_match` and its N `table_filter_count`. Then Apply (`table_filter_apply`) and "Clear
/// filter" (`table_filter_clear`). A dismissal closes it.
/// @param menu The menu; it must outlive the node.
/// @return The dialog's node.
[[nodiscard]] MORPH_API Node filterMenuDialog(FilterMenu& menu);

}  // namespace morph::ui
