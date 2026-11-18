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
#include <charconv>
#include <cmath>
#include <core/async/ExecutorContext.hpp>
#include <core/async/ParkedWork.hpp>
#include <core/async/Task.hpp>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <glaze/glaze.hpp>
#include <iterator>
#include <limits>
#include <memory>
#include <morph/attributes.hpp>
#include <morph/core/action_refusal.hpp>
#include <morph/core/effects.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/payload_schema.hpp>
#include <morph/core/records.hpp>
#include <morph/forms/forms.hpp>
#include <morph/session/session.hpp>
#include <morph/table/data_source.hpp>
#include <morph/table/query.hpp>
#include <morph/table/runtime.hpp>
#include <morph/util/rational.hpp>
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

    /// @brief Member-wise equality.
    /// @param other The column to compare with.
    /// @return `true` when every member matches.
    [[nodiscard]] bool operator==(TableColumn const& other) const = default;
};

/// @brief A declared group of columns, filtered together.
struct ColumnGroupInfo {
    /// @brief The group's id, which a column's `group` names.
    std::string id;
    /// @brief The group's text.
    std::string label;
    /// @brief The ids of its columns, 2 to 16 of one kind.
    std::vector<std::string> columns;

    /// @brief Member-wise equality.
    /// @param other The group to compare with.
    /// @return `true` when every member matches.
    [[nodiscard]] bool operator==(ColumnGroupInfo const& other) const = default;
};

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
    /// @brief The declared column groups.
    std::vector<ColumnGroupInfo> groups;
    /// @brief Every data level a column names, the base level excluded.
    std::vector<std::string> levels;
    /// @brief What the server offers on the table.
    TableCapabilities capabilities;
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
[[nodiscard]] inline TableDescription describe(TableDescription description) {
    description.revision.clear();
    auto const json = glz::write_json(description).value_or(std::string{});
    description.revision = model::detail::hex64(model::detail::fnv1a64(json));
    return description;
}

/// @brief The columns of @p description as the engine reads them.
/// @param description The description.
/// @return Each column's `info`, in the description's order.
[[nodiscard]] inline std::vector<ColumnInfo> columnInfos(TableDescription const& description) {
    std::vector<ColumnInfo> out;
    out.reserve(description.columns.size());
    for (auto const& column : description.columns) {
        out.push_back(column.info);
    }
    return out;
}

/// @brief One row of a generic page: one cell per entry of the page's `columns`.
using GenericRow = std::vector<Cell>;

/// @brief A page of a table read through its description: row arrays under one
///        header of column ids.
///
/// Its JSON members, in order: `revision`, `columns`, `keys`, `rows`, `total`
/// (absent when not counted), `totalKind` and `offset`.
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
inline void appendInteger(std::string& out, std::int64_t value) {
    std::array<char, 24> buffer{};
    auto const result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    out.append(buffer.data(), result.ptr);
}

/// @brief Appends a string as a JSON string, escaped as glaze escapes it.
/// @param out  The text to append to.
/// @param text The string.
inline void appendJsonString(std::string& out, std::string_view text) {
    static constexpr std::string_view kHex = "0123456789abcdef";
    out += '"';
    for (char const character : text) {
        switch (character) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(character) < 0x20U) {
                    auto const code = static_cast<unsigned char>(character);
                    out += "\\u00";
                    out += kHex[code >> 4U];
                    out += kHex[code & 0xFU];
                } else {
                    out += character;
                }
                break;
        }
    }
    out += '"';
}

/// @brief Appends a `Rational` as `[numerator, denominator]`.
///
/// With `p` its decimal places, the pair is scaled to `[n × 10^p / d, 10^p]`
/// when `10^p` is a multiple of the denominator and the scaled numerator fits
/// `int64`, so a reader takes the places back from the denominator; otherwise
/// it is the canonical pair.
/// @param out   The text to append to.
/// @param value The value.
inline void appendRational(std::string& out, math::Rational const& value) {
    auto numerator = value.numerator;
    auto denominator = value.denominator;
    auto const scale = powerOfTen(std::min(value.decimalPlaces.value, math::kMaxDecimalPlaces));
    if (denominator > 0 && scale % denominator == 0) {
        auto const factor = scale / denominator;
        constexpr auto kMax = std::numeric_limits<std::int64_t>::max();
        constexpr auto kMin = std::numeric_limits<std::int64_t>::min();
        if (factor == 1 || (numerator <= kMax / factor && numerator >= kMin / factor)) {
            numerator *= factor;
            denominator = scale;
        }
    }
    out += '[';
    appendInteger(out, numerator);
    out += ',';
    appendInteger(out, denominator);
    out += ']';
}

