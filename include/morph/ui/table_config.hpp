// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file ui/table_config.hpp
/// @brief How a user configures a table, as pure functions: sort gestures, layout gestures, the header and table
///        menus, value filters, conditions, copy text and the status line's messages.
///
/// Nothing here holds state or talks to a backend: the lowering of a table node calls these with what it holds
/// and applies what they return. See `docs/spec/table/table.md`, "Configuring a table".

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <iterator>
#include <map>
#include <morph/export.hpp>
#include <morph/table/engine.hpp>
#include <morph/ui/backend.hpp>
#include <morph/ui/measure.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace morph::ui {

/// @brief A group's column bound, `QueryLimits::maxGroupColumns`.
inline constexpr std::size_t kMaxGroupColumns = 16;

/// @brief A table's sort chain, most significant key first.
using SortChain = table::SortChain;

// ---------------------------------------------------------------------------------------------------------------
// The status line's messages
// ---------------------------------------------------------------------------------------------------------------

/// @brief Writes a count with its digits grouped by three.
/// @param n         The count.
/// @param separator The grouping separator: the session's numeric one (`","` in English).
/// @return The digits, a leading `-` for a negative count, grouped from the right (`10,000`).
[[nodiscard]] MORPH_API std::string groupedCount(std::int64_t n, std::string_view separator = ",");

/// @brief What a table can let its user change about its columns, as `configurable` lists it.
enum class Configure : std::uint8_t {
    None = 0,     ///< Nothing.
    Reorder = 1,  ///< Moving columns.
    Hide = 2,     ///< Hiding and showing columns.
    Pin = 4,      ///< Pinning and unpinning columns.
    Resize = 8,   ///< Setting and clearing widths.
    All = 15,     ///< Everything.
};

/// @brief The union of two capability sets.
/// @param lhs One set.
/// @param rhs The other.
/// @return Every capability either holds.
[[nodiscard]] constexpr Configure operator|(Configure lhs, Configure rhs) noexcept {
    return static_cast<Configure>(static_cast<std::uint8_t>(lhs) | static_cast<std::uint8_t>(rhs));
}

/// @brief The intersection of two capability sets.
/// @param lhs One set.
/// @param rhs The other.
/// @return The capabilities both hold.
[[nodiscard]] constexpr Configure operator&(Configure lhs, Configure rhs) noexcept {
    return static_cast<Configure>(static_cast<std::uint8_t>(lhs) & static_cast<std::uint8_t>(rhs));
}

/// @brief Whether a capability set allows something.
/// @param set  What the table allows.
/// @param what The capabilities asked for.
/// @return `true` when @p set holds every capability of @p what.
[[nodiscard]] constexpr bool allows(Configure set, Configure what) noexcept { return (set & what) == what; }

