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
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "../core/callback_scope.hpp"
#include "../forms/engine/form_session.hpp"
#include "../reactive/runtime.hpp"
#include "../reactive/signal.hpp"
#include "../table/data_source.hpp"
#include "table_rows.hpp"
#include "value.hpp"

namespace morph::ui {

/// @brief The record member a grid's row record holds its row's key in. No member of a row type has the name: `#`
///        introduces a row key in `forms::FormSession::field` paths.
inline constexpr std::string_view kGridKeyField = "#key";

namespace detail {

/// @brief The column kind a member of a form's row type sorts and filters as.
/// @param kind The member's field kind.
/// @return `Integer` for an integer or a slider, `Decimal` for a decimal or a quantity, `Number` for a plain number,
///         `Date` and `DateTime` for dates, `Bool` for a Boolean, `Text` for every other kind.
[[nodiscard]] inline table::ColumnKind columnKindOf(forms::FieldKind kind) noexcept {
    using forms::FieldKind;
    switch (kind) {
        case FieldKind::Integer:
        case FieldKind::Slider:
            return table::ColumnKind::Integer;
        case FieldKind::Decimal:
        case FieldKind::Quantity:
            return table::ColumnKind::Decimal;
        case FieldKind::Number:
            return table::ColumnKind::Number;
        case FieldKind::Date:
        case FieldKind::EpochDate:
            return table::ColumnKind::Date;
        case FieldKind::DateTime:
        case FieldKind::EpochDateTime:
            return table::ColumnKind::DateTime;
        case FieldKind::Boolean:
            return table::ColumnKind::Bool;
        default:
            return table::ColumnKind::Text;
    }
}

/// @brief A member's cell: its encoding as a cell of its column's kind, empty when blank, and, when the draft does
///        not encode, the draft's text, which a column of any kind but `Text` reads as an invalid cell.
/// @param field The member. Read untracked by the caller.
/// @param kind  The column's kind.
/// @return The cell.
// One case per kind of the column.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
[[nodiscard]] inline table::Cell memberCell(forms::FieldState const& field, table::ColumnKind kind) {
    forms::Encoding const& encoding = field.encoded().get();
    if (!encoding.has_value()) {
        return std::monostate{};
    }
    if (!encoding->has_value()) {
        return std::string{field.draft()};
    }
    forms::WireValue const& wire = **encoding;
    Value const& value = wire.value;
    switch (kind) {
        case table::ColumnKind::Integer:
            if (std::int64_t const* const whole = value.asInt()) {
                return *whole;
            }
            break;
        case table::ColumnKind::Decimal:
            if (Decimal const* const exact = value.asDecimal()) {
                return exact->value;
            }
            if (std::int64_t const* const whole = value.asInt()) {
                return math::Rational{*whole, math::DecimalPlaces{0}};
            }
            break;
        case table::ColumnKind::Number: {
            if (Decimal const* const exact = value.asDecimal()) {
                return exact->value.toDouble();
            }
            if (std::int64_t const* const whole = value.asInt()) {
                return static_cast<double>(*whole);
            }
            break;
        }
        case table::ColumnKind::Bool:
            if (bool const* const flag = value.asBool()) {
                return *flag;
            }
            break;
        case table::ColumnKind::Date: {
            if (std::int64_t const* const days = value.asInt()) {
                return *days;
            }
            std::string_view const text = forms::detail::engine::trim(field.draft());
            if (auto const midnight = forms::detail::engine::parseWallClock(std::string{text} + "T00:00")) {
                return static_cast<std::int64_t>(
                    std::chrono::floor<std::chrono::days>(*midnight).time_since_epoch().count());
            }
            break;
        }
        case table::ColumnKind::DateTime:
            if (std::int64_t const* const seconds = value.asInt()) {
                return *seconds;
            }
            if (time::DateTime const* const instant = value.asTimestamp()) {
                return static_cast<std::int64_t>(
                    std::chrono::floor<std::chrono::seconds>(instant->value).time_since_epoch().count());
            }
            break;
        default:
            if (std::string const* const text = value.asString()) {
                return *text;
            }
            return wire.json;
    }
    return std::string{field.draft()};
}

/// @brief The rows of a form's collection as a snapshot: each row's key, its cells and its record.
class FormRowsSnapshot final : public table::RowSnapshot {
public:
    /// @brief A snapshot of rows.
    /// @param columns How many columns each row has.
    /// @param keys    The rows' keys.
    /// @param cells   The rows' cells, one vector per row.
    /// @param records The rows' records.
    FormRowsSnapshot(std::size_t columns, std::vector<table::RowId> keys, std::vector<std::vector<table::Cell>> cells,
                     std::vector<Value> records)
        // Parentheses for the records: braces would make a list of one value holding them all.
        : _columns{columns}, _keys{std::move(keys)}, _cells{std::move(cells)}, _records(std::move(records)) {}

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

private:
    std::size_t _columns;
    std::vector<table::RowId> _keys;
    std::vector<std::vector<table::Cell>> _cells;
    std::vector<Value> _records;
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
class FormRowsSource final : public table::DataSource {
public:
    /// @brief A source over a collection of @p form.
    /// @param form      The form. Borrowed: it must outlive the source.
    /// @param arrayPath The collection's path (`forms::FormSession::field`).
    /// @param columns   The columns, each id the name of a member of the row type.
    /// @throws std::out_of_range when no field has @p arrayPath.
    /// @throws std::invalid_argument when the field is not a collection, or a column names no member.
    FormRowsSource(forms::FormSession& form, std::string arrayPath, std::vector<table::ColumnInfo> columns)
        : _form{&form}, _arrayPath{std::move(arrayPath)}, _array{&form.field(_arrayPath)}, _columns{std::move(columns)} {
        if (_array->spec().kind != forms::FieldKind::ObjectArray || _array->spec().truncated) {
            throw std::invalid_argument{"morph::ui::FormRowsSource: `" + _arrayPath + "` is not a collection"};
        }
        for (table::ColumnInfo const& column : _columns) {
            if (std::ranges::none_of(_array->spec().children,
                                     [&column](forms::FieldSpec const& member) { return member.name == column.id; })) {
                throw std::invalid_argument{"morph::ui::FormRowsSource: no member `" + column.id + "` in `" +
                                            _arrayPath + "`"};
            }
        }
        _held = build();
        _watch = std::make_unique<reactive::Effect>(form.runtime(), [this] { watch(); });
    }

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
    [[nodiscard]] forms::RowState* row(table::RowId const& key) const {
        std::int64_t const* const number = std::get_if<std::int64_t>(&key);
        if (number == nullptr || *number < 0) {
            return nullptr;
        }
        return _form->runtime().untracked([&]() -> forms::RowState* {
            auto const& rows = _array->rows();
            auto const found = std::ranges::find(rows, static_cast<std::uint64_t>(*number), &forms::RowState::key);
            return found == rows.end() ? nullptr : found->get();
        });
    }