/// @brief Appends a finite or non-finite `double`.
///
/// A finite value is written as glaze writes it, with `.0` added when that
/// text has no fraction or exponent, so it never reads back as an integer. A
/// non-finite one, which JSON cannot hold as a number, is the string `"nan"`,
/// `"inf"` or `"-inf"`.
/// @param out   The text to append to.
/// @param value The value.
inline void appendDouble(std::string& out, double value) {
    if (std::isnan(value)) {
        out += "\"nan\"";
        return;
    }
    if (std::isinf(value)) {
        out += value > 0 ? "\"inf\"" : "\"-inf\"";
        return;
    }
    auto const text = glz::write_json(value).value_or(std::string{"null"});
    out += text;
    if (text.find_first_of(".eE") == std::string::npos) {
        out += ".0";
    }
}

/// @brief Appends one cell as JSON, by its `Cell` alternative.
///
/// Empty and unloaded are `null`; an integer, a string and a boolean are themselves; a
/// `double` is written by `appendDouble`; a `Rational` is a pair by
/// `appendRational`; a `QuantityCell` is `[n, d, [fn, fd]]`, its amount and
/// factor each written as a `Rational`, or the plain pair of its amount when
/// the factor is 1.
/// @param out  The text to append to.
/// @param cell The cell.
inline void appendCellJson(std::string& out, Cell const& cell) {
    std::visit(
        [&out]<typename T>(T const& value) {
            if constexpr (std::is_same_v<T, std::monostate> || std::is_same_v<T, Unloaded>) {
                out += "null";
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                appendInteger(out, value);
            } else if constexpr (std::is_same_v<T, double>) {
                appendDouble(out, value);
            } else if constexpr (std::is_same_v<T, std::string>) {
                appendJsonString(out, value);
            } else if constexpr (std::is_same_v<T, bool>) {
                out += value ? "true" : "false";
            } else if constexpr (std::is_same_v<T, math::Rational>) {
                appendRational(out, value);
            } else {
                static_assert(std::is_same_v<T, QuantityCell>);
                if (value.toCanonical.numerator == 1 && value.toCanonical.denominator == 1) {
                    appendRational(out, value.amount);
                    return;
                }
                appendRational(out, value.amount);
                out.back() = ',';
                appendRational(out, value.toCanonical);
                out += ']';
            }
        },
        cell);
}

/// @brief Reads JSON text token by token, for the shapes a cell has.
class CellCursor {
public:
    /// @brief A cursor at the start of @p text.
    /// @param text The JSON text.
    explicit CellCursor(std::string_view text) noexcept : _text{text} {}

    /// @brief Skips JSON whitespace.
    void skipSpace() noexcept {
        while (_at < _text.size() &&
               (_text[_at] == ' ' || _text[_at] == '\t' || _text[_at] == '\n' || _text[_at] == '\r')) {
            ++_at;
        }
    }

    /// @brief Consumes @p character after any whitespace.
    /// @param character The character expected.
    /// @return `true` when it was next and is consumed.
    [[nodiscard]] bool eat(char character) noexcept {
        skipSpace();
        if (_at < _text.size() && _text[_at] == character) {
            ++_at;
            return true;
        }
        return false;
    }

    /// @brief Whether only whitespace is left.
    /// @return `true` at the end of the text.
    [[nodiscard]] bool atEnd() noexcept {
        skipSpace();
        return _at == _text.size();
    }

    /// @brief Consumes a JSON integer token that fits `int64`.
    /// @return The integer, or nothing (and nothing consumed) when the next
    ///         token is not one.
    [[nodiscard]] std::optional<std::int64_t> integer() noexcept {
        skipSpace();
        auto const token = numberToken(_text.substr(_at));
        if (token.empty() || token.find_first_of(".eE") != std::string_view::npos) {
            return std::nullopt;
        }
        std::int64_t value = 0;
        auto const [end, error] = std::from_chars(token.data(), token.data() + token.size(), value);
        if (error != std::errc{} || end != token.data() + token.size()) {
            return std::nullopt;
        }
        _at += token.size();
        return value;
    }