/// @brief The status line's and the menus' messages, in exact English copy.
namespace tableText {

/// @brief The refusal of a sort, filter or arrangement gesture while the table's header is disabled.
inline constexpr std::string_view kHeaderBusy = "Sorting, filtering and arranging are unavailable right now";

/// @brief The refusal of a sort key past the bound.
/// @param maxSortKeys The table's bound.
/// @return `A table sorts by at most {n} columns`.
[[nodiscard]] inline std::string sortBound(std::size_t maxSortKeys) {
    return "A table sorts by at most " + groupedCount(static_cast<std::int64_t>(maxSortKeys)) + " columns";
}

/// @brief The refusal of a range over the bound.
/// @param maxRows   The bound.
/// @param separator The grouping separator.
/// @return `A range selects at most {n} rows`.
[[nodiscard]] inline std::string rangeBound(std::size_t maxRows, std::string_view separator = ",") {
    return "A range selects at most " + groupedCount(static_cast<std::int64_t>(maxRows), separator) + " rows";
}

/// @brief The refusal of a copy over the bound.
/// @param maxRows   The bound.
/// @param separator The grouping separator.
/// @return `A copy holds at most {n} rows`.
[[nodiscard]] inline std::string copyBound(std::size_t maxRows, std::string_view separator = ",") {
    return "A copy holds at most " + groupedCount(static_cast<std::int64_t>(maxRows), separator) + " rows";
}

/// @brief The notice after a copy.
/// @param rows      The rows copied.
/// @param unloaded  The cells copied as empty because they were not loaded.
/// @param separator The grouping separator.
/// @return `Copied {n} rows` (`Copied 1 row`), and with unloaded cells `, {m} cells were not loaded`
///         (`, 1 cell was not loaded`).
[[nodiscard]] MORPH_API std::string copied(std::size_t rows, std::size_t unloaded, std::string_view separator = ",");

/// @brief The refusal of a gesture on a locked column.
/// @param label The column's label.
/// @return `{label} is locked`.
[[nodiscard]] inline std::string locked(std::string_view label) { return std::string{label} + " is locked"; }

/// @brief The refusal of a gesture the table does not allow.
/// @param what The capability lacking; with several, the first of reorder, hide, pin and resize names it, and
///             with none, reorder does.
/// @return `This table's columns cannot be moved` (`hidden`, `pinned`, `resized`).
[[nodiscard]] MORPH_API std::string notConfigurable(Configure what);

/// @brief The refusal of a group of too few or too many columns.
/// @return `A group has 2 to 16 columns`.
[[nodiscard]] inline std::string groupSize() { return "A group has 2 to 16 columns"; }

/// @brief The refusal of a group of columns of different kinds.
/// @return `A group's columns hold one kind of value`.
[[nodiscard]] inline std::string groupKind() { return "A group's columns hold one kind of value"; }

/// @brief The refusal of a group that takes a column another user-formed group holds.
/// @param label The column's label.
/// @param group The other group's label.
/// @return `{label} is already in {group}`.
[[nodiscard]] inline std::string alreadyGrouped(std::string_view label, std::string_view group) {
    return std::string{label} + " is already in " + std::string{group};
}

/// @brief The refusal of hiding the last visible column.
/// @return `At least one column stays visible`.
[[nodiscard]] inline std::string lastVisible() { return "At least one column stays visible"; }

/// @brief Why a values list is disabled: both sides are over `QueryLimits::maxSetValues`.
/// @return `Too many values to list; filter with a condition`.
[[nodiscard]] inline std::string tooManyValues() { return "Too many values to list; filter with a condition"; }

/// @brief Why a date tree applies nothing: its buckets are over `QueryLimits::maxEntries`.
/// @return `Too many dates to list; filter with a condition`.
[[nodiscard]] inline std::string tooManyDates() { return "Too many dates to list; filter with a condition"; }

/// @brief The note under a values list some rows were left out of while their cells load.
/// @return `The list may grow while data loads`.
[[nodiscard]] inline std::string mayGrow() { return "The list may grow while data loads"; }

/// @brief The note under a values list about invalid cells.
/// @param cells     How many cells could not be read.
/// @param separator The grouping separator.
/// @return `{n} cells could not be read` (`1 cell could not be read`).
[[nodiscard]] MORPH_API std::string unreadable(std::size_t cells, std::string_view separator = ",");

}  // namespace tableText

// ---------------------------------------------------------------------------------------------------------------
// Sort chains
// ---------------------------------------------------------------------------------------------------------------

/// @brief A user's gesture on a column's place in the sort chain.
enum class SortGesture : std::uint8_t {
    Click,       ///< A plain click: by this column alone, ascending, or descending when it is the first key ascending.
    Extend,      ///< `Shift`+click: flips the column's key in place, or appends it ascending.
    Ascending,   ///< "Sort ascending": by this column alone, ascending.
    Descending,  ///< "Sort descending": by this column alone, descending.
    Add,         ///< "Add to sort": appends the column ascending when the chain does not hold it.
    Remove,      ///< "Remove from sort": removes the column's key.
};

/// @brief What a sort gesture makes of a chain.
struct SortChange {
    /// @brief The new chain; the old one when the gesture was refused.
    SortChain chain;
    /// @brief The status line's refusal, when the gesture was refused.
    std::optional<std::string> refusal;
};

/// @brief Applies a sort gesture to a chain.
///
/// A gesture on a column the chain holds is decided before the bound, so at the bound a key in the chain still
/// flips; a new key past the bound is refused and the chain is unchanged, never shortened.
/// @param chain       The current chain.
/// @param column      The column's id.
/// @param gesture     The gesture.
/// @param maxSortKeys The table's bound on the chain.
/// @return The new chain, or the old one with `tableText::sortBound(maxSortKeys)`.
[[nodiscard]] MORPH_API SortChange applySortGesture(SortChain const& chain, std::string const& column,
                                                    SortGesture gesture, std::size_t maxSortKeys);

// ---------------------------------------------------------------------------------------------------------------
// Layout gestures
// ---------------------------------------------------------------------------------------------------------------

/// @brief What a layout gesture needs to know of a column.
struct LayoutColumn {
    /// @brief The column's id.
    std::string id;
    /// @brief The column's label, which a refusal names.
    std::string label;
    /// @brief The user cannot move, hide, pin, unpin or resize it.
    bool locked = false;
    /// @brief What the column holds; a user-formed group's columns are of one kind.
    table::ColumnKind kind = table::ColumnKind::Text;
};

/// @brief Moves a column among the visible columns by `by` places (negative: towards the leading edge).
struct MoveColumn {
    /// @brief The column's id.
    std::string id;
    /// @brief The places to move, clamped to the visible columns.
    int by = 0;
};

