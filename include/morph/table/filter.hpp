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
/// - a group counts the columns whose cell satisfies its entries against its
///   quantifier (`any` by default), or in `Together` mode finds each entry in a
///   column of its own; a match on one of its `exclude` entries rejects the row;
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
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
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

    /// @brief Member-wise equality.
    /// @param other The entry to compare with.
    /// @return `true` when every member matches.
    [[nodiscard]] bool operator==(FilterEntry const& other) const = default;
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

    /// @brief Member-wise equality.
    /// @param other The filter to compare with.
    /// @return `true` when every member matches.
    [[nodiscard]] bool operator==(ColumnFilter const& other) const = default;
};

/// @brief How a group's `include` entries are found among its columns.
enum class GroupMode : std::uint8_t {
    Columns,   ///< Count the columns whose cell satisfies the entries, against `match`.
    Together,  ///< Every entry is found, each in a column of its own.
};

/// @brief How many of a group's columns must satisfy its entries, in `Columns` mode.
///
/// Its JSON is `"any"`, `"all"`, `{"atLeast": n}` or `{"exactly": n}`. Any other shape reads as `atLeast -1`, which
/// every group refuses as malformed, in either mode.
struct ColumnCount {
    /// @brief A quantifier.
    enum class Kind : std::uint8_t {
        Any,      ///< At least one column.
        All,      ///< Every column.
        AtLeast,  ///< At least `n` columns.
        Exactly,  ///< Exactly `n` columns.
    };
    /// @brief The quantifier.
    Kind kind = Kind::Any;
    /// @brief The count, for `AtLeast` and `Exactly`.
    std::int64_t n = 1;

    /// @brief The JSON shape: a quantifier's name, or an object of one quantifier and its count.
    using Wire = std::variant<std::string, std::map<std::string, std::int64_t>>;

    /// @brief Reads the JSON shape.
    /// @param wire The shape as read.
    void setWire(Wire const& wire) {
        *this = ColumnCount{.kind = Kind::AtLeast, .n = -1};
        if (auto const* const name = std::get_if<std::string>(&wire)) {
            if (*name == "any" || *name == "all") {
                *this = ColumnCount{.kind = *name == "any" ? Kind::Any : Kind::All, .n = 1};
            }
            return;
        }
        auto const& counted = std::get<std::map<std::string, std::int64_t>>(wire);
        if (counted.size() != 1) {
            return;
        }
        auto const& [name, count] = *counted.begin();
        if (name == "atLeast" || name == "exactly") {
            *this = ColumnCount{.kind = name == "atLeast" ? Kind::AtLeast : Kind::Exactly, .n = count};
        }
    }

    /// @brief The JSON shape.
    /// @return `"any"` or `"all"`, or `{"atLeast": n}` or `{"exactly": n}`.
    [[nodiscard]] Wire getWire() const {
        switch (kind) {
            case Kind::All:
                return std::string{"all"};
            case Kind::AtLeast:
                return std::map<std::string, std::int64_t>{{"atLeast", n}};
            case Kind::Exactly:
                return std::map<std::string, std::int64_t>{{"exactly", n}};
            case Kind::Any:
            default:
                return std::string{"any"};
        }
    }

    /// @brief Member-wise equality.
    /// @param other The count to compare with.
    /// @return `true` when the kind and the count match.
    [[nodiscard]] bool operator==(ColumnCount const& other) const = default;
};

/// @brief A filter over several columns, named by a declared group or listed: in `Columns` mode a row passes when
///        the number of columns whose cell satisfies `include` meets `match`; in `Together` mode when each `include`
///        entry matches a column of its own. A match on an `exclude` entry in any column rejects the row.
///
/// A filter that sets only `columns`, `combine` and `include` is the plain group: the entries match on any of the
/// columns, which is what a quick-search box needs.
struct GroupFilter {
    /// @brief A declared group's id; or empty, and `columns` lists the columns.
    std::string group;
    /// @brief The columns, when `group` is empty.
    std::vector<std::string> columns;
    /// @brief How `include` entries are found among the columns.
    GroupMode mode = GroupMode::Columns;
    /// @brief `Columns` mode: how many columns must satisfy `include`.
    ColumnCount match;
    /// @brief `Columns` mode: how one column's cell must satisfy `include`.
    Combine combine = Combine::Any;
    /// @brief The conditions; empty means the group's `exclude` alone decides.
    std::vector<FilterEntry> include;
    /// @brief Conditions that reject a row when any matches in any of the columns.
    std::vector<FilterEntry> exclude;

    /// @brief Member-wise equality.
    /// @param other The filter to compare with.
    /// @return `true` when every member matches.
    [[nodiscard]] bool operator==(GroupFilter const& other) const = default;
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

    /// @brief Member-wise equality.
    /// @param other The filter to compare with.
    /// @return `true` when every column filter and group matches.
    [[nodiscard]] bool operator==(FilterSpec const& other) const = default;
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
    /// @brief Augmenting-path searches a `Together` group started over its known cells.
    std::uint64_t augmentations = 0;
    /// @brief Augmenting-path searches a `Together` group started with its unknown columns matching every entry.
    std::uint64_t wildcardAugmentations = 0;
};