    /// @brief The JSON number token at the start of @p text.
    /// @param text The text.
    /// @return The token: `-?digits(.digits)?([eE][+-]?digits)?`, or empty when
    ///         @p text does not start with one.
    [[nodiscard]] static std::string_view numberToken(std::string_view text) noexcept {
        std::size_t at = 0;
        auto const digits = [&] {
            auto const from = at;
            while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
                ++at;
            }
            return at > from;
        };
        if (at < text.size() && text[at] == '-') {
            ++at;
        }
        if (!digits()) {
            return {};
        }
        if (at < text.size() && text[at] == '.') {
            ++at;
            if (!digits()) {
                return {};
            }
        }
        if (at < text.size() && (text[at] == 'e' || text[at] == 'E')) {
            ++at;
            if (at < text.size() && (text[at] == '+' || text[at] == '-')) {
                ++at;
            }
            if (!digits()) {
                return {};
            }
        }
        return text.substr(0, at);
    }

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
[[nodiscard]] inline std::optional<math::Rational> readPair(CellCursor& cursor) {
    auto const numerator = cursor.integer();
    if (!numerator || !cursor.eat(',')) {
        return std::nullopt;
    }
    auto const denominator = cursor.integer();
    if (!denominator || *denominator <= 0) {
        return std::nullopt;
    }
    return math::Rational{math::Numerator{*numerator}, math::Denominator{*denominator},
                          math::DecimalPlaces{placesOf(*denominator)}};
}

/// @brief Reads an exact cell: `[n, d]` or `[n, d, [fn, fd]]`.
/// @param text The JSON text, starting with `[`.
/// @return The `Rational` or `QuantityCell`, or nothing when the array has
///         another shape.
[[nodiscard]] inline std::optional<Cell> readExactCell(std::string_view text) {
    CellCursor cursor{text};
    if (!cursor.eat('[')) {
        return std::nullopt;
    }
    auto const amount = readPair(cursor);
    if (!amount) {
        return std::nullopt;
    }
    if (cursor.eat(']')) {
        return cursor.atEnd() ? std::optional<Cell>{Cell{*amount}} : std::nullopt;
    }
    if (!cursor.eat(',') || !cursor.eat('[')) {
        return std::nullopt;
    }
    auto const factor = readPair(cursor);
    if (!factor || !cursor.eat(']') || !cursor.eat(']') || !cursor.atEnd()) {
        return std::nullopt;
    }
    return Cell{QuantityCell{.amount = *amount, .toCanonical = *factor}};
}

/// @brief The `double` a `Number` column holds for the strings JSON writes
///        a non-finite value as.
/// @param text The decoded string.
/// @return NaN, +inf or -inf, or nothing for any other string.
[[nodiscard]] inline std::optional<double> nonFinite(std::string_view text) noexcept {
    if (text == "nan") {
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (text == "inf") {
        return std::numeric_limits<double>::infinity();
    }
    if (text == "-inf") {
        return -std::numeric_limits<double>::infinity();
    }
    return std::nullopt;
}

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
[[nodiscard]] inline Cell cellFromJson(std::string_view json, std::optional<ColumnKind> kind) {
    auto const first = json.find_first_not_of(" \t\n\r");
    if (first == std::string_view::npos) {
        return Cell{std::string{json}};
    }
    auto const text = json.substr(first, json.find_last_not_of(" \t\n\r") - first + 1);
    if (text == "null") {
        return Cell{};
    }
    if (text == "true" || text == "false") {
        return Cell{text == "true"};
    }
    bool const number = kind == ColumnKind::Number;
    if (text.front() == '"') {
        std::string decoded;
        if (glz::read<glz::opts{.null_terminated = false}>(decoded, text)) {
            return Cell{std::string{text}};
        }
        if (number) {
            if (auto const value = nonFinite(decoded)) {
                return Cell{*value};
            }
        }
        return Cell{std::move(decoded)};
    }
    if (text.front() == '[') {
        if (auto cell = readExactCell(text)) {
            return std::move(*cell);
        }
        return Cell{std::string{text}};
    }
    if (CellCursor::numberToken(text).size() == text.size()) {
        if (!number && text.find_first_of(".eE") == std::string_view::npos) {
            std::int64_t value = 0;
            auto const [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            if (error == std::errc{} && end == text.data() + text.size()) {
                return Cell{value};
            }
        }
        double value = 0;
        if (!glz::read<glz::opts{.null_terminated = false}>(value, text) && std::isfinite(value)) {
            return Cell{value};
        }
    }
    return Cell{std::string{text}};
}

/// @brief Reads a page's rows with no column kinds; `readGenericPage` reads
///        them again with their columns' kinds.
/// @param page The page to fill.
/// @param rows Each row's cells as JSON text.
inline void readPageRows(GenericPage& page, std::vector<std::vector<glz::raw_json_view>> const& rows) {
    page.rows.clear();
    page.rows.reserve(rows.size());
    for (auto const& row : rows) {
        GenericRow cells;
        cells.reserve(row.size());
        for (auto const& cell : row) {
            cells.push_back(cellFromJson(cell.str, std::nullopt));
        }
        page.rows.push_back(std::move(cells));
    }
}

/// @brief Writes a page's rows, each cell by `appendCellJson`.
/// @param page The page.
/// @return The `rows` array as JSON.
[[nodiscard]] inline glz::raw_json writePageRows(GenericPage const& page) {
    glz::raw_json out;
    out.str += '[';
    for (std::size_t row = 0; row < page.rows.size(); ++row) {
        out.str += row == 0 ? "[" : ",[";
        auto const& cells = page.rows[row];
        for (std::size_t column = 0; column < cells.size(); ++column) {
            if (column > 0) {
                out.str += ',';
            }
            appendCellJson(out.str, cells[column]);
        }
        out.str += ']';
    }
    out.str += ']';
    return out;
}

}  // namespace detail

// NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)

}  // namespace morph::table