    /// @brief What `EngineRows` reads a row's key and record through: the grid's own record, `{"<member>": the member's
    ///        encoded value, null when blank or not encoding, …, "#key": the row's key}`.
    /// @return The reader; it holds nothing of the source.
    [[nodiscard]] RecordOf recordOf() const {
        return [](table::RowSnapshot const& snapshot, std::size_t sourceRow) {
            auto const* const rows = dynamic_cast<detail::FormRowsSnapshot const*>(&snapshot);
            return RowRecord{.key = snapshot.rowId(sourceRow), .row = rows == nullptr ? Value{} : rows->record(sourceRow)};
        };
    }

    /// @brief Takes in the field's rows as they are now, notifying what changed since they were last taken in.
    void sync() {
        _posted = false;
        auto next = build();
        auto const& before = *_held;
        auto const& after = *next;
        if (before.keys() == after.keys()) {
            std::vector<std::size_t> changed;
            for (std::size_t row = 0; row < after.rowCount(); ++row) {
                if (before.cells().at(row) != after.cells().at(row) || !(before.record(row) == after.record(row))) {
                    changed.push_back(row);
                }
            }
            if (changed.empty()) {
                return;
            }
            _held = std::move(next);
            _listeners.notify(table::RowChange{.kind = table::ChangeKind::Updated, .rows = std::move(changed)});
            return;
        }
        std::optional<table::RowChange> const change = structural(before, after);
        _held = std::move(next);
        _listeners.notify(change.value_or(table::RowChange{.kind = table::ChangeKind::Reset, .rows = {}}));
    }

private:
    // An insertion or a removal that keeps every other row and its cells: its indices; anything else is a reset.
    [[nodiscard]] static std::optional<table::RowChange> structural(detail::FormRowsSnapshot const& before,
                                                                    detail::FormRowsSnapshot const& after) {
        bool const growing = after.rowCount() > before.rowCount();
        detail::FormRowsSnapshot const& longer = growing ? after : before;
        detail::FormRowsSnapshot const& shorter = growing ? before : after;
        std::vector<std::size_t> rows;
        std::size_t kept = 0;
        for (std::size_t at = 0; at < longer.rowCount(); ++at) {
            if (kept < shorter.rowCount() && longer.keys().at(at) == shorter.keys().at(kept)) {
                if (longer.cells().at(at) != shorter.cells().at(kept) ||
                    !(longer.record(at) == shorter.record(kept))) {
                    return std::nullopt;
                }
                ++kept;
            } else {
                rows.push_back(at);
            }
        }
        if (kept != shorter.rowCount()) {
            return std::nullopt;
        }
        return table::RowChange{.kind = growing ? table::ChangeKind::Inserted : table::ChangeKind::Removed,
                                .rows = std::move(rows)};
    }