/// @brief Moves a column to a place among the visible columns, as a drag drops it.
struct MoveColumnTo {
    /// @brief The column's id.
    std::string id;
    /// @brief Its index among the visible columns after the move, clamped to the last.
    std::size_t position = 0;
};

/// @brief Hides a column.
struct HideColumn {
    /// @brief The column's id.
    std::string id;
};

/// @brief Shows a hidden column.
struct ShowColumn {
    /// @brief The column's id.
    std::string id;
};

/// @brief Pins a column: it moves right after the pinned columns, and the pinned count grows by one.
struct PinColumn {
    /// @brief The column's id.
    std::string id;
};

/// @brief Unpins a column: it moves to the first place after the pinned columns, which shrink by one.
struct UnpinColumn {
    /// @brief The column's id.
    std::string id;
};

/// @brief Sets or clears a column's width.
struct ResizeColumn {
    /// @brief The column's id.
    std::string id;
    /// @brief The width in units, above zero; `nullopt` clears it, and the column takes its declared width.
    std::optional<int> units;
};

/// @brief Restores the declared order, visibility, pinning and widths.
struct ResetColumns {};

/// @brief Sets or clears the unit a column shows one of its dimensions in.
struct SetDisplayUnit {
    /// @brief The column's id.
    std::string id;
    /// @brief The dimension, such as `mass`.
    std::string dimension;
    /// @brief The unit's id; `nullopt`: the cells show their own units.
    std::optional<std::string> unit;
};

/// @brief Forms a group of columns of one kind under a label.
struct FormGroup {
    /// @brief The group's label.
    std::string label;
    /// @brief The columns' ids, 2 to 16.
    std::vector<std::string> columns;
};

/// @brief Removes a user-formed group.
struct RemoveGroup {
    /// @brief The group's id.
    std::string id;
};

/// @brief Any gesture on a table's layout.
using LayoutGesture = std::variant<MoveColumn, MoveColumnTo, HideColumn, ShowColumn, PinColumn, UnpinColumn,
                                   ResizeColumn, ResetColumns, SetDisplayUnit, FormGroup, RemoveGroup>;

/// @brief What a layout gesture did.
struct LayoutResult {
    /// @brief Whether the layout's stored value changed.
    bool changed = false;
    /// @brief The status line's refusal, when the gesture was refused.
    std::optional<std::string> refusal;
};

namespace detail {

/// @brief What a layout gesture needs before it applies.
struct GestureNeeds {
    /// @brief The capability the table must allow; `None` for a gesture that needs none.
    Configure capability = Configure::None;
    /// @brief Whether a locked column refuses the gesture.
    bool lockable = false;
    /// @brief The column the gesture acts on; empty for none.
    std::string_view column;
};

/// @brief What a layout gesture needs.
/// @param gesture The gesture; the result views its column id.
/// @return Its capability, whether `locked` refuses it, and its column.
[[nodiscard]] MORPH_API GestureNeeds needsOf(LayoutGesture const& gesture);

/// @brief Moves a column next to another in a layout's order.
/// @param layout The layout.
/// @param columnId     The column to move.
/// @param anchor The column it moves beside; @p id itself leaves the order as it is.
/// @param after  `true` to place it right after @p anchor, `false` right before.
MORPH_API void placeBeside(table::ColumnLayout& layout, std::string const& columnId, std::string const& anchor,
                           bool after);

/// @brief A column's index among the visible columns.
/// @param visible The visible columns.
/// @param columnId      The column.
/// @return The index; `visible.size()` when the column is not visible.
[[nodiscard]] inline std::size_t visibleIndex(std::vector<std::string> const& visible, std::string const& columnId) {
    return static_cast<std::size_t>(std::ranges::find(visible, columnId) - visible.begin());
}

}  // namespace detail

/// @brief Applies a user's gesture to a table's layout.
///
/// A move, hide, pin, unpin or resize of a locked column is refused with `tableText::locked(label)` before anything
/// else; then a gesture whose capability @p allowed lacks with `tableText::notConfigurable`. A reset is allowed while
/// any capability is; forming or removing a group while `reorder` or `hide` is, as the column chooser offers it.
/// Display units need no capability: they change what a column shows, not how the columns are arranged. Hiding the
/// last visible column is refused with `tableText::lastVisible()`. A gesture on a column @p columns does not list
/// changes nothing.
///
/// The set of pinned columns changes only by `PinColumn` and `UnpinColumn`, and by `MoveColumnTo`, which pins a
/// column dropped among the pinned ones and unpins one dropped past them: `MoveColumn` stays on its side of the pin
/// boundary, and hiding a pinned column or showing one among the pinned columns adjusts the count.
/// @param layout  The layout, changed in place.
/// @param columns The table's columns.
/// @param allowed What the table lets its user change.
/// @param gesture The gesture.
/// @return Whether the stored layout changed, or the refusal.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): one visit over every gesture kind.
[[nodiscard]] MORPH_API LayoutResult applyLayoutGesture(table::ColumnLayout& layout,
                                                        std::span<LayoutColumn const> columns, Configure allowed,
                                                        LayoutGesture const& gesture);

