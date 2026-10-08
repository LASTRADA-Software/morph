// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file table/filter.hpp
/// @brief Filter specs as data, compiled once and evaluated column at a time.
///
/// A `FilterSpec` is plain data in the JSON shape of spec 7 §6, so a document
/// can hold it, a user can save it and a server can receive it. Compiling it
/// against a table's columns parses every value once for its column's kind;
/// evaluating it narrows a candidate list one column at a time.
///
/// Semantics, normative for every client and server:
/// - the `include` entries of a column combine by `combine` (`any`, the
///   default, or `all`); a match on an `exclude` entry rejects the row;
///   columns, and groups, combine with each other by AND;
/// - a group matches a row when its entries match on any of its columns;
/// - no operator but `isEmpty` and `notEmpty` matches an empty or invalid cell;
/// - text operators compare through the collator's primary-strength fold
///   unless the entry sets `caseSensitive`; `%` and `_` are ordinary characters.
///
/// See `docs/spec/table/engine.md`.

#include <algorithm>
#include <array>
#include <core/async/StopToken.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <morph/table/data_source.hpp>
#include <morph/table/sort.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace morph::table {

/// @brief One filter condition. Exactly one operator member is set.
///
/// Values are exact text, parsed for the column's kind when the filter is
/// compiled: `"1.5"` for a decimal, `"2026-10-08"` for a date, `"true"` for a
/// boolean.
struct FilterEntry {
    /// @brief Equal to (text: at primary strength unless `caseSensitive`).
    std::optional<std::string> eq;
    /// @brief Not equal to.
    std::optional<std::string> ne;
    /// @brief Less than. Numeric, date and date-time columns.
    std::optional<std::string> lt;
    /// @brief Less than or equal to. Numeric, date and date-time columns.
    std::optional<std::string> le;
    /// @brief Greater than. Numeric, date and date-time columns.
    std::optional<std::string> gt;
    /// @brief Greater than or equal to. Numeric, date and date-time columns.
    std::optional<std::string> ge;
    /// @brief Between two values, both inclusive. Numeric, date and date-time columns.
    std::optional<std::array<std::string, 2>> between;
    /// @brief Text contains. Text columns.
    std::optional<std::string> contains;
    /// @brief Text starts with. Text columns.
    std::optional<std::string> startsWith;
    /// @brief Text ends with. Text columns.
    std::optional<std::string> endsWith;
    /// @brief `true`: the cell is empty. Any column.
    std::optional<bool> isEmpty;
    /// @brief `true`: the cell is not empty. Any column.
    std::optional<bool> notEmpty;
    /// @brief Text operators compare the text as given rather than folded.
    std::optional<bool> caseSensitive;
};

/// @brief How a column's `include` entries combine.
enum class Combine : std::uint8_t {
    Any,  ///< A row matches when any entry matches.
    All,  ///< A row matches when every entry matches.
};

/// @brief The filter on one column.
struct ColumnFilter {
    /// @brief How `include` entries combine.
    Combine combine = Combine::Any;
    /// @brief Conditions a row must meet (combined by `combine`); empty means no condition.
    std::vector<FilterEntry> include;
    /// @brief Conditions that reject a row when any matches.
    std::vector<FilterEntry> exclude;
};

/// @brief A filter over several columns: a row matches when the entries match
///        on any of them, which is what a quick-search box needs.
struct GroupFilter {
    /// @brief The columns searched.
    std::vector<std::string> columns;
    /// @brief How `include` entries combine on one column.
    Combine combine = Combine::Any;
    /// @brief The conditions.
    std::vector<FilterEntry> include;
};

/// @brief A table's whole filter.
struct FilterSpec {
    /// @brief Per-column filters, by column id.
    std::map<std::string, ColumnFilter> columns;
    /// @brief Multi-column groups.
    std::vector<GroupFilter> groups;

    /// @brief Whether the spec filters nothing.
    /// @return `true` when it has no column filter and no group.
    [[nodiscard]] bool empty() const noexcept { return columns.empty() && groups.empty(); }
};

