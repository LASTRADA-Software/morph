// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file table/data_source.hpp
/// @brief The rows a table engine reads, and the services it is given.
///
/// A `DataSource` publishes a table's columns, an immutable `RowSnapshot` of
/// its rows, and notifications when rows change. The engine reads cells only
/// through a snapshot, never through the source, so a snapshot can be handed
/// to a worker thread while the owner keeps changing the source.
///
/// The injected services (`TextCollator`, `DateParser`, `ProgressSink`, named
/// comparators) are the only locale- and toolkit-dependent parts of sorting
/// and filtering; each has a deterministic default here.
///
/// See `docs/spec/table/engine.md`.

#include <algorithm>
#include <array>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <morph/core/callback_scope.hpp>
#include <morph/util/datetime.hpp>
#include <morph/util/quantity.hpp>
#include <morph/util/rational.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace morph::table {

// NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) -- row and column indices are bounded by the snapshot's own counts, and the sort and filter loops index once per comparison, where at() would check every access

/// @brief What a column holds, which decides how its keys are built and compared.
enum class ColumnKind : std::uint8_t {
    Integer,   ///< `int64`, compared numerically.
    Decimal,   ///< An exact `Rational`.
    Quantity,  ///< An exact `Rational` in the column's canonical unit.
    Number,    ///< A `double`; the only kind that holds one.
    Text,      ///< Text, ordered and matched through the `TextCollator`.
    Date,      ///< Days since 1970-01-01.
    DateTime,  ///< UTC seconds since 1970-01-01T00:00:00Z.
    Bool,      ///< `false` before `true`.
    Key,       ///< An `int64` key, compared numerically.
    Custom,    ///< Ordered by a named comparator from `Services::comparators`.
};

/// @brief A row's identity: the table's key, stable across reloads.
using RowId = std::variant<std::int64_t, std::string>;

/// @brief A quantity in its own unit, with the factor that converts it to the
///        column's canonical unit.
///
/// The engine multiplies the two with checked arithmetic; a product that does
/// not fit is an invalid cell, never a saturated one. A plain `Rational` in a
/// `Quantity` column is taken as already canonical.
struct QuantityCell {
    /// @brief The amount in the cell's own unit.
    math::Rational amount;
    /// @brief Multiplier from the cell's unit to the column's canonical unit.
    math::Rational toCanonical;
};

/// @brief One cell as a data source hands it out.
///
/// `std::monostate` is an empty cell. A `std::string` in a non-text column is
/// parsed for the column's kind when its key is built; text that does not parse
/// is an invalid cell.
using Cell = std::variant<std::monostate, std::int64_t, math::Rational, double, std::string, bool, QuantityCell>;

/// @brief A column's identity and kind.
struct ColumnInfo {
    /// @brief The column's id, unique within the table (the row member's name).
    std::string id;
    /// @brief How the column's keys are built and compared.
    ColumnKind kind = ColumnKind::Text;
    /// @brief For a `Custom` column, the name of its comparator in `Services::comparators`.
    std::string comparator;
    /// @brief For a `Date` or `DateTime` column holding text, the format handed to the `DateParser`.
    std::string format;

    /// @brief Member-wise equality.
    /// @param other The column to compare with.
    /// @return `true` when id, kind, comparator and format match.
    [[nodiscard]] bool operator==(ColumnInfo const& other) const = default;
};

/// @brief What went wrong in a fallible table call.
enum class TableErrorCode : std::uint8_t {
    UnknownColumn,        ///< A sort or filter names a column the table does not have.
    UnsupportedOperator,  ///< A filter operator the column's kind does not define.
    InvalidValue,         ///< A filter value that does not parse for its column's kind.
    InvalidSpec,          ///< A filter entry with no operator or more than one.
    LimitExceeded,        ///< A `TableQuery` over one of the server's bounds.
    ColumnNotReadable,    ///< A sort or filter on a column the principal may not read.
    Unavailable,          ///< An operation the table's mode does not offer.
    ServiceFailed,        ///< An injected service (collator, date parser, comparator) threw.
};

/// @brief The error every fallible table call returns.
struct TableError {
    /// @brief What went wrong.
    TableErrorCode code = TableErrorCode::InvalidSpec;
    /// @brief The column concerned, or empty when the error is not about one column.
    std::string column;
    /// @brief A human-readable description.
    std::string message;
};

