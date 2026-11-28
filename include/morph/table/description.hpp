// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file table/description.hpp
/// @brief A table's description and the generic page: a table a client draws
///        and reads with no compile-time knowledge of its rows.
///
/// A `TableDescription` says everything a client needs to draw a table: its
/// columns, their labels and groups, which column is the key. `describe` stamps
/// it with a revision, a hash of the rest, so two descriptions are equal
/// exactly when their revisions are.
///
/// A `GenericPage` is a page of such a table: row arrays under one header of
/// column ids. Each cell is written by its `Cell` alternative and read back by
/// its JSON shape and its column's kind, so a cell the server keyed as invalid
/// is invalid on the client too.
///
/// A model serves a table by a function returning a `ServedTable`, registered
/// with `BRIDGE_REGISTER_TABLE`; the registration makes the table's actions
/// (`QueryTable`, `DescribeTable`) and its record (`TableOf`).
///
/// See `docs/spec/table/table.md`, "A table is a description", "A model's tables" and "Rows on the wire".

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <charconv>
#include <cmath>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <morph/core/action_refusal.hpp>
#include <morph/core/effects.hpp>
#include <morph/export.hpp>
#include <morph/table/bands.hpp>
#include <morph/table/level_cache.hpp>
#include <morph/table/runtime.hpp>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <typeindex>
#include <utility>
#include <variant>
#include <vector>

namespace morph::table {

// NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) -- cell and column indices are bounded by the page's own counts, checked before they are used

/// @brief How a column rounds a value it shows at fewer places than the value has: the expression language's two
///        modes, so a value a document rounds and the same value a column shows agree. JSON `"halfEven"`, `"halfUp"`.
enum class Rounding : std::uint8_t {
    HalfEven,  ///< A half rounds to the even last kept digit: 0.125 at 2 places is 0.12.
    HalfUp,    ///< A half rounds away from zero: 0.125 at 2 places is 0.13, -0.125 is -0.13.
};

}  // namespace morph::table

/// @brief JSON names of `morph::table::Rounding`: `halfEven`, `halfUp`.
template <>
struct glz::meta<morph::table::Rounding> {
    using enum morph::table::Rounding;
    /// @brief Each enumerator after its JSON name.
    static constexpr auto value = glz::enumerate("halfEven", HalfEven, "halfUp", HalfUp);
};

namespace morph::table {

/// @brief A text a column shows for one particular value: a code's word, a flag in words.
struct ValueLabel {
    /// @brief The value as a filter entry takes it: `"true"`, `"3"`, `"V"`.
    std::string operand;
    /// @brief What the cell shows for it.
    std::string text;

    /// @brief Member-wise equality.
    /// @param other The label to compare with.
    /// @return `true` when both members match.
    [[nodiscard]] bool operator==(ValueLabel const& other) const = default;
};

/// @brief How a column turns its cells into text on the client. It changes what a cell shows and nothing a sort, a
///        filter, a distinct value or a selection uses.
struct ColumnDisplay {
    /// @brief Places shown for an `Integer`, `Decimal`, `Quantity` or `Number` cell; absent shows the value's own
    ///        places (a `Rational`'s declared places, a `double`'s shortest round trip).
    std::optional<std::uint32_t> decimals;
    /// @brief How a value with more places than `decimals` is rounded for showing.
    Rounding rounding = Rounding::HalfEven;
    /// @brief With `decimals`, whether a value of fewer places is padded with zeros: 2 places shows 1.5 as "1.50".
    bool padPlaces = true;
    /// @brief Whether digits are grouped by the locale: 12,500.
    bool grouping = false;
    /// @brief What an empty cell shows: "-", "n/a". Empty shows nothing.
    std::string empty;
    /// @brief The unit of a plain numeric column, shown in its header; a `Measure` column's units are its cells'.
    std::string unit;
    /// @brief A `Date` or `DateTime` column's pattern; empty takes the session's.
    std::string date;
    /// @brief Text for particular values; a value no entry names shows as `decimals` says.
    std::vector<ValueLabel> labels;

    /// @brief Member-wise equality.
    /// @param other The display to compare with.
    /// @return `true` when every member matches.
    [[nodiscard]] bool operator==(ColumnDisplay const& other) const = default;
};

/// @brief The action that edits a column's cells, on a model's table (`docs/spec/table/editing.md`, "Cells of a
/// model's
///        table").
struct EditInfo {
    /// @brief The action's id: it takes a `CellEdit` or a `RowEdit` of the column's table.
    std::string action;
    /// @brief For a `RowEdit`, the member of its `values` the column edits; empty for a `CellEdit`.
    std::string member;
    /// @brief The id of a companion column whose `true` makes the row's cell read-only; empty for none.
    std::string readonly;

    /// @brief Member-wise equality.
    /// @param other The edit to compare with.
    /// @return `true` when every member matches.
    [[nodiscard]] bool operator==(EditInfo const& other) const = default;
};

/// @brief One column of a table's description.
struct TableColumn {
    /// @brief The column's id and kind, as the engine reads them.
    ColumnInfo info;
    /// @brief The header's text.
    std::string label;
    /// @brief The id of the declared column group the column belongs to, or empty.
    std::string group;
    /// @brief Whether the user may sort by the column.
    bool sortable = true;
    /// @brief Whether the user may filter the column.
    bool filterable = true;
    /// @brief Whether the column is shown by default.
    bool shown = true;
    /// @brief How the column's cells are shown.
    ColumnDisplay display;
    /// @brief The id of the `Text` column whose cell names this column's cell's tone (`neutral`, `info`, `ok`,
    ///        `warn`, `err`); empty for none.
    std::string tone;
    /// @brief The id of the `Text` column whose cell is this column's cell's tooltip; empty for none.
    std::string tooltip;
    /// @brief The action that edits the column's cells on a model's table; none leaves the column read-only.
    std::optional<EditInfo> edit;

    /// @brief Member-wise equality.
    /// @param other The column to compare with.
    /// @return `true` when every member matches.
    [[nodiscard]] bool operator==(TableColumn const& other) const = default;
};

}  // namespace morph::table

/// @brief The JSON of `morph::table::TableColumn`: every member by its name, `display`, `tone`, `tooltip` and `edit`
/// left
///        out while they hold their defaults, so a column that shows its cells plainly costs nothing for them. A
///        reader restores a member left out to its default.
template <>
struct glz::meta<morph::table::TableColumn> {
    /// @brief The column type.
    using T = morph::table::TableColumn;
    /// @brief Whether a member is left out.
    /// @tparam V The member's type.
    /// @param member  The member.
    /// @param key     Its JSON name.
    /// @param context Unused.
    /// @return `true` for a default `display`, an empty `tone` or `tooltip`, and no `edit`.
    template <typename V>
    // NOLINTNEXTLINE(readability-identifier-naming): glaze looks this hook up by name.
    static constexpr bool skip_if(V const& member, std::string_view key, glz::meta_context const& context) {
        static_cast<void>(context);
        if constexpr (std::is_same_v<V, morph::table::ColumnDisplay>) {
            return key == "display" && member == morph::table::ColumnDisplay{};
        } else if constexpr (std::is_same_v<V, std::string>) {
            return (key == "tone" || key == "tooltip") && member.empty();
        } else if constexpr (std::is_same_v<V, std::optional<morph::table::EditInfo>>) {
            return !member.has_value();
        } else {
            return false;
        }
    }
    /// @brief Each member after its JSON name.
    static constexpr auto value =
        glz::object("info", &T::info, "label", &T::label, "group", &T::group, "sortable", &T::sortable, "filterable",
                    &T::filterable, "shown", &T::shown, "display", &T::display, "tone", &T::tone, "tooltip",
                    &T::tooltip, "edit", &T::edit);
};

// The JSON above names each member: a member added to `TableColumn` and not listed there would never reach the wire.
static_assert(glz::detail::count_members<morph::table::TableColumn> == 10,
              "a TableColumn member is missing from glz::meta<morph::table::TableColumn>: list it there, then update "
              "this count");

namespace morph::table {

/// @brief What the server offers on a table beyond paging, sorting and filtering.
struct TableCapabilities {
    /// @brief Keyed queries.
    bool keys = true;
    /// @brief Distinct values of a column.
    bool distinct = true;
    /// @brief Sort keys a query may hold.
    std::uint32_t maxSortKeys = 8;