// ---------------------------------------------------------------------------------------------------------------
// Value filters
// ---------------------------------------------------------------------------------------------------------------

/// @brief One tick in a values list, recorded as an operation and resolved at Apply.
///
/// Under a search, "(Select all)" means the values the search shows, which the menu does not hold when they pass
/// a page; so a tick is kept as what the user did, not as the values it reached.
struct TickOp {
    /// @brief The search the list showed; empty for none.
    std::string search;
    /// @brief Set: one value's box, by operand. Unset: "(Select all)" under `search`.
    std::optional<std::string> operand;
    /// @brief The box's new state.
    bool ticked = true;
};

/// @brief A values list's ticks, as operations.
struct ValueTicks {
    /// @brief Every value's state before the first operation (true with no filter on the column).
    bool ticked = true;
    /// @brief The operations in order; a later one wins.
    std::vector<TickOp> ops;
    /// @brief "(Blanks)": `true` ticked, `false` unticked, `nullopt` when the list does not show it.
    std::optional<bool> blanks;
};

/// @brief A values list's ticks, resolved to values.
struct ResolvedTicks {
    /// @brief The state of every value no operation reached.
    bool ticked = true;
    /// @brief The final state of every value an operation reached, by operand.
    std::map<std::string, bool> covered;
    /// @brief "(Blanks)", as `ValueTicks::blanks`.
    std::optional<bool> blanks;
};

/// @brief Resolves tick operations to the values they reached.
///
/// The menu first gathers, for each distinct non-empty search among the operand-less operations, every value the
/// search shows (every page of `distinctValues`), and reads them back here through @p valuesUnder.
/// @param ticks       The operations.
/// @param valuesUnder The operands a search shows; called only for a non-empty search of an operand-less operation.
/// @return The state every reached value ended in; an operation with no search and no operand clears what the
///         earlier ones reached and sets every value's state.
[[nodiscard]] MORPH_API ResolvedTicks resolveTicks(
    ValueTicks const& ticks, std::function<std::vector<std::string>(std::string const& search)> const& valuesUnder);

/// @brief Which side of a values list a filter sends, and how many values it names.
struct ValueFilterPlan {
    /// @brief The side.
    enum class Side : std::uint8_t {
        All,       ///< Every value ticked, "(Blanks)" not unticked: no values filter.
        Include,   ///< The ticked values, the smaller side.
        Exclude,   ///< The unticked values, the smaller side or a tie.
        Nothing,   ///< No value ticked, "(Blanks)" not ticked: no row passes, and the menu does not apply.
        Disabled,  ///< Both sides over the bound: the list is disabled, and the user narrows with a condition.
    };
    /// @brief The side.
    Side side = Side::All;
    /// @brief How many values the side names.
    std::size_t members = 0;
    /// @brief The side names values no operation reached, so the menu pages every value to name them.
    bool needsComplement = false;
};

/// @brief Decides which side of a values list to send.
///
/// With D = @p distinct, C = `ticks.covered`, T the ticked values (every value outside C when `ticks.ticked`, plus
/// those C marks ticked) and U = D − T: `All` when T = D and "(Blanks)" is not unticked; `Nothing` when T = 0 and
/// "(Blanks)" is not ticked; otherwise `Include` when T < U, else `Exclude`, so a tie sends the unticked side.
/// @param distinct     The column's distinct values, blanks not counted.
/// @param ticks        The resolved ticks.
/// @param maxSetValues The bound on one `oneOf` entry (`QueryLimits::maxSetValues`).
/// @return The side, with `members = min(T, U)`; `Disabled` when that exceeds @p maxSetValues.
[[nodiscard]] MORPH_API ValueFilterPlan planValueFilter(std::int64_t distinct, ResolvedTicks const& ticks,
                                                        std::size_t maxSetValues);

/// @brief How a values list's members are written.
enum class ValueForm : std::uint8_t {
    Set,     ///< One `oneOf` entry.
    Ranges,  ///< One `between` entry a member: date buckets, whose operand is `a..b`.
};