/// @brief Parses exact decimal text (`"-12.50"`) into a `Rational`.
///
/// Accepts an optional sign, digits, and an optional fraction of at most 18
/// digits; nothing else (no exponent, no grouping, no surrounding space). The
/// result carries as many decimal places as the text had.
/// @param text The decimal text.
/// @return The exact value, or `InvalidValue` when the text is not a decimal or
///         does not fit an `int64` numerator.
[[nodiscard]] inline std::expected<math::Rational, TableError> parseDecimal(std::string_view text) {
    auto const fail = [&] {
        return std::unexpected(TableError{
            .code = TableErrorCode::InvalidValue, .column = {}, .message = "not a decimal: " + std::string{text}});
    };
    std::size_t cursor = 0;
    bool negative = false;
    if (cursor < text.size() && (text[cursor] == '-' || text[cursor] == '+')) {
        negative = text[cursor] == '-';
        ++cursor;
    }
    std::uint64_t magnitude = 0;
    std::uint32_t fractionDigits = 0;
    std::size_t digits = 0;
    bool inFraction = false;
    // The largest magnitude whose negation still fits: |INT64_MIN| is one more
    // than INT64_MAX, and parsing to that bound keeps "-9223372036854775808" exact.
    auto const bound = negative ? static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1U
                                : static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    for (; cursor < text.size(); ++cursor) {
        char const character = text[cursor];
        if (character == '.' && !inFraction) {
            inFraction = true;
            continue;
        }
        if (character < '0' || character > '9') {
            return fail();
        }
        auto const digit = static_cast<std::uint64_t>(character - '0');
        if (magnitude > (bound - digit) / 10U) {
            return fail();
        }
        magnitude = (magnitude * 10U) + digit;
        ++digits;
        if (inFraction) {
            ++fractionDigits;
        }
    }
    if (digits == 0 || fractionDigits > math::kMaxDecimalPlaces) {
        return fail();
    }
    std::int64_t const numerator =
        negative ? static_cast<std::int64_t>(0U - magnitude) : static_cast<std::int64_t>(magnitude);
    if (fractionDigits == 0) {
        // The whole-number constructor keeps INT64_MIN; the canonicalising
        // one would have to negate it and clamps it instead.
        return math::Rational{numerator, math::DecimalPlaces{0}};
    }
    if (numerator == std::numeric_limits<std::int64_t>::min()) {
        return fail();
    }
    std::int64_t denominator = 1;
    for (std::uint32_t i = 0; i < fractionDigits; ++i) {
        denominator *= 10;
    }
    return math::Rational{math::Numerator{numerator}, math::Denominator{denominator},
                          math::DecimalPlaces{fractionDigits}};
}

/// @brief The text a cell shows when it has to be ordered or matched as text.
/// @param cell The cell.
/// @return Its text: the string itself, a number in decimal form (a decimal
///         to its own places, `"12.5"`), `true` or `false`, or empty for an
///         empty cell.
[[nodiscard]] inline std::string displayText(Cell const& cell) {
    return std::visit(
        []<typename T>(T const& value) -> std::string {
            if constexpr (std::is_same_v<T, std::monostate>) {
                return {};
            } else if constexpr (std::is_same_v<T, std::string>) {
                return value;
            } else if constexpr (std::is_same_v<T, bool>) {
                return value ? "true" : "false";
            } else if constexpr (std::is_same_v<T, QuantityCell>) {
                return units::detail::formatRationalDecimal(value.amount);
            } else if constexpr (std::is_same_v<T, math::Rational>) {
                return units::detail::formatRationalDecimal(value);
            } else {
                return std::format("{}", value);
            }
        },
        cell);
}

// ── Services ────────────────────────────────────────────────────────────────

/// @brief Orders and matches text. The renderer injects the toolkit's collator
///        for the user's locale; `CodePointCollator` is the default.
///
/// Called from the engine's worker. An implementation either is safe to call
/// concurrently, and returns null from `cloneForTask`, or returns a fresh
/// instance there that one task uses alone.
// NOLINTBEGIN(cppcoreguidelines-special-member-functions)
class TextCollator {
public:
    virtual ~TextCollator() = default;

    /// @brief A key whose byte order is the collation order of @p text.
    /// @param text The text to order.
    /// @return The collation key.
    [[nodiscard]] virtual std::string sortKey(std::string_view text) const = 0;

    /// @brief @p text folded to primary strength: two texts a user would call
    ///        equal ignoring case and accents fold to the same bytes.
    /// @param text The text to fold.
    /// @return The folded text, which filters match against.
    [[nodiscard]] virtual std::string fold(std::string_view text) const = 0;

    /// @brief An instance for one task to use alone.
    /// @return Null when this instance is safe to call concurrently, else a copy.
    [[nodiscard]] virtual std::shared_ptr<TextCollator const> cloneForTask() const { return nullptr; }
};
// NOLINTEND(cppcoreguidelines-special-member-functions)

