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
/// - on an unloaded cell every column filter is unknown, `isEmpty`, `notEmpty`
///   and `exclude` included: a row is rejected when any column filter or group
///   rejects it, otherwise undecided when any is unknown, otherwise admitted,
///   and a group admits a row as soon as one of its columns does;
/// - text operators compare through the collator's primary-strength fold
///   unless the entry sets `caseSensitive`; `%` and `_` are ordinary characters.
///
/// See `docs/spec/table/engine.md`.

#include <algorithm>
#include <array>
#include <core/async/StopToken.hpp>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <morph/table/data_source.hpp>
#include <morph/table/sort.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace morph::table {

// NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) -- row and column indices are bounded by the snapshot's own counts, and the sort and filter loops index once per comparison, where at() would check every access

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
    /// @brief Matches a cell whose value equals any of these. Each is parsed for the column's kind, as `eq`'s
    ///        operand is.
    std::optional<std::vector<std::string>> oneOf;
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

/// @brief What a filter says of a row whose cells may be unloaded.
enum class Match : std::uint8_t {
    No,       ///< Rejected by what is loaded.
    Yes,      ///< Admitted by what is loaded.
    Unknown,  ///< Neither, until an unloaded cell loads: the row is undecided.
};

/// @brief What evaluating a filter cost, counted when a caller passes one to `CompiledFilter::evaluate`.
struct FilterCounters {
    /// @brief Probes of a value set or of a group's value index: one per valid cell such an entry reads.
    std::uint64_t setLookups = 0;
    /// @brief Cells a group read.
    std::uint64_t cellReads = 0;
    /// @brief Augmenting-path searches a `Together` group started.
    std::uint64_t augmentations = 0;
};

namespace detail {

/// @brief A hash of an exact value: of its canonical numerator and denominator, which `Rational`'s equality
///        compares.
struct RationalHash {
    /// @brief Hashes @p value.
    /// @param value The value.
    /// @return The hash.
    [[nodiscard]] std::size_t operator()(math::Rational const& value) const noexcept {
        auto const numerator = std::hash<std::int64_t>{}(value.numerator);
        auto const denominator = std::hash<std::int64_t>{}(value.denominator);
        return numerator ^ (denominator + 0x9e3779b97f4a7c15ULL + (numerator << 6U) + (numerator >> 2U));
    }
};

/// @brief A transparent string hash, so a set of strings is probed with a view and nothing is copied.
struct TextHash {
    /// @brief Marks the hash transparent.
    using is_transparent = void;
    /// @brief Hashes @p text.
    /// @param text The text.
    /// @return The hash.
    [[nodiscard]] std::size_t operator()(std::string_view text) const noexcept {
        return std::hash<std::string_view>{}(text);
    }
};

}  // namespace detail

/// @brief The members of one `oneOf` entry, parsed for one column: a cell is matched by one lookup, whatever the
///        set's size.
///
/// Members are keyed as the column's cells are: integers, keys, decimals and quantities exactly, `Number` as a
/// double, dates, date-times and booleans as their integer, text by its fold, or as given when the entry sets
/// `caseSensitive`.
class ValueSet {
public:
    /// @brief An empty set for a column of @p kind.
    /// @param kind          The column's kind.
    /// @param caseSensitive Text members are kept as given rather than folded.
    explicit ValueSet(ColumnKind kind, bool caseSensitive = false) noexcept
        : _kind{kind}, _caseSensitive{caseSensitive} {}

    /// @brief Adds an integer member: a date, a date-time or a boolean.
    /// @param value The member.
    void add(std::int64_t value) { _ints.insert(value); }

    /// @brief Adds an exact member: an integer, a key, a decimal or a quantity.
    /// @param value The member.
    void add(math::Rational const& value) { _exact.insert(value); }

    /// @brief Adds a `Number` member; zero is one member whatever its sign.
    /// @param value The member.
    void add(double value) { _reals.insert(value == 0.0 ? 0.0 : value); }

    /// @brief Adds a text member, already folded unless the set is case-sensitive.
    /// @param value The member.
    void add(std::string value) { _texts.insert(std::move(value)); }