    // The rows as the field holds them now, read untracked.
    [[nodiscard]] std::shared_ptr<detail::FormRowsSnapshot const> build() const {
        return _form->runtime().untracked([&] {
            std::vector<table::RowId> keys;
            std::vector<std::vector<table::Cell>> cells;
            std::vector<Value> records;
            for (auto const& row : _array->rows()) {
                keys.emplace_back(static_cast<std::int64_t>(row->key()));
                std::vector<table::Cell> rowCells;
                rowCells.reserve(_columns.size());
                for (table::ColumnInfo const& column : _columns) {
                    forms::FieldState const* const member = row->field(column.id);
                    rowCells.push_back(member == nullptr ? table::Cell{} : detail::memberCell(*member, column.kind));
                }
                cells.push_back(std::move(rowCells));
                ValueRecord record;
                for (auto const& member : row->fields()) {
                    forms::Encoding const& encoding = member->encoded().get();
                    record.insert_or_assign(member->spec().name, encoding.has_value() && encoding->has_value()
                                                                      ? (*encoding)->value
                                                                      : Value{});
                }
                record.insert_or_assign(std::string{kGridKeyField}, Value{static_cast<std::int64_t>(row->key())});
                records.emplace_back(std::move(record));
            }
            return std::make_shared<detail::FormRowsSnapshot const>(_columns.size(), std::move(keys), std::move(cells),
                                                                    std::move(records));
        });
    }

    // Tracks the rows and every member's draft; a change is taken in at the owner's next turn.
    void watch() {
        for (auto const& row : _array->rows()) {
            for (auto const& member : row->fields()) {
                static_cast<void>(member->encoded().get());
                static_cast<void>(member->draft());
            }
        }
        if (_watching && !_posted) {
            _posted = true;
            _form->runtime().owner().post(_alive.guard([this] { sync(); }));
        }
        _watching = true;
    }

    forms::FormSession* _form;
    std::string _arrayPath;
    forms::FieldState* _array;
    std::vector<table::ColumnInfo> _columns;
    std::shared_ptr<detail::FormRowsSnapshot const> _held;
    table::detail::Listeners _listeners;
    bool _watching = false;
    bool _posted = false;
    async::CallbackScope _alive;
    std::unique_ptr<reactive::Effect> _watch;
};

}  // namespace morph::ui