    /// @brief Member-wise equality.
    /// @param other The capabilities to compare with.
    /// @return `true` when every member matches.
    [[nodiscard]] bool operator==(TableCapabilities const& other) const = default;
};

/// @brief Everything a client needs to draw a table, with no compile-time
///        knowledge of its rows.
struct TableDescription {
    /// @brief The table's id, unique in the process across every model.
    std::string id;
    /// @brief The table's text.
    std::string label;
    /// @brief The columns, in the declared order.
    std::vector<TableColumn> columns;
    /// @brief The id of the column that is the row's key.
    std::string key;
    /// @brief The declared column groups, as a client draws them: the source's `groups()`, which the server
    ///        evaluates.
    std::vector<ColumnGroupInfo> groups;
    /// @brief The header bands over the columns, outermost first (`checkBands` says what a band may be).
    std::vector<BandInfo> bands;
    /// @brief Every data level a column names, the base level excluded.
    std::vector<std::string> levels;
    /// @brief The units of the dimensions the table's `Measure` columns allow (`unitsFor`): the only place a client
    ///        learns units from, to parse, convert and show a measure.
    std::vector<UnitInfo> units;
    /// @brief What the server offers on the table.
    TableCapabilities capabilities;
    /// @brief The id of the `Text` column whose cell names a row's tone; empty for none.
    std::string rowTone;
    /// @brief The id of the `Text` column whose cell, when it holds text, locks the row and is its tooltip; empty
    ///        for none.
    std::string rowLock;
    /// @brief A hash of every other member, set by `describe`; a model leaves it empty.
    std::string revision;

    /// @brief Member-wise equality.
    /// @param other The description to compare with.
    /// @return `true` when every member matches.
    [[nodiscard]] bool operator==(TableDescription const& other) const = default;
};

/// @brief Stamps @p description with its revision.
///
/// The revision is the 64-bit FNV-1a hash of the description's JSON as glaze
/// writes it, members in declaration order, with the revision itself empty: so
/// describing a described description gives the same revision, and any change
/// of a member a client reads gives another.
/// @param description The description; its `revision` is ignored.
/// @return The description with `revision` set to 16 lowercase hex digits.
[[nodiscard]] MORPH_API TableDescription describe(TableDescription description);

/// @brief The columns of @p description as the engine reads them.
/// @param description The description.
/// @return Each column's `info`, in the description's order.
[[nodiscard]] MORPH_API std::vector<ColumnInfo> columnInfos(TableDescription const& description);

/// @brief A registry of the units @p description lists: how a client resolves the unit ids of its measures.
///
/// The ratio's unit `""` is the builder's own and is skipped when listed; the numbers are this registry's, not the
/// server's, since the wire names a unit by its id.
/// @param description The description.
/// @return The registry, or what `UnitRegistryBuilder::add` refuses for a listed unit.
[[nodiscard]] MORPH_API std::expected<std::shared_ptr<UnitRegistry const>, TableError> registryOf(
    TableDescription const& description);

/// @brief Refuses a column display or a companion that cannot apply to @p description.
///
/// `decimals` or `unit` on a `Measure` column is refused, naming that column: a measure shows in its own unit, to
/// that unit's decimals. A `tone`, `tooltip`, `rowTone` or `rowLock` that names no column, or a column that is not
/// `Text`, is refused, naming the id it gives.
/// @param description The description.
/// @return Nothing, or `SourceContract` naming the offending column.
[[nodiscard]] MORPH_API std::expected<void, TableError> validateDisplay(TableDescription const& description);

/// @brief Refuses header bands a table cannot draw.
///
/// A band nested past `kMaxBandLevels` levels, a band naming no column, a band naming a column id the table does not
/// have, a child band over a column its parent lacks, and a column in two bands of one level are each refused.
/// @param bands   The bands, outermost first.
/// @param columns The table's columns.
/// @return Nothing, or `SourceContract` whose message names the band's label.
[[nodiscard]] MORPH_API std::expected<void, TableError> checkBands(std::span<BandInfo const> bands,
                                                                   std::span<TableColumn const> columns);

/// @brief Which companions a document overrides with its own expressions.
struct CompanionOverrides {
    /// @brief The column ids whose document part gives `tone`.
    std::set<std::string, std::less<>> tone;
    /// @brief The column ids whose document part gives `tooltip`.
    std::set<std::string, std::less<>> tooltip;
    /// @brief Whether the node gives `rowTone`.
    bool rowTone = false;
};

/// @brief The columns a query fetches for @p columns: each with its companions, then the row's.
///
/// For each id of @p columns in order: the id, then its `tone` companion unless @p overrides gives that column's
/// tone, then its `tooltip` companion likewise; then `rowTone` unless overridden; then `rowLock`, which travels with
/// every row since a locked row cannot be drawn without it. Empty names are skipped, and an id already listed is not
/// listed again.
/// @param description The description.
/// @param columns     The column ids, in the order the query names them.
/// @param overrides   What the document gives itself.
/// @return The ids.
[[nodiscard]] MORPH_API std::vector<std::string> columnsWithCompanions(TableDescription const& description,
                                                                       std::span<std::string const> columns,
                                                                       CompanionOverrides const& overrides = {});

/// @brief Every column @p description names as a companion: columns no user arranges.
/// @param description The description.
/// @return Every non-empty `tone`, `tooltip`, `rowTone` and `rowLock` name, sorted, without duplicates.
[[nodiscard]] MORPH_API std::vector<std::string> companionColumns(TableDescription const& description);

/// @brief One row of a generic page: one cell per entry of the page's `columns`.
using GenericRow = std::vector<Cell>;

/// @brief A page of a table read through its description: row arrays under one
///        header of column ids.
///
/// Its JSON members, in order: `revision`, `columns`, `keys`, `rows`, `total`
/// (absent when not counted), `totalKind`, `offset`, `unloadedRows`,
/// `undecided`, `awaiting`, `failed`, `indices` and `distinct`. An empty
/// `failed` or `indices` is left out, as is a row's empty `failed` in
/// `unloadedRows`, and `distinct` is written only on a distinct-values reply;
/// the reader takes each member from `unloadedRows` on as present or absent, so
/// a page without them is a fully loaded page that is not a keyed reply.
struct GenericPage {
    /// @brief The revision of the description the page was written against.
    std::string revision;
    /// @brief The columns of each row, in the description's order.
    std::vector<std::string> columns;
    /// @brief Each row's key, whether or not the key column is among `columns`.
    std::vector<RowId> keys;
    /// @brief The rows, in view order.
    std::vector<GenericRow> rows;
    /// @brief Rows in the whole filtered view, or a lower bound (see `totalKind`);
    ///        absent when the server did not count them.
    std::optional<std::int64_t> total;
    /// @brief Whether `total` is the count or a lower bound.
    TotalKind totalKind = TotalKind::Exact;
    /// @brief The view row of `rows[0]`.
    std::int64_t offset = 0;
    /// @brief The rows of this page that have unloaded levels, ascending by index. A row not listed is fully
    ///        loaded; a cell at a listed level is `null` on the wire.
    std::vector<UnloadedRow> unloadedRows;
    /// @brief Rows the filter can neither admit nor reject until a level loads. They are in no page.
    std::int64_t undecided = 0;
    /// @brief The order levels with a load wanted or in flight for at least one row.
    std::vector<std::string> awaiting;
    /// @brief The order levels whose load failed for at least one row, and is not being tried again yet.
    std::vector<std::string> failed;
    /// @brief A keyed reply only: the view row of each of `rows`.
    std::vector<std::int64_t> indices;
    /// @brief A distinct-values reply only: the values; the page then has no rows.
    std::optional<DistinctValues> distinct;
    /// @brief The indices, into `rows`, of the retained rows (`TableQuery::retain`) the filter rejects, ascending.
    ///        Left out of the JSON when empty.
    std::vector<std::int64_t> unmatched;
    /// @brief The view's membership (`MembershipDigest::text`): two pages with the same one are of views of the same
    ///        rows, whatever their sort. Empty on a distinct-values reply, and then left out of the JSON.
    std::string viewRevision;
    /// @brief The id of each unit number the page's `Measure` cells hold, by which they are written: filled by
    ///        `writeGenericPage` and `readGenericPage`. Not a member of the JSON, where a measure names its unit's id.
    std::map<UnitId, std::string> units;
};

namespace detail {

/// @brief `10^k` for `k` in `[0, kMaxDecimalPlaces]`.
/// @param k The exponent, at most `math::kMaxDecimalPlaces`.
/// @return The power.
[[nodiscard]] constexpr std::int64_t powerOfTen(std::uint32_t k) noexcept {
    std::int64_t out = 1;
    for (std::uint32_t i = 0; i < k; ++i) {
        out *= 10;
    }
    return out;
}

/// @brief Appends an integer as JSON.
/// @param out   The text to append to.
/// @param value The integer.
MORPH_API void appendInteger(std::string& out, std::int64_t value);

/// @brief Appends a string as a JSON string, escaped as glaze escapes it.
/// @param out  The text to append to.
/// @param text The string.
MORPH_API void appendJsonString(std::string& out, std::string_view text);

/// @brief Appends a `Rational` as `[numerator, denominator]`.
///
/// With `p` its decimal places, the pair is scaled to `[n × 10^p / d, 10^p]`
/// when `10^p` is a multiple of the denominator and the scaled numerator fits
/// `int64`, so a reader takes the places back from the denominator; otherwise
/// it is the canonical pair.
/// @param out   The text to append to.
/// @param value The value.
MORPH_API void appendRational(std::string& out, math::Rational const& value);

/// @brief Appends a finite or non-finite `double`.
///
/// A finite value is written as glaze writes it, with `.0` added when that
/// text has no fraction or exponent, so it never reads back as an integer. A
/// non-finite one, which JSON cannot hold as a number, is the string `"nan"`,
/// `"inf"` or `"-inf"`.
/// @param out   The text to append to.
/// @param value The value.
MORPH_API void appendDouble(std::string& out, double value);

/// @brief Appends one cell as JSON, by its `Cell` alternative.
///
/// Empty and unloaded are `null`; an integer, a string and a boolean are themselves; a
/// `double` is written by `appendDouble`; a `Rational` is a pair by
/// `appendRational`; a `QuantityCell` is `[n, d, [fn, fd]]`, its amount and
/// factor each written as a `Rational`, or the plain pair of its amount when
/// the factor is 1; a `Measure` is `[n, d, "<unit id>"]`, its unit `null` when
/// @p units has no id for its number.
/// @param out   The text to append to.
/// @param cell  The cell.
/// @param units The id of each unit number a measure may hold.
MORPH_API void appendCellJson(std::string& out, Cell const& cell, std::map<UnitId, std::string> const& units = {});

/// @brief Reads JSON text token by token, for the shapes a cell has.
class MORPH_API CellCursor {
public:
    /// @brief A cursor at the start of @p text.
    /// @param text The JSON text.
    explicit CellCursor(std::string_view text) noexcept : _text{text} {}