namespace detail {

/// @brief The golden-ratio constant a hash combine mixes in, cut to the width of `std::size_t`.
inline constexpr auto kHashSeed = static_cast<std::size_t>(0x9e3779b97f4a7c15ULL);

/// @brief A hash of an exact value: of its canonical numerator and denominator, which `Rational`'s equality
///        compares.
struct RationalHash {
    /// @brief Hashes @p value.
    /// @param value The value.
    /// @return The hash.
    [[nodiscard]] std::size_t operator()(math::Rational const& value) const noexcept {
        auto const numerator = std::hash<std::int64_t>{}(value.numerator);
        auto const denominator = std::hash<std::int64_t>{}(value.denominator);
        return numerator ^ (denominator + kHashSeed + (numerator << 6U) + (numerator >> 2U));
    }
};

/// @brief A measure's key: its dimension's place in the column and its amount in the dimension's base unit.
struct MeasureKey {
    /// @brief The dimension's place.
    std::uint32_t place = 0;
    /// @brief The base amount.
    math::Rational amount;

    /// @brief Member-wise equality, the amount by value.
    /// @param other The key to compare with.
    /// @return `true` when the places and the amounts are equal.
    [[nodiscard]] bool operator==(MeasureKey const& other) const = default;
};

/// @brief A hash of a measure's key: of its place and its amount's canonical numerator and denominator.
struct MeasureKeyHash {
    /// @brief Hashes @p key.
    /// @param key The key.
    /// @return The hash.
    [[nodiscard]] std::size_t operator()(MeasureKey const& key) const noexcept {
        auto const amount = RationalHash{}(key.amount);
        return amount ^ (std::hash<std::uint32_t>{}(key.place) + kHashSeed + (amount << 6U) + (amount >> 2U));
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
/// `caseSensitive`, and a measure by its dimension's place and its base amount.
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

    /// @brief Adds a measure member.
    /// @param place  Its dimension's place in the column.
    /// @param amount Its amount in the dimension's base unit.
    void add(std::uint32_t place, math::Rational const& amount) {
        _measures.insert(detail::MeasureKey{.place = place, .amount = amount});
    }

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
            case ColumnKind::Measure:
                return _measures.contains(detail::MeasureKey{.place = keys.places[row], .amount = keys.exact[row]});
            case ColumnKind::Custom:
            default:
                return false;
        }
    }

    /// @brief The members, duplicates merged.
    /// @return The member count.
    [[nodiscard]] std::size_t size() const noexcept {
        return _ints.size() + _exact.size() + _reals.size() + _texts.size() + _measures.size();
    }

private:
    ColumnKind _kind;
    bool _caseSensitive;
    std::unordered_set<std::int64_t> _ints;
    std::unordered_set<math::Rational, detail::RationalHash> _exact;
    std::unordered_set<double> _reals;
    std::unordered_set<std::string, detail::TextHash, std::equal_to<>> _texts;
    std::unordered_set<detail::MeasureKey, detail::MeasureKeyHash> _measures;
};

/// @brief A group column's value entries, by value: one probe gives the bits of every `eq` and `oneOf` entry the
///        cell's value matches.
///
/// Keyed as a `ValueSet` is; text by its fold, so only entries that are not `caseSensitive` are indexed, and a measure
/// by its dimension's place and its base amount, so equal amounts of two dimensions are two values.
class ValueIndex {
public:
    /// @brief An empty index for a column of @p kind.
    /// @param kind The column's kind.
    explicit ValueIndex(ColumnKind kind = ColumnKind::Text) noexcept : _kind{kind} {}

    /// @brief Adds @p bits to a date, date-time or boolean value.
    /// @param value The value.
    /// @param bits  The entries' bits.
    void add(std::int64_t value, std::uint32_t bits) { _ints[value] |= bits; }

    /// @brief Adds @p bits to an exact value: an integer, a key, a decimal or a quantity.
    /// @param value The value.
    /// @param bits  The entries' bits.
    void add(math::Rational const& value, std::uint32_t bits) { _exact[value] |= bits; }

    /// @brief Adds @p bits to a `Number` value; zero is one value whatever its sign.
    /// @param value The value.
    /// @param bits  The entries' bits.
    void add(double value, std::uint32_t bits) { _reals[value == 0.0 ? 0.0 : value] |= bits; }

    /// @brief Adds @p bits to a folded text.
    /// @param value The text, folded.
    /// @param bits  The entries' bits.
    void add(std::string const& value, std::uint32_t bits) {
        if (auto const found = _texts.find(std::string_view{value}); found != _texts.end()) {
            found->second |= bits;
        } else {
            _texts.emplace(value, bits);
        }
    }

    /// @brief Adds @p bits to a measure.
    /// @param place  Its dimension's place in the column.
    /// @param amount Its amount in the dimension's base unit.
    /// @param bits   The entries' bits.
    void add(std::uint32_t place, math::Rational const& amount, std::uint32_t bits) {
        _measures[detail::MeasureKey{.place = place, .amount = amount}] |= bits;
    }

    /// @brief Whether the index holds no value.
    /// @return `true` when nothing was added.
    [[nodiscard]] bool empty() const noexcept {
        return _ints.empty() && _exact.empty() && _reals.empty() && _texts.empty() && _measures.empty();
    }

