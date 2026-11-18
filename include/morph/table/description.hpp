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
/// See `docs/spec/table/table.md`, "A table is a description" and "Rows on the wire".

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <glaze/glaze.hpp>
#include <limits>
#include <morph/core/payload_schema.hpp>
#include <morph/table/data_source.hpp>
#include <morph/table/query.hpp>
#include <morph/util/rational.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
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
/// Empty is `null`; an integer, a string and a boolean are themselves; a
/// `double` is written by `appendDouble`; a `Rational` is a pair by
/// `appendRational`; a `QuantityCell` is `[n, d, [fn, fd]]`, its amount and
/// factor each written as a `Rational`, or the plain pair of its amount when
/// the factor is 1.
/// @param out  The text to append to.
/// @param cell The cell.
inline void appendCellJson(std::string& out, Cell const& cell) {
    std::visit(
        [&out]<typename T>(T const& value) {
            if constexpr (std::is_same_v<T, std::monostate>) {
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