/// @brief The JSON of `morph::table::GenericPage`: its members in declaration
///        order, `rows` written and read cell by cell.
template <>
struct glz::meta<morph::table::GenericPage> {
    /// @brief The page type.
    using T = morph::table::GenericPage;
    /// @brief Each member after its JSON name.
    static constexpr auto value =
        glz::object("revision", &T::revision, "columns", &T::columns, "keys", &T::keys, "rows",
                    glz::custom<[](T& page, std::vector<std::vector<glz::raw_json_view>> const& rows) {
                        morph::table::detail::readPageRows(page, rows);
                    },
                                [](T const& page) { return morph::table::detail::writePageRows(page); }>,
                    "total", &T::total, "totalKind", &T::totalKind, "offset", &T::offset);
};

namespace morph::table {

// NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) -- cell and column indices are bounded by the page's own counts, checked before they are used

/// @brief Builds the generic page of @p page's rows.
///
/// Reads only the cells of @p columns, for the page's rows only, so a source
/// may produce every other cell lazily or not at all.
/// @param page    The page's source rows, total and offset, as `apply` returns them.
/// @param source  The rows; read through one snapshot.
/// @param columns The column ids to write, already ordered and without
///                duplicates (`validate` makes a query's list so). An id the
///                source lacks is written as empty cells.
/// @return The page, its `revision` left empty for the caller to set.
[[nodiscard]] inline GenericPage writeGenericPage(PageRows const& page, DataSource const& source,
                                                  std::span<std::string const> columns) {
    auto const snapshot = source.snapshot();
    auto const infos = source.columns();
    std::vector<std::optional<std::size_t>> indices;
    indices.reserve(columns.size());
    for (auto const& column : columns) {
        indices.push_back(detail::findColumn(infos, column));
    }
    GenericPage out;
    out.columns.assign(columns.begin(), columns.end());
    out.keys.reserve(page.rows.size());
    out.rows.reserve(page.rows.size());
    for (auto const row : page.rows) {
        out.keys.push_back(snapshot->rowId(row));
        GenericRow cells;
        cells.reserve(indices.size());
        for (auto const& index : indices) {
            cells.push_back(index ? snapshot->cell(row, *index) : Cell{});
        }
        out.rows.push_back(std::move(cells));
    }
    out.total = page.total;
    out.offset = page.offset;
    return out;
}

/// @brief Reads a generic page against its table's description.
///
/// Each cell is read by `detail::cellFromJson` with its column's kind. A cell
/// that does not read as its kind keeps the page and is an invalid cell; a
/// page whose structure is wrong is refused.
/// @param json        The page's JSON.
/// @param description The table's description.
/// @return The page, or `UnknownColumn` naming a `columns` entry the
///         description lacks, or `InvalidValue` when the text is not a page, a
///         row's length differs from `columns`, or `keys` and `rows` differ in
///         length.
[[nodiscard]] inline std::expected<GenericPage, TableError> readGenericPage(std::string_view json,
                                                                            TableDescription const& description) {
    auto const invalid = [](std::string message) {
        return std::unexpected(
            TableError{.code = TableErrorCode::InvalidValue, .column = {}, .message = std::move(message)});
    };
    GenericPage page;
    if (glz::read<glz::opts{.null_terminated = false, .error_on_unknown_keys = false}>(page, json)) {
        return invalid("not a generic page");
    }
    std::vector<ColumnKind> kinds;
    kinds.reserve(page.columns.size());
    for (auto const& id : page.columns) {
        auto const found =
            std::ranges::find(description.columns, id, [](TableColumn const& c) -> auto const& { return c.info.id; });
        if (found == description.columns.end()) {
            return std::unexpected(detail::unknownColumn(id));
        }
        kinds.push_back(found->info.kind);
    }
    if (page.keys.size() != page.rows.size()) {
        return invalid("a page of " + std::to_string(page.keys.size()) + " keys and " +
                       std::to_string(page.rows.size()) + " rows");
    }
    for (std::size_t row = 0; row < page.rows.size(); ++row) {
        if (page.rows[row].size() != kinds.size()) {
            return invalid("row " + std::to_string(row) + " has " + std::to_string(page.rows[row].size()) +
                           " cells under " + std::to_string(kinds.size()) + " columns");
        }
    }
    // The rows were read with no kind; only a `Number` column reads differently, taking an integer token or a
    // non-finite string as a double.
    for (std::size_t column = 0; column < kinds.size(); ++column) {
        if (kinds[column] != ColumnKind::Number) {
            continue;
        }
        for (auto& cells : page.rows) {
            auto& cell = cells[column];
            if (auto const* const integer = std::get_if<std::int64_t>(&cell)) {
                cell = static_cast<double>(*integer);
            } else if (auto const* const text = std::get_if<std::string>(&cell)) {
                if (auto const real = detail::nonFinite(*text)) {
                    cell = *real;
                }
            }
        }
    }
    return page;
}

// NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)

}  // namespace morph::table