/// @brief A filter compiled against a table's columns: every value parsed.
class CompiledFilter {
public:
    /// @brief An operator.
    enum class Op : std::uint8_t {
        Eq,
        Ne,
        Lt,
        Le,
        Gt,
        Ge,
        Between,
        Contains,
        StartsWith,
        EndsWith,
        IsEmpty,
        NotEmpty
    };

    /// @brief One compiled condition.
    struct Entry {
        /// @brief The operator.
        Op op = Op::IsEmpty;
        /// @brief Text operators compare raw text.
        bool caseSensitive = false;
        /// @brief Integer operands (dates, date-times, booleans).
        std::array<std::int64_t, 2> ints{};
        /// @brief Exact operands (integers, keys, decimals, quantities).
        std::array<math::Rational, 2> exact{};
        /// @brief Floating-point operands (`Number`).
        std::array<double, 2> reals{};
        /// @brief Text operand: folded, or raw when `caseSensitive`.
        std::string text;
    };

    /// @brief One column's compiled filter.
    struct Column {
        /// @brief Column index.
        std::size_t column = 0;
        /// @brief How `include` combines.
        Combine combine = Combine::Any;
        /// @brief Compiled `include` entries.
        std::vector<Entry> include;
        /// @brief Compiled `exclude` entries.
        std::vector<Entry> exclude;
    };

    /// @brief One compiled group: per column, the entries that apply to it.
    struct Group {
        /// @brief The group's columns, each with the entries that compiled for it.
        std::vector<Column> columns;
    };

    /// @brief Whether the filter matches every row.
    /// @return `true` when there is nothing to evaluate.
    [[nodiscard]] bool empty() const noexcept { return _columns.empty() && _groups.empty(); }

    /// @brief The column indices whose keys evaluation reads.
    /// @return Sorted, unique column indices.
    [[nodiscard]] std::vector<std::size_t> columnsUsed() const {
        std::vector<std::size_t> out;
        for (auto const& column : _columns) {
            out.push_back(column.column);
        }
        for (auto const& group : _groups) {
            for (auto const& column : group.columns) {
                out.push_back(column.column);
            }
        }
        std::ranges::sort(out);
        auto const [first, last] = std::ranges::unique(out);
        out.erase(first, last);
        return out;
    }

    /// @brief The compiled column filters, in evaluation order.
    /// @return The filters.
    [[nodiscard]] std::span<Column const> columns() const noexcept { return _columns; }

    /// @brief The compiled groups, in evaluation order.
    /// @return The groups.
    [[nodiscard]] std::span<Group const> groups() const noexcept { return _groups; }

    /// @brief Whether a row passes one column filter.
    /// @param filter The column filter.
    /// @param keys   That column's keys.
    /// @param row    The row.
    /// @return `true` when the row passes.
    [[nodiscard]] static bool passes(Column const& filter, KeyColumn const& keys, std::uint32_t row) {
        if (!filter.include.empty()) {
            bool const all = filter.combine == Combine::All;
            bool result = all;
            for (auto const& entry : filter.include) {
                if (matches(entry, keys, row) != all) {
                    result = !all;
                    break;
                }
            }
            if (!result) {
                return false;
            }
        }
        return std::ranges::none_of(filter.exclude, [&](Entry const& entry) { return matches(entry, keys, row); });
    }

