// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file ui/form_rows.hpp
/// @brief `FormRowsSource`: the `table::DataSource` over a form's `ObjectArray` field, whose rows a grid of the form
///        lays out.
///
/// Specified in `docs/spec/table/editing.md`, "A grid of a form".

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <morph/export.hpp>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "../forms/engine/form_session.hpp"
#include "table_rows.hpp"

namespace morph::ui {

/// @brief The record member a grid's row record holds its row's key in. No member of a row type has the name: `#`
///        introduces a row key in `forms::FormSession::field` paths.
inline constexpr std::string_view kGridKeyField = "#key";

namespace detail {

/// @brief The column kind a member of a form's row type sorts and filters as.
/// @param kind The member's field kind.
/// @return `Integer` for an integer or a slider, `Decimal` for a decimal or a quantity, `Number` for a plain number,
///         `Date` and `DateTime` for dates, `Bool` for a Boolean, `Text` for every other kind.
[[nodiscard]] MORPH_API table::ColumnKind columnKindOf(forms::FieldKind kind) noexcept;

/// @brief A member's cell: its encoding as a cell of its column's kind, empty when blank, and, when the draft does
///        not encode, the draft's text, which a column of any kind but `Text` reads as an invalid cell.
/// @param field The member. Read untracked by the caller.
/// @param kind  The column's kind.
/// @return The cell.
// One case per kind of the column.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] MORPH_API table::Cell memberCell(forms::FieldState const& field, table::ColumnKind kind);

/// @brief The cell of a member a column edits per row, whose kind varies by row: text, so the column sorts and filters
///        as one kind. A member that encodes a string (an Enum's or a Choice's option, a text) gives the string, not
///        the draft's JSON literal; any other gives its draft.
/// @param field The member. Read untracked by the caller.
/// @return The cell; empty while the draft is blank.
[[nodiscard]] MORPH_API table::Cell perRowCell(forms::FieldState const& field);

/// @brief The rows of a form's collection as a snapshot: each row's key, its cells and its record.
class FormRowsSnapshot final : public table::RowSnapshot {
public:
    /// @brief A snapshot of rows.
    /// @param columns How many columns each row has.
    /// @param keys    The rows' keys.
    /// @param cells   The rows' cells, one vector per row.
    /// @param records The rows' records.
    /// @param runs    Each run's heading record, in the array's order; none for rows that are not grouped.
    FormRowsSnapshot(std::size_t columns, std::vector<table::RowId> keys, std::vector<std::vector<table::Cell>> cells,
                     std::vector<Value> records, std::vector<Value> runs = {})
        // Parentheses for the records: braces would make a list of one value holding them all.
        : _columns{columns},
          _keys{std::move(keys)},
          _cells{std::move(cells)},
          _records(std::move(records)),
          _runs(std::move(runs)) {}

    /// @brief How many rows.
    /// @return The count.
    [[nodiscard]] std::size_t rowCount() const override { return _keys.size(); }

    /// @brief A row's key.
    /// @param row The row.
    /// @return Its key.
    [[nodiscard]] table::RowId rowId(std::size_t row) const override { return _keys.at(row); }

    /// @brief A row's cell.
    /// @param row    The row.
    /// @param column The column.
    /// @return The cell; empty past the columns.
    [[nodiscard]] table::Cell cell(std::size_t row, std::size_t column) const override {
        return column < _columns ? _cells.at(row).at(column) : table::Cell{};
    }

    /// @brief A row's record.
    /// @param row The row.
    /// @return The record.
    [[nodiscard]] Value const& record(std::size_t row) const { return _records.at(row); }

    /// @brief Every row's cells.
    /// @return They, by row.
    [[nodiscard]] std::vector<std::vector<table::Cell>> const& cells() const noexcept { return _cells; }

    /// @brief Every row's record.
    /// @return They, by row.
    [[nodiscard]] std::vector<Value> const& records() const noexcept { return _records; }

    /// @brief Every row's key.
    /// @return They, in the array's order.
    [[nodiscard]] std::vector<table::RowId> const& keys() const noexcept { return _keys; }

    /// @brief Each run's heading record.
    /// @return They, by run index.
    [[nodiscard]] std::vector<Value> const& runs() const noexcept { return _runs; }

private:
    std::size_t _columns;
    std::vector<table::RowId> _keys;
    std::vector<std::vector<table::Cell>> _cells;
    std::vector<Value> _records;
    std::vector<Value> _runs;
};

}  // namespace detail

/// @brief The rows of a form's `ObjectArray` field as a `table::DataSource`, in the array's order.
///
/// A row's id is its `forms::RowState::key()`. A cell is its member's encoding as a cell of the column's kind
/// (`detail::memberCell`), so sorting and filtering follow the table's rules, and the source never writes the array: a
/// grid's order is the view's, never the record's. A change of the field's rows or of a member's draft is taken in at
/// the next turn of the owner, and at once by `sync`, which whatever reads the rows calls first: a draft change of one
/// member notifies `Updated` for its row alone, rows added at the end or removed notify `Inserted` or `Removed`, and
/// any other change of the rows `Reset`.
class MORPH_API FormRowsSource final : public table::DataSource {
public:
    /// @brief A source over a collection of @p form.
    /// @param form      The form. Borrowed: it must outlive the source.
    /// @param arrayPath The collection's path (`forms::FormSession::field`).
    /// @param columns   The columns.
    /// @param members   The member of the row type each column reads, by index into @p columns; empty: each column's
    ///                  id names its member.
    /// @param perRow    For a column whose member varies by row, by index into @p columns, the member's name for a
    /// row;
    ///                  its member in @p members is then empty, and its cell is text (`detail::perRowCell`, a `Text`
    ///                  column).
    ///                  Empty: no column varies.
    /// @throws std::out_of_range when no field has @p arrayPath.
    /// @throws std::invalid_argument when the field is not a collection, @p members is neither empty nor one per
    ///         column, @p perRow is neither empty nor one per column, or a column reads no member.
    FormRowsSource(forms::FormSession& form, std::string arrayPath, std::vector<table::ColumnInfo> columns,
                   std::vector<std::string> members = {},
                   std::vector<std::function<std::string(forms::RowState const&)>> perRow = {});