// ── A model's tables ───────────────────────────────────────────────────────

namespace morph::table {

class LevelSource;

/// @brief Who asks for a table: what a table's function reads to decide what it serves.
struct TableContext {
    /// @brief The caller's session; an empty one outside a dispatch.
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

/// @brief One row of a table that an action changed.
struct ChangedRow {
    /// @brief The row's key.
    RowId key;

    /// @brief Member-wise equality.
    /// @param other The row to compare with.
    /// @return `true` when the keys match.
    [[nodiscard]] bool operator==(ChangedRow const& other) const = default;
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
/// Its JSON is `{"rows": [{"key": …}, …]}`. The result walk reports `TableOf<Function>` at its `rows` member,
/// keyed by `key`, so an action's changed rows are collected as any record's are.
/// @tparam Function The table's function, as `BRIDGE_REGISTER_TABLE` names it.
template <auto Function>
struct ChangedRows {
    /// @brief The rows changed, in any order.
    std::vector<ChangedRow> rows;

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
struct rowScoped {};

/// @brief Registration option: a change of any of @p Records is a change of the table's record.
/// @tparam Records Registered record types.
template <typename... Records>
struct derivedFrom {};

// NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) -- row indices are bounded by the snapshot's own counts

namespace detail {

/// @brief A source holding one snapshot: what a reply is built from off the owner's thread.
///
/// A `DataSource` is owner-affine and only its snapshot may cross threads, so the snapshot is taken on the
/// owner's thread and this source carries it, with a copy of the columns. It never changes.
class PinnedSource final : public DataSource {
public:
    /// @brief Holds @p snapshot under @p columns.
    /// @param snapshot The rows.
    /// @param columns  The source's columns when the snapshot was taken.
    PinnedSource(std::shared_ptr<RowSnapshot const> snapshot, std::vector<ColumnInfo> columns)
        : _snapshot{std::move(snapshot)}, _columns{std::move(columns)} {}

    /// @brief The columns.
    /// @return The copy taken with the snapshot.
    [[nodiscard]] std::span<ColumnInfo const> columns() const override { return _columns; }

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
    std::shared_ptr<RowSnapshot const> _snapshot;
    std::vector<ColumnInfo> _columns;
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
class ScopedSource final : public DataSource {
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