    /// @brief Skips JSON whitespace.
    void skipSpace() noexcept;

    /// @brief Consumes @p character after any whitespace.
    /// @param character The character expected.
    /// @return `true` when it was next and is consumed.
    [[nodiscard]] bool eat(char character) noexcept;

    /// @brief Whether only whitespace is left.
    /// @return `true` at the end of the text.
    [[nodiscard]] bool atEnd() noexcept;

    /// @brief The text not consumed yet, after any whitespace.
    /// @return The rest of the text.
    [[nodiscard]] std::string_view rest() noexcept;

    /// @brief Consumes a JSON integer token that fits `int64`.
    /// @return The integer, or nothing (and nothing consumed) when the next
    ///         token is not one.
    [[nodiscard]] std::optional<std::int64_t> integer() noexcept;

    /// @brief The JSON number token at the start of @p text.
    /// @param text The text.
    /// @return The token: `-?digits(.digits)?([eE][+-]?digits)?`, or empty when
    ///         @p text does not start with one.
    [[nodiscard]] static std::string_view numberToken(std::string_view text) noexcept;

private:
    std::string_view _text;
    std::size_t _at = 0;
};

/// @brief The decimal places a denominator implies.
/// @param denominator A positive denominator.
/// @return `k` when it is `10^k` with `k ≤ kMaxDecimalPlaces`, otherwise
///         `kMaxDecimalPlaces`.
[[nodiscard]] constexpr std::uint32_t placesOf(std::int64_t denominator) noexcept {
    for (std::uint32_t k = 0; k <= math::kMaxDecimalPlaces; ++k) {
        if (powerOfTen(k) == denominator) {
            return k;
        }
    }
    return math::kMaxDecimalPlaces;
}

/// @brief Reads `n, d` of a pair whose `[` is consumed, and the `]` after it.
/// @param cursor The cursor.
/// @return The `Rational`, its places from the denominator, or nothing when
///         the pair is not two integers with a positive denominator.
[[nodiscard]] MORPH_API std::optional<math::Rational> readPair(CellCursor& cursor);

/// @brief Reads an exact cell: `[n, d]` or `[n, d, [fn, fd]]`.
/// @param text The JSON text, starting with `[`.
/// @return The `Rational` or `QuantityCell`, or nothing when the array has
///         another shape.
[[nodiscard]] MORPH_API std::optional<Cell> readExactCell(std::string_view text);

/// @brief Reads a measure's JSON: `[n, d, "<unit id>"]`, or `[n, d, null]` for a unit its writer had no id for.
/// @param text The JSON text, starting with `[`.
/// @return The amount and the unit's id (nothing for `null`), or nothing when the array has another shape.
[[nodiscard]] MORPH_API std::optional<std::pair<math::Rational, std::optional<std::string>>> readMeasureCell(
    std::string_view text);

/// @brief The `double` a `Number` column holds for the strings JSON writes
///        a non-finite value as.
/// @param text The decoded string.
/// @return NaN, +inf or -inf, or nothing for any other string.
[[nodiscard]] MORPH_API std::optional<double> nonFinite(std::string_view text) noexcept;

/// @brief Reads one cell from its JSON text, by the text's shape.
///
/// `null` is empty. A number token with no `.`, `e` or `E` is an `int64`,
/// unless @p kind is `Number` or it does not fit, when it is a `double`; any
/// other number is a `double` whatever the kind, so a `double` held in an
/// integer or exact column stays invalid there. A string is text, except
/// `"nan"`, `"inf"` and `"-inf"` in a `Number` column. `true` and `false` are
/// booleans. `[n, d]` with `d > 0` is a `Rational` whose places are `k` when
/// `d` is `10^k` and `kMaxDecimalPlaces` otherwise; `[n, d, [fn, fd]]` is a
/// `QuantityCell`. Anything else is the text of its JSON, which no kind but
/// `Text` reads as valid.
/// @param json The cell's JSON text.
/// @param kind The column's kind, or nothing when it is not known.
/// @return The cell.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): one sequential pass whose steps share its running state; splitting it would thread that state through parameters.
[[nodiscard]] MORPH_API Cell cellFromJson(std::string_view json, std::optional<ColumnKind> kind);

/// @brief Reads a page's rows with no column kinds; `readGenericPage` reads
///        them again with their columns' kinds.
/// @param page The page to fill.
/// @param rows Each row's cells as JSON text.
MORPH_API void readPageRows(GenericPage& page, std::vector<std::vector<glz::raw_json_view>> const& rows);

/// @brief Writes a page's rows, each cell by `appendCellJson`.
/// @param page The page.
/// @return The `rows` array as JSON.
[[nodiscard]] MORPH_API glz::raw_json writePageRows(GenericPage const& page);

}  // namespace detail

// NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)

}  // namespace morph::table

/// @brief The JSON of `morph::table::GenericPage`: its members in declaration
///        order, `rows` written and read cell by cell.
template <>
struct glz::meta<morph::table::GenericPage> {
    /// @brief The page type.
    using T = morph::table::GenericPage;
    /// @brief Whether a member is left out of the JSON written.
    /// @tparam V The member's type.
    /// @param member  The member.
    /// @param key     Its JSON name.
    /// @param context Unused.
    /// @return `true` for an empty `failed`, `indices`, `unmatched` or `viewRevision`.
    template <typename V>
    // NOLINTNEXTLINE(readability-identifier-naming): glaze looks this hook up by name.
    static constexpr bool skip_if(V const& member, std::string_view key, glz::meta_context const& context) {
        static_cast<void>(context);
        return morph::table::detail::omittedPageMember(member, key);
    }
    /// @brief Each member after its JSON name.
    static constexpr auto value = glz::object(
        "revision", &T::revision, "columns", &T::columns, "keys", &T::keys, "rows",
        glz::custom<[](T& page, std::vector<std::vector<glz::raw_json_view>> const& rows) {
            morph::table::detail::readPageRows(page, rows);
        },
                    [](T const& page) { return morph::table::detail::writePageRows(page); }>,
        "total", &T::total, "totalKind", &T::totalKind, "offset", &T::offset, "unloadedRows", &T::unloadedRows,
        "undecided", &T::undecided, "awaiting", &T::awaiting, "failed", &T::failed, "indices", &T::indices, "distinct",
        &T::distinct, "unmatched", &T::unmatched, "viewRevision", &T::viewRevision);
};