/// @brief The default collator: code-point order, with ASCII and Latin-1
///        letters case-folded. It does not fold accents.
///
/// Stateless and safe to call concurrently.
class CodePointCollator final : public TextCollator {
public:
    /// @brief The case-folded text; its bytes order as its code points.
    /// @param text UTF-8 text.
    /// @return The collation key.
    [[nodiscard]] std::string sortKey(std::string_view text) const override { return fold(text); }

    /// @brief ASCII `A`-`Z` and Latin-1 `À`-`Þ` (except `×`) lowered; every
    ///        other byte copied as it is.
    /// @param text UTF-8 text.
    /// @return The folded text.
    [[nodiscard]] std::string fold(std::string_view text) const override {
        std::string out;
        out.reserve(text.size());
        for (std::size_t i = 0; i < text.size(); ++i) {
            auto const byte = static_cast<unsigned char>(text[i]);
            if (byte >= 'A' && byte <= 'Z') {
                out.push_back(static_cast<char>(byte + ('a' - 'A')));
                continue;
            }
            // U+00C0..U+00DE encode as 0xC3 0x80..0x9E; lowering adds 0x20 to
            // the second byte. U+00D7 (multiplication sign) has no lower case.
            if (byte == 0xC3 && i + 1 < text.size()) {
                auto const next = static_cast<unsigned char>(text[i + 1]);
                if (next >= 0x80 && next <= 0x9E && next != 0x97) {
                    out.push_back(text[i]);
                    out.push_back(static_cast<char>(next + 0x20));
                    ++i;
                    continue;
                }
            }
            out.push_back(text[i]);
        }
        return out;
    }
};

/// @brief Parses text to dates and date-times for `Date` and `DateTime`
///        columns that hold text, and for filter values on those columns.
///
/// The same thread-safety contract as `TextCollator`.
// NOLINTBEGIN(cppcoreguidelines-special-member-functions)
class DateParser {
public:
    virtual ~DateParser() = default;

    /// @brief Parses a date.
    /// @param text   The text.
    /// @param format The column's format; empty for the parser's default.
    /// @return Days since 1970-01-01, or nothing when @p text is not a date.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters) - text then format, as every parser reads them
    [[nodiscard]] virtual std::optional<std::int64_t> parseDate(std::string_view text,
                                                                std::string_view format) const = 0;

    /// @brief Parses a date-time.
    /// @param text   The text.
    /// @param format The column's format; empty for the parser's default.
    /// @return UTC seconds since the epoch, or nothing when @p text is not a date-time.
    [[nodiscard]] virtual std::optional<std::int64_t> parseDateTime(std::string_view text,
                                                                    std::string_view format) const = 0;

    /// @brief An instance for one task to use alone.
    /// @return Null when this instance is safe to call concurrently, else a copy.
    [[nodiscard]] virtual std::shared_ptr<DateParser const> cloneForTask() const { return nullptr; }
};
// NOLINTEND(cppcoreguidelines-special-member-functions)

/// @brief The default date parser: ISO 8601 only, whatever the format.
///
/// A date is `YYYY-MM-DD`. A date-time is what `morph::time::DateTime::fromIso8601`
/// reads, or a bare date, which is midnight UTC. Stateless and safe to call
/// concurrently.
class IsoDateParser final : public DateParser {
public:
    /// @brief Parses `YYYY-MM-DD`.
    /// @param text   The text.
    /// @param format Ignored.
    /// @return Days since 1970-01-01, or nothing.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters) - text then format, as every parser reads them
    [[nodiscard]] std::optional<std::int64_t> parseDate(std::string_view text,
                                                        std::string_view format) const override {
        static_cast<void>(format);
        if (text.size() != 10 || text[4] != '-' || text[7] != '-') {
            return std::nullopt;
        }
        auto const number = [&](std::size_t offset, std::size_t count) -> std::optional<int> {
            int value = 0;
            for (std::size_t i = offset; i < offset + count; ++i) {
                if (text[i] < '0' || text[i] > '9') {
                    return std::nullopt;
                }
                value = (value * 10) + (text[i] - '0');
            }
            return value;
        };
        auto const year = number(0, 4);
        auto const month = number(5, 2);
        auto const day = number(8, 2);
        if (!year || !month || !day) {
            return std::nullopt;
        }
        std::chrono::year_month_day const date{std::chrono::year{*year},
                                               std::chrono::month{static_cast<unsigned>(*month)},
                                               std::chrono::day{static_cast<unsigned>(*day)}};
        if (!date.ok()) {
            return std::nullopt;
        }
        return std::chrono::sys_days{date}.time_since_epoch().count();
    }