    /// @brief The bits of the entries a valid cell's value matches: one probe.
    /// @param keys     The column's keys; the cell at @p row is valid.
    /// @param row      The row.
    /// @param counters Counts the probe in `setLookups` when not null.
    /// @return The bits; zero when no entry matches.
    [[nodiscard]] std::uint32_t lookup(KeyColumn const& keys, std::uint32_t row, FilterCounters* counters) const {
        if (counters != nullptr) {
            ++counters->setLookups;
        }
        auto const countIn = [](auto const& map, auto const& key) -> std::uint32_t {
            auto const found = map.find(key);
            return found == map.end() ? 0U : found->second;
        };
        switch (_kind) {
            case ColumnKind::Integer:
            case ColumnKind::Key:
                return countIn(_exact, math::Rational{keys.ints[row], math::DecimalPlaces{0}});
            case ColumnKind::Decimal:
            case ColumnKind::Quantity:
                return countIn(_exact, keys.exact[row]);
            case ColumnKind::Number: {
                auto const value = keys.reals[row];
                return countIn(_reals, value == 0.0 ? 0.0 : value);
            }
            case ColumnKind::Date:
            case ColumnKind::DateTime:
            case ColumnKind::Bool:
                return countIn(_ints, keys.ints[row]);
            case ColumnKind::Text:
                return countIn(_texts, std::string_view{keys.folded[row]});
            case ColumnKind::Measure:
                return countIn(_measures, detail::MeasureKey{.place = keys.places[row], .amount = keys.exact[row]});
            case ColumnKind::Custom:
            default:
                return 0U;
        }
    }

private:
    ColumnKind _kind;
    std::unordered_map<std::int64_t, std::uint32_t> _ints;
    std::unordered_map<math::Rational, std::uint32_t, detail::RationalHash> _exact;
    std::unordered_map<double, std::uint32_t> _reals;
    std::unordered_map<std::string, std::uint32_t, detail::TextHash, std::equal_to<>> _texts;
    std::unordered_map<detail::MeasureKey, std::uint32_t, detail::MeasureKeyHash> _measures;
};

class CompiledFilter;