    /// @brief The scope's rows of the shared source's current snapshot.
    /// @return The snapshot.
    [[nodiscard]] std::shared_ptr<RowSnapshot const> snapshot() const override {
        auto base = _source->snapshot();
        std::scoped_lock const lock{_mutex};
        if (base != _base || _scoped == nullptr) {
            std::vector<std::size_t> rows;
            rows.reserve(std::min(_keys.size(), base->rowCount()));
            for (std::size_t row = 0; row < base->rowCount(); ++row) {
                if (_keys.contains(base->rowId(row))) {
                    rows.push_back(row);
                }
            }
            _scoped = std::make_shared<ScopedSnapshot const>(base, std::move(rows));
            _base = std::move(base);
        }
        return _scoped;
    }

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
struct AnswerContext {};

/// @brief @p served with its source replaced by a `PinnedSource` of one snapshot taken now.
///
/// Called on the source's owner thread, the model's strand, before the reply is built anywhere else.
/// @param served The table as the model served it.
/// @return The same table over a pinned source; a null source stays null.
[[nodiscard]] inline ServedTable pinned(ServedTable served) {
    if (served.source != nullptr) {
        auto const columns = served.source->columns();
        served.source = std::make_shared<PinnedSource const>(served.source->snapshot(),
                                                             std::vector<ColumnInfo>(columns.begin(), columns.end()));
    }
    return served;
}

/// @brief Answers @p query over a served table: the page a `Table.<id>` action returns.
///
/// The query is validated against the description's columns, its limits and `mayRead`; the page's columns
/// are the validated `columns`, or, absent, every description column `mayRead` allows, in the description's
/// order. Every description column must be one of the source's, or nothing is read. The rows are read from
/// the source's snapshot only.
/// @param served  The table, its source pinned (`pinned`).
/// @param query   The query, as received.
/// @param context What the reply is built with.
/// @return The page, its `revision` the description's, or the error the client shows.
[[nodiscard]] inline std::expected<GenericPage, TableError> answerQuery(ServedTable const& served,
                                                                        TableQuery const& query,
                                                                        AnswerContext const& context = {}) {
    static_cast<void>(context);
    if (served.source == nullptr) {
        return std::unexpected(
            TableError{.code = TableErrorCode::Unavailable, .column = {}, .message = "the table serves no source"});
    }
    auto const infos = columnInfos(served.description);
    auto checked = validate(query, infos, served.limits, served.mayRead);
    if (!checked) {
        return std::unexpected(std::move(checked.error()));
    }
    auto const sourceColumns = served.source->columns();
    for (auto const& info : infos) {
        if (!findColumn(sourceColumns, info.id)) {
            return std::unexpected(TableError{.code = TableErrorCode::UnknownColumn,
                                              .column = info.id,
                                              .message = "the table's source has no column " + info.id});
        }
    }
    std::vector<std::string> columns;
    if (checked->columns) {
        columns = std::move(*checked->columns);
    } else {
        columns.reserve(infos.size());
        for (auto const& info : infos) {
            if (!served.mayRead || served.mayRead(info.id)) {
                columns.push_back(info.id);
            }
        }
    }
    auto rows = apply(*served.source, *checked,
                      ApplyOptions{.limits = served.limits, .services = served.services, .mayRead = served.mayRead});
    if (!rows) {
        return std::unexpected(std::move(rows.error()));
    }
    auto page = writeGenericPage(*rows, *served.source, columns);
    page.revision = served.description.revision;
    return page;
}

/// @brief The name a `TableErrorCode` has on the wire.
/// @param code The code.
/// @return Its JSON name, as `unknownColumn`, without quotes.
[[nodiscard]] inline std::string errorCodeName(TableErrorCode code) {
    auto text = glz::write_json(code).value_or(std::string{});
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
        text = text.substr(1, text.size() - 2);
    }
    return text;
}

/// @brief The refusal a table's action throws for @p error.
/// @param error The error.
/// @return `ActionRefusal{"table." + code, {{"column", column}}, message}`.
[[nodiscard]] inline model::ActionRefusal refusalOf(TableError const& error) {
    return model::ActionRefusal{"table." + errorCodeName(error.code),
                                {model::RefusalArg{.name = "column", .value = error.column}},
                                error.message};
}

/// @brief The caller's `TableContext`: the session installed on the calling thread, or an empty one.
/// @return The context, valid while the session stays installed.
[[nodiscard]] inline TableContext callerContext() {
    static session::Context const empty{};
    auto const* const current = session::current();
    return TableContext{.session = current != nullptr ? *current : empty};
}

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
using TableRow = typename TableRowOf<std::remove_cvref_t<F>>::type;

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
/// @param name The record's wire name, `<model type>.<table id>`.
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
                auto cell = reader(row);
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
/// and each once. The key is the key member of the record registered for `Row`.
/// @tparam Row The row type.
/// @param id   The table's id.
/// @param rows The rows the caller may see; null serves none.
/// @return The served table.
/// @throws std::logic_error when `Row` is not a registered record with a key.
/// @throws std::invalid_argument when `Row::columnLevels()` names no column.
template <typename Row>
[[nodiscard]] ServedTable typedTable(std::string_view id, std::shared_ptr<std::vector<Row> const> rows) {
    auto const& records = effects::detail::resolved().records;
    auto const record = std::ranges::find(records, std::type_index{typeid(Row)}, &record::RecordInfo::type);
    if (record == records.end() || !record->keyField) {
        throw std::logic_error{"table " + std::string{id} + ": its row type is not a registered record with a key"};
    }
    auto key = rowKeyNamed<Row>(*record->keyField);
    if (!key) {
        throw std::logic_error{"table " + std::string{id} + ": its row type has no member " + *record->keyField};
    }
    ServedTable served;
    served.description.id = std::string{id};
    served.description.label = forms::detail::inferTitle(id);
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
    served.source = std::make_shared<RowsSource<Row> const>(std::move(rows), std::move(*key));
    return served;
}

/// @brief A table's `ServedTable`, from its model: the one way to it.
///
/// Calls the registered function on @p model, on the model's strand, with @p context; for a `typed<Row>` table
/// it builds the description and the source from the rows. The description is stamped with its revision.
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
        served = typedTable<typename Traits::row>(Traits::id, (model.*Function)(context));
    }
    served.description = describe(std::move(served.description));
    return served;
}

/// @brief The awaiter that builds a reply on the table executor and resumes the awaiting coroutine where it
///        was running.
///
/// The work item owns the pinned table and the query. The coroutine resumes through the executor it was
/// running on when it suspended, its strand, or inline on the worker when it was running on none.
class AnswerAwaiter {
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
    void await_suspend(std::coroutine_handle<> awaiting) {
        auto shared = _shared;
        shared->continuation = awaiting;
        shared->context = ::core::async::ResumeTarget::current();
        // Nothing of this awaiter is touched after the post: the work may resume the coroutine, and free the
        // awaiter with its frame, before `post` returns.
        auto* const work = _work;
        work->post([shared, served = std::move(_served), query = std::move(_query), context = _context] {
            try {
                shared->answer = answerQuery(served, query, context);
            } catch (...) {
                shared->error = std::current_exception();
            }
            if (shared->context) {
                shared->context.submit(::core::async::ParkedWork{.resume = shared->continuation, .abandon = {}});
            } else {
                shared->continuation.resume();
            }
        });
    }

    /// @brief The answer, or what building it threw.
    /// @return The answer.
    std::expected<GenericPage, TableError> await_resume() {
        if (_shared->error) {
            std::rethrow_exception(_shared->error);
        }
        return std::move(_shared->answer);
    }

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
    /// @throws morph::model::ActionRefusal `table.<code>` with the argument `column`, for a query the table
    ///         refuses.
    template <typename Model>
    static ::core::async::Task<GenericPage> invoke(Model& model, QueryTable& action);
};

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
    /// @return The description, its revision set.
    template <typename Model>
    static TableDescription invoke(Model& model, DescribeTable& action) {
        static_cast<void>(action);
        return describe(detail::serve<Function>(model, detail::callerContext()).description);
    }
};

template <auto Function>
template <typename Model>
::core::async::Task<GenericPage> QueryTable<Function>::invoke(Model& model, QueryTable& action) {
    // Read before the first suspension: a server installs the runtime for the dispatch's first step only.
    auto const* const runtime = currentTableRuntime();
    exec::IExecutor* const work = runtime != nullptr ? runtime->work : nullptr;
    detail::AnswerContext const context{};
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