/// @brief Writes a values list's choice as a column filter.
///
/// `Include` is `include = [oneOf members] + [isEmpty]` when "(Blanks)" is ticked, `Exclude` is
/// `exclude = [oneOf members] + [isEmpty]` when it is unticked, both under `combine: any`. The `oneOf` entry is left
/// out when there are no members, so ticking only "(Blanks)" is `include = [isEmpty]`. `Ranges` writes one
/// `between` a member in place of the `oneOf`.
/// @param side    The side `planValueFilter` chose.
/// @param members The values on that side, as operands.
/// @param blanks  "(Blanks)", as `ValueTicks::blanks`.
/// @param form    How the members are written.
/// @return The filter; `nullopt` for `All`, `Nothing` and `Disabled`, and for a side left with no entry.
[[nodiscard]] MORPH_API std::optional<table::ColumnFilter> valueFilter(ValueFilterPlan::Side side,
                                                                       std::vector<std::string> members,
                                                                       std::optional<bool> blanks,
                                                                       ValueForm form = ValueForm::Set);

/// @brief Which part of a column's filter menu a filter belongs to. A column has one or the other, never both:
///        one `ColumnFilter` has one `combine`.
enum class FilterPart : std::uint8_t {
    None,        ///< No filter.
    Values,      ///< A values list's shape.
    Conditions,  ///< Anything else.
};

namespace detail {

/// @brief How many operators an entry sets.
/// @param entry The entry.
/// @return The count; `caseSensitive` is a modifier and is not counted.
[[nodiscard]] MORPH_API std::size_t operatorCount(table::FilterEntry const& entry);

/// @brief Whether entries are one side of a values list.
/// @param entries The side's entries.
/// @param kind    The column's kind.
/// @return `true` for `{oneOf}`, `{oneOf, isEmpty}`, a date column's `between` buckets with or without `isEmpty`, or
///         a lone `isEmpty` on a column that has a values list (not `Custom`).
[[nodiscard]] MORPH_API bool valuesShape(std::span<table::FilterEntry const> entries, table::ColumnKind kind);

}  // namespace detail

/// @brief Tells whether a column's filter is a values list's or a set of conditions.
/// @param filter The column's filter.
/// @param kind   The column's kind; a `Custom` column has no values list, so its lone `isEmpty` is a condition.
/// @return `None` with no entry; `Values` under `combine: any` with one side empty and the other a values list's
///         shape (`{oneOf}`, `{oneOf, isEmpty}`, a date column's `between` buckets with or without `isEmpty`, or a
///         lone `isEmpty`); `Conditions` otherwise.
[[nodiscard]] MORPH_API FilterPart classifyFilter(table::ColumnFilter const& filter, table::ColumnKind kind);

// Every switch over `ColumnKind` here names each kind and has no `default`, so a kind added to the enum fails to
// compile until it is handled: clang and gcc warn through -Wswitch, and MSVC, whose C4062 is off by default, is told
// to treat it as an error.
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(push)
#pragma warning(error : 4062)
#endif

/// @brief The wire form of an operand the user typed into a condition.
///
/// A number in the locale's form becomes a decimal with a point and no grouping (`"1.234,5"` under a comma locale
/// is `"1234.5"`), read with the locale's own separators; an `Integer` or `Key` operand has no fraction. A `Measure`
/// operand is read by `parseMeasure` with the column's units and written by `measureOperand` (`"0,3 kg"` under a
/// comma locale is `"0.3 kg"`). A date is the picker's ISO text and, as text and every other kind, passes unchanged.
/// @param typed      What the user typed.
/// @param kind       The column's kind.
/// @param locale     The locale the user types in.
/// @param units      A `Measure` column's units: the table's description's.
/// @param chosenUnit A `Measure` operand's unit when the user typed none: the one the menu shows as chosen.
/// @return The operand, or `nullopt` when it does not read as the kind.
[[nodiscard]] MORPH_API std::optional<std::string> canonicalOperand(std::string_view typed, table::ColumnKind kind,
                                                                    LocaleFacts const& locale,
                                                                    std::span<table::UnitInfo const> units = {},
                                                                    std::string_view chosenUnit = {});

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#endif

/// @brief How a column group's ticked values must be found among its columns.
enum class GroupMatch : std::uint8_t {
    AnyColumn,    ///< "in any column".
    EveryColumn,  ///< "in every column".
    AtLeast,      ///< "in at least N columns".
    Exactly,      ///< "in exactly N columns".
    Together,     ///< "all of them, each in a column".
};

/// @brief Writes a column group's values list as a group filter.
/// @param group        The group's id.
/// @param groupColumns How many columns the group has.
/// @param ticked       The ticked values, as operands.
/// @param blanks       "(Blanks)"; ticked, it adds an `isEmpty` entry under `AnyColumn` only.
/// @param match        How the values must be found.
/// @param n            N, for `AtLeast` (1 to @p groupColumns) and `Exactly` (0 to @p groupColumns).
/// @return One `oneOf` entry with the quantifier `match` names; for `Together`, one `eq` a value in `together` mode,
///         refused unless 2 to @p groupColumns values are ticked. Refused, with why, when nothing is ticked or N is
///         out of range.
[[nodiscard]] MORPH_API std::expected<table::GroupFilter, std::string> groupValueFilter(
    std::string group, std::size_t groupColumns, std::vector<std::string> ticked, std::optional<bool> blanks,
    GroupMatch match, std::int64_t n);