/// @brief Compiles @p spec against @p columns.
///
/// In a group, an entry whose operator a column's kind does not define, or
/// whose value does not parse for it, does not apply to that column; it is an
/// error only when it applies to none of the group's columns. A quick-search
/// text therefore searches the text columns of a group that also holds numbers.
/// A group with no include and no exclude entry compiles to nothing.
/// @param spec     The filter.
/// @param columns  The table's columns.
/// @param services The collator (values of text filters are folded with it)
///                 and date parser; null members mean their defaults.
/// @param groups   The table's declared groups, which a group filter's `group` names.
/// @return The compiled filter, or the first error, naming its column.
[[nodiscard]] inline std::expected<CompiledFilter, TableError> compileFilter(
    FilterSpec const& spec, std::span<ColumnInfo const> columns, Services const& services,
    std::span<ColumnGroupInfo const> groups = {});

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
        /// @brief Exact operands (integers, keys, decimals, quantities; a measure's base amount).
        std::array<math::Rational, 2> exact{};
        /// @brief A measure operand's dimension place in the column; `Between`'s two are equal.
        std::array<std::uint32_t, 2> places{};
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

    /// @brief How one column of a group evaluates its entries: which entry each is, and the value index.
    struct GroupColumn {
        /// @brief The bit of each of the column's include entries: its place in the group's `include`.
        std::vector<std::uint32_t> includeBits;
        /// @brief The bit of each of the column's exclude entries: its place in the group's `exclude`.
        std::vector<std::uint32_t> excludeBits;
        /// @brief The include entries matched by value (`eq` and `oneOf`, not case-sensitive).
        ValueIndex include;
        /// @brief The exclude entries matched by value.
        ValueIndex exclude;
        /// @brief The include entries the index does not hold, by their place in `Column::include`: tested one by
        ///        one.
        std::vector<std::size_t> includeTested;
        /// @brief The exclude entries the index does not hold, by their place in `Column::exclude`.
        std::vector<std::size_t> excludeTested;
        /// @brief Every bit of `includeBits`.
        std::uint32_t compiled = 0;
        /// @brief Whether the column can satisfy the include entries in `Columns` mode: with `combine: all` it
        ///        compiled every one, with `any` at least one. A column that cannot is a known *no*.
        bool usable = false;
    };

    /// @brief One compiled group: per column, the entries that apply to it.
    struct Group {
        /// @brief The group's columns that compiled at least one entry, each with those entries, in the group's
        ///        order.
        std::vector<Column> columns;
        /// @brief Per entry of `columns`, its bits and value index.
        std::vector<GroupColumn> index;
        /// @brief The columns the group names, whether or not an entry compiled on them.
        std::size_t k = 0;
        /// @brief How the include entries are found.
        GroupMode mode = GroupMode::Columns;
        /// @brief `Columns` mode's quantifier.
        ColumnCount match;
        /// @brief How one column's cell satisfies the include entries in `Columns` mode.
        Combine combine = Combine::Any;
        /// @brief The group's include entries.
        std::size_t includeCount = 0;
        /// @brief The group's exclude entries.
        std::size_t excludeCount = 0;
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

    /// @brief What one group says of a row, three-valued over its unloaded cells.
    ///
    /// Each column's cell is read once, and gives the bits of the include entries it matches: for value entries
    /// one probe of the column's index, a condition tested per entry. An unloaded cell's column is unknown. In
    /// `Columns` mode, with y the columns that satisfy, q the unknown ones and r those not read yet, reading stops
    /// as soon as the quantifier is decided: `any` is `Yes` at y ≥ 1 and `No` at y + q + r = 0; `all` `Yes` at
    /// y = k and `No` at y + q + r < k; `atLeast n` `Yes` at y ≥ n and `No` at y + q + r < n; `exactly n` `Yes` at
    /// y = n with q = r = 0 and `No` at y > n or y + q + r < n; otherwise `Unknown`. In `Together` mode every
    /// column is read; a row whose known cells leave an entry unmatched and has no unknown column is `No` with no
    /// matching tried; otherwise a matching of entries to distinct columns over the known cells is `Yes`, and
    /// failing that one that counts each unknown column as matching every entry compiled on it is `Unknown`, and
    /// none is `No`. A known cell matching an exclude entry makes the group `No`; otherwise an unknown column with
    /// exclude entries makes it `Unknown`. A group with exclude entries reads every column.
    /// @param group    The group.
    /// @param keys     Key columns by column index; every column of the group must be non-null.
    /// @param row      The row.
    /// @param counters Counts cell reads, index lookups and augmentations when not null.
    /// @return The group's answer.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): a per-row hot path: one pass over the group's columns that decides early, kept in one function so the state stays in registers.
    [[nodiscard]] static Match evaluateGroup(Group const& group, std::span<KeyColumn const* const> keys,
                                             std::uint32_t row, FilterCounters* counters = nullptr) {
        constexpr std::size_t kMaxTogether = 16;
        bool const together = group.mode == GroupMode::Together;
        bool const hasExclude = group.excludeCount > 0;
        auto include = group.includeCount == 0 ? std::optional{Match::Yes} : std::optional<Match>{};
        std::int64_t y = 0;
        std::int64_t unloaded = 0;
        std::int64_t remaining = 0;
        for (auto const& column : group.index) {
            remaining += column.usable ? 1 : 0;
        }
        auto const k = static_cast<std::int64_t>(group.k);
        // Yes once `met`, No once `lost`, otherwise still open.
        auto const settle = [](bool met, bool lost) -> std::optional<Match> {
            if (met) {
                return Match::Yes;
            }
            if (lost) {
                return Match::No;
            }
            return std::nullopt;
        };
        auto const decide = [&] {
            auto const needed = group.match.n;
            switch (group.match.kind) {
                case ColumnCount::Kind::All:
                    return settle(y == k, y + unloaded + remaining < k);
                case ColumnCount::Kind::AtLeast:
                    return settle(y >= needed, y + unloaded + remaining < needed);
                case ColumnCount::Kind::Exactly:
                    if (y > needed || y + unloaded + remaining < needed) {
                        return std::optional{Match::No};
                    }
                    return y == needed && unloaded == 0 && remaining == 0 ? std::optional{Match::Yes}
                                                                          : std::optional<Match>{};
                case ColumnCount::Kind::Any:
                default:
                    return settle(y >= 1, y + unloaded + remaining == 0);
            }
        };
        if (!include && !together) {
            include = decide();
            if (include == Match::No) {
                return Match::No;
            }
        }
        std::array<std::uint32_t, kMaxTogether> masks{};
        std::uint32_t unknownColumns = 0;
        bool excludeUnknown = false;
        for (std::size_t at = 0; at < group.columns.size(); ++at) {
            if (include && !hasExclude) {
                break;
            }
            auto const& column = group.columns[at];
            auto const& index = group.index[at];
            auto const& cells = *keys[column.column];
            if (counters != nullptr) {
                ++counters->cellReads;
            }
            bool const unknown = cells.states[row] == CellState::Unloaded;
            if (!include && together) {
                if (unknown) {
                    unknownColumns |= 1U << at;
                } else {
                    masks.at(at) = entryBits(column.include, index.includeBits, index.include, index.includeTested,
                                             cells, row, counters);
                }
            } else if (!include && index.usable) {
                --remaining;
                if (unknown) {
                    ++unloaded;
                } else {
                    auto const bits = entryBits(column.include, index.includeBits, index.include, index.includeTested,
                                                cells, row, counters);
                    bool const satisfies = group.combine == Combine::All ? bits == index.compiled : bits != 0;
                    y += satisfies ? 1 : 0;
                }
                include = decide();
                if (include == Match::No) {
                    return Match::No;
                }
            }
            if (hasExclude && !index.excludeBits.empty()) {
                if (unknown) {
                    excludeUnknown = true;
                } else if (entryBits(column.exclude, index.excludeBits, index.exclude, index.excludeTested, cells, row,
                                     counters) != 0) {
                    return Match::No;
                }
            }
        }
        if (!include && together) {
            include = matchTogether(group, std::span{masks}.first(group.columns.size()), unknownColumns, counters);
            if (include == Match::No) {
                return Match::No;
            }
        }
        if (!include || *include == Match::Unknown || excludeUnknown) {
            return Match::Unknown;
        }
        return Match::Yes;
    }

private:
    /// @brief The bits of the entries of one group column that one cell matches.
    /// @param entries The column's compiled entries.
    /// @param bits    Each entry's bit.
    /// @param index   The value index over the entries it holds.
    /// @param tested  The entries the index does not hold, by place in @p entries.
    /// @param keys    The column's keys.
    /// @param row     The row; its cell is loaded.
    /// @param counters Counts the index probe when not null.
    /// @return The bits; an empty or invalid cell matches only `isEmpty` or `notEmpty` entries, with no probe.
    [[nodiscard]] static std::uint32_t entryBits(std::span<Entry const> entries, std::span<std::uint32_t const> bits,
                                                 ValueIndex const& index, std::span<std::size_t const> tested,
                                                 KeyColumn const& keys, std::uint32_t row, FilterCounters* counters) {
        std::uint32_t out = 0;
        if (keys.states[row] != CellState::Valid) {
            for (std::size_t at = 0; at < entries.size(); ++at) {
                if (matches(entries[at], keys, row)) {
                    out |= bits[at];
                }
            }
            return out;
        }
        if (!index.empty()) {
            out = index.lookup(keys, row, counters);
        }
        for (auto const column : tested) {
            if (matches(entries[column], keys, row, counters)) {
                out |= bits[column];
            }
        }
        return out;
    }

    /// @brief `Together` mode's answer once every column is read.
    /// @param group          The group.
    /// @param masks          Each column's include bits; zero for an unknown column.
    /// @param unknownColumns The unknown columns, as bits of their places in `group.columns`.
    /// @param counters       Counts the known pass's augmentations in `augmentations` and the pass with the unknown
    ///                       columns matching every entry in `wildcardAugmentations`, when not null.
    /// @return `Yes` when a matching over the known cells covers every entry; `Unknown` when one does only with
    ///         every unknown column matching every entry compiled on it; `No` otherwise.
    [[nodiscard]] static Match matchTogether(Group const& group, std::span<std::uint32_t> masks,
                                             std::uint32_t unknownColumns, FilterCounters* counters) {
        auto const entries = group.includeCount;
        std::uint32_t const full = entries >= 32 ? ~0U : (1U << entries) - 1U;
        std::uint32_t covered = 0;
        for (auto const mask : masks) {
            covered |= mask;
        }
        if (covered == full && cover(masks, entries, counters)) {
            return Match::Yes;
        }
        if (unknownColumns == 0) {
            return Match::No;
        }
        covered = 0;
        std::size_t usable = 0;
        for (std::size_t at = 0; at < masks.size(); ++at) {
            if (((unknownColumns >> at) & 1U) != 0U) {
                masks[at] = group.index[at].compiled;
            }
            covered |= masks[at];
            usable += masks[at] != 0 ? 1U : 0U;
        }
        // Each entry needs a column of its own, so fewer columns that match any entry than there are entries
        // cannot be matched (Hall's condition on the column side). The unknown columns' masks are full, so the
        // covered test alone passes on nearly every row, and the search would then run a failing augmenting-path
        // search for up to every entry of every such row.
        if (covered != full || usable < entries) {
            return Match::No;
        }
        return cover(masks, entries, counters, true) ? Match::Unknown : Match::No;
    }

    /// @brief Whether every entry can be given a column of its own among @p masks: augmenting paths, one search an
    ///        entry.
    /// @param masks    Each column's entry bits.
    /// @param entries  The entries to cover, bits `0..entries-1`.
    /// @param counters Counts each search when not null.
    /// @param wildcard The search counts in `wildcardAugmentations` rather than `augmentations`.
    /// @return `true` when a matching covers every entry.
    [[nodiscard]] static bool cover(std::span<std::uint32_t const> masks, std::size_t entries,
                                    FilterCounters* counters, bool wildcard = false) {
        constexpr std::size_t kMaxTogether = 16;
        std::array<int, kMaxTogether> owner{};
        owner.fill(-1);
        for (std::size_t entry = 0; entry < entries; ++entry) {
            if (counters != nullptr) {
                ++(wildcard ? counters->wildcardAugmentations : counters->augmentations);
            }
            std::uint32_t visited = 0;
            if (!augment(static_cast<int>(entry), masks, owner, visited)) {
                return false;
            }
        }
        return true;
    }

    /// @brief One augmenting-path search from @p entry: a free column that matches it first, so a dense row is
    ///        matched without following owners; only then an owned one whose owner can move.
    /// @param entry   The entry to place.
    /// @param masks   Each column's entry bits.
    /// @param owner   The entry each column holds, or -1.
    /// @param visited The columns this search has visited.
    /// @return `true` when @p entry was placed.
    // NOLINTNEXTLINE(misc-no-recursion): the augmenting-path search of a bipartite matching; the path is at most one step per column.
    [[nodiscard]] static bool augment(int entry, std::span<std::uint32_t const> masks, std::span<int> owner,
                                      std::uint32_t& visited) {
        auto const bit = static_cast<unsigned>(entry);
        for (std::size_t column = 0; column < masks.size(); ++column) {
            if (((masks[column] >> bit) & 1U) != 0 && owner[column] < 0 && ((visited >> column) & 1U) == 0) {
                visited |= 1U << column;
                owner[column] = entry;
                return true;
            }
        }
        for (std::size_t column = 0; column < masks.size(); ++column) {
            if (((masks[column] >> bit) & 1U) == 0 || ((visited >> column) & 1U) != 0) {
                continue;
            }
            visited |= 1U << column;
            if (owner[column] < 0 || augment(owner[column], masks, owner, visited)) {
                owner[column] = entry;
                return true;
            }
        }
        return false;
    }

public:
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
            case ColumnKind::Measure:
                // `ne` matches a value of another dimension, which is not equal to the operand; every other
                // comparison is false across dimensions, whose amounts do not compare.
                if (entry.op == Op::Ne) {
                    return keys.places[row] != entry.places[0] || !(keys.exact[row] == entry.exact[0]);
                }
                return keys.places[row] == entry.places[0] && compare(entry, keys.exact[row], entry.exact);
            case ColumnKind::Custom:
            default:
                return false;
        }
    }