    /// @brief Parses an ISO 8601 date-time, or a bare date as midnight UTC.
    /// @param text   The text.
    /// @param format Ignored.
    /// @return UTC seconds since the epoch, or nothing.
    [[nodiscard]] std::optional<std::int64_t> parseDateTime(std::string_view text,
                                                            std::string_view format) const override {
        if (auto const days = parseDate(text, format)) {
            return *days * std::int64_t{86400};
        }
        auto const instant = time::DateTime::fromIso8601(text);
        if (!instant) {
            return std::nullopt;
        }
        return std::chrono::floor<std::chrono::seconds>(instant->value).time_since_epoch().count();
    }
};

/// @brief Receives progress of a long sort or filter, on the owner thread.
// NOLINTBEGIN(cppcoreguidelines-special-member-functions)
class ProgressSink {
public:
    virtual ~ProgressSink() = default;

    /// @brief Reports progress through the current job.
    /// @param done  Work units finished.
    /// @param total Work units in the job.
    virtual void progress(std::size_t done, std::size_t total) = 0;
};
// NOLINTEND(cppcoreguidelines-special-member-functions)

/// @brief Orders two cells of a `Custom` column. Called from the worker, so it
///        must be safe to call concurrently.
using CellComparator = std::function<std::strong_ordering(Cell const&, Cell const&)>;

/// @brief The services an engine sorts and filters with. A null member is
///        replaced by its default.
struct Services {
    /// @brief Text order and matching; null means `CodePointCollator`.
    std::shared_ptr<TextCollator const> collator;
    /// @brief Date parsing; null means `IsoDateParser`.
    std::shared_ptr<DateParser const> dates;
    /// @brief Comparators for `Custom` columns, by the name `ColumnInfo::comparator` gives.
    std::map<std::string, CellComparator, std::less<>> comparators;
    /// @brief Optional progress receiver.
    std::shared_ptr<ProgressSink> progress;

    /// @brief The collator to use.
    /// @return `collator`, or the shared default.
    [[nodiscard]] std::shared_ptr<TextCollator const> collatorOrDefault() const {
        static auto const fallback = std::make_shared<CodePointCollator const>();
        return collator != nullptr ? collator : fallback;
    }

    /// @brief The date parser to use.
    /// @return `dates`, or the shared default.
    [[nodiscard]] std::shared_ptr<DateParser const> datesOrDefault() const {
        static auto const fallback = std::make_shared<IsoDateParser const>();
        return dates != nullptr ? dates : fallback;
    }

    /// @brief The services one task uses: collator and parser cloned when they
    ///        are not safe to share, defaults filled in.
    /// @return A copy fit to hand to a worker.
    [[nodiscard]] Services forTask() const {
        Services out = *this;
        out.collator = collatorOrDefault();
        if (auto clone = out.collator->cloneForTask()) {
            out.collator = std::move(clone);
        }
        out.dates = datesOrDefault();
        if (auto clone = out.dates->cloneForTask()) {
            out.dates = std::move(clone);
        }
        return out;
    }
};

// ── Snapshots ───────────────────────────────────────────────────────────────

/// @brief Receives a run of one column's cells.
// NOLINTBEGIN(cppcoreguidelines-special-member-functions)
class ColumnSink {
public:
    virtual ~ColumnSink() = default;

    /// @brief Receives consecutive cells of one column.
    /// @param firstRow The row of `cells[0]`.
    /// @param cells    The cells of rows `firstRow`, `firstRow + 1`, ….
    virtual void cells(std::size_t firstRow, std::span<Cell const> cells) = 0;
};
// NOLINTEND(cppcoreguidelines-special-member-functions)

/// @brief An immutable view of a table's rows at one moment.
///
/// Nothing changes a snapshot after it is published, so it is safe to read
/// from any thread, concurrently, for as long as it is held.
// NOLINTBEGIN(cppcoreguidelines-special-member-functions)
class RowSnapshot {
public:
    RowSnapshot() = default;
    virtual ~RowSnapshot() = default;

    /// @brief The number of rows.
    /// @return Row count.
    [[nodiscard]] virtual std::size_t rowCount() const = 0;

    /// @brief A row's key.
    /// @param row Row index, below `rowCount()`.
    /// @return The row's `RowId`.
    [[nodiscard]] virtual RowId rowId(std::size_t row) const = 0;

    /// @brief One cell.
    /// @param row    Row index, below `rowCount()`.
    /// @param column Column index, below the source's column count.
    /// @return The cell, by value.
    [[nodiscard]] virtual Cell cell(std::size_t row, std::size_t column) const = 0;