namespace morph::table {

// NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) -- cell and column indices are bounded by the page's own counts, checked before they are used

/// @brief Builds the generic page of @p page's rows.
///
/// Reads only the cells of @p columns, for the page's rows only, so a source
/// may produce every other cell lazily or not at all. A row is listed in
/// `unloadedRows` with the levels of the named columns whose cell it holds as
/// `Unloaded`, sorted; a level no named column has is not looked up. Every
/// member of @p page beyond its rows is copied but `distinct`: a distinct-values
/// reply is built by `detail::answerQuery`, not from a page of rows. A measure
/// is written with its unit's id from @p units; a unit @p units does not hold
/// is written `null`, which reads back as an invalid cell.
/// @param page    The page's source rows and the view's state, as `apply` returns them.
/// @param source  The rows; read through one snapshot.
/// @param columns The column ids to write, already ordered and without
///                duplicates (`validate` makes a query's list so). An id the
///                source lacks is written as empty cells.
/// @param units   The registry the source's measures are numbered in. Not null when a written column is `Measure`:
///                a precondition, which `detail::answerQuery` checks before it calls this.
/// @return The page, its `revision` left empty for the caller to set.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): one sequential pass whose steps share its running state; splitting it would thread that state through parameters.
[[nodiscard]] MORPH_API GenericPage writeGenericPage(PageRows const& page, DataSource const& source,
                                                     std::span<std::string const> columns,
                                                     UnitRegistry const* units = nullptr);

/// @brief Reads a generic page against its table's description.
///
/// Each cell is read by `detail::cellFromJson` with its column's kind. A cell
/// that does not read as its kind keeps the page and is an invalid cell; a
/// page whose structure is wrong is refused. A cell of a `Measure` column
/// written `[n, d, "<unit id>"]` is a `Measure` whose unit is the id's number
/// in @p units, or `kUnknownUnit`, an invalid cell, for an id it lacks.
/// @param json        The page's JSON.
/// @param description The table's description.
/// @param units       The registry a measure's unit id is resolved through. Null resolves through
///                    `registryOf(description)`, built once for the call and only when a column of the page is
///                    `Measure`: a client has no registry of its own, only the description's units.
/// @return The page, or `UnknownColumn` naming a `columns` entry the
///         description lacks, or `InvalidValue` when the text is not a page, a
///         row's length differs from `columns`, `keys` and `rows` differ in
///         length, an `unloadedRows` index is outside the rows or not above the
///         one before it, `indices` is neither empty nor one non-negative
///         view row per row, or what `registryOf` refuses.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): one sequential pass whose steps share its running state; splitting it would thread that state through parameters.
[[nodiscard]] MORPH_API std::expected<GenericPage, TableError> readGenericPage(std::string_view json,
                                                                               TableDescription const& description,
                                                                               UnitRegistry const* units = nullptr);

namespace detail {

/// @brief The cells of one page of a generic window, by column, so a column can be added or dropped without
///        touching the others.
struct MORPH_API GenericWindowCells {
    /// @brief Each held column's cells, one per row.
    std::map<std::string, std::vector<Cell>, std::less<>> cells;

    /// @brief Whether the page holds a column.
    /// @param column The column's id.
    /// @return `true` when held.
    [[nodiscard]] bool holds(std::string_view column) const { return cells.contains(column); }
    /// @brief The columns held, which a rest-of-page fetch names.
    /// @return The ids, sorted.
    [[nodiscard]] std::vector<std::string> columns() const;
    /// @brief Keeps the first @p count rows of every column.
    /// @param count Rows kept.
    void truncate(std::size_t count);
};

}  // namespace detail

/// @brief The window of every `table` node: generic pages, a row's key its entry of the page's `keys`, and the
///        columns of each page held by column.
///
/// The rules are `PageWindow<Row>`'s. In addition `visible` names the columns shown: a page held complete that
/// lacks some of them is fetched again for the same rows naming only those, and the reply's cells are added
/// when its keys are the page's, row for row. A page holds at most `maxColumns` columns; over that, the columns
/// least recently named by `visible` go first. A patch drops from a page the columns its reply lacks, so the
/// next `visible` fetches them again rather than showing a cell older than its row.
template <>
class MORPH_API PageWindow<GenericRow> : public detail::WindowCore<detail::GenericWindowCells> {
    using Core = detail::WindowCore<detail::GenericWindowCells>;

public:
    /// @brief An empty window.
    /// @param pageSize   Rows per page (default 200).
    /// @param maxPages   Pages held at most; the farthest from the visible rows go first.
    /// @param maxKeys    Keys one keyed fetch asks for at most; more refetch the pages instead.
    /// @param maxColumns Columns a page holds at most.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters) - sizes and bounds, each documented above
    explicit PageWindow(std::int64_t pageSize = 200, std::size_t maxPages = 8, std::size_t maxKeys = 1000,
                        std::size_t maxColumns = 96)
        : Core{pageSize, maxPages, true, maxKeys, maxColumns} {}

    /// @brief Declares the visible rows and the columns shown; returns the fetches to send.
    /// @param first   First visible view row.
    /// @param count   Visible rows.
    /// @param columns The columns shown, in order; empty asks for every column.
    /// @return The fetches to send, each now in flight.
    [[nodiscard]] std::vector<PageFetch> visible(std::int64_t first, std::int64_t count,
                                                 std::span<std::string const> columns = {}) {
        return visibleColumns(first, count, columns);
    }

    /// @brief Stores a page reply.
    /// @param fetch The fetch the reply answers, as `visible` returned it.
    /// @param page  The page the server returned.
    /// @return `false` when the reply was ignored: its fetch belongs to an earlier query, or is not the one in
    ///         flight for its page.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): one sequential pass whose steps share its running state; splitting it would thread that state through parameters.
    bool accept(PageFetch const& fetch, GenericPage page);

    /// @brief Patches held rows from the reply to a `RowsFetch`.
    /// @param fetch The keyed fetch the reply answers, as `refresh` or `owed` returned it.
    /// @param reply The keyed reply.
    /// @return The view rows replaced, ascending; empty when the reply was ignored or the window invalidated.
    std::vector<std::int64_t> patch(RowsFetch const& fetch, GenericPage reply);

    /// @brief The key of the held row at a view row.
    /// @param index View row.
    /// @return The key, or null when the row is not held.
    [[nodiscard]] RowId const* key(std::int64_t index) const;

    /// @brief The cell of a held row in a column.
    /// @param index  View row.
    /// @param column The column's id.
    /// @return The cell, or null when the row or the column is not held.
    [[nodiscard]] Cell const* cell(std::int64_t index, std::string_view column) const;

    /// @brief Whether a held row's column is held.
    /// @param index  View row.
    /// @param column The column's id.
    /// @return `true` when `cell(index, column)` is not null.
    [[nodiscard]] bool holds(std::int64_t index, std::string_view column) const {
        return cell(index, column) != nullptr;
    }

private:
    [[nodiscard]] static detail::WindowReply replyOf(GenericPage const& page);

    // Over the bound, the columns least recently named by `visible` go first, and a column shown now never goes:
    // dropping one would make the next `visible` fetch it again, and the next accept drop it again. `visible`
    // takes at most `maxColumns` columns, so the columns not shown always cover the excess.
    void limitColumns(detail::GenericWindowCells& content) const;
};

// NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)

}  // namespace morph::table

// ── A model's tables ───────────────────────────────────────────────────────

namespace morph::table {

/// @brief Who asks for a table: what a table's function reads to decide what it serves.
struct TableContext {
    /// @brief The caller's session; an empty one outside a dispatch.
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members): a read-only view of the dispatching session, valid for the dispatch it is built in; never reseated or assigned.
    session::Context const& session;
};

/// @brief What a model's table function returns: everything the framework needs to answer a query.
///
/// The function runs per request on the model's strand and returns shared, immutable things; the framework
/// pins the source's snapshot there and builds the reply off the strand.
struct ServedTable {
    /// @brief The table as this caller sees it; its `revision` is set by the framework.
    TableDescription description;
    /// @brief The rows the caller may see, held in memory or read lazily: `apply` runs over it.
    std::shared_ptr<DataSource const> source;
    /// @brief The slow levels, one per level the description names.
    std::vector<std::shared_ptr<LevelSource>> levels;
    /// @brief Whether the caller may read a column; empty allows every column of the description.
    std::function<bool(std::string_view column)> mayRead;
    /// @brief Names everything about the caller that changes the view beyond the query: the row scope and the
    ///        collation. Empty when every caller sees the same rows in the same order.
    std::string scope;
    /// @brief The collator, date parser, comparators and progress sink this caller's order uses.
    Services services;
    /// @brief The bounds a query to this table is held to.
    QueryLimits limits;
};