    /// @brief Whether one entry matches one cell.
    /// @param entry The entry.
    /// @param keys  The column's keys.
    /// @param row   The row.
    /// @return `true` on a match.
    [[nodiscard]] static bool matches(Entry const& entry, KeyColumn const& keys, std::uint32_t row) {
        auto const state = keys.states[row];
        if (entry.op == Op::IsEmpty) {
            return state == CellState::Empty;
        }
        if (entry.op == Op::NotEmpty) {
            return state != CellState::Empty;
        }
        if (state != CellState::Valid) {
            return false;
        }
        switch (keys.kind) {
            case ColumnKind::Integer:
            case ColumnKind::Key:
                return compare(entry, math::Rational{keys.ints[row], math::DecimalPlaces{0}}, entry.exact);
            case ColumnKind::Decimal:
            case ColumnKind::Quantity:
                return compare(entry, keys.exact[row], entry.exact);
            case ColumnKind::Number:
                return compare(entry, keys.reals[row], entry.reals);
            case ColumnKind::Date:
            case ColumnKind::DateTime:
            case ColumnKind::Bool:
                return compare(entry, keys.ints[row], entry.ints);
            case ColumnKind::Text:
                return matchText(entry, entry.caseSensitive ? keys.raw[row] : keys.folded[row]);
            case ColumnKind::Custom:
            default:
                return false;
        }
    }

private:
    friend std::expected<CompiledFilter, TableError> compileFilter(FilterSpec const& spec,
                                                                   std::span<ColumnInfo const> columns,
                                                                   Services const& services);

    template <typename T>
    [[nodiscard]] static bool compare(Entry const& entry, T const& value, std::array<T, 2> const& operand) {
        switch (entry.op) {
            case Op::Eq:
                return value == operand[0];
            case Op::Ne:
                return !(value == operand[0]);
            case Op::Lt:
                return value < operand[0];
            case Op::Le:
                return value <= operand[0];
            case Op::Gt:
                return value > operand[0];
            case Op::Ge:
                return value >= operand[0];
            case Op::Between:
                return operand[0] <= value && value <= operand[1];
            case Op::Contains:
            case Op::StartsWith:
            case Op::EndsWith:
            case Op::IsEmpty:
            case Op::NotEmpty:
            default:
                return false;
        }
    }

    [[nodiscard]] static bool matchText(Entry const& entry, std::string_view text) {
        switch (entry.op) {
            case Op::Eq:
                return text == entry.text;
            case Op::Ne:
                return text != entry.text;
            case Op::Contains:
                return text.find(entry.text) != std::string_view::npos;
            case Op::StartsWith:
                return text.starts_with(entry.text);
            case Op::EndsWith:
                return text.ends_with(entry.text);
            case Op::Lt:
            case Op::Le:
            case Op::Gt:
            case Op::Ge:
            case Op::Between:
            case Op::IsEmpty:
            case Op::NotEmpty:
            default:
                return false;
        }
    }

    std::vector<Column> _columns;
    std::vector<Group> _groups;
};