    /// @brief Hands rows `[first, last)` of @p column to @p sink.
    ///
    /// The default calls `cell` per row. A snapshot that stores cells
    /// contiguously overrides it to hand whole runs, which is how the engine
    /// reads a column of 100,000 rows without as many virtual calls.
    /// @param column Column index.
    /// @param sink   Receives the cells.
    /// @param first  First row.
    /// @param last   One past the last row.
    virtual void readColumn(std::size_t column, ColumnSink& sink, std::size_t first, std::size_t last) const {
        for (std::size_t row = first; row < last; ++row) {
            Cell const value = cell(row, column);
            sink.cells(row, std::span<Cell const>{&value, 1});
        }
    }

protected:
    /// @brief Copies the base of a snapshot: a derived snapshot makes a changed
    ///        copy of itself this way.
    RowSnapshot(RowSnapshot const&) = default;
    /// @brief Moves the base of a snapshot.
    RowSnapshot(RowSnapshot&&) = default;
    /// @brief Copy-assigns the base of a snapshot.
    /// @return `*this`.
    RowSnapshot& operator=(RowSnapshot const&) = default;
    /// @brief Move-assigns the base of a snapshot.
    /// @return `*this`.
    RowSnapshot& operator=(RowSnapshot&&) = default;
};
// NOLINTEND(cppcoreguidelines-special-member-functions)

/// @brief A `RowSnapshot` that stores rows in shared, immutable chunks.
///
/// Changing one row produces a new snapshot that shares every chunk but the
/// one holding the row, so a stream of updates against a table a worker is
/// still reading costs one chunk copy per update, not one table copy.
class TableSnapshot final : public RowSnapshot {
public:
    /// @brief Rows per chunk: small enough that copying one is cheap, large
    ///        enough that a column read hands long runs.
    static constexpr std::size_t kChunkRows = 256;

    /// @brief Constructs an empty snapshot.
    /// @param columnCount Number of columns every row has.
    explicit TableSnapshot(std::size_t columnCount) : _columnCount{columnCount} {}

    /// @brief Builds a snapshot from rows.
    /// @param columnCount Number of columns.
    /// @param ids         The rows' keys.
    /// @param rows        The rows' cells; each row has @p columnCount cells
    ///                    (missing cells are empty, extra cells are dropped).
    /// @return The snapshot.
    [[nodiscard]] static std::shared_ptr<TableSnapshot const> fromRows(std::size_t columnCount, std::vector<RowId> ids,
                                                                       std::vector<std::vector<Cell>> rows) {
        auto out = std::make_shared<TableSnapshot>(columnCount);
        out->assign(std::move(ids), std::move(rows), 0);
        return out;
    }

    /// @brief The number of rows.
    /// @return Row count.
    [[nodiscard]] std::size_t rowCount() const override { return _rows; }

    /// @brief The number of columns.
    /// @return Column count.
    [[nodiscard]] std::size_t columnCount() const noexcept { return _columnCount; }

    /// @brief A row's key.
    /// @param row Row index.
    /// @return The key.
    [[nodiscard]] RowId rowId(std::size_t row) const override {
        return _chunks[row / kChunkRows]->ids[row % kChunkRows];
    }

    /// @brief One cell.
    /// @param row    Row index.
    /// @param column Column index.
    /// @return The cell.
    [[nodiscard]] Cell cell(std::size_t row, std::size_t column) const override {
        return _chunks[row / kChunkRows]->columns[column][row % kChunkRows];
    }

    /// @brief Hands rows `[first, last)` of @p column to @p sink, one run per chunk.
    /// @param column Column index.
    /// @param sink   Receives the cells.
    /// @param first  First row.
    /// @param last   One past the last row.
    void readColumn(std::size_t column, ColumnSink& sink, std::size_t first, std::size_t last) const override {
        while (first < last) {
            auto const& chunk = *_chunks[first / kChunkRows];
            auto const offset = first % kChunkRows;
            auto const count = std::min(last - first, chunk.ids.size() - offset);
            sink.cells(first, std::span<Cell const>{chunk.columns[column]}.subspan(offset, count));
            first += count;
        }
    }

    /// @brief A copy of this snapshot with one row's cells replaced. Only that
    ///        row's chunk is copied.
    /// @param row   Row index.
    /// @param cells The new cells.
    /// @return The new snapshot.
    [[nodiscard]] std::shared_ptr<TableSnapshot const> withRow(std::size_t row, std::vector<Cell> cells) const {
        auto out = std::make_shared<TableSnapshot>(*this);
        auto chunk = std::make_shared<Chunk>(*_chunks[row / kChunkRows]);
        cells.resize(_columnCount);
        for (std::size_t column = 0; column < _columnCount; ++column) {
            chunk->columns[column][row % kChunkRows] = std::move(cells[column]);
        }
        out->_chunks[row / kChunkRows] = std::move(chunk);
        return out;
    }