/// @brief The most sort keys a table's `limits` may allow.
inline constexpr std::size_t kMaxSortKeysBound = 32;

/// @brief The description a served table answers with: its sort bound from `limits` and its units, then its
///        revision.
///
/// The bound is set before `describe` takes the revision, because `capabilities` is part of what it hashes: a
/// table whose limits raise the bound has another revision than the same table at the default. Every reply that
/// names the table's revision, a page included, takes it from here, so a page and the describe reply agree.
///
/// A table with a `Measure` column lists its units (`TableDescription::units`) from `services.units` by `unitsFor`
/// when its function left them empty, also before the revision is taken, so a client learns them from the
/// description and from every page's revision alike.
/// @param served The table as its function returned it.
/// @return The description, its `capabilities.maxSortKeys` the limits', its units listed and its revision set;
///         `SourceContract` when `limits.maxSortKeys` is 0 or above `kMaxSortKeysBound`, or when a column is
///         `Measure` and `services.units` is null.
[[nodiscard]] MORPH_API std::expected<TableDescription, TableError> servedDescription(ServedTable const& served);

/// @brief One row of a table that an action changed.
struct ChangedRow {
    /// @brief The row's key.
    RowId key;
    /// @brief The key the row had before the action gave it `key`, when the action changed it: saving a modified row
    ///        as a new one. The result walk collects it beside `key`, so the change event names both.
    std::optional<RowId> from;

    /// @brief Member-wise equality.
    /// @param other The row to compare with.
    /// @return `true` when the keys and the previous keys match.
    [[nodiscard]] bool operator==(ChangedRow const& other) const = default;
};

/// @brief One row of a table an action could not change, and why.
struct FailedRow {
    /// @brief The row's key.
    RowId key;
    /// @brief What the action says went wrong, for the person who ran it.
    std::string message;

    /// @brief Member-wise equality.
    /// @param other The row to compare with.
    /// @return `true` when key and message match.
    [[nodiscard]] bool operator==(FailedRow const& other) const = default;
};

/// @brief What `BRIDGE_REGISTER_TABLE` declares about a table, by its function.
///
/// The primary template is no table; the registration specialises it with `model`, `row` (`void` for a table
/// whose function returns a `ServedTable`, the row type for a `typed<Row>` one), `registered`, `id` and `serve`.
/// @tparam Function The table's function.
template <auto Function>
struct TableTraits {
    /// @brief `false`: no registration names @p Function.
    static constexpr bool registered = false;
};

/// @brief A table's record type: a tag made from the table's function, distinct for each table.
///
/// Named by `updatesTable`, `changesTable`, `ChangedRows` and the record registry. A function no registration
/// names does not compile as a table.
/// @tparam Function The table's function.
template <auto Function>
struct TableOf {
    static_assert(TableTraits<Function>::registered,
                  "not a registered table: BRIDGE_REGISTER_TABLE must name this function before it is used");
};

/// @brief The rows of one table that an action changed: the slot an `updatesTable<Function>` effect reads.
///
/// Its JSON is `{"rows": [{"key": …, "from": …}, …], "failed": [{"key": …, "message": …}, …]}`. The result walk
/// reports `TableOf<Function>` at its `rows` member, keyed by `key` and by `from`, so an action's changed rows are
/// collected as any record's are; a `failed` row is not changed, and is not collected.
/// @tparam Function The table's function, as `BRIDGE_REGISTER_TABLE` names it.
template <auto Function>
struct ChangedRows {
    /// @brief The rows changed, in any order.
    std::vector<ChangedRow> rows;
    /// @brief The rows the action could not change, each with why.
    std::vector<FailedRow> failed;

    /// @brief The record the rows belong to.
    using heldRecord = TableOf<Function>;

    /// @brief The member that holds the record's rows.
    /// @return `"rows"`.
    [[nodiscard]] static constexpr std::string_view heldMember() noexcept { return "rows"; }
};

/// @brief A table function that returns typed rows, as `typed<Row>` registers it.
/// @tparam Row The row type: a glaze-reflectable aggregate registered as a record.
/// @tparam Fn  The function's type: `std::shared_ptr<std::vector<Row> const> (Model::*)(TableContext const&)`.
template <typename Row, typename Fn>
struct Typed {
    /// @brief The model's function.
    Fn function;
};

/// @brief Registers a table from typed rows: its description is built from @p Row and its rows are served
///        through a `RowsSource<Row>`.
/// @tparam Row The row type: a glaze-reflectable aggregate registered as a record, whose key is the table's.
/// @tparam Fn  The function's type, deduced.
/// @param function The model's function returning the rows the caller may see.
/// @return The registration's function, for `BRIDGE_REGISTER_TABLE`.
template <typename Row, typename Fn>
[[nodiscard]] constexpr Typed<Row, Fn> typed(Fn function) noexcept {
    static_assert(std::is_member_function_pointer_v<Fn>, "table::typed: the function is a member of the model");
    return Typed<Row, Fn>{function};
}

/// @brief Registration option: callers see different rows, so the table's record keeps its keys private.
// NOLINTNEXTLINE(readability-identifier-naming): a registration option, spelled in lower case like the registration call it is passed to.
struct rowScoped {};

/// @brief Registration option: a change of any of @p Records is a change of the table's record.
/// @tparam Records Registered record types.
template <typename... Records>
// NOLINTNEXTLINE(readability-identifier-naming): a registration option, spelled in lower case like the registration call it is passed to.
struct derivedFrom {};

// NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) -- row indices are bounded by the snapshot's own counts

namespace detail {

/// @brief A source holding one snapshot: what a reply is built from off the owner's thread.
///
/// A `DataSource` is owner-affine and only its snapshot may cross threads, so the snapshot is taken on the
/// owner's thread and this source carries it, with a copy of the columns and the groups. It never changes.
class MORPH_API PinnedSource final : public DataSource {
public:
    /// @brief Holds @p snapshot under @p columns and @p groups.
    /// @param snapshot The rows.
    /// @param columns  The source's columns when the snapshot was taken.
    /// @param groups   The source's declared groups when the snapshot was taken.
    PinnedSource(std::shared_ptr<RowSnapshot const> snapshot, std::vector<ColumnInfo> columns,
                 std::vector<ColumnGroupInfo> groups = {})
        : _snapshot{std::move(snapshot)},
          _columns{std::move(columns)},
          _groups{std::move(groups)},
          _pinnedAt{nextPin()} {}

    PinnedSource(PinnedSource const&) = delete;
    PinnedSource& operator=(PinnedSource const&) = delete;
    PinnedSource(PinnedSource&&) = delete;
    PinnedSource& operator=(PinnedSource&&) = delete;
    ~PinnedSource() override = default;

    /// @brief When the snapshot was pinned, among every pin of the process: larger for later.
    /// @return The pin's sequence number.
    [[nodiscard]] std::uint64_t pinnedAt() const noexcept { return _pinnedAt; }

    /// @brief The columns.
    /// @return The copy taken with the snapshot.
    [[nodiscard]] std::span<ColumnInfo const> columns() const override { return _columns; }

    /// @brief The declared groups.
    /// @return The copy taken with the snapshot.
    [[nodiscard]] std::span<ColumnGroupInfo const> groups() const override { return _groups; }

    /// @brief The one snapshot.
    /// @return The snapshot.
    [[nodiscard]] std::shared_ptr<RowSnapshot const> snapshot() const override { return _snapshot; }

    /// @brief Does nothing: the source never changes.
    /// @param listener Ignored.
    void subscribe(ChangeListener& listener) override { static_cast<void>(listener); }

    /// @brief Does nothing: the source never changes.
    /// @param listener Ignored.
    void unsubscribe(ChangeListener& listener) override { static_cast<void>(listener); }

private:
    [[nodiscard]] MORPH_PROCESS_UNIQUE static std::uint64_t nextPin() noexcept;