// ---------------------------------------------------------------------------------------------------------------
// Conditions
// ---------------------------------------------------------------------------------------------------------------

/// @brief One condition of a column's filter menu.
enum class ConditionOp : std::uint8_t {
    Equals,          ///< `eq`.
    NotEquals,       ///< `ne`.
    Contains,        ///< `contains`.
    BeginsWith,      ///< `startsWith`.
    EndsWith,        ///< `endsWith`.
    Less,            ///< `lt`.
    LessOrEqual,     ///< `le`.
    Greater,         ///< `gt`.
    GreaterOrEqual,  ///< `ge`.
    Between,         ///< `between`.
    IsEmpty,         ///< `isEmpty`.
    IsNotEmpty,      ///< `notEmpty`.
};

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(push)
#pragma warning(error : 4062)
#endif

/// @brief The conditions a column's kind offers.
/// @param kind The column's kind.
/// @return Text: equals, does not equal, contains, begins with, ends with. Numbers, measures and dates: =, ≠, <, ≤,
///         >, ≥, between. `Bool`: none (the values list alone). `Custom`: is empty, is not empty.
[[nodiscard]] MORPH_API std::vector<ConditionOp> conditionOps(table::ColumnKind kind);

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#endif

/// @brief A condition's label in the menu.
/// @param op   The condition.
/// @param kind The column's kind: a text column's conditions are words, a number's and a date's symbols.
/// @return The label, such as `does not equal` or `≠`.
[[nodiscard]] MORPH_API std::string_view conditionLabel(ConditionOp op, table::ColumnKind kind);

/// @brief Writes a condition as a filter entry.
/// @param op        The condition.
/// @param operand   Its operand, in wire form (`canonicalOperand`); unused by `IsEmpty` and `IsNotEmpty`.
/// @param second    `Between`'s upper end; unused otherwise.
/// @param matchCase "match case": sets `caseSensitive` on a text condition.
/// @return The entry, with exactly one operator.
[[nodiscard]] MORPH_API table::FilterEntry conditionEntry(ConditionOp op, std::string operand, std::string second,
                                                          bool matchCase);

// ---------------------------------------------------------------------------------------------------------------
// Menus
// ---------------------------------------------------------------------------------------------------------------

/// @brief What a table menu's entry does when chosen.
enum class MenuCommand : std::uint8_t {
    SortAscending,    ///< "Sort ascending".
    SortDescending,   ///< "Sort descending".
    AddToSort,        ///< "Add to sort".
    RemoveFromSort,   ///< "Remove from sort".
    Filter,           ///< "Filter…": the column's filter menu.
    FilterGroup,      ///< "Filter {group}, {k} columns…": the group's filter menu; the argument is the group's id.
    MoveLeft,         ///< "Move left".
    MoveRight,        ///< "Move right".
    PinColumn,        ///< "Pin column".
    UnpinColumn,      ///< "Unpin".
    HideColumn,       ///< "Hide column".
    DisplayUnit,      ///< "Display unit": the argument is the dimension and the unit, joined by U+001F.
    Columns,          ///< "Columns…": the column chooser.
    SortDialog,       ///< "Sort…": the sort dialog.
    ClearAllFilters,  ///< "Clear all filters".
    ResetColumns,     ///< "Reset columns".
    Copy,             ///< "Copy".
    CopyWithHeaders,  ///< "Copy with headers".
    ApplyView,        ///< One of the user's views in "Views": the argument is the view's name.
    SaveView,         ///< "Save view…": the name dialog; a name the user already has replaces that view.
    RenameView,       ///< "Rename view…": the name dialog, for the view applied.
    RemoveView,       ///< "Remove view": the view applied.
    SetDefaultView,   ///< "Set as default": the view applied becomes the default.
    DefaultOnMount,   ///< "Start from the default view", checkable.
    ApplySharedView,  ///< One of the shared views: the argument is the view's name.
    PublishView,      ///< "Publish view…": the arrangement published to the shared views.
    SaveAsMyView,     ///< A shared view saved among the user's own: the argument is the view's name.
    None,             ///< Nothing: an entry that opens a submenu, or a heading.
};