namespace detail {

/// @brief The operator an entry sets and its operand text.
struct RawEntry {
    /// @brief The operator.
    CompiledFilter::Op op = CompiledFilter::Op::IsEmpty;
    /// @brief Its operands; the second only for `between`.
    std::array<std::string_view, 2> values{};
};

/// @brief Reads which operator an entry sets.
/// @param entry  The entry.
/// @param column The column, for the error.
/// @return The operator and operands, or `InvalidSpec` when the entry sets none or several.
[[nodiscard]] inline std::expected<RawEntry, TableError> readEntry(FilterEntry const& entry, std::string_view column) {
    using Op = CompiledFilter::Op;
    RawEntry out;
    int set = 0;
    auto const take = [&](std::optional<std::string> const& member, Op op) {
        if (member) {
            ++set;
            out.op = op;
            out.values[0] = *member;
        }
    };
    take(entry.eq, Op::Eq);
    take(entry.ne, Op::Ne);
    take(entry.lt, Op::Lt);
    take(entry.le, Op::Le);
    take(entry.gt, Op::Gt);
    take(entry.ge, Op::Ge);
    take(entry.contains, Op::Contains);
    take(entry.startsWith, Op::StartsWith);
    take(entry.endsWith, Op::EndsWith);
    if (entry.between) {
        ++set;
        out.op = Op::Between;
        out.values = {(*entry.between)[0], (*entry.between)[1]};
    }
    if (entry.isEmpty.value_or(false)) {
        ++set;
        out.op = Op::IsEmpty;
    }
    if (entry.notEmpty.value_or(false)) {
        ++set;
        out.op = Op::NotEmpty;
    }
    if (set != 1) {
        return std::unexpected(TableError{.code = TableErrorCode::InvalidSpec,
                                          .column = std::string{column},
                                          .message = "a filter entry sets exactly one operator"});
    }
    return out;
}

/// @brief Whether a column kind defines an operator.
/// @param kind The column's kind.
/// @param op   The operator.
/// @return `true` when the operator applies to the kind.
[[nodiscard]] inline bool operatorDefined(ColumnKind kind, CompiledFilter::Op op) {
    using Op = CompiledFilter::Op;
    if (op == Op::IsEmpty || op == Op::NotEmpty) {
        return true;
    }
    bool const textOp = op == Op::Contains || op == Op::StartsWith || op == Op::EndsWith;
    switch (kind) {
        case ColumnKind::Text:
            return textOp || op == Op::Eq || op == Op::Ne;
        case ColumnKind::Bool:
            return op == Op::Eq || op == Op::Ne;
        case ColumnKind::Custom:
            return false;
        case ColumnKind::Integer:
        case ColumnKind::Key:
        case ColumnKind::Decimal:
        case ColumnKind::Quantity:
        case ColumnKind::Number:
        case ColumnKind::Date:
        case ColumnKind::DateTime:
        default:
            return !textOp;
    }
}

/// @brief Parses one operand for the column's kind into a slot of @p entry.
/// @param info     The column.
/// @param services Collator (text is folded with it) and date parser.
/// @param text     The operand text.
/// @param slot     0, or 1 for the upper bound of `between`.
/// @param entry    Receives the parsed operand.
/// @return `false` when the text does not parse for the kind.
[[nodiscard]] inline bool parseOperand(ColumnInfo const& info, Services const& services, std::string_view text,
                                       std::size_t slot, CompiledFilter::Entry& entry) {
    switch (info.kind) {
        case ColumnKind::Integer:
        case ColumnKind::Key:
        case ColumnKind::Decimal:
        case ColumnKind::Quantity: {
            auto const value = parseDecimal(text);
            if (!value) {
                return false;
            }
            entry.exact.at(slot) = *value;
            return true;
        }
        case ColumnKind::Number: {
            auto const value = parseReal(text);
            if (!value) {
                return false;
            }
            entry.reals.at(slot) = *value;
            return true;
        }
        case ColumnKind::Date:
        case ColumnKind::DateTime: {
            auto const& parser = *services.datesOrDefault();
            auto const value = info.kind == ColumnKind::Date ? parser.parseDate(text, info.format)
                                                             : parser.parseDateTime(text, info.format);
            if (!value) {
                return false;
            }
            entry.ints.at(slot) = *value;
            return true;
        }
        case ColumnKind::Bool:
            if (text != "true" && text != "false") {
                return false;
            }
            entry.ints.at(slot) = text == "true" ? 1 : 0;
            return true;
        case ColumnKind::Text:
            entry.text = entry.caseSensitive ? std::string{text} : services.collatorOrDefault()->fold(text);
            return true;
        case ColumnKind::Custom:
        default:
            return false;
    }
}

/// @brief Compiles one entry for one column.
/// @param entry    The entry.
/// @param info     The column.
/// @param services Collator and date parser.
/// @return The compiled entry, or the error naming the column.
[[nodiscard]] inline std::expected<CompiledFilter::Entry, TableError> compileEntry(FilterEntry const& entry,
                                                                                   ColumnInfo const& info,
                                                                                   Services const& services) {
    auto const raw = readEntry(entry, info.id);
    if (!raw) {
        return std::unexpected(raw.error());
    }
    if (!operatorDefined(info.kind, raw->op)) {
        return std::unexpected(TableError{.code = TableErrorCode::UnsupportedOperator,
                                          .column = info.id,
                                          .message = "operator not defined for column " + info.id});
    }
    CompiledFilter::Entry out;
    out.op = raw->op;
    out.caseSensitive = entry.caseSensitive.value_or(false);
    if (out.op == CompiledFilter::Op::IsEmpty || out.op == CompiledFilter::Op::NotEmpty) {
        return out;
    }
    std::size_t const operands = out.op == CompiledFilter::Op::Between ? 2 : 1;
    for (std::size_t slot = 0; slot < operands; ++slot) {
        if (!parseOperand(info, services, raw->values.at(slot), slot, out)) {
            return std::unexpected(TableError{.code = TableErrorCode::InvalidValue,
                                              .column = info.id,
                                              .message = "value \"" + std::string{raw->values.at(slot)} +
                                                         "\" does not parse for column " + info.id});
        }
    }
    return out;
}

/// @brief The index of the column with id @p id.
/// @param columns The table's columns.
/// @param id      A column id.
/// @return Its index, or nothing.
[[nodiscard]] inline std::optional<std::size_t> findColumn(std::span<ColumnInfo const> columns, std::string_view id) {
    for (std::size_t i = 0; i < columns.size(); ++i) {
        if (columns[i].id == id) {
            return i;
        }
    }
    return std::nullopt;
}

}  // namespace detail