    std::shared_ptr<RowSnapshot const> _snapshot;
    std::vector<ColumnInfo> _columns;
    std::vector<ColumnGroupInfo> _groups;
    std::uint64_t _pinnedAt;
};

/// @brief The rows of a shared snapshot whose keys a scope holds, in the shared snapshot's order.
class ScopedSnapshot final : public RowSnapshot {
public:
    /// @brief Maps @p rows onto @p base.
    /// @param base The shared rows.
    /// @param rows The rows of @p base in the scope, in order.
    ScopedSnapshot(std::shared_ptr<RowSnapshot const> base, std::vector<std::size_t> rows)
        : _base{std::move(base)}, _rows{std::move(rows)} {}

    /// @brief The rows in the scope.
    /// @return Row count.
    [[nodiscard]] std::size_t rowCount() const override { return _rows.size(); }

    /// @brief A row's key.
    /// @param row Row index in the scope.
    /// @return The shared row's key.
    [[nodiscard]] RowId rowId(std::size_t row) const override { return _base->rowId(_rows[row]); }

    /// @brief One cell, read from the shared row.
    /// @param row    Row index in the scope.
    /// @param column Column index.
    /// @return The cell.
    [[nodiscard]] Cell cell(std::size_t row, std::size_t column) const override {
        return _base->cell(_rows[row], column);
    }

private:
    std::shared_ptr<RowSnapshot const> _base;
    std::vector<std::size_t> _rows;
};

/// @brief A scope's view of a shared source: the rows whose key the scope holds, sharing every cell.
///
/// Its row map is rebuilt, under a mutex, on the first `snapshot()` after the shared source's snapshot
/// changed. It notifies no change of its own: a reader sees the shared source's changes on its next snapshot.
class MORPH_API ScopedSource final : public DataSource {
public:
    /// @brief Scopes @p source to @p keys.
    /// @param source The shared source.
    /// @param keys   The keys of the scope's rows; a key the source lacks is skipped.
    ScopedSource(std::shared_ptr<DataSource const> source, std::vector<RowId> keys)
        : _source{std::move(source)},
          _keys(std::make_move_iterator(keys.begin()), std::make_move_iterator(keys.end())) {}

    /// @brief The shared source's columns.
    /// @return The columns.
    [[nodiscard]] std::span<ColumnInfo const> columns() const override { return _source->columns(); }

    /// @brief The shared source's declared groups.
    /// @return The groups.
    [[nodiscard]] std::span<ColumnGroupInfo const> groups() const override { return _source->groups(); }

    /// @brief The scope's rows of the shared source's current snapshot.
    /// @return The snapshot.
    [[nodiscard]] std::shared_ptr<RowSnapshot const> snapshot() const override;

    /// @brief Does nothing: the scope notifies no change of its own.
    /// @param listener Ignored.
    void subscribe(ChangeListener& listener) override { static_cast<void>(listener); }

    /// @brief Does nothing: the scope notifies no change of its own.
    /// @param listener Ignored.
    void unsubscribe(ChangeListener& listener) override { static_cast<void>(listener); }

private:
    std::shared_ptr<DataSource const> _source;
    std::set<RowId> _keys;
    mutable std::mutex _mutex;
    mutable std::shared_ptr<RowSnapshot const> _base;
    mutable std::shared_ptr<RowSnapshot const> _scoped;
};

}  // namespace detail

/// @brief A source of the rows of @p source whose key is in @p keys, without copying a cell.
///
/// What a model with row scopes keeps per scope: built when the scope is first asked for and shared from then
/// on. Its rows keep the shared source's order; keys the source lacks are skipped. A change of the shared
/// source is seen by the next `snapshot()`.
/// @param source The shared source.
/// @param keys   The scope's row keys.
/// @return The scoped source.
[[nodiscard]] inline std::shared_ptr<DataSource const> scoped(std::shared_ptr<DataSource const> source,
                                                              std::vector<RowId> keys) {
    return std::make_shared<detail::ScopedSource const>(std::move(source), std::move(keys));
}