    FormRowsSource(FormRowsSource const&) = delete;
    FormRowsSource& operator=(FormRowsSource const&) = delete;
    FormRowsSource(FormRowsSource&&) = delete;
    FormRowsSource& operator=(FormRowsSource&&) = delete;
    /// @brief Stops watching the field.
    ~FormRowsSource() override = default;

    /// @brief The columns.
    /// @return They, as given.
    [[nodiscard]] std::span<table::ColumnInfo const> columns() const override { return _columns; }

    /// @brief The rows as last taken in.
    /// @return The same pointer until they change.
    [[nodiscard]] std::shared_ptr<table::RowSnapshot const> snapshot() const override { return _held; }

    /// @brief Registers @p listener.
    /// @param listener The listener.
    void subscribe(table::ChangeListener& listener) override { _listeners.add(listener); }

    /// @brief Removes @p listener; safe from inside a notification.
    /// @param listener The listener.
    void unsubscribe(table::ChangeListener& listener) override { _listeners.remove(listener); }

    /// @brief The form.
    /// @return It.
    [[nodiscard]] forms::FormSession& form() const noexcept { return *_form; }

    /// @brief The collection's path.
    /// @return It, as given.
    [[nodiscard]] std::string const& arrayPath() const noexcept { return _arrayPath; }

    /// @brief The collection field.
    /// @return It.
    [[nodiscard]] forms::FieldState& array() const noexcept { return *_array; }

    /// @brief A row of the collection by its id. Untracked.
    /// @param key The row's id.
    /// @return The row, or null when the collection has none with that key.
    [[nodiscard]] forms::RowState* row(table::RowId const& key) const;

    /// @brief Groups the rows by @p by: adds the column `kGroupRunColumn` (`Integer`, laid out by no grid), whose cell
    ///        is a row's run index, runs counted over the array's order, a new run where `by` differs from the
    ///        previous row's (`detail::sameElement`). Notifies `Reset`.
    ///
    /// `by` is read untracked, whenever the rows are taken in.
    /// @param groupBy A row's group value; empty: no groups, and the column goes.
    void setGroup(std::function<Value(forms::RowState const&)> groupBy);

    /// @brief Each run's heading record, `{key, count, first, last, items}` with its heading key in
    ///        `detail::kGroupHeadingField`, by run index, as of the rows last taken in.
    /// @return They; none while the rows are not grouped.
    [[nodiscard]] std::span<Value const> runRecords() const noexcept { return _held->runs(); }

    /// @brief How many times the rows were read whole into a snapshot: once at construction and once per change taken
    ///        in, whatever reads the rows in between.
    /// @return The count.
    [[nodiscard]] std::size_t builds() const noexcept { return _builds; }

    /// @brief What `EngineRows` reads a row's key and record through: the grid's own record, `{"<member>": the
    /// member's
    ///        encoded value, null when blank or not encoding, …, "#key": the row's key}`.
    /// @return The reader; it holds nothing of the source.
    [[nodiscard]] static RecordOf recordOf();

    /// @brief Takes in the field's rows as they are now, notifying what changed since they were last taken in. Costs
    ///        nothing while neither the rows nor a member's draft changed since.
    /// @return Whether anything was taken in.
    bool sync();

private:
    // An insertion or a removal that keeps every other row and its cells: its indices; anything else is a reset.
    [[nodiscard]] static std::optional<table::RowChange> structural(detail::FormRowsSnapshot const& before,
                                                                    detail::FormRowsSnapshot const& after);

    // The version of what the rows hold, read untracked.
    [[nodiscard]] std::uint64_t currentVersion() const {
        return _form->runtime().untracked([&] { return _version->get(); });
    }

    // The rows as the field holds them now, read untracked.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): one pass building the snapshot's parallel vectors.
    [[nodiscard]] std::shared_ptr<detail::FormRowsSnapshot const> build() const;

    // The runs of the rows in the array's order: each row's run index appended to its cells, and each run's heading
    // record. A heading's key is its run's key and its ordinal among the runs with an equal key, as a `rows` table's.
    [[nodiscard]] std::vector<Value> groupRuns(std::vector<Value> const& records,
                                               std::vector<std::vector<table::Cell>>& cells) const;

    // Tracks the rows and every member's draft; a change is taken in at the owner's next turn.
    void watch();

    forms::FormSession* _form;
    std::string _arrayPath;
    forms::FieldState* _array;
    std::vector<table::ColumnInfo> _columns;
    std::vector<std::string> _members;
    std::vector<std::function<std::string(forms::RowState const&)>> _perRow;
    std::function<Value(forms::RowState const&)> _group;
    std::shared_ptr<detail::FormRowsSnapshot const> _held;
    table::detail::Listeners _listeners;
    bool _watching = false;
    bool _posted = false;
    async::CallbackScope _alive;
    mutable std::uint64_t _versions = 0;
    std::uint64_t _synced = 0;
    mutable std::uint64_t _indexed = 0;
    mutable std::unordered_map<std::uint64_t, forms::RowState*> _byKey;
    mutable std::size_t _builds = 0;
    std::unique_ptr<reactive::Computed<std::uint64_t>> _version;
    std::unique_ptr<reactive::Effect> _watch;
};

}  // namespace morph::ui