/// @brief Compiles @p spec against @p columns.
///
/// In a group, an entry whose operator a column's kind does not define, or
/// whose value does not parse for it, does not apply to that column; it is an
/// error only when it applies to none of the group's columns. A quick-search
/// text therefore searches the text columns of a group that also holds numbers.
/// @param spec     The filter.
/// @param columns  The table's columns.
/// @param services The collator (values of text filters are folded with it)
///                 and date parser; null members mean their defaults.
/// @return The compiled filter, or the first error, naming its column.
[[nodiscard]] inline std::expected<CompiledFilter, TableError> compileFilter(FilterSpec const& spec,
                                                                             std::span<ColumnInfo const> columns,
                                                                             Services const& services) {
    CompiledFilter out;
    for (auto const& [id, filter] : spec.columns) {
        auto const index = detail::findColumn(columns, id);
        if (!index) {
            return std::unexpected(
                TableError{.code = TableErrorCode::UnknownColumn, .column = id, .message = "no column " + id});
        }
        CompiledFilter::Column compiled{.column = *index, .combine = filter.combine, .include = {}, .exclude = {}};
        for (auto const& entry : filter.include) {
            auto entryOut = detail::compileEntry(entry, columns[*index], services);
            if (!entryOut) {
                return std::unexpected(entryOut.error());
            }
            compiled.include.push_back(std::move(*entryOut));
        }
        for (auto const& entry : filter.exclude) {
            auto entryOut = detail::compileEntry(entry, columns[*index], services);
            if (!entryOut) {
                return std::unexpected(entryOut.error());
            }
            compiled.exclude.push_back(std::move(*entryOut));
        }
        if (!compiled.include.empty() || !compiled.exclude.empty()) {
            out._columns.push_back(std::move(compiled));
        }
    }
    for (auto const& group : spec.groups) {
        CompiledFilter::Group compiled;
        std::vector<std::size_t> appliesTo(group.include.size(), 0);
        std::optional<TableError> lastError;
        for (auto const& id : group.columns) {
            auto const index = detail::findColumn(columns, id);
            if (!index) {
                return std::unexpected(
                    TableError{.code = TableErrorCode::UnknownColumn, .column = id, .message = "no column " + id});
            }
            CompiledFilter::Column column{.column = *index, .combine = group.combine, .include = {}, .exclude = {}};
            for (std::size_t i = 0; i < group.include.size(); ++i) {
                auto entryOut = detail::compileEntry(group.include[i], columns[*index], services);
                if (!entryOut) {
                    if (entryOut.error().code == TableErrorCode::InvalidSpec) {
                        return std::unexpected(entryOut.error());
                    }
                    lastError = entryOut.error();
                    continue;
                }
                ++appliesTo[i];
                column.include.push_back(std::move(*entryOut));
            }
            // With `all`, a column that cannot evaluate every entry cannot
            // match; with `any`, one that evaluates none cannot either.
            bool const usable = group.combine == Combine::All ? column.include.size() == group.include.size()
                                                              : !column.include.empty();
            if (usable) {
                compiled.columns.push_back(std::move(column));
            }
        }
        for (std::size_t i = 0; i < appliesTo.size(); ++i) {
            if (appliesTo[i] == 0) {
                return std::unexpected(lastError.value_or(TableError{
                    .code = TableErrorCode::InvalidSpec, .column = {}, .message = "a filter group names no column"}));
            }
        }
        if (!group.include.empty()) {
            out._groups.push_back(std::move(compiled));
        }
    }
    return out;
}