namespace detail {

/// @brief What a reply is built with beyond the table and the query.
struct AnswerContext {
    /// @brief The server's view cache; null computes the view for the request alone.
    ViewCache* views = nullptr;
    /// @brief The table's record name, `<model type>.<id>`: the first member of a view's key. Empty caches nothing.
    std::string_view record;
    /// @brief The registration's `rowScoped`: a reply then needs a `ServedTable::scope`.
    bool rowScoped = false;
};

/// @brief The canonical text of a validated query's sort and filter: the part of a view's key the query gives.
/// @param validated The query as `validate` returned it.
/// @return The JSON of `sort`, then of `filters`; nothing when either cannot be written, and the view is then
///         computed for the request alone rather than cached under a key two queries could share.
[[nodiscard]] MORPH_API std::optional<std::string> viewQueryOf(TableQuery const& validated);

/// @brief @p served with its source replaced by a `PinnedSource` of one snapshot taken now.
///
/// Called on the source's owner thread, the model's strand, before the reply is built anywhere else.
/// @param served The table as the model served it.
/// @return The same table over a pinned source; a null source stays null.
[[nodiscard]] MORPH_API ServedTable pinned(ServedTable served);

/// @brief A sorted, filtered or listed column of @p level: the column a refused order use names.
/// @param query    The validated query.
/// @param columns  The table's columns.
/// @param level    The level.
/// @param services The services the filter compiles with.
/// @param groups   The table's declared groups.
/// @return The first sort key's column at @p level, else the first filtered column at it, else the first column a
///         `distinct` request lists at it; empty when none is.
[[nodiscard]] MORPH_API std::string orderColumnOf(TableQuery const& query, std::span<ColumnInfo const> columns,
                                                  std::string_view level, Services const& services,
                                                  std::span<ColumnGroupInfo const> groups = {});

/// @brief Answers @p query over a served table: the page a `Table.<id>` action returns.
///
/// The query is validated against the description's columns, its limits and `mayRead`; the page's columns
/// are the validated `columns`, or, absent, every description column `mayRead` allows, in the description's
/// order. Every base-level description column must be one of the source's, and every column of a level that
/// `served.levels` serves one of that level's columns, or nothing is read.
///
/// The levels the query needs (`levelsNeeded`) are read from one snapshot of each, taken at the start, through a
/// `LeveledSource` over the pinned source: a level cell no snapshot holds is `Unloaded`, and a level no entry of
/// `served.levels` serves is `Unloaded` in every row. `unloadedRows` names each row of the page with the needed
/// levels its snapshot does not hold, `failed` those whose load failed; `awaiting` and `failed` name the order
/// levels unloaded, or failed, for some row of the view or of the undecided rows. Then each level is told what
/// the reply needed, with `query.retry`: an order level every row of the view and the undecided rows, a display
/// level the page's rows. An order use the level refuses refuses the query, `Unavailable` naming a sorted or
/// filtered column of the level, and no page is returned.
///
/// The view comes from `context.views` when it is set and `context.record` is not empty: the entry of the
/// record, `served.scope` and the canonical sort and filter that is valid for the pinned source's snapshot and
/// the order levels' generations, computed once and read by every page and keyed fetch of it. A row-scoped table
/// (`context.rowScoped`) whose `scope` is empty is refused `SourceContract` before anything is read: its callers
/// would share views of different rows.
///
/// A keyed query to a table whose capabilities exclude keyed queries is `Unavailable`, which a client answers by
/// refetching its pages.
///
/// A query with `distinct` set is answered by `distinctValues` over a `LeveledSource` of the levels the request
/// needs (its listed column's level is an order level), each level then wanted as `Order` for every row, with
/// `query.retry`: a page with no columns, keys or rows and `distinct` set; `Unavailable` when the table's
/// capabilities exclude distinct requests, or when a level refuses the order use.
/// A group filter's or a distinct request's `group` resolves through the source's declared groups, which are
/// checked first (`detail::checkGroups`).
/// @param served  The table, its source pinned (`pinned`).
/// @param query   The query, as received.
/// @param context What the reply is built with.
/// @return The page, its `revision` that of `servedDescription(served)`, or the error the client shows:
///         `SourceContract` when the table's `limits.maxSortKeys` is out of range.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): one sequential pass whose steps share its running state; splitting it would thread that state through parameters.
[[nodiscard]] MORPH_API std::expected<GenericPage, TableError> answerQuery(ServedTable const& served,
                                                                           TableQuery const& query,
                                                                           AnswerContext const& context = {});

/// @brief The name a `TableErrorCode` has on the wire.
/// @param code The code.
/// @return Its JSON name, as `unknownColumn`, without quotes.
[[nodiscard]] MORPH_API std::string errorCodeName(TableErrorCode code);

/// @brief The refusal a table's action throws for @p error.
/// @param error The error.
/// @return `ActionRefusal{"table." + code, {{"column", column}}, message}`.
[[nodiscard]] MORPH_API model::ActionRefusal refusalOf(TableError const& error);

/// @brief The caller's `TableContext`: the session installed on the calling thread, or an empty one.
/// @return The context, valid while the session stays installed.
[[nodiscard]] MORPH_API TableContext callerContext();

/// @brief The row type of a registration's function: `void` for one returning a `ServedTable`.
/// @tparam F The registration's function type.
template <typename F>
struct TableRowOf {
    /// @brief `void`: the function returns a `ServedTable`.
    using type = void;
};

/// @brief The row type of a `typed<Row>(…)` registration.
/// @tparam Row The row type.
/// @tparam Fn  The function's type.
template <typename Row, typename Fn>
struct TableRowOf<Typed<Row, Fn>> {
    /// @brief The row type.
    using type = Row;
};

/// @brief The row type of a registration's function.
/// @tparam F The registration's function type, as `decltype` gives it.
template <typename F>
using TableRow = TableRowOf<std::remove_cvref_t<F>>::type;

/// @brief The member function a registration keys its table on.
/// @tparam F The registration's function type.
/// @param function A member function pointer, or a `typed<Row>(…)` registration.
/// @return The member function pointer.
template <typename F>
[[nodiscard]] constexpr auto tableKey(F function) noexcept {
    if constexpr (std::is_void_v<TableRow<F>>) {
        return function;
    } else {
        return function.function;
    }
}

/// @brief The record option a table registration option stands for.
/// @tparam O A table registration option.
template <typename O>
struct RecordOptionOf {
    static_assert(sizeof(O) == 0, "BRIDGE_REGISTER_TABLE: an option is table::rowScoped or table::derivedFrom<...>");
};

/// @brief `rowScoped` is the record's `privateKeys`.
template <>
struct RecordOptionOf<rowScoped> {
    /// @brief The record option.
    using type = record::privateKeys;
};

/// @brief `derivedFrom<Records…>` is the record's.
/// @tparam Records The source records.
template <typename... Records>
struct RecordOptionOf<derivedFrom<Records...>> {
    /// @brief The record option.
    using type = record::derivedFrom<Records...>;
};

/// @brief Static-init helper for `BRIDGE_REGISTER_TABLE`: registers the table's record.
/// @tparam Key     The table's function.
/// @tparam Options The registration's options.
/// @param name The record's wire name, `<model type>.<id of the table>`.
/// @return Always true, so the result can initialise a namespace-scope variable.
template <auto Key, typename... Options>
inline bool registerTableRecord(std::string const& name) noexcept {
    return record::detail::registerTableRecordOnce<TableOf<Key>, typename RecordOptionOf<Options>::type...>(name);
}

/// @brief The key of a typed row, read from its member named @p keyField.
/// @tparam Row The row type.
/// @param keyField The key member's JSON name.
/// @return The projection, or nothing when @p Row has no such member.
template <typename Row>
[[nodiscard]] std::optional<RowKey<Row>> rowKeyNamed(std::string_view keyField) {
    auto const readers = cellReaders<Row>();
    for (std::size_t index = 0; index < readers.size(); ++index) {
        if (glz::reflect<Row>::keys[index] == keyField) {
            return RowKey<Row>{[reader = readers[index]](Row const& row) -> RowId {
                auto cell = reader(row, nullptr);
                if (auto* const text = std::get_if<std::string>(&cell)) {
                    return RowId{std::move(*text)};
                }
                if (auto const* const integer = std::get_if<std::int64_t>(&cell)) {
                    return RowId{*integer};
                }
                return RowId{std::int64_t{0}};
            }};
        }
    }
    return std::nullopt;
}

/// @brief The table a `typed<Row>` registration serves: a description built from @p Row and its rows through a
///        `RowsSource<Row>`.
///
/// The columns are `Row`'s reflected members in member order, each labelled by its field metadata or else by
/// its name made a title, each with the level `Row::columnLevels()` gives it; `levels` lists those levels, sorted
/// and each once. The groups are the source's (`Row::columnGroups()`), each labelled by its id made a title, and
/// each column names the group it is in. The key is the key member of the record registered for `Row`. @p units
/// resolves the rows' `MeasureValue` unit ids and is the table's `services.units`.
/// @tparam Row The row type.
/// @param tableId The table's id.
/// @param rows  The rows the caller may see; null serves none.
/// @param units The model's unit registry (`tableUnits`); null when the model declares none.
/// @return The served table.
/// @throws std::logic_error when `Row` is not a registered record with a key.
/// @throws std::invalid_argument when `Row::columnLevels()` names no column.
template <typename Row>
[[nodiscard]] ServedTable typedTable(std::string_view tableId, std::shared_ptr<std::vector<Row> const> rows,
                                     std::shared_ptr<UnitRegistry const> units = nullptr) {
    auto const& records = effects::detail::resolved().records;
    auto const record = std::ranges::find(records, std::type_index{typeid(Row)}, &record::RecordInfo::type);
    if (record == records.end() || !record->keyField) {
        throw std::logic_error{"table " + std::string{tableId} +
                               ": its row type is not a registered record with a key"};
    }
    auto key = rowKeyNamed<Row>(*record->keyField);
    if (!key) {
        throw std::logic_error{"table " + std::string{tableId} + ": its row type has no member " + *record->keyField};
    }
    ServedTable served;
    served.description.id = std::string{tableId};
    served.description.label = forms::detail::inferTitle(tableId);
    served.description.key = *record->keyField;
    for (auto& info : reflectedColumns<Row>()) {
        auto const* const meta = forms::detail::findFieldMeta<Row>(info.id);
        std::string label =
            meta != nullptr && !meta->label.empty() ? std::string{meta->label} : forms::detail::inferTitle(info.id);
        if (!info.level.empty()) {
            served.description.levels.push_back(info.level);
        }
        served.description.columns.push_back(
            TableColumn{.info = std::move(info), .label = std::move(label), .group = {}});
    }
    std::ranges::sort(served.description.levels);
    auto const [firstDuplicate, lastDuplicate] = std::ranges::unique(served.description.levels);
    served.description.levels.erase(firstDuplicate, lastDuplicate);
    if (rows == nullptr) {
        rows = std::make_shared<std::vector<Row> const>();
    }
    auto source = std::make_shared<RowsSource<Row> const>(
        std::move(rows), std::move(*key), std::map<std::string, ColumnKind, std::less<>>{},
        std::map<std::string, std::string, std::less<>>{}, LevelLoaded<Row>{}, units);
    served.services.units = std::move(units);
    for (auto group : source->groups()) {
        group.label = forms::detail::inferTitle(group.id);
        for (auto& column : served.description.columns) {
            if (std::ranges::find(group.columns, column.info.id) != group.columns.end()) {
                column.group = group.id;
            }
        }
        served.description.groups.push_back(std::move(group));
    }
    served.source = std::move(source);
    return served;
}

/// @brief A table's `ServedTable`, from its model: the one way to it.
///
/// Calls the registered function on @p model, on the model's strand, with @p context; for a `typed<Row>` table
/// it builds the description and the source from the rows, and a model that declares
/// `std::shared_ptr<morph::table::UnitRegistry const> tableUnits(table::TableContext const&)` has it called there too,
/// its registry resolving the rows' measures. The description is stamped with its revision.
/// @tparam Function The table's function, as `BRIDGE_REGISTER_TABLE` keys it.
/// @tparam Model    The model type.
/// @param model   The model instance.
/// @param context Who asks.
/// @return The served table, its source live.
template <auto Function, typename Model>
[[nodiscard]] ServedTable serve(Model& model, TableContext const& context) {
    using Traits = TableTraits<Function>;
    static_assert(Traits::registered, "not a registered table");
    ServedTable served;
    if constexpr (std::is_void_v<typename Traits::row>) {
        served = (model.*Function)(context);
    } else {
        std::shared_ptr<UnitRegistry const> units;
        if constexpr (requires {
                          { model.tableUnits(context) } -> std::convertible_to<std::shared_ptr<UnitRegistry const>>;
                      }) {
            units = model.tableUnits(context);
        }
        served = typedTable<typename Traits::row>(Traits::id, (model.*Function)(context), std::move(units));
    }
    served.description = describe(std::move(served.description));
    return served;
}

/// @brief A table's record: its name and whether it keeps its keys private.
/// @tparam Function The table's function.
/// @return The registered record's name, valid for the process, and its `privateKeys`; empty and false when the
///         registries hold no such record.
template <auto Function>
[[nodiscard]] std::pair<std::string_view, bool> tableRecordOf() {
    auto const& records = effects::detail::resolved().records;
    auto const found =
        std::ranges::find(records, std::type_index{typeid(TableOf<Function>)}, &record::RecordInfo::type);
    if (found == records.end()) {
        return {std::string_view{}, false};
    }
    return {std::string_view{found->name}, found->privateKeys};
}

/// @brief The awaiter that builds a reply on the table executor and resumes the awaiting coroutine where it
///        was running.
///
/// The work item owns the pinned table and the query. The coroutine resumes through the executor it was
/// running on when it suspended, its strand, or inline on the worker when it was running on none.
class MORPH_API AnswerAwaiter {
    struct Shared {
        std::expected<GenericPage, TableError> answer;
        std::exception_ptr error;
        std::coroutine_handle<> continuation;
        ::core::async::ResumeTarget context;
    };

public:
    /// @brief An answer of @p query over @p served, to run on @p work.
    /// @param work    The table executor; must outlive the work item.
    /// @param served  The table, its source pinned.
    /// @param query   The query.
    /// @param context What the reply is built with.
    AnswerAwaiter(exec::IExecutor& work MORPH_LIFETIMEBOUND, ServedTable served, TableQuery query,
                  AnswerContext context)
        : _work{&work},
          _served{std::move(served)},
          _query{std::move(query)},
          _context{context},
          _shared{std::make_shared<Shared>()} {}

