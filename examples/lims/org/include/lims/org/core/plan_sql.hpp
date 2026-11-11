// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <Lightweight/DataBinder/SqlVariant.hpp>
#include <Lightweight/DataMapper/DataMapper.hpp>
#include <Lightweight/SqlError.hpp>
#include <Lightweight/SqlStatement.hpp>
#include <cstdint>
#include <expected>
#include <glaze/glaze.hpp>
#include <map>
#include <morph/table/query.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "lims/org/core/errors.hpp"
#include "lims/org/core/model_support.hpp"

/// @file
/// Translates a `morph::table::QueryPlan` into SQLite for the lims models that page a table in the database.
///
/// The translation keeps the table engine's meaning of a query (docs/spec/table/database_pages.md): empty cells sort
/// after valid ones in either direction, no operator but `IsEmpty` and `NotEmpty` matches an empty cell, an exclude
/// keeps the rows whose cell is empty, integer operands are never rounded, a date-time is compared by its whole UTC
/// second, and text is compared and ordered by the default collator's fold. SQLite has no such fold, so it is spelled
/// out in the statement; a text column whose store comparison cannot be made to match is left out of the column map
/// and so refused.
///
/// The statement text is built from the model's own column mapping, enumerator names and the clamped numbers of the
/// plan; nothing a client sent is written into it. Every operand a client sent is a `?` in the statement and a
/// value in `Translation::params`, bound when the statement runs. A text operand holding a NUL byte is refused, since
/// SQLite's `length` and `instr` stop at it and would not compare what the table engine compares.

namespace lims::plansql {

/// @brief How one reflected column of a row type is read in SQL.
struct Column {
    /// @brief The kind the table engine gives the column: `Integer`, `DateTime` or `Text`.
    ::morph::table::ColumnKind kind = ::morph::table::ColumnKind::Integer;
    /// @brief The SQL expression of the cell, `NULL` when the cell is empty. An integer column's value is an integer,
    /// a
    ///        date-time column's value is whole UTC seconds, and a text column's value is the text, `NULL` for the
    ///        empty string.
    std::string expr;
};

/// @brief The translatable columns of a row type, by column id.
using Columns = std::map<std::string, Column, std::less<>>;

/// @brief A plan as the parts of a `SELECT`.
struct Translation {
    /// @brief The filter, a SQL boolean that is never `NULL`.
    std::string where;
    /// @brief The `ORDER BY` clause, total: it ends with the row key.
    std::string order;
    /// @brief The values of the `?` markers in `where`, in the order the markers appear.
    std::vector<Lightweight::SqlVariant> params;
};

/// @brief The ids a page holds and the size of the filtered view.
struct Fetched {
    /// @brief The page's row keys, in order.
    std::vector<std::int64_t> ids;
    /// @brief The rows in the whole filtered view.
    std::int64_t total = 0;
};

namespace detail {

/// @brief @p text as a SQL string literal, for text the code itself supplies (the fold's byte pairs and enumerator
///        names). An operand a client sent is never written this way; it is bound.
/// @param text The text, without a NUL byte.
/// @return The literal.
[[nodiscard]] inline std::string literal(std::string_view text) {
    std::string out = "'";
    for (char const character : text) {
        if (character == '\'') {
            out += '\'';
        }
        out += character;
    }
    out += '\'';
    return out;
}

/// @brief Collects the operands of a statement as it is written: each call returns the `?` marker and keeps the value.
///
/// The markers appear in the statement in the order the calls are made, so a caller sequences the calls in the order
/// the pieces are concatenated.
class Binds {
public:
    /// @brief Adds an integer operand.
    /// @param value The value.
    /// @return `?`.
    [[nodiscard]] std::string integer(std::int64_t value) {
        _values.emplace_back(Lightweight::SqlVariant::InnerType{std::in_place_type<long long>, value});
        return "?";
    }