/// @brief One entry of a table's menu: what it shows and what it does.
// NOLINTNEXTLINE(misc-no-recursion): a submenu is a vector of entries, so copying an entry copies its submenu.
struct TableMenuEntry {
    /// @brief What a backend shows; its own `items` stay empty, and `menuEntries` fills them from this entry's.
    MenuEntry entry;
    /// @brief What choosing it does; not read on a separator or an entry with a submenu.
    MenuCommand command = MenuCommand::SortAscending;
    /// @brief The command's argument: a group's id for `FilterGroup`, the dimension and the unit joined by
    ///        U+001F for a display unit, else empty.
    std::string argument;
    /// @brief The submenu.
    std::vector<TableMenuEntry> items;
};

/// @brief A column group as a menu names it.
struct MenuGroup {
    /// @brief The group's id.
    std::string id;
    /// @brief The group's label.
    std::string label;
    /// @brief How many columns it has.
    std::size_t columns = 0;
};

/// @brief One dimension a column's values have, and the units it can show them in.
struct MenuDimension {
    /// @brief The dimension, such as `mass`.
    std::string dimension;
    /// @brief Unit ids, in the description's order.
    std::vector<std::string> units;
    /// @brief The layout's display unit for the dimension; empty when the cells show their own.
    std::string chosen;
};

/// @brief What a column's header menu is built from.
struct HeaderMenuInput {
    /// @brief The column.
    LayoutColumn column;
    /// @brief Whether it can be sorted by.
    bool sortable = false;
    /// @brief Whether it can be filtered.
    bool filterable = false;
    /// @brief Its place in the sort chain; `nullopt` when the chain does not hold it.
    std::optional<std::size_t> sortPlace;
    /// @brief The chain's length.
    std::size_t chainSize = 0;
    /// @brief The table's bound on the chain.
    std::size_t maxSortKeys = 8;
    /// @brief Its index among the visible columns.
    std::size_t visibleIndex = 0;
    /// @brief How many columns are visible.
    std::size_t visibleCount = 0;
    /// @brief How many of the first visible columns are pinned.
    std::size_t pinned = 0;
    /// @brief The dimensions its values have; non-empty adds "Display unit".
    std::vector<MenuDimension> dimensions;
    /// @brief The groups it is in.
    std::vector<MenuGroup> groups;
    /// @brief What the table lets its user change.
    Configure allowed = Configure::All;
};

/// @brief What a table's "Views" submenu is built from.
struct ViewsMenuInput {
    /// @brief The names of the user's own views, in their document's order.
    std::vector<std::string> own;
    /// @brief The names of the shared views, in their document's order; empty when the store does not share.
    std::vector<std::string> shared;
    /// @brief The view the arrangement was taken from: the state's `view`; none when it names none.
    std::optional<std::string> current;
    /// @brief Whether `current` is a shared view, applied from "Shared views", rather than one of the user's own.
    bool currentShared = false;
    /// @brief Whether the arrangement differs from `current` as fitted to the table.
    bool modified = false;
    /// @brief Whether the default view's sort and filters apply at every mount.
    bool defaultOnMount = false;
    /// @brief Whether the store keeps the shared views for everyone, which offers "Publish view…".
    bool shares = false;
};

/// @brief What a table's own menu is built from.
struct TableMenuInput {
    /// @brief Whether any column or group has a filter.
    bool anyFilter = false;
    /// @brief The table's groups, declared and user-formed.
    std::vector<MenuGroup> groups;
    /// @brief What the table lets its user change.
    Configure allowed = Configure::All;
    /// @brief The saved views; none for a table that keeps no state, which has no "Views".
    std::optional<ViewsMenuInput> views;
};

namespace detail {

/// @brief A menu entry that runs a command.
/// @param label    The caption.
/// @param command  The command.
/// @param enabled  Whether it can be chosen.
/// @param keys     The chord shown beside it; empty for none.
/// @param argument The command's argument.
/// @return The entry, with no submenu.
[[nodiscard]] MORPH_API TableMenuEntry command(std::string label, MenuCommand command, bool enabled = true,
                                               std::string keys = {}, std::string argument = {});

/// @brief Joins menu sections.
/// @param sections The sections; an empty one is left out.
/// @return Their entries, with a separator between each two.
[[nodiscard]] MORPH_API std::vector<TableMenuEntry> joinSections(std::vector<std::vector<TableMenuEntry>> sections);

/// @brief A group's filter entry's caption.
/// @param group The group.
/// @return `Filter {label}, {k} columns…`.
[[nodiscard]] inline std::string groupLabel(MenuGroup const& group) {
    return "Filter " + group.label + ", " + std::to_string(group.columns) + " columns…";
}

/// @brief A heading line: a label that is not a command.
/// @param label The caption.
/// @return The entry, which no backend lets a user choose.
[[nodiscard]] MORPH_API TableMenuEntry heading(std::string label);

/// @brief A view's entry in "Views": checked while it is the view the arrangement was taken from, and labelled
///        `{name} (modified)` while the arrangement differs from it.
/// @param name     The view's name.
/// @param command  `ApplyView` or `ApplySharedView`.
/// @param current  Whether it is the view the arrangement was taken from.
/// @param modified Whether the arrangement differs from it.
/// @return The entry; its argument is the name.
[[nodiscard]] MORPH_API TableMenuEntry viewEntry(std::string const& name, MenuCommand command, bool current,
                                                 bool modified);

}  // namespace detail