/// @brief Evaluates a compiled filter over a candidate list, column at a time,
///        in steps that yield at a deadline or a stop request.
///
/// Each column filter, then each group, compacts the surviving candidates in
/// place, so later columns test fewer rows. The order of the candidates is
/// kept, which is how a filter over a cached sorted order stays sorted.
class FilterRun {
public:
    /// @brief Prepares to filter @p candidates.
    /// @param filter     The compiled filter; must outlive the run.
    /// @param keys       Key columns by column index; every index in
    ///                   `filter.columnsUsed()` must be non-null and outlive the run.
    /// @param candidates The rows to filter, in the order to keep.
    FilterRun(CompiledFilter const& filter, std::vector<KeyColumn const*> keys, std::vector<std::uint32_t> candidates)
        : _filter{&filter}, _keys{std::move(keys)}, _rows{std::move(candidates)}, _size{_rows.size()} {}

    /// @brief Filters until done, the deadline passes, or a stop is requested.
    /// @param deadline When to yield.
    /// @param stop     A stop request ends the step early.
    /// @return `true` when every stage has run.
    bool run(Deadline deadline, ::core::async::StopToken const& stop) {
        auto const stages = _filter->columns().size() + _filter->groups().size();
        while (_stage < stages) {
            while (_read < _size) {
                auto const end = std::min(_read + detail::kCheckEvery, _size);
                for (; _read < end; ++_read) {
                    auto const row = _rows[_read];
                    if (keep(row)) {
                        _rows[_write++] = row;
                    }
                }
                if (_read < _size && detail::shouldYield(deadline, stop)) {
                    return false;
                }
            }
            _size = _write;
            _read = 0;
            _write = 0;
            ++_stage;
            if (_stage < stages && detail::shouldYield(deadline, stop)) {
                return false;
            }
        }
        return true;
    }

    /// @brief The surviving rows. Valid once `run` returned `true`.
    /// @return The rows, moved out.
    [[nodiscard]] std::vector<std::uint32_t> take() && {
        _rows.resize(_size);
        return std::move(_rows);
    }

private:
    [[nodiscard]] bool keep(std::uint32_t row) const {
        auto const columns = _filter->columns();
        if (_stage < columns.size()) {
            auto const& column = columns[_stage];
            return CompiledFilter::passes(column, *_keys[column.column], row);
        }
        auto const& group = _filter->groups()[_stage - columns.size()];
        return std::ranges::any_of(group.columns, [&](CompiledFilter::Column const& column) {
            return CompiledFilter::passes(column, *_keys[column.column], row);
        });
    }

    CompiledFilter const* _filter;
    std::vector<KeyColumn const*> _keys;
    std::vector<std::uint32_t> _rows;
    std::size_t _size;
    std::size_t _stage = 0;
    std::size_t _read = 0;
    std::size_t _write = 0;
};

}  // namespace morph::table

/// @brief JSON names of `morph::table::Combine`: `any` and `all`.
template <>
struct glz::meta<morph::table::Combine> {
    using enum morph::table::Combine;
    /// @brief Each enumerator after its JSON name.
    static constexpr auto value = glz::enumerate("any", Any, "all", All);
};