    /// @brief A copy with a row inserted before @p row. Chunks before it are shared.
    /// @param row   Insert position, at most `rowCount()`.
    /// @param key   The new row's key.
    /// @param cells The new row's cells.
    /// @return The new snapshot.
    [[nodiscard]] std::shared_ptr<TableSnapshot const> withInserted(std::size_t row, RowId key,
                                                                    std::vector<Cell> cells) const {
        auto [ids, rows] = tail(row);
        ids.insert(ids.begin(), std::move(key));
        rows.insert(rows.begin(), std::move(cells));
        auto out = std::make_shared<TableSnapshot>(*this);
        out->assign(std::move(ids), std::move(rows), row);
        return out;
    }

    /// @brief A copy with row @p row removed. Chunks before it are shared.
    /// @param row Row index.
    /// @return The new snapshot.
    [[nodiscard]] std::shared_ptr<TableSnapshot const> withRemoved(std::size_t row) const {
        auto [ids, rows] = tail(row);
        ids.erase(ids.begin());
        rows.erase(rows.begin());
        auto out = std::make_shared<TableSnapshot>(*this);
        out->assign(std::move(ids), std::move(rows), row);
        return out;
    }

    /// @brief Whether this snapshot and @p other share the chunk holding @p row.
    /// @param other Another snapshot.
    /// @param row   Row index valid in both.
    /// @return `true` when the chunk is the same object.
    [[nodiscard]] bool sharesChunk(TableSnapshot const& other, std::size_t row) const noexcept {
        return _chunks[row / kChunkRows] == other._chunks[row / kChunkRows];
    }

private:
    struct Chunk {
        std::vector<RowId> ids;
        std::vector<std::vector<Cell>> columns;  // columns[column][row within chunk]
    };

    // Rows from `first` on, as plain vectors, for rebuilding the tail.
    [[nodiscard]] std::pair<std::vector<RowId>, std::vector<std::vector<Cell>>> tail(std::size_t first) const {
        std::vector<RowId> ids;
        std::vector<std::vector<Cell>> rows;
        ids.reserve(_rows - first);
        rows.reserve(_rows - first);
        for (std::size_t row = first; row < _rows; ++row) {
            ids.push_back(rowId(row));
            std::vector<Cell> cells;
            cells.reserve(_columnCount);
            for (std::size_t column = 0; column < _columnCount; ++column) {
                cells.push_back(cell(row, column));
            }
            rows.push_back(std::move(cells));
        }
        return {std::move(ids), std::move(rows)};
    }

    // Replaces rows from `first` on with the given rows, keeping the chunks
    // wholly before `first` and the rows of `first`'s chunk that precede it.
    void assign(std::vector<RowId> ids, std::vector<std::vector<Cell>> rows, std::size_t first) {
        auto const keepChunks = first / kChunkRows;
        std::vector<RowId> headIds;
        std::vector<std::vector<Cell>> headRows;
        if (keepChunks < _chunks.size()) {
            headIds.reserve(first - (keepChunks * kChunkRows));
            headRows.reserve(first - (keepChunks * kChunkRows));
            for (std::size_t row = keepChunks * kChunkRows; row < first; ++row) {
                headIds.push_back(rowId(row));
                std::vector<Cell> cells;
                cells.reserve(_columnCount);
                for (std::size_t column = 0; column < _columnCount; ++column) {
                    cells.push_back(cell(row, column));
                }
                headRows.push_back(std::move(cells));
            }
        }
        _chunks.resize(std::min(keepChunks, _chunks.size()));
        headIds.insert(headIds.end(), std::make_move_iterator(ids.begin()), std::make_move_iterator(ids.end()));
        headRows.insert(headRows.end(), std::make_move_iterator(rows.begin()), std::make_move_iterator(rows.end()));
        _rows = (_chunks.size() * kChunkRows) + headIds.size();
        for (std::size_t at = 0; at < headIds.size(); at += kChunkRows) {
            auto chunk = std::make_shared<Chunk>();
            auto const end = std::min(at + kChunkRows, headIds.size());
            chunk->ids.assign(std::make_move_iterator(headIds.begin() + static_cast<std::ptrdiff_t>(at)),
                              std::make_move_iterator(headIds.begin() + static_cast<std::ptrdiff_t>(end)));
            chunk->columns.resize(_columnCount);
            for (auto& column : chunk->columns) {
                column.reserve(end - at);
            }
            for (std::size_t row = at; row < end; ++row) {
                auto& cells = headRows[row];
                cells.resize(_columnCount);
                for (std::size_t column = 0; column < _columnCount; ++column) {
                    chunk->columns[column].push_back(std::move(cells[column]));
                }
            }
            _chunks.push_back(std::move(chunk));
        }
    }

    std::size_t _columnCount = 0;
    std::size_t _rows = 0;
    std::vector<std::shared_ptr<Chunk const>> _chunks;
};

// ── Sources ─────────────────────────────────────────────────────────────────