/// @brief A table's "Views" submenu.
///
/// In order, each part with a separator between: the user's own views, one checkable entry a view, checked while
/// the arrangement was taken from it and labelled `{name} (modified)` while it differs from it; "Save view…",
/// "Rename view…", "Remove view" and "Set as default", the last three enabled while the arrangement was taken from
/// one of the user's own views, then "Start from the default view", checkable; while there are shared views, the
/// heading "Shared views" and one entry a shared view, marked as an own view is; then "Save as my view" while the
/// arrangement was taken from a shared view, and "Publish view…" while the store shares.
/// @param input What the submenu is built from.
/// @return The "Views" entry, its submenu in `items`.
[[nodiscard]] MORPH_API TableMenuEntry viewsMenu(ViewsMenuInput const& input);

/// @brief A column's header menu.
///
/// In order, each part with a separator between: the sort entries (a sortable column); "Filter…" (a filterable
/// one) and one "Filter {group}, {k} columns…" a group the column is in; the arranging entries the table allows
/// ("Move left" and "Move right" for reorder, "Pin column" or "Unpin" for pin, "Hide column" for hide), disabled on
/// a locked column; and "Display unit", with a checkable entry `{dimension}: {unit}` a unit, when the column has
/// dimensions. "Add to sort" is disabled for a column in the chain and at the bound, "Remove from sort" for one
/// not in it. "Move left" and "Move right" stop at the pin boundary as at the ends. The lowering turns a choice of
/// the checked unit into `SetDisplayUnit` with no unit, which clears it.
/// @param input What the menu is built from.
/// @return The entries.
[[nodiscard]] MORPH_API std::vector<TableMenuEntry> headerMenu(HeaderMenuInput const& input);

/// @brief A table's own menu.
///
/// "Columns…" (while the table allows any arranging), "Sort…", one "Filter {group}, {k} columns…" a group,
/// "Clear all filters" (disabled with no filter), "Reset columns" (while any arranging is allowed); then "Views"
/// (`viewsMenu`) on a table that keeps a state; then "Copy" (`Ctrl+C`) and "Copy with headers".
/// @param input What the menu is built from.
/// @return The entries.
[[nodiscard]] MORPH_API std::vector<TableMenuEntry> tableMenu(TableMenuInput const& input);

/// @brief What a backend shows for a table's menu.
/// @param entries The menu.
/// @return Each entry's `MenuEntry`, its submenu filled from the entry's `items`, in the same order, so a path of
///         indices names the same entry in both.
// NOLINTNEXTLINE(misc-no-recursion): one call per submenu level, bounded by the menu's depth.
[[nodiscard]] MORPH_API std::vector<MenuEntry> menuEntries(std::span<TableMenuEntry const> entries);

/// @brief The entry a backend reports chosen.
/// @param entries The menu.
/// @param path    The indices, from the top menu down through each submenu.
/// @return The entry; null when the path leaves the menu or names a separator, a heading, a disabled entry or an
///         entry with a submenu.
[[nodiscard]] MORPH_API TableMenuEntry const* chosen(std::span<TableMenuEntry const> entries,
                                                     std::span<std::size_t const> path);

// ---------------------------------------------------------------------------------------------------------------
// Copy
// ---------------------------------------------------------------------------------------------------------------

/// @brief The text a copy puts on the clipboard.
struct CopiedText {
    /// @brief One line a row, its cells separated by tabs.
    std::string text;
    /// @brief The cells copied as empty because they were not loaded.
    std::size_t unloaded = 0;
};

namespace detail {

/// @brief Appends a cell's text as one clipboard field: each tab, CR LF, lone CR and lone LF becomes one space.
/// @param out  The text to append to.
/// @param text The cell's text.
MORPH_API void appendField(std::string& out, std::string_view text);

}  // namespace detail

/// @brief Writes rows as clipboard text.
/// @param labels The columns' labels, a first line; empty for no header line.
/// @param rows   The rows' cell texts, in view order, by the visible columns; `nullopt` for an unloaded cell.
/// @return The text, lines separated by `\n` with none after the last; an unloaded cell is empty and counted.
[[nodiscard]] MORPH_API CopiedText copyText(std::span<std::string const> labels,
                                            std::span<std::vector<std::optional<std::string>> const> rows);

}  // namespace morph::ui