    /// @brief Whether a valid cell's value is a member: one probe of the set.
    /// @param keys     The column's keys; the cell at @p row is valid.
    /// @param row      The row.
    /// @param counters Counts the probe in `setLookups` when not null.
    /// @return `true` when the cell's value is in the set.
    [[nodiscard]] bool contains(KeyColumn const& keys, std::uint32_t row, FilterCounters* counters) const {
        if (counters != nullptr) {
            ++counters->setLookups;
        }
        switch (_kind) {
            case ColumnKind::Integer:
            case ColumnKind::Key:
                return _exact.contains(math::Rational{keys.ints[row], math::DecimalPlaces{0}});
            case ColumnKind::Decimal:
            case ColumnKind::Quantity:
                return _exact.contains(keys.exact[row]);
            case ColumnKind::Number: {
                auto const value = keys.reals[row];
                return _reals.contains(value == 0.0 ? 0.0 : value);
            }
            case ColumnKind::Date:
            case ColumnKind::DateTime:
            case ColumnKind::Bool:
                return _ints.contains(keys.ints[row]);
            case ColumnKind::Text:
                return _texts.contains(std::string_view{_caseSensitive ? keys.raw[row] : keys.folded[row]});
            case ColumnKind::Custom:
            default:
                return false;
        }
    }

    /// @brief The members, duplicates merged.
    /// @return The member count.
    [[nodiscard]] std::size_t size() const noexcept {
        return _ints.size() + _exact.size() + _reals.size() + _texts.size();
    }

private:
    ColumnKind _kind;
    bool _caseSensitive;
    std::unordered_set<std::int64_t> _ints;
    std::unordered_set<math::Rational, detail::RationalHash> _exact;
    std::unordered_set<double> _reals;
    std::unordered_set<std::string, detail::TextHash, std::equal_to<>> _texts;
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
        NotEmpty,
        OneOf
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
        /// @brief The operands as the filter spec gave them (`Between` uses both); empty for `IsEmpty`, `NotEmpty`
        ///        and `OneOf`.
        std::array<std::string, 2> operands;
        /// @brief `OneOf`'s members, parsed for the column; null for every other operator.
        std::shared_ptr<ValueSet const> set;
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
        out.reserve(_columns.size());
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

    /// @brief What one column filter says of a row.
    ///
    /// An unloaded cell is `Unknown` whatever the entries: whether it is empty,
    /// and whether an exclude entry matches it, is not known either.
    /// @param filter   The column filter.
    /// @param keys     That column's keys.
    /// @param row      The row.
    /// @param counters Counts the lookups of value sets when not null.
    /// @return `Unknown` on an unloaded cell, else `Yes` when the row passes and `No` when it does not.
    [[nodiscard]] static Match evaluate(Column const& filter, KeyColumn const& keys, std::uint32_t row,
                                        FilterCounters* counters = nullptr) {
        if (keys.states[row] == CellState::Unloaded) {
            return Match::Unknown;
        }
        if (!filter.include.empty()) {
            bool const all = filter.combine == Combine::All;
            bool result = all;
            for (auto const& entry : filter.include) {
                if (matches(entry, keys, row, counters) != all) {
                    result = !all;
                    break;
                }
            }
            if (!result) {
                return Match::No;
            }
        }
        bool const excluded = std::ranges::any_of(
            filter.exclude, [&](Entry const& entry) { return matches(entry, keys, row, counters); });
        return excluded ? Match::No : Match::Yes;
    }

    /// @brief Whether a row passes one column filter.
    /// @param filter The column filter.
    /// @param keys   That column's keys.
    /// @param row    The row.
    /// @return `true` when `evaluate` says `Yes`.
    [[nodiscard]] static bool passes(Column const& filter, KeyColumn const& keys, std::uint32_t row) {
        return evaluate(filter, keys, row) == Match::Yes;
    }

    /// @brief What one group says of a row: `Yes` when any of its columns says
    ///        `Yes`, otherwise `Unknown` when any says `Unknown`, otherwise `No`.
    /// @param group The group.
    /// @param keys  Key columns by column index; every column of the group must be non-null.
    /// @param row   The row.
    /// @return The group's answer.
    [[nodiscard]] static Match evaluateGroup(Group const& group, std::span<KeyColumn const* const> keys,
                                             std::uint32_t row) {
        auto out = Match::No;
        for (auto const& column : group.columns) {
            auto const match = evaluate(column, *keys[column.column], row);
            if (match == Match::Yes) {
                return Match::Yes;
            }
            if (match == Match::Unknown) {
                out = Match::Unknown;
            }
        }
        return out;
    }