private:
    friend std::expected<CompiledFilter, TableError> compileFilter(FilterSpec const& spec,
                                                                   std::span<ColumnInfo const> columns,
                                                                   Services const& services,
                                                                   std::span<ColumnGroupInfo const> groups);

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
// NOLINTNEXTLINE(misc-no-recursion): OneOf is defined as Eq is, one call deep.
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
        case ColumnKind::Measure:
        default:
            return !textOp;
    }
}

/// @brief Parses one operand for the column's kind into a slot of @p entry.
///
/// A `Measure` operand is a decimal, or a decimal, a space and a unit id (`detail::splitMeasureOperand`); its unit is
/// live in `services.units`, its dimension allowed by the column, and its base amount fits.
/// @param info     The column.
/// @param services Collator (text is folded with it), date parser and units.
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
        case ColumnKind::Measure: {
            auto const operand = splitMeasureOperand(text);
            if (!operand || services.units == nullptr) {
                return false;
            }
            auto const& registry = *services.units;
            auto const unit = registry.find(operand->second);
            if (!unit) {
                return false;
            }
            auto const place = dimensionPlace(info, registry, registry.info(*unit).dimension);
            auto const base = math::checkedMul(operand->first, registry.info(*unit).toBase);
            if (place < 0 || !base) {
                return false;
            }
            entry.exact.at(slot) = *base;
            entry.places.at(slot) = static_cast<std::uint32_t>(place);
            return true;
        }
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
            return std::unexpected(
                TableError{.code = TableErrorCode::InvalidValue,
                           .column = info.id,
                           .message = "value \"" + member + "\" does not parse for column " + info.id});
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
            case ColumnKind::Measure:
                out.add(parsed.places[0], parsed.exact[0]);
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
/// @param services Collator, date parser and units.
/// @return The compiled entry, or the error naming the column: `SourceContract` for a `Measure` column without a
///         unit registry, and `InvalidValue` for a `between` whose two measure operands are of two dimensions.
[[nodiscard]] inline std::expected<CompiledFilter::Entry, TableError> compileEntry(FilterEntry const& entry,
                                                                                   ColumnInfo const& info,
                                                                                   Services const& services) {
    auto const raw = readEntry(entry, info.id);
    if (!raw) {
        return std::unexpected(raw.error());
    }
    if (auto missing = unitsMissing(info, services)) {
        return std::unexpected(std::move(*missing));
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
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access): an entry whose operator is OneOf carries its values.
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
    if (info.kind == ColumnKind::Measure && out.op == CompiledFilter::Op::Between && out.places[0] != out.places[1]) {
        return std::unexpected(TableError{.code = TableErrorCode::InvalidValue,
                                          .column = info.id,
                                          .message = "a between on column " + info.id + " spans two dimensions"});
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

/// @brief Adds a compiled `eq` or `oneOf` entry's values to a group column's value index.
/// @param entry  The compiled entry; not case-sensitive.
/// @param raw    The entry as the spec gave it (a `oneOf`'s members are parsed again for the index).
/// @param info   The column.
/// @param services Collator and date parser.
/// @param bit    The entry's bit.
/// @param index  The index to add to.
inline void indexEntry(CompiledFilter::Entry const& entry, FilterEntry const& raw, ColumnInfo const& info,
                       Services const& services, std::uint32_t bit, ValueIndex& index) {
    auto const add = [&](CompiledFilter::Entry const& parsed) {
        switch (info.kind) {
            case ColumnKind::Integer:
            case ColumnKind::Key:
                if (parsed.exact[0].isInteger()) {
                    index.add(parsed.exact[0], bit);
                }
                break;
            case ColumnKind::Decimal:
            case ColumnKind::Quantity:
                index.add(parsed.exact[0], bit);
                break;
            case ColumnKind::Number:
                index.add(parsed.reals[0], bit);
                break;
            case ColumnKind::Date:
            case ColumnKind::DateTime:
            case ColumnKind::Bool:
                index.add(parsed.ints[0], bit);
                break;
            case ColumnKind::Text:
                index.add(parsed.text, bit);
                break;
            case ColumnKind::Measure:
                index.add(parsed.places[0], parsed.exact[0], bit);
                break;
            case ColumnKind::Custom:
            default:
                break;
        }
    };
    if (entry.op == CompiledFilter::Op::Eq) {
        add(entry);
        return;
    }
    CompiledFilter::Entry parsed;
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access): an entry whose operator is OneOf carries its values.
    for (auto const& member : *raw.oneOf) {
        if (parseOperand(info, services, member, 0, parsed)) {
            add(parsed);
        }
    }
}

/// @brief Compiles one side of a group's entries for one column, appending what compiled to @p entries.
/// @param raw       The side's entries, as the spec gave them.
/// @param info      The column.
/// @param services  Collator and date parser.
/// @param appliesTo Per entry, the columns it compiled on so far.
/// @param lastError The latest error of an entry that did not apply to a column.
/// @param entries   Receives the compiled entries.
/// @param bits      Receives each compiled entry's bit.
/// @param index     Receives the values of the compiled `eq` and `oneOf` entries that are not case-sensitive.
/// @param tested    Receives the place in @p entries of every other compiled entry.
/// @return Nothing, or an error that is not one column's: `InvalidSpec` for an entry that sets no operator, or
///         several, and `SourceContract` for a column read without what it needs.
[[nodiscard]] inline std::expected<void, TableError> compileGroupSide(
    std::span<FilterEntry const> raw, ColumnInfo const& info, Services const& services,
    std::vector<std::size_t>& appliesTo, std::optional<TableError>& lastError,
    std::vector<CompiledFilter::Entry>& entries, std::vector<std::uint32_t>& bits, ValueIndex& index,
    std::vector<std::size_t>& tested) {
    for (std::size_t i = 0; i < raw.size(); ++i) {
        auto compiled = compileEntry(raw[i], info, services);
        if (!compiled) {
            if (compiled.error().code == TableErrorCode::InvalidSpec ||
                compiled.error().code == TableErrorCode::SourceContract) {
                return std::unexpected(std::move(compiled.error()));
            }
            lastError = std::move(compiled.error());
            continue;
        }
        ++appliesTo[i];
        auto const bit = 1U << i;
        bool const byValue = (compiled->op == CompiledFilter::Op::Eq || compiled->op == CompiledFilter::Op::OneOf) &&
                             !compiled->caseSensitive;
        if (byValue) {
            indexEntry(*compiled, raw[i], info, services, bit, index);
        } else {
            tested.push_back(entries.size());
        }
        bits.push_back(bit);
        entries.push_back(std::move(*compiled));
    }
    return {};
}

/// @brief Compiles a group: per column, the entries that apply to it, and a value index over them.
///
/// Exactly one of `group` and `columns` names the columns (`InvalidSpec`); `group` resolves through @p groups
/// (`UnknownColumn` naming it) and a column named twice is `InvalidSpec`. `Together` takes 0, or 2 to k, include
/// entries and at most 16 columns; `atLeast` and `exactly` take `0 <= n <= k` (`InvalidSpec`). More than 32
/// include entries, or 32 exclude entries, is `LimitExceeded`: each side's entries are bits of one 32-bit mask,
/// and the bound is checked here because a filter set on an engine is compiled without a query's limits. In a
/// group, an entry that does not apply to a column (its operator undefined, or its value not parsing, for the
/// column's kind) is left out on that column, and is an error only when it applies to none.
/// @param group    The group.
/// @param columns  The table's columns.
/// @param services Collator and date parser.
/// @param groups   The table's declared groups.
/// @return The compiled group, or the first error.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): one sequential pass whose steps share its running state; splitting it would thread that state through parameters.
[[nodiscard]] inline std::expected<CompiledFilter::Group, TableError> compileGroup(
    GroupFilter const& group, std::span<ColumnInfo const> columns, Services const& services,
    std::span<ColumnGroupInfo const> groups) {
    constexpr std::size_t kMaxSide = 32;
    constexpr std::size_t kMaxTogether = 16;
    auto const invalid = [](std::string message) {
        return std::unexpected(
            TableError{.code = TableErrorCode::InvalidSpec, .column = {}, .message = std::move(message)});
    };
    if (group.group.empty() == group.columns.empty()) {
        return invalid("a filter group names a declared group or its columns");
    }
    std::span<std::string const> names = group.columns;
    if (!group.group.empty()) {
        auto const declared = std::ranges::find(groups, group.group, &ColumnGroupInfo::id);
        if (declared == groups.end()) {
            return std::unexpected(TableError{.code = TableErrorCode::UnknownColumn,
                                              .column = group.group,
                                              .message = "no column group " + group.group});
        }
        names = declared->columns;
    }
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (std::find(names.begin(), names.begin() + static_cast<std::ptrdiff_t>(i), names[i]) !=
            names.begin() + static_cast<std::ptrdiff_t>(i)) {
            return invalid("a filter group names column " + names[i] + " twice");
        }
    }
    if (group.include.size() > kMaxSide || group.exclude.size() > kMaxSide) {
        return std::unexpected(TableError{.code = TableErrorCode::LimitExceeded,
                                          .column = {},
                                          .message = "a filter group has more than 32 entries on a side"});
    }
    auto const k = names.size();
    bool const counted =
        group.match.kind == ColumnCount::Kind::AtLeast || group.match.kind == ColumnCount::Kind::Exactly;
    if (counted && group.match.n < 0) {
        return invalid(R"(a group's match is not "any", "all", {"atLeast": n} or {"exactly": n} with n at least 0)");
    }
    if (group.mode == GroupMode::Together) {
        if (k > kMaxTogether) {
            return invalid("a together group has at most 16 columns");
        }
        if (!group.include.empty() && (group.include.size() < 2 || group.include.size() > k)) {
            return invalid("a together group takes 2 to as many include entries as it has columns");
        }
    } else if (counted && std::cmp_greater(group.match.n, k)) {
        return invalid("a group's count is above its column count");
    }
    CompiledFilter::Group out;
    out.k = k;
    out.mode = group.mode;
    out.match = group.match;
    out.combine = group.combine;
    out.includeCount = group.include.size();
    out.excludeCount = group.exclude.size();
    std::vector<std::size_t> includeApplies(group.include.size(), 0);
    std::vector<std::size_t> excludeApplies(group.exclude.size(), 0);
    std::optional<TableError> lastError;
    for (auto const& columnId : names) {
        auto const found = findColumn(columns, columnId);
        if (!found) {
            return std::unexpected(unknownColumn(columnId));
        }
        auto const& info = columns[*found];
        CompiledFilter::Column column{.column = *found, .combine = group.combine, .include = {}, .exclude = {}};
        CompiledFilter::GroupColumn index{.includeBits = {},
                                          .excludeBits = {},
                                          .include = ValueIndex{info.kind},
                                          .exclude = ValueIndex{info.kind},
                                          .includeTested = {},
                                          .excludeTested = {},
                                          .compiled = 0,
                                          .usable = false};
        if (auto side = compileGroupSide(group.include, info, services, includeApplies, lastError, column.include,
                                         index.includeBits, index.include, index.includeTested);
            !side) {
            return std::unexpected(std::move(side.error()));
        }
        if (auto side = compileGroupSide(group.exclude, info, services, excludeApplies, lastError, column.exclude,
                                         index.excludeBits, index.exclude, index.excludeTested);
            !side) {
            return std::unexpected(std::move(side.error()));
        }
        for (auto const bit : index.includeBits) {
            index.compiled |= bit;
        }
        // With `all`, a column that cannot evaluate every entry cannot satisfy them; with `any`, one that
        // evaluates none cannot either.
        index.usable =
            group.combine == Combine::All ? column.include.size() == group.include.size() : !column.include.empty();
        if (!column.include.empty() || !column.exclude.empty()) {
            out.columns.push_back(std::move(column));
            out.index.push_back(std::move(index));
        }
    }
    auto const unapplied = [](std::vector<std::size_t> const& applies) {
        return std::ranges::find(applies, std::size_t{0}) != applies.end();
    };
    if (unapplied(includeApplies) || unapplied(excludeApplies)) {
        return std::unexpected(lastError.value_or(TableError{.code = TableErrorCode::InvalidSpec,
                                                             .column = {},
                                                             .message = "a filter group entry applies to no column"}));
    }
    return out;
}

}  // namespace detail