    /// @brief Adds a text operand.
    /// @param value The text.
    /// @return `?`.
    [[nodiscard]] std::string text(std::string value) {
        _values.emplace_back(Lightweight::SqlVariant::InnerType{std::in_place_type<std::string>, std::move(value)});
        return "?";
    }

    /// @brief Hands over the collected values.
    /// @return They, in marker order.
    [[nodiscard]] std::vector<Lightweight::SqlVariant> take() { return std::move(_values); }

private:
    std::vector<Lightweight::SqlVariant> _values;
};

/// @brief The default collator's fold, as an SQL expression over text.
///
/// The collator lowers ASCII `A`-`Z` and Latin-1 `À`-`Þ` except `×`, and copies every other byte. SQLite's `lower`
/// lowers ASCII only, and each Latin-1 letter is a two-byte UTF-8 sequence `C3 80` to `C3 9E`, which `replace`
/// lowers one at a time. A SQLite built with ICU lowers more than the collator does; the conformance run in the
/// tests is what notices that.
/// @param expr The text expression.
/// @return The folded expression; `NULL` for `NULL`.
[[nodiscard]] inline std::string fold(std::string const& expr) {
    std::string out = "lower(" + expr + ")";
    for (int second = 0x80; second <= 0x9E; ++second) {
        if (second == 0x97) {
            continue;
        }
        std::string const upper{'\xC3', static_cast<char>(second)};
        std::string const lowered{'\xC3', static_cast<char>(second + 0x20)};
        out = "replace(" + out + ", " + literal(upper) + ", " + literal(lowered) + ")";
    }
    return out;
}

/// @brief Floor of @p numerator over @p denominator.
/// @param numerator The numerator.
/// @param denominator The denominator, positive.
/// @return The greatest integer not above the quotient.
[[nodiscard]] constexpr std::int64_t floorDivide(std::int64_t numerator, std::int64_t denominator) noexcept {
    std::int64_t quotient = numerator / denominator;
    if (numerator % denominator != 0 && numerator < 0) {
        --quotient;
    }
    return quotient;
}

/// @brief An integer column compared with an exact operand, without rounding the operand.
///
/// An integral operand compares as it is. A non-integral one lies between two integers: nothing equals it, and
/// "below" and "above" are decided by its floor.
/// @param op The operator: `Eq`, `Ne`, `Lt`, `Le`, `Gt` or `Ge`.
/// @param operand The operand.
/// @param expr The column's integer expression.
/// @param binds Receives the operand.
/// @return The SQL boolean, `NULL` for an empty cell.
[[nodiscard]] inline std::string integerCompare(::morph::table::FilterOp op, ::morph::math::Rational const& operand,
                                                std::string const& expr, Binds& binds) {
    using Op = ::morph::table::FilterOp;
    bool const whole = operand.denominator == 1;
    std::int64_t const floored = floorDivide(operand.numerator, operand.denominator);
    switch (op) {
        case Op::Eq:
            return whole ? "(" + expr + " = " + binds.integer(operand.numerator) + ")" : "0";
        case Op::Ne:
            return whole ? "(" + expr + " <> " + binds.integer(operand.numerator) + ")" : "(" + expr + " IS NOT NULL)";
        case Op::Lt:
            return whole ? "(" + expr + " < " + binds.integer(operand.numerator) + ")"
                         : "(" + expr + " <= " + binds.integer(floored) + ")";
        case Op::Le:
            return "(" + expr + " <= " + binds.integer(floored) + ")";
        case Op::Gt:
            return "(" + expr + " > " + binds.integer(floored) + ")";
        case Op::Ge:
            return whole ? "(" + expr + " >= " + binds.integer(operand.numerator) + ")"
                         : "(" + expr + " > " + binds.integer(floored) + ")";
        case Op::Between:
        case Op::Contains:
        case Op::StartsWith:
        case Op::EndsWith:
        case Op::IsEmpty:
        case Op::NotEmpty:
        default:
            return "0";
    }
}

/// @brief A text column compared with an operand.
/// @param condition The condition; its value is the folded operand, or the raw one when case-sensitive.
/// @param column The column.
/// @param binds Receives the operand.
/// @return The SQL boolean, `NULL` for an empty cell; or the error when the operand cannot be compared.
[[nodiscard]] inline std::expected<std::string, ::morph::table::TableError> textCondition(
    ::morph::table::PlannedCondition const& condition, Column const& column, Binds& binds) {
    using Op = ::morph::table::FilterOp;
    auto const* const needle = std::get_if<std::string>(&condition.value);
    if (needle == nullptr) {
        return std::unexpected(::morph::table::TableError{.code = ::morph::table::TableErrorCode::InvalidValue,
                                                          .column = condition.column,
                                                          .message = "a text condition has no text operand"});
    }
    if (needle->find('\0') != std::string::npos) {
        return std::unexpected(
            ::morph::table::TableError{.code = ::morph::table::TableErrorCode::InvalidValue,
                                       .column = condition.column,
                                       .message = "a text operand holds a NUL byte, which the store cannot compare"});
    }
    if (needle->empty()) {
        // Every non-empty cell contains, starts with and ends with the empty text; none equals it.
        return condition.op == Op::Eq ? std::string{"0"} : "(" + column.expr + " IS NOT NULL)";
    }
    std::string const cell = condition.caseSensitive ? column.expr : fold(column.expr);
    switch (condition.op) {
        case Op::Eq:
            return "(" + cell + " = " + binds.text(*needle) + ")";
        case Op::Ne:
            return "(" + cell + " <> " + binds.text(*needle) + ")";
        case Op::Contains:
            return "(instr(" + cell + ", " + binds.text(*needle) + ") > 0)";
        case Op::StartsWith: {
            // The markers are taken in the order they are written.
            std::string const length = binds.text(*needle);
            std::string const text = binds.text(*needle);
            return "(substr(" + cell + ", 1, length(" + length + ")) = " + text + ")";
        }
        case Op::EndsWith: {
            std::string const atLeast = binds.text(*needle);
            std::string const from = binds.text(*needle);
            std::string const text = binds.text(*needle);
            return "(length(" + cell + ") >= length(" + atLeast + ") AND substr(" + cell + ", -length(" + from +
                   ")) = " + text + ")";
        }
        case Op::Lt:
        case Op::Le:
        case Op::Gt:
        case Op::Ge:
        case Op::Between:
        case Op::IsEmpty:
        case Op::NotEmpty:
        default:
            return std::unexpected(
                ::morph::table::TableError{.code = ::morph::table::TableErrorCode::UnsupportedOperator,
                                           .column = condition.column,
                                           .message = "operator not defined for column " + condition.column});
    }
}

/// @brief One condition as a SQL boolean that is never `NULL`: true when the cell is present and meets it.
/// @param condition The condition.
/// @param columns The translatable columns.
/// @param binds Receives the operands.
/// @return The boolean, or the error naming the column.
[[nodiscard]] inline std::expected<std::string, ::morph::table::TableError> conditionSql(
    ::morph::table::PlannedCondition const& condition, Columns const& columns, Binds& binds) {
    using Op = ::morph::table::FilterOp;
    auto const found = columns.find(condition.column);
    if (found == columns.end()) {
        return std::unexpected(::morph::table::TableError{
            .code = ::morph::table::TableErrorCode::Unavailable,
            .column = condition.column,
            .message = "column " + condition.column + " cannot be filtered in the database"});
    }
    Column const& column = found->second;
    if (condition.op == Op::IsEmpty) {
        return "(" + column.expr + " IS NULL)";
    }
    if (condition.op == Op::NotEmpty) {
        return "(" + column.expr + " IS NOT NULL)";
    }
    std::string sql;
    switch (column.kind) {
        case ::morph::table::ColumnKind::Integer:
        case ::morph::table::ColumnKind::Key: {
            auto const* const value = std::get_if<::morph::math::Rational>(&condition.value);
            auto const* const upper = std::get_if<::morph::math::Rational>(&condition.upper);
            if (value == nullptr || (condition.op == Op::Between && upper == nullptr)) {
                return std::unexpected(::morph::table::TableError{.code = ::morph::table::TableErrorCode::InvalidValue,
                                                                  .column = condition.column,
                                                                  .message = "an integer condition has no operand"});
            }
            if (condition.op == Op::Between) {
                std::string const low = integerCompare(Op::Ge, *value, column.expr, binds);
                std::string const high = integerCompare(Op::Le, *upper, column.expr, binds);
                sql = "(" + low + " AND " + high + ")";
            } else {
                sql = integerCompare(condition.op, *value, column.expr, binds);
            }
            break;
        }
        case ::morph::table::ColumnKind::DateTime: {
            auto const* const value = std::get_if<std::int64_t>(&condition.value);
            auto const* const upper = std::get_if<std::int64_t>(&condition.upper);
            if (value == nullptr || (condition.op == Op::Between && upper == nullptr)) {
                return std::unexpected(::morph::table::TableError{.code = ::morph::table::TableErrorCode::InvalidValue,
                                                                  .column = condition.column,
                                                                  .message = "a date-time condition has no operand"});
            }
            switch (condition.op) {
                case Op::Eq:
                    sql = "(" + column.expr + " = " + binds.integer(*value) + ")";
                    break;
                case Op::Ne:
                    sql = "(" + column.expr + " <> " + binds.integer(*value) + ")";
                    break;
                case Op::Lt:
                    sql = "(" + column.expr + " < " + binds.integer(*value) + ")";
                    break;
                case Op::Le:
                    sql = "(" + column.expr + " <= " + binds.integer(*value) + ")";
                    break;
                case Op::Gt:
                    sql = "(" + column.expr + " > " + binds.integer(*value) + ")";
                    break;
                case Op::Ge:
                    sql = "(" + column.expr + " >= " + binds.integer(*value) + ")";
                    break;
                case Op::Between: {
                    std::string const low = binds.integer(*value);
                    std::string const high = binds.integer(*upper);
                    sql = "(" + column.expr + " BETWEEN " + low + " AND " + high + ")";
                    break;
                }
                case Op::Contains:
                case Op::StartsWith:
                case Op::EndsWith:
                case Op::IsEmpty:
                case Op::NotEmpty:
                default:
                    sql = "0";
                    break;
            }
            break;
        }
        case ::morph::table::ColumnKind::Text: {
            auto text = textCondition(condition, column, binds);
            if (!text) {
                return std::unexpected(std::move(text.error()));
            }
            sql = std::move(*text);
            break;
        }
        case ::morph::table::ColumnKind::Decimal:
        case ::morph::table::ColumnKind::Quantity:
        case ::morph::table::ColumnKind::Number:
        case ::morph::table::ColumnKind::Date:
        case ::morph::table::ColumnKind::Bool:
        case ::morph::table::ColumnKind::Custom:
        default:
            return std::unexpected(::morph::table::TableError{
                .code = ::morph::table::TableErrorCode::Unavailable,
                .column = condition.column,
                .message = "column " + condition.column + " cannot be filtered in the database"});
    }
    // A cell that is empty makes the comparison NULL, and an empty cell meets no condition.
    return "COALESCE(" + sql + ", 0)";
}

/// @brief One column's filter as a SQL boolean that is never `NULL`.
/// @param filter The filter.
/// @param columns The translatable columns.
/// @return The boolean, or the error naming the column.
[[nodiscard]] inline std::expected<std::string, ::morph::table::TableError> columnFilter(
    ::morph::table::PlannedColumnFilter const& filter, Columns const& columns, Binds& binds) {
    std::vector<std::string> parts;
    if (!filter.include.empty()) {
        std::string joined;
        for (auto const& entry : filter.include) {
            auto sql = conditionSql(entry, columns, binds);
            if (!sql) {
                return std::unexpected(std::move(sql.error()));
            }
            if (!joined.empty()) {
                joined += filter.combine == ::morph::table::Combine::All ? " AND " : " OR ";
            }
            joined += *sql;
        }
        parts.push_back("(" + joined + ")");
    }
    for (auto const& entry : filter.exclude) {
        auto sql = conditionSql(entry, columns, binds);
        if (!sql) {
            return std::unexpected(std::move(sql.error()));
        }
        // The row is rejected when the cell is present and meets the condition; an empty cell is kept.
        parts.push_back("NOT " + *sql);
    }
    std::string out;
    for (auto const& part : parts) {
        out += out.empty() ? part : " AND " + part;
    }
    return out.empty() ? std::string{"1"} : "(" + out + ")";
}

}  // namespace detail

/// @brief The SQL expression of an integer-valued enum column as the enumerator's name.
///
/// The table engine holds an enum as the name glaze writes for it, so a column filtered or ordered as text is mapped
/// to that name rather than ordered by its ordinal.
/// @tparam Enum An enumeration with a glaze name for each value in `[0, count)`.
/// @param column The column holding the ordinal.
/// @param count The number of enumerators.
/// @return The `CASE` expression; `NULL` for an ordinal outside the enumeration.
template <typename Enum>
[[nodiscard]] std::string enumName(std::string_view column, int count) {
    std::string out = "CASE " + std::string{column};
    for (int ordinal = 0; ordinal < count; ++ordinal) {
        std::string json;
        if (glz::write_json(static_cast<Enum>(ordinal), json)) {
            continue;
        }
        std::string_view name = json;
        name.remove_prefix(1);
        name.remove_suffix(1);
        out += " WHEN " + std::to_string(ordinal) + " THEN " + detail::literal(name);
    }
    return out + " END";
}

/// @brief The whole UTC second of an instant held in epoch milliseconds, as the table engine floors it.
/// @param column The column holding milliseconds.
/// @return The SQL expression.
[[nodiscard]] inline std::string wholeSeconds(std::string_view column) {
    std::string const ms{column};
    return "((" + ms + " - (((" + ms + " % 1000) + 1000) % 1000)) / 1000)";
}

/// @brief An integer column.
/// @param expr Its SQL expression, `NULL` when the cell is empty.
/// @return The column.
[[nodiscard]] inline Column integer(std::string expr) {
    return Column{.kind = ::morph::table::ColumnKind::Integer, .expr = std::move(expr)};
}

/// @brief A date-time column.
/// @param expr Its SQL expression in whole UTC seconds.
/// @return The column.
[[nodiscard]] inline Column dateTime(std::string expr) {
    return Column{.kind = ::morph::table::ColumnKind::DateTime, .expr = std::move(expr)};
}

/// @brief A text column.
/// @param expr Its SQL expression, `NULL` for the empty text.
/// @return The column.
[[nodiscard]] inline Column text(std::string expr) {
    return Column{.kind = ::morph::table::ColumnKind::Text, .expr = std::move(expr)};
}

/// @brief The text of a stored string column, with the empty string as an empty cell.
/// @param column The column.
/// @return The column.
[[nodiscard]] inline Column storedText(std::string_view column) {
    return text("NULLIF(" + std::string{column} + ", '')");
}

/// @brief Whether the database can filter and order by @p column.
/// @param columns The translatable columns.
/// @param column The column's id.
/// @return `true` when it is one of them.
[[nodiscard]] inline bool translatable(Columns const& columns, std::string_view column) {
    return columns.contains(column);
}

/// @brief Translates @p plan.
/// @param plan The validated plan.
/// @param columns The translatable columns; the plan names no other.
/// @param rowKey The SQL name of the unique row key, which ends the ordering ascending.
/// @return The filter and the ordering, or the error naming a column or operand that cannot be written.
[[nodiscard]] inline std::expected<Translation, ::morph::table::TableError> translate(
    ::morph::table::QueryPlan const& plan, Columns const& columns, std::string_view rowKey) {
    Translation out;
    detail::Binds binds;
    std::vector<std::string> clauses;
    for (auto const& filter : plan.filters) {
        auto sql = detail::columnFilter(filter, columns, binds);
        if (!sql) {
            return std::unexpected(std::move(sql.error()));
        }
        clauses.push_back(std::move(*sql));
    }
    for (auto const& group : plan.groups) {
        // A row meets a group when its entries are met on any of the group's columns; no usable column meets none.
        std::string any;
        for (auto const& filter : group.columns) {
            auto sql = detail::columnFilter(filter, columns, binds);
            if (!sql) {
                return std::unexpected(std::move(sql.error()));
            }
            any += any.empty() ? *sql : " OR " + *sql;
        }
        clauses.push_back(any.empty() ? std::string{"0"} : "(" + any + ")");
    }
    for (auto const& clause : clauses) {
        out.where += out.where.empty() ? clause : " AND " + clause;
    }
    if (out.where.empty()) {
        out.where = "1";
    }
    out.order = "ORDER BY ";
    for (auto const& key : plan.sort) {
        auto const found = columns.find(key.column);
        if (found == columns.end()) {
            return std::unexpected(
                ::morph::table::TableError{.code = ::morph::table::TableErrorCode::Unavailable,
                                           .column = key.column,
                                           .message = "column " + key.column + " cannot be ordered in the database"});
        }
        Column const& column = found->second;
        std::string const value =
            column.kind == ::morph::table::ColumnKind::Text ? detail::fold(column.expr) : column.expr;
        // Empty cells come last in either direction, then the value.
        out.order += "(" + column.expr + " IS NULL), " + value +
                     (key.dir == ::morph::table::SortDirection::Ascending ? " ASC, " : " DESC, ");
    }
    out.order += std::string{rowKey} + " ASC";
    out.params = binds.take();
    return out;
}

/// @brief Counts the filtered view and reads one page of its row keys, from one connection.
///
/// The caller opens a transaction on @p mapper before calling, so the count and the page see one state of the table.
/// @param mapper The open mapper.
/// @param table The table.
/// @param rowKey The unique row key column.
/// @param scope A SQL boolean the model's own scope (company, visibility) contributes; it holds no operand markers.
/// @param translation The translated plan.
/// @param plan The plan, for its offset and limit.
/// @return The count of the filtered view and the keys of the page; no keys when the limit is zero.
/// @throws Conflict if the database is busy or locked.
/// @throws Lightweight::SqlException if a statement fails for another reason.
[[nodiscard]] inline Fetched fetch(Lightweight::DataMapper& mapper, std::string_view table, std::string_view rowKey,
                                   std::string_view scope, Translation const& translation,
                                   ::morph::table::QueryPlan const& plan) {
    std::string const from =
        " FROM " + std::string{table} + " WHERE (" + std::string{scope} + ") AND " + translation.where;
    Fetched out;
    try {
        {
            Lightweight::SqlStatement counting{mapper.Connection()};
            counting.Prepare("SELECT COUNT(*)" + from);
            auto cursor = counting.ExecuteWithVariants(translation.params);
            if (cursor.FetchRow()) {
                out.total = cursor.GetColumn<std::int64_t>(1);
            }
        }
        if (plan.limit <= 0) {
            return out;
        }
        Lightweight::SqlStatement paging{mapper.Connection()};
        paging.Prepare("SELECT " + std::string{rowKey} + from + " " + translation.order + " LIMIT " +
                       std::to_string(plan.limit) + " OFFSET " + std::to_string(plan.offset));
        auto cursor = paging.ExecuteWithVariants(translation.params);
        while (cursor.FetchRow()) {
            out.ids.push_back(cursor.GetColumn<std::int64_t>(1));
        }
    } catch (Lightweight::SqlException const& error) {
        if (!isBusy(error)) {
            throw;
        }
        throw Conflict{"the database is busy with another write; try again"};
    }
    return out;
}

}  // namespace lims::plansql