    /// @brief What the whole filter says of a row: `No` when any column filter
    ///        or group says `No`, otherwise `Unknown` when any says `Unknown`,
    ///        otherwise `Yes`.
    /// @param keys Key columns by column index; every index in `columnsUsed()` must be non-null.
    /// @param row  The row.
    /// @return The row's answer.
    [[nodiscard]] Match evaluateRow(std::span<KeyColumn const* const> keys, std::uint32_t row) const {
        auto out = Match::Yes;
        for (auto const& column : _columns) {
            auto const match = evaluate(column, *keys[column.column], row);
            if (match == Match::No) {
                return Match::No;
            }
            if (match == Match::Unknown) {
                out = Match::Unknown;
            }
        }
        for (auto const& group : _groups) {
            auto const match = evaluateGroup(group, keys, row);
            if (match == Match::No) {
                return Match::No;
            }
            if (match == Match::Unknown) {
                out = Match::Unknown;
            }
        }
        return out;
    }

    /// @brief Whether one entry matches one cell.
    /// @param entry    The entry.
    /// @param keys     The column's keys.
    /// @param row      The row.
    /// @param counters Counts a value set's lookup when not null.
    /// @return `true` on a match.
    [[nodiscard]] static bool matches(Entry const& entry, KeyColumn const& keys, std::uint32_t row,
                                      FilterCounters* counters = nullptr) {
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
        if (entry.op == Op::OneOf) {
            return entry.set != nullptr && entry.set->contains(keys, row, counters);
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
            case Op::OneOf:
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
                return text.contains(entry.text);
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
            case Op::OneOf:
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
    if (entry.oneOf) {
        ++set;
        out.op = Op::OneOf;
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
    if (op == Op::OneOf) {
        return operatorDefined(kind, Op::Eq);
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

/// @brief Parses the members of a `oneOf` entry for one column into a set.
///
/// Each member parses as `eq`'s operand does. On an integer or key column a member that is not a whole number is
/// left out: it can never match, as `eq "1.5"` cannot.
/// @param members       The members, as the filter spec gave them.
/// @param info          The column.
/// @param services      Collator (text is folded with it) and date parser.
/// @param caseSensitive Text members are kept as given.
/// @return The set, or `InvalidValue` naming the column when a member does not parse.
[[nodiscard]] inline std::expected<ValueSet, TableError> compileValueSet(std::span<std::string const> members,
                                                                         ColumnInfo const& info,
                                                                         Services const& services,
                                                                         bool caseSensitive) {
    ValueSet out{info.kind, caseSensitive};
    CompiledFilter::Entry parsed;
    parsed.caseSensitive = caseSensitive;
    for (auto const& member : members) {
        if (!parseOperand(info, services, member, 0, parsed)) {
            return std::unexpected(TableError{.code = TableErrorCode::InvalidValue,
                                              .column = info.id,
                                              .message = "value \"" + member + "\" does not parse for column " +
                                                         info.id});
        }
        switch (info.kind) {
            case ColumnKind::Integer:
            case ColumnKind::Key:
                if (parsed.exact[0].isInteger()) {
                    out.add(parsed.exact[0]);
                }
                break;
            case ColumnKind::Decimal:
            case ColumnKind::Quantity:
                out.add(parsed.exact[0]);
                break;
            case ColumnKind::Number:
                out.add(parsed.reals[0]);
                break;
            case ColumnKind::Date:
            case ColumnKind::DateTime:
            case ColumnKind::Bool:
                out.add(parsed.ints[0]);
                break;
            case ColumnKind::Text:
                out.add(std::move(parsed.text));
                break;
            case ColumnKind::Custom:
            default:
                break;
        }
    }
    return out;
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
    if (out.op == CompiledFilter::Op::OneOf) {
        auto set = compileValueSet(*entry.oneOf, info, services, out.caseSensitive);
        if (!set) {
            return std::unexpected(std::move(set.error()));
        }
        out.set = std::make_shared<ValueSet const>(std::move(*set));
        return out;
    }
    std::size_t const operands = out.op == CompiledFilter::Op::Between ? 2 : 1;
    for (std::size_t slot = 0; slot < operands; ++slot) {
        out.operands.at(slot) = std::string{raw->values.at(slot)};
        if (!parseOperand(info, services, raw->values.at(slot), slot, out)) {
            return std::unexpected(TableError{.code = TableErrorCode::InvalidValue,
                                              .column = info.id,
                                              .message = "value \"" + std::string{raw->values.at(slot)} +
                                                         "\" does not parse for column " + info.id});
        }
    }
    return out;
}

/// @brief The index of the column with id @p columnId.
/// @param columns  The table's columns.
/// @param columnId A column id.
/// @return Its index, or nothing.
[[nodiscard]] inline std::optional<std::size_t> findColumn(std::span<ColumnInfo const> columns,
                                                           std::string_view columnId) {
    for (std::size_t i = 0; i < columns.size(); ++i) {
        if (columns[i].id == columnId) {
            return i;
        }
    }
    return std::nullopt;
}

/// @brief An `UnknownColumn` error.
/// @param columnId The column id that names no column.
/// @return The error.
[[nodiscard]] inline TableError unknownColumn(std::string const& columnId) {
    return TableError{.code = TableErrorCode::UnknownColumn, .column = columnId, .message = "no column " + columnId};
}

/// @brief Compiles entries for one column, appending them to @p out.
/// @param entries  The entries.
/// @param info     The column.
/// @param services Collator and date parser.
/// @param out      Receives the compiled entries.
/// @return Nothing, or the first entry's error.
[[nodiscard]] inline std::expected<void, TableError> compileEntries(std::span<FilterEntry const> entries,
                                                                    ColumnInfo const& info, Services const& services,
                                                                    std::vector<CompiledFilter::Entry>& out) {
    for (auto const& entry : entries) {
        auto compiled = compileEntry(entry, info, services);
        if (!compiled) {
            return std::unexpected(std::move(compiled.error()));
        }
        out.push_back(std::move(*compiled));
    }
    return {};
}

/// @brief Compiles the filter on one column.
/// @param columnId The column's id.
/// @param filter   Its filter.
/// @param columns  The table's columns.
/// @param services Collator and date parser.
/// @return The compiled filter, or the first error.
[[nodiscard]] inline std::expected<CompiledFilter::Column, TableError> compileColumnFilter(
    std::string const& columnId, ColumnFilter const& filter, std::span<ColumnInfo const> columns,
    Services const& services) {
    auto const index = findColumn(columns, columnId);
    if (!index) {
        return std::unexpected(unknownColumn(columnId));
    }
    CompiledFilter::Column out{.column = *index, .combine = filter.combine, .include = {}, .exclude = {}};
    if (auto included = compileEntries(filter.include, columns[*index], services, out.include); !included) {
        return std::unexpected(std::move(included.error()));
    }
    if (auto excluded = compileEntries(filter.exclude, columns[*index], services, out.exclude); !excluded) {
        return std::unexpected(std::move(excluded.error()));
    }
    return out;
}

/// @brief Compiles a group: per column, the entries that apply to it.
/// @param group    The group.
/// @param columns  The table's columns.
/// @param services Collator and date parser.
/// @return The compiled group, or an error when a column is unknown, an entry
///         is malformed, or an entry applies to none of the group's columns.
[[nodiscard]] inline std::expected<CompiledFilter::Group, TableError> compileGroup(GroupFilter const& group,
                                                                                   std::span<ColumnInfo const> columns,
                                                                                   Services const& services) {
    CompiledFilter::Group out;
    std::vector<std::size_t> appliesTo(group.include.size(), 0);
    std::optional<TableError> lastError;
    for (auto const& columnId : group.columns) {
        auto const index = findColumn(columns, columnId);
        if (!index) {
            return std::unexpected(unknownColumn(columnId));
        }
        CompiledFilter::Column column{.column = *index, .combine = group.combine, .include = {}, .exclude = {}};
        for (std::size_t i = 0; i < group.include.size(); ++i) {
            auto compiled = compileEntry(group.include[i], columns[*index], services);
            if (compiled) {
                ++appliesTo[i];
                column.include.push_back(std::move(*compiled));
            } else if (compiled.error().code == TableErrorCode::InvalidSpec) {
                return std::unexpected(std::move(compiled.error()));
            } else {
                lastError = std::move(compiled.error());
            }
        }
        // With `all`, a column that cannot evaluate every entry cannot
        // match; with `any`, one that evaluates none cannot either.
        bool const usable =
            group.combine == Combine::All ? column.include.size() == group.include.size() : !column.include.empty();
        if (usable) {
            out.columns.push_back(std::move(column));
        }
    }
    if (std::ranges::find(appliesTo, std::size_t{0}) != appliesTo.end()) {
        return std::unexpected(lastError.value_or(TableError{
            .code = TableErrorCode::InvalidSpec, .column = {}, .message = "a filter group names no column"}));
    }
    return out;
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
    for (auto const& [columnId, filter] : spec.columns) {
        auto compiled = detail::compileColumnFilter(columnId, filter, columns, services);
        if (!compiled) {
            return std::unexpected(std::move(compiled.error()));
        }
        if (!compiled->include.empty() || !compiled->exclude.empty()) {
            out._columns.push_back(std::move(*compiled));
        }
    }
    for (auto const& group : spec.groups) {
        auto compiled = detail::compileGroup(group, columns, services);
        if (!compiled) {
            return std::unexpected(std::move(compiled.error()));
        }
        if (!group.include.empty()) {
            out._groups.push_back(std::move(*compiled));
        }
    }
    return out;
}

/// @brief `compileFilter`, with an exception from the collator or the date parser returned as `ServiceFailed`.
///
/// The one compile step `apply` and `Engine` share, so a throwing service is an error value in both.
/// @param spec     The filter.
/// @param columns  The table's columns.
/// @param services The collator and date parser; null members mean their defaults.
/// @return The compiled filter, or the first error.
[[nodiscard]] inline std::expected<CompiledFilter, TableError> compileFilterGuarded(
    FilterSpec const& spec, std::span<ColumnInfo const> columns, Services const& services) {
    auto const failed = [](std::string const& what) {
        return std::unexpected(
            TableError{.code = TableErrorCode::ServiceFailed, .column = {}, .message = "a service failed: " + what});
    };
    try {
        return compileFilter(spec, columns, services);
    } catch (std::exception const& error) {
        return failed(error.what());
    } catch (...) {
        return failed("unknown exception");
    }
}

/// @brief Evaluates a compiled filter over a candidate list, column at a time,
///        in steps that yield at a deadline or a stop request.
///
/// Each column filter, then each group, compacts the surviving candidates in
/// place, so later columns test fewer rows. The order of the candidates is
/// kept, which is how a filter over a cached sorted order stays sorted.
///
/// A stage that answers `No` drops a row; one that answers `Unknown` keeps it
/// marked, and a later stage may still drop it. The unmarked survivors are the
/// view (`take`), the marked ones the undecided rows (`takeUndecided`).
class FilterRun {
public:
    /// @brief Prepares to filter @p candidates.
    /// @param filter     The compiled filter; must outlive the run.
    /// @param keys       Key columns by column index; every index in
    ///                   `filter.columnsUsed()` must be non-null and outlive the run.
    /// @param candidates The rows to filter, in the order to keep.
    FilterRun(CompiledFilter const& filter, std::vector<KeyColumn const*> keys, std::vector<std::uint32_t> candidates)
        : _filter{&filter},
          _keys{std::move(keys)},
          _rows{std::move(candidates)},
          _marks(_rows.size(), std::uint8_t{0}),
          _size{_rows.size()} {}

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
                    auto const match = stage(row);
                    if (match != Match::No) {
                        _marks[_write] = match == Match::Unknown ? std::uint8_t{1} : _marks[_read];
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

    /// @brief The rows every stage admitted. Valid once `run` returned `true`;
    ///        may be called once, before or after `takeUndecided`.
    /// @return The rows, in candidate order.
    [[nodiscard]] std::vector<std::uint32_t> take() && { return survivors(0); }

    /// @brief The rows no stage rejected and some stage could not decide.
    ///        Valid once `run` returned `true`; may be called once, before or after `take`.
    /// @return The rows, in candidate order.
    [[nodiscard]] std::vector<std::uint32_t> takeUndecided() && { return survivors(1); }

private:
    [[nodiscard]] Match stage(std::uint32_t row) const {
        auto const columns = _filter->columns();
        if (_stage < columns.size()) {
            auto const& column = columns[_stage];
            return CompiledFilter::evaluate(column, *_keys[column.column], row);
        }
        return CompiledFilter::evaluateGroup(_filter->groups()[_stage - columns.size()], _keys, row);
    }

    // The survivors whose mark is `mark`: 0 for the admitted rows, 1 for the undecided ones.
    [[nodiscard]] std::vector<std::uint32_t> survivors(std::uint8_t mark) const {
        std::vector<std::uint32_t> out;
        for (std::size_t i = 0; i < _size; ++i) {
            if (_marks[i] == mark) {
                out.push_back(_rows[i]);
            }
        }
        return out;
    }

    CompiledFilter const* _filter;
    std::vector<KeyColumn const*> _keys;
    std::vector<std::uint32_t> _rows;
    std::vector<std::uint8_t> _marks;  // 1 once a stage answered Unknown; compacted with `_rows`
    std::size_t _size;
    std::size_t _stage = 0;
    std::size_t _read = 0;
    std::size_t _write = 0;
};

// NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)

}  // namespace morph::table

/// @brief JSON names of `morph::table::Combine`: `any` and `all`.
template <>
struct glz::meta<morph::table::Combine> {
    using enum morph::table::Combine;
    /// @brief Each enumerator after its JSON name.
    static constexpr auto value = glz::enumerate("any", Any, "all", All);
};