[[nodiscard]] inline std::expected<CompiledFilter, TableError> compileFilter(FilterSpec const& spec,
                                                                             std::span<ColumnInfo const> columns,
                                                                             Services const& services,
                                                                             std::span<ColumnGroupInfo const> groups) {
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
        auto compiled = detail::compileGroup(group, columns, services, groups);
        if (!compiled) {
            return std::unexpected(std::move(compiled.error()));
        }
        if (!group.include.empty() || !group.exclude.empty()) {
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
/// @param groups   The table's declared groups.
/// @return The compiled filter, or the first error.
[[nodiscard]] inline std::expected<CompiledFilter, TableError> compileFilterGuarded(
    FilterSpec const& spec, std::span<ColumnInfo const> columns, Services const& services,
    std::span<ColumnGroupInfo const> groups = {}) {
    auto const failed = [](std::string const& what) {
        return std::unexpected(
            TableError{.code = TableErrorCode::ServiceFailed, .column = {}, .message = "a service failed: " + what});
    };
    try {
        return compileFilter(spec, columns, services, groups);
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

/// @brief JSON names of `morph::table::GroupMode`: `columns` and `together`.
template <>
struct glz::meta<morph::table::GroupMode> {
    using enum morph::table::GroupMode;
    /// @brief Each enumerator after its JSON name.
    static constexpr auto value = glz::enumerate("columns", Columns, "together", Together);
};

/// @brief The JSON of `morph::table::ColumnCount`: `"any"`, `"all"`, `{"atLeast": n}` or `{"exactly": n}`.
template <>
struct glz::meta<morph::table::ColumnCount> {
    /// @brief Read and written through its wire shape.
    static constexpr auto value =
        glz::custom<&morph::table::ColumnCount::setWire, &morph::table::ColumnCount::getWire>;
};

/// @brief JSON names of `morph::table::Combine`: `any` and `all`.
template <>
struct glz::meta<morph::table::Combine> {
    using enum morph::table::Combine;
    /// @brief Each enumerator after its JSON name.
    static constexpr auto value = glz::enumerate("any", Any, "all", All);
};