    /// @return Always false: the answer is built elsewhere.
    // Called through the awaiter by the compiler, so not static.
    // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
    [[nodiscard]] constexpr bool await_ready() const noexcept { return false; }

    /// @brief Posts the answer to the table executor.
    /// @param awaiting The coroutine performing the `co_await`.
    void await_suspend(std::coroutine_handle<> awaiting);

    /// @brief The answer, or what building it threw.
    /// @return The answer.
    std::expected<GenericPage, TableError> await_resume();

private:
    exec::IExecutor* _work;
    ServedTable _served;
    TableQuery _query;
    AnswerContext _context;
    std::shared_ptr<Shared> _shared;
};

}  // namespace detail

/// @brief The action `Table.<id>`: a page of a table, read by a `TableQuery`.
///
/// Its JSON is the `TableQuery`'s. It runs on the model's strand: the table's function is called with the
/// caller's session and the source's snapshot is pinned there; the page is then built on the server's table
/// executor (`ServerConfig::tableWork`) and the coroutine resumes on the strand with it. With no table executor
/// installed, as on a `LocalBackend`, the page is built on the strand.
/// @tparam Function The table's function, as `BRIDGE_REGISTER_TABLE` keys it.
template <auto Function>
struct QueryTable {
    /// @brief The query.
    TableQuery query;

    /// @brief Answers the query.
    /// @tparam Model The model type.
    /// @param model  The model instance.
    /// @param action The action.
    /// @return The page.
    /// @throws morph::model::ActionRefusal `table.<reason>` with the argument `column`, for a query the table
    ///         refuses.
    template <typename Model>
    static ::core::async::Task<GenericPage> invoke(Model& model, QueryTable& action);
};

/// @brief Checks every editable column of a description against its edit action's input: the field it edits must
///        exist and be of a kind the column's kind is edited with. Defined in `table/edits.hpp`, which this header
///        includes at its end.
///
/// A column's field is the `value` member of a `CellEdit` action (an `EditInfo` with no `member`), or the named member
/// of a `RowEdit` action's `values`. The fits are `Integer` with an integer, slider, enum or choice field; `Key` with
/// an integer or choice; `Decimal` with a decimal; `Quantity` and `Measure` with a quantity; `Number` with a number or
/// slider; `Text` with a text, multiline, enum or choice; `Date` with a date or epoch date; `DateTime` with a
/// date-time or epoch date-time; `Bool` with a Boolean; `Custom` with a text, enum or choice.
/// @param description The description.
/// @param schemaOf    An action's input schema by its id; nothing for an action that is not known.
/// @return `SourceContract` naming the first column whose action is unknown or does not read, whose field is missing,
///         or whose field's kind does not fit the column's; nothing when every edit fits.
[[nodiscard]] MORPH_API std::expected<void, TableError> checkEdits(
    TableDescription const& description,
    std::function<std::optional<std::string>(std::string_view action)> const& schemaOf);

/// @brief The action `Table.<id>.describe`: the table's description as the caller sees it.
///
/// It runs on the model's strand and reads neither the source nor a snapshot.
/// @tparam Function The table's function, as `BRIDGE_REGISTER_TABLE` keys it.
template <auto Function>
struct DescribeTable {
    /// @brief Describes the table.
    /// @tparam Model The model type.
    /// @param model  The model instance.
    /// @param action The action.
    /// @return `servedDescription` of the served table: its sort bound from `limits`, its revision set.
    /// @throws model::ActionRefusal when `servedDescription` answers `SourceContract`, or when a column's edit does
    ///         not fit its action's input (`checkEdits`, over the schemas the server serves).
    template <typename Model>
    static TableDescription invoke(Model& model, DescribeTable& action) {
        static_cast<void>(action);
        auto const served = detail::serve<Function>(model, detail::callerContext());
        if (auto shown = validateDisplay(served.description); !shown) {
            throw detail::refusalOf(shown.error());
        }
        auto described = servedDescription(served);
        if (!described) {
            throw detail::refusalOf(described.error());
        }
        if (auto const* const runtime = currentTableRuntime(); runtime != nullptr && runtime->schemaOf) {
            auto const edits =
                checkEdits(*described, [runtime](std::string_view tableId) -> std::optional<std::string> {
                    std::string schema = runtime->schemaOf(tableId);
                    return schema.empty() ? std::nullopt : std::optional{std::move(schema)};
                });
            if (!edits) {
                throw detail::refusalOf(edits.error());
            }
        }
        return std::move(*described);
    }
};

/// @brief Answers the query.
/// @tparam Model The model type.
/// @param model  The model instance.
/// @param action The action.
/// @return The page.
/// @throws morph::model::ActionRefusal `table.<reason>` with the argument `column`, for a query the table refuses.
template <auto Function>
template <typename Model>
// NOLINTNEXTLINE(cppcoreguidelines-avoid-reference-coroutine-parameters): the dispatcher keeps the model and the action alive until the task completes.
::core::async::Task<GenericPage> QueryTable<Function>::invoke(Model& model, QueryTable& action) {
    // Read before the first suspension: a server installs the runtime for the dispatch's first step only.
    auto const* const runtime = currentTableRuntime();
    exec::IExecutor* const work = runtime != nullptr ? runtime->work : nullptr;
    auto const [record, rowScoped] = detail::tableRecordOf<Function>();
    detail::AnswerContext const context{
        .views = runtime != nullptr ? runtime->views : nullptr, .record = record, .rowScoped = rowScoped};
    auto served = detail::pinned(detail::serve<Function>(model, detail::callerContext()));
    std::expected<GenericPage, TableError> answer;
    if (work == nullptr) {
        answer = detail::answerQuery(served, action.query, context);
    } else {
        // Its own statement: MSVC cannot tail-call a call that shares a full-expression with a `co_await`.
        auto awaited = co_await detail::AnswerAwaiter{*work, std::move(served), action.query, context};
        answer = std::move(awaited);
    }
    if (!answer) {
        throw detail::refusalOf(answer.error());
    }
    co_return std::move(*answer);
}

// NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)

}  // namespace morph::table

/// @brief The JSON of `morph::table::QueryTable`: its `TableQuery`'s.
/// @tparam Function The table's function.
template <auto Function>
struct glz::meta<morph::table::QueryTable<Function>> {
    /// @brief The action type.
    using T = morph::table::QueryTable<Function>;
    /// @brief The action is written as its query.
    static constexpr auto value = &T::query;
};

// The selection types `BRIDGE_REGISTER_TABLE` registers `Table.<id>.resolve` with; they build on this header.
// NOLINTNEXTLINE(misc-header-include-cycle): row_selection.hpp needs the declarations above, so it is included after them.
#include <morph/table/row_selection.hpp>

// The commits of edited cells and the edit actions of a model's table; `checkEdits` above is defined there.
// NOLINTNEXTLINE(misc-header-include-cycle): edits.hpp needs the declarations above, so it is included after them.
#include <morph/table/edits.hpp>