/// @brief How rows changed.
enum class ChangeKind : std::uint8_t {
    Updated,   ///< `rows` changed their cells; row indices are unchanged.
    Inserted,  ///< `rows` are the new rows' indices in the new snapshot.
    Removed,   ///< `rows` are the removed rows' indices in the old snapshot.
    Reset,     ///< Everything may have changed, the columns included.
};

/// @brief One change notification.
struct RowChange {
    /// @brief What happened.
    ChangeKind kind = ChangeKind::Reset;
    /// @brief The rows concerned; empty for `Reset`.
    std::vector<std::size_t> rows;
};

/// @brief Receives a `DataSource`'s change notifications, on the owner thread.
// NOLINTBEGIN(cppcoreguidelines-special-member-functions)
class ChangeListener {
public:
    virtual ~ChangeListener() = default;

    /// @brief Called after the source changed; its `snapshot()` already shows the change.
    /// @param change What changed.
    virtual void rowsChanged(RowChange const& change) = 0;
};
// NOLINTEND(cppcoreguidelines-special-member-functions)

/// @brief The rows of a table, as the engine reads them.
///
/// Owner-affine: every member is called on the owner thread. The engine reads
/// cells only from `snapshot()`, which it may hand to a worker.
// NOLINTBEGIN(cppcoreguidelines-special-member-functions)
class DataSource {
public:
    virtual ~DataSource() = default;

    /// @brief The table's columns.
    /// @return The columns, in source order.
    [[nodiscard]] virtual std::span<ColumnInfo const> columns() const = 0;

    /// @brief The current rows as an immutable snapshot. Taking one is cheap.
    /// @return The snapshot.
    [[nodiscard]] virtual std::shared_ptr<RowSnapshot const> snapshot() const = 0;

    /// @brief Registers @p listener for change notifications.
    /// @param listener Receives notifications until `unsubscribe`; must outlive the subscription.
    virtual void subscribe(ChangeListener& listener) = 0;

    /// @brief Removes @p listener.
    /// @param listener A listener previously subscribed.
    virtual void unsubscribe(ChangeListener& listener) = 0;
};
// NOLINTEND(cppcoreguidelines-special-member-functions)

namespace detail {

/// @brief A source's listeners, notified so that a listener may unsubscribe
///        itself or another, or destroy the source, from inside a notification.
///
/// A listener unsubscribed during a notification is not called after that,
/// even if it was registered when the notification began: unsubscribing is
/// how its owner says it is about to be destroyed. A source destroyed during
/// a notification ends it.
class Listeners {
public:
    /// @brief Registers @p listener.
    /// @param listener The listener.
    void add(ChangeListener& listener) { _listeners.push_back(&listener); }

    /// @brief Removes @p listener.
    /// @param listener The listener.
    void remove(ChangeListener& listener) { std::erase(_listeners, &listener); }

    /// @brief Calls every listener registered now that is still registered when its turn comes.
    /// @param change What changed.
    void notify(RowChange const& change) {
        auto const alive = _alive.token();
        auto const listeners = _listeners;
        for (auto* const listener : listeners) {
            if (alive.expired()) {
                return;
            }
            if (std::ranges::find(_listeners, listener) != _listeners.end()) {
                listener->rowsChanged(change);
            }
        }
    }

private:
    std::vector<ChangeListener*> _listeners;
    async::CallbackScope _alive;
};

}  // namespace detail

/// @brief Replaces a row by key; what a committed cell edit patches.
// NOLINTBEGIN(cppcoreguidelines-special-member-functions)
class RowPatcher {
public:
    virtual ~RowPatcher() = default;

    /// @brief Replaces the cells of the row keyed @p id.
    /// @param key   The row's key.
    /// @param cells The new cells.
    /// @return `false` when no row has that key.
    virtual bool patchRow(RowId const& key, std::vector<Cell> cells) = 0;
};
// NOLINTEND(cppcoreguidelines-special-member-functions)

/// @brief A `DataSource` over rows held in memory, for tests, servers and
///        sources that cannot share their own storage.
class VectorSource final : public DataSource, public RowPatcher {
public:
    /// @brief Constructs an empty source with @p columns.
    /// @param columns The table's columns.
    explicit VectorSource(std::vector<ColumnInfo> columns)
        : _columns{std::move(columns)}, _snapshot{std::make_shared<TableSnapshot const>(_columns.size())} {}

    /// @brief The table's columns.
    /// @return The columns.
    [[nodiscard]] std::span<ColumnInfo const> columns() const override { return _columns; }

    /// @brief The current rows; a pointer copy.
    /// @return The snapshot.
    [[nodiscard]] std::shared_ptr<RowSnapshot const> snapshot() const override { return _snapshot; }

    /// @brief The current rows, typed.
    /// @return The snapshot.
    [[nodiscard]] std::shared_ptr<TableSnapshot const> const& tableSnapshot() const noexcept { return _snapshot; }

    /// @brief Registers @p listener.
    /// @param listener The listener.
    void subscribe(ChangeListener& listener) override { _listeners.add(listener); }

    /// @brief Removes @p listener; safe from inside a notification.
    /// @param listener The listener.
    void unsubscribe(ChangeListener& listener) override { _listeners.remove(listener); }

    /// @brief Replaces every row (and optionally the columns), then notifies `Reset`.
    /// @param ids     The rows' keys.
    /// @param rows    The rows' cells.
    /// @param columns New columns, or nothing to keep the current ones.
    void setRows(std::vector<RowId> ids, std::vector<std::vector<Cell>> rows,
                 std::optional<std::vector<ColumnInfo>> columns = std::nullopt) {
        if (columns) {
            _columns = std::move(*columns);
        }
        _snapshot = TableSnapshot::fromRows(_columns.size(), std::move(ids), std::move(rows));
        reindex();
        _listeners.notify(RowChange{.kind = ChangeKind::Reset, .rows = {}});
    }

    /// @brief Replaces one row's cells, then notifies `Updated`.
    /// @param row   Row index.
    /// @param cells The new cells.
    void updateRow(std::size_t row, std::vector<Cell> cells) {
        _snapshot = _snapshot->withRow(row, std::move(cells));
        _listeners.notify(RowChange{.kind = ChangeKind::Updated, .rows = {row}});
    }

    /// @brief Replaces the cells of the row keyed @p id, then notifies `Updated`.
    /// @param key   The row's key.
    /// @param cells The new cells.
    /// @return `false` when no row has that key.
    bool patchRow(RowId const& key, std::vector<Cell> cells) override {
        auto const found = _index.find(key);
        if (found == _index.end()) {
            return false;
        }
        updateRow(found->second, std::move(cells));
        return true;
    }

    /// @brief Inserts a row before @p row, then notifies `Inserted`.
    /// @param row   Insert position.
    /// @param key   The new row's key.
    /// @param cells The new row's cells.
    void insertRow(std::size_t row, RowId key, std::vector<Cell> cells) {
        _snapshot = _snapshot->withInserted(row, std::move(key), std::move(cells));
        reindex();
        _listeners.notify(RowChange{.kind = ChangeKind::Inserted, .rows = {row}});
    }

    /// @brief Removes row @p row, then notifies `Removed`.
    /// @param row Row index.
    void removeRow(std::size_t row) {
        _snapshot = _snapshot->withRemoved(row);
        reindex();
        _listeners.notify(RowChange{.kind = ChangeKind::Removed, .rows = {row}});
    }

    /// @brief The index of the row keyed @p id.
    /// @param key The key.
    /// @return Its row index, or nothing.
    [[nodiscard]] std::optional<std::size_t> indexOf(RowId const& key) const {
        auto const found = _index.find(key);
        return found == _index.end() ? std::nullopt : std::optional{found->second};
    }

private:
    void reindex() {
        _index.clear();
        for (std::size_t row = 0; row < _snapshot->rowCount(); ++row) {
            _index.emplace(_snapshot->rowId(row), row);
        }
    }

    std::vector<ColumnInfo> _columns;
    std::shared_ptr<TableSnapshot const> _snapshot;
    std::unordered_map<RowId, std::size_t> _index;
    detail::Listeners _listeners;
};

// NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)

}  // namespace morph::table

/// @brief JSON names of `morph::table::ColumnKind`: `integer`, `decimal`, ….
template <>
struct glz::meta<morph::table::ColumnKind> {
    using enum morph::table::ColumnKind;
    /// @brief Each enumerator after its JSON name.
    static constexpr auto value =
        glz::enumerate("integer", Integer, "decimal", Decimal, "quantity", Quantity, "number", Number, "text", Text,
                       "date", Date, "dateTime", DateTime, "bool", Bool, "key", Key, "custom", Custom);
};

/// @brief JSON names of `morph::table::TableErrorCode`, for a typed error on the wire.
template <>
struct glz::meta<morph::table::TableErrorCode> {
    using enum morph::table::TableErrorCode;
    /// @brief Each enumerator after its JSON name.
    static constexpr auto value =
        glz::enumerate("unknownColumn", UnknownColumn, "unsupportedOperator", UnsupportedOperator, "invalidValue",
                       InvalidValue, "invalidSpec", InvalidSpec, "limitExceeded", LimitExceeded, "columnNotReadable",
                       ColumnNotReadable, "unavailable", Unavailable, "serviceFailed", ServiceFailed);
};
