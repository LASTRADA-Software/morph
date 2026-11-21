// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file ui/grid.hpp
/// @brief A grid: a table whose cells are edited. `GridController` holds a grid's focus, its cell editor and its walk
///        over a `GridSource`; `FormGridSource` is a grid of a form's `ObjectArray` field, and `makeFormGrid` the node
///        that shows one.
///
/// Specified in `docs/spec/table/editing.md`.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "../forms/engine/field_control.hpp"
#include "../forms/engine/form_session.hpp"
#include "../reactive/runtime.hpp"
#include "../reactive/signal.hpp"
#include "cell_format.hpp"
#include "expression.hpp"
#include "form_rows.hpp"
#include "table_rows.hpp"
#include "view.hpp"

namespace morph::ui {

/// @brief Where `Enter` moves after it commits a cell's editor.
enum class EnterMoves : std::uint8_t {
    Down,   ///< To the first editable cell below, in the same column.
    Right,  ///< To the next editable cell of the row.
    None,   ///< Nowhere: focus stays on the cell.
};

/// @brief The order `Tab` walks a grid's editable cells in.
enum class Walk : std::uint8_t {
    Rows,     ///< Along a row, then the next row.
    Columns,  ///< Down a column, then the next column.
};

/// @brief One column of a grid of a form: a member of the row type it edits, or a display column.
struct GridColumn {
    /// @brief The column's id (`TableColumn::id`).
    std::string id;
    /// @brief The header's text.
    std::string label;
    /// @brief The member of the row type the column edits; empty for a display column.
    std::string field;
    /// @brief A display column's text for a row; read only while `field` is empty.
    std::function<std::string(forms::RowState const&)> value;
};

/// @brief How a grid walks and what activating a read-only cell does.
struct GridOptions {
    /// @brief Where `Enter` moves after a commit.
    EnterMoves enterMoves = EnterMoves::Down;
    /// @brief The order `Tab` follows.
    Walk walk = Walk::Rows;
    /// @brief The table's `onActivate`: what `Enter` or a double click on a read-only cell runs, with its row's key.
    std::function<void(Key)> onActivate;
};

/// @brief Every fact a cell's read-only state follows from; `cellReadOnly` is the one rule over them.
struct CellFacts {
    /// @brief The cell's field; null for a column with no field (a display column), or a cell with none.
    forms::FieldState const* field = nullptr;
    /// @brief Whether the column has an edit: a form's member, or an edit action.
    bool hasEdit = true;
    /// @brief The column's `readonly` overlay, for the cell's row.
    bool overlayReadOnly = false;
    /// @brief The column's `EditInfo::readonly` companion, for the cell's row.
    bool companionReadOnly = false;
    /// @brief The row is busy with an action that is not the table's own edit.
    bool busy = false;
    /// @brief The row is locked by someone else.
    bool locked = false;
    /// @brief The cell's value is not loaded.
    bool unloaded = false;
};

/// @brief Whether a cell is read-only: what the walk skips, what refuses an edit, a paste and a fill, and what draws
///        on the read-only surface. Tracked through the field's `readonly()`.
/// @param facts The cell's facts.
/// @return `true` when the cell has no field or no edit, its field is read-only (`x-readonly`, `x-computed`,
///         `x-derived`, a `readonlyWhen` reaching it), or any other fact holds.
[[nodiscard]] inline bool cellReadOnly(CellFacts const& facts) {
    if (facts.field == nullptr || !facts.hasEdit) {
        return true;
    }
    return facts.field->readonly().get() || facts.overlayReadOnly || facts.companionReadOnly || facts.busy ||
           facts.locked || facts.unloaded;
}

/// @brief What a grid edits: its rows by view index, which a model's table holds only a window of, and its cells by
///        row key and column id.
// NOLINTBEGIN(cppcoreguidelines-special-member-functions)
class GridSource {
public:
    virtual ~GridSource() = default;

    /// @brief The view's rows, fetched or not.
    /// @return The count.
    [[nodiscard]] virtual std::size_t rowCount() const = 0;

    /// @brief The key of the row at a view position.
    /// @param viewRow The position.
    /// @return The key; empty while that row is not fetched.
    [[nodiscard]] virtual std::optional<Key> keyAt(std::size_t viewRow) const = 0;

    /// @brief Where a key's row is in the view now.
    /// @param key The key.
    /// @return The view position; empty when the view has no row with the key.
    [[nodiscard]] virtual std::optional<std::size_t> positionOf(Key const& key) const = 0;

    /// @brief The ids of the visible columns in layout order: the walk's order. A cell is named by its column's id.
    /// @return The ids.
    [[nodiscard]] virtual std::vector<std::string> columns() const = 0;

    /// @brief The editor of an editable cell, built once per opening: the forms control of the cell's field. Never
    ///        asked of a read-only cell, or of one that `toggles`.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return The editor.
    [[nodiscard]] virtual Node editor(Key const& key, std::string_view column) = 0;

    /// @brief Whether a cell's field is a Boolean, which `Space` flips and which opens no editor.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return `true` for a Boolean's cell.
    [[nodiscard]] virtual bool toggles(Key const& key, std::string_view column) const = 0;

    /// @brief Whether a cell is read-only (`cellReadOnly`). Tracked.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return `true` when read-only.
    [[nodiscard]] virtual bool readOnly(Key const& key, std::string_view column) const = 0;

    /// @brief Whether a cell is covered by a span.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return `true` when covered.
    [[nodiscard]] virtual bool covered(Key const& key, std::string_view column) const = 0;

    /// @brief A cell's draft, as the user typed it. Untracked.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return The draft; empty for a cell with no field.
    [[nodiscard]] virtual std::string draft(Key const& key, std::string_view column) const = 0;

    /// @brief A cell's display text. Tracked.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return The text.
    [[nodiscard]] virtual std::string text(Key const& key, std::string_view column) const = 0;

    /// @brief Why a cell's value is refused. Tracked.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return The message; empty for none.
    [[nodiscard]] virtual std::optional<std::string> error(Key const& key, std::string_view column) const = 0;

    /// @brief Writes a user's draft into a cell.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @param draft  The draft.
    virtual void write(Key const& key, std::string_view column, std::string draft) = 0;

    /// @brief An edit is meant: called once per change a user commits.
    virtual void committed() = 0;

    /// @brief A cell's marks. Tracked: every read it depends on is tracked, so a binding over it follows drafts,
    ///        errors, edits and row states.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return The marks.
    [[nodiscard]] virtual CellMarks marks(Key const& key, std::string_view column) const = 0;

    /// @brief The key of the row a record is: a cell view knows its row only through the record its signal holds.
    /// @param record The record.
    /// @return The key; empty for a record with no key, which draws no marks.
    [[nodiscard]] virtual std::optional<Key> keyOf(Value const& record) const = 0;

    /// @brief Whether some cell of a row is modified, or the row was added since the load. Tracked.
    /// @param key The row's key.
    /// @return `true` when modified.
    [[nodiscard]] virtual bool rowModified(Key const& key) const = 0;

    /// @brief The data rows, by view index: what `rowCount`, `keyAt` and `positionOf` count.
    /// @return The rows.
    [[nodiscard]] virtual TableRows& view() = 0;

    /// @brief Restores a cell's loaded value. A source with no loaded values does nothing.
    /// @param key    The row's key.
    /// @param column The column's id.
    virtual void revertCell(Key const& key, std::string_view column) {
        static_cast<void>(key);
        static_cast<void>(column);
    }

    /// @brief Restores a row's loaded values. A source with no loaded values does nothing.
    /// @param key The row's key.
    virtual void revertRow(Key const& key) { static_cast<void>(key); }
};
// NOLINTEND(cppcoreguidelines-special-member-functions)

/// @brief The rows of a grid of a form: the engine's view over a `FormRowsSource`, less the rows added since the sort
///        or the filter last changed, which follow it in the order they were added and pass the filter until it
///        changes. With no sort and no filter the view is the array's order, an inserted row in its place.
///
/// A grid of a form runs no action per row: its `states()` hold nothing, and `moveKey` does nothing, since a form
/// row's key never changes.
class GridRows final : public TableRows, private table::ChangeListener {
public:
    /// @brief Rows over a form's collection.
    /// @param source The collection's rows. Must not be null.
    /// @throws std::invalid_argument when @p source is null.
    explicit GridRows(std::shared_ptr<FormRowsSource> source) : _source{checked(std::move(source))} {
        // Subscribed before the engine, so a row added while the view is ordered is known to be added when the
        // engine's change for it arrives.
        _source->subscribe(*this);
        table::EngineOptions options;
        options.owner = &_source->form().runtime().owner();
        options.worker = nullptr;
        options.services.collator = std::make_shared<ValueRowsCollator const>();
        _engine = std::make_unique<EngineRows>(_source, _source->recordOf(), std::move(options));
        rebuild();
        _engine->onChange([this](table::ViewChange const& change) {
            bool const appendedBefore = !_appended.empty();
            rebuild();
            if (!_onChange) {
                return;
            }
            if (!appendedBefore && _appended.empty()) {
                _onChange(change);
                return;
            }
            table::ViewChange reset;
            reset.ops.push_back(table::ViewOp{.kind = table::ViewOp::Kind::Reset, .first = 0, .count = 0, .to = 0});
            _onChange(reset);
        });
    }

    GridRows(GridRows const&) = delete;
    GridRows& operator=(GridRows const&) = delete;
    GridRows(GridRows&&) = delete;
    GridRows& operator=(GridRows&&) = delete;

    /// @brief Stops hearing the source; the engine goes first.
    ~GridRows() override {
        _engine.reset();
        _source->unsubscribe(*this);
    }

    /// @brief The source.
    /// @return It.
    [[nodiscard]] FormRowsSource& source() const noexcept { return *_source; }

    /// @brief Takes in the field's rows as they are now, and the engine the change at once, so the view read next is
    ///        the field's.
    void sync() {
        if (_source->sync()) {
            _engine->engine().flush();
        }
    }

    /// @brief Every row of the view, counted exactly.
    /// @return The total.
    [[nodiscard]] TableTotal total() const override {
        return TableTotal{.rows = static_cast<std::int64_t>(_items.size()), .kind = table::TotalKind::Exact, .known = true};
    }

    /// @brief The row at a view index.
    /// @param index The view index.
    /// @return The row; nothing past the view.
    [[nodiscard]] std::optional<TableRowRef> row(std::int64_t index) const override {
        if (index < 0 || static_cast<std::size_t>(index) >= _items.size()) {
            return std::nullopt;
        }
        Item const& item = _items.at(static_cast<std::size_t>(index));
        if (item.engine) {
            return _engine->row(static_cast<std::int64_t>(*item.engine));
        }
        RowRecord const* const record = heldRecord(item.key);
        if (record == nullptr) {
            return std::nullopt;
        }
        return TableRowRef{.key = record->key, .row = record->row};
    }

    /// @brief No level: every cell of a form is here.
    /// @param index Unused.
    /// @return Nothing unloaded.
    [[nodiscard]] table::UnloadedLevels unloadedLevels(std::int64_t index) const override {
        static_cast<void>(index);
        return {};
    }

    /// @brief Does nothing: every row is present.
    /// @param first   Unused.
    /// @param count   Unused.
    /// @param columns Unused.
    void visible(std::int64_t first, std::int64_t count, std::span<std::string const> columns) override {
        static_cast<void>(first);
        static_cast<void>(count);
        static_cast<void>(columns);
    }

    /// @brief The keys of a range of view rows, at once.
    /// @param first The first view row.
    /// @param count The rows.
    /// @return The keys; `InvalidSpec` for a range outside the view.
    async::Completion<std::expected<std::vector<table::RowId>, table::TableError>> keysOf(std::int64_t first,
                                                                                       std::int64_t count) override {
        using Result = std::expected<std::vector<table::RowId>, table::TableError>;
        auto [completion, promise] = async::Completion<Result>::makeSettleable(&_source->form().runtime().owner());
        auto const rows = static_cast<std::int64_t>(_items.size());
        if (first < 0 || count < 0 || count > rows - first) {
            promise.resolve(Result{std::unexpected(table::TableError{
                .code = table::TableErrorCode::InvalidSpec, .column = {}, .message = "a range outside the view"})});
            return std::move(completion);
        }
        std::vector<table::RowId> keys;
        for (std::int64_t at = first; at < first + count; ++at) {
            keys.push_back(_items.at(static_cast<std::size_t>(at)).key);
        }
        promise.resolve(Result{std::move(keys)});
        return std::move(completion);
    }

    /// @brief Finds a key's view index.
    /// @param key  The row's key.
    /// @param stop Once a stop is requested, @p done is not called.
    /// @param done Called on the owner with the index, or nothing when the view lacks the key.
    void locateRow(table::RowId key, std::stop_token stop, std::function<void(RowLocation const&)> done) override {
        std::optional<std::size_t> const at = positionOf(key);
        RowLocation const found{at ? std::optional{static_cast<std::int64_t>(*at)} : std::nullopt};
        _source->form().runtime().owner().post([stop = std::move(stop), done = std::move(done), found] {
            if (!stop.stop_requested()) {
                done(found);
            }
        });
    }

    /// @brief Rows by key, from the engine.
    /// @param keys    The keys.
    /// @param columns The columns.
    /// @return The engine's answer.
    async::Completion<std::expected<std::vector<FetchedRow>, table::TableError>> fetchRows(
        std::vector<table::RowId> keys, std::vector<std::string> columns) override {
        return _engine->fetchRows(std::move(keys), std::move(columns));
    }

    /// @brief Sets the sort: the rows added since the last sort or filter change join the order.
    /// @param sort The keys.
    /// @return The engine's error, or nothing.
    std::expected<void, table::TableError> setSort(SortChain sort) override {
        sync();
        _sorted = !sort.empty();
        _appended.clear();
        auto result = _engine->setSort(std::move(sort));
        rebuild();
        return result;
    }

    /// @brief Sets the filter: the rows added since the last sort or filter change join the view or leave it.
    /// @param filter The filter.
    /// @return The engine's error, or nothing.
    std::expected<void, table::TableError> setFilter(table::FilterSpec filter) override {
        sync();
        _filtered = !(filter == table::FilterSpec{});
        _appended.clear();
        auto result = _engine->setFilter(std::move(filter));
        rebuild();
        return result;
    }

    /// @brief A column's distinct values, from the engine.
    /// @param request The request.
    /// @return The engine's answer.
    async::Completion<std::expected<table::DistinctValues, table::TableError>> distinctValues(
        table::DistinctRequest request) override {
        return _engine->distinctValues(std::move(request));
    }

    /// @brief The counts: the view's rows, and the engine's source and pending state.
    /// @return The counts.
    [[nodiscard]] TableCountsView counts() const override {
        TableCountsView counts = _engine->counts();
        counts.view = total();
        return counts;
    }

    /// @brief No row of a form is busy or failed.
    /// @return The empty states.
    [[nodiscard]] RowStates& states() override { return _states; }

    /// @brief No row of a form is busy or failed.
    /// @return The empty states.
    [[nodiscard]] RowStates const& states() const override { return _states; }

    /// @brief Does nothing: a form row's key never changes.
    /// @param from Unused.
    /// @param to   Unused.
    void moveKey(table::RowId const& from, table::RowId const& to) override {
        static_cast<void>(from);
        static_cast<void>(to);
    }

    /// @brief Sets the one receiver of the view's changes.
    /// @param handler The receiver; empty to stop receiving.
    void onChange(std::function<void(table::ViewChange const&)> handler) override { _onChange = std::move(handler); }

    /// @brief Where a key is in the view.
    /// @param key The key.
    /// @return Its view index, or nothing.
    [[nodiscard]] std::optional<std::size_t> positionOf(table::RowId const& key) const {
        auto const found = _positions.find(key);
        return found == _positions.end() ? std::nullopt : std::optional{found->second};
    }

private:
    // A view row: its key, and its place in the engine's view, if it has one.
    struct Item {
        std::optional<std::size_t> engine;
        table::RowId key;
    };

    [[nodiscard]] static std::shared_ptr<FormRowsSource> checked(std::shared_ptr<FormRowsSource> source) {
        if (source == nullptr) {
            throw std::invalid_argument{"morph::ui::GridRows: the source must not be null"};
        }
        return source;
    }

    // A row added while the view is ordered is shown at its end until the order next changes.
    void rowsChanged(table::RowChange const& change) override {
        _records.clear();
        if (change.kind != table::ChangeKind::Inserted || (!_sorted && !_filtered)) {
            return;
        }
        auto const snapshot = _source->snapshot();
        for (std::size_t const row : change.rows) {
            if (row < snapshot->rowCount()) {
                _appended.push_back(snapshot->rowId(row));
            }
        }
    }

    // The view: the engine's rows but the added ones, then the added ones still in the source.
    void rebuild() {
        _items.clear();
        _positions.clear();
        std::unordered_set<table::RowId> const appended(_appended.begin(), _appended.end());
        TableTotal const engineTotal = _engine->total();
        std::unordered_map<table::RowId, std::size_t> inEngine;
        for (std::int64_t at = 0; at < engineTotal.rows; ++at) {
            std::optional<TableRowRef> const ref = _engine->row(at);
            if (!ref) {
                continue;
            }
            inEngine.emplace(ref->key, static_cast<std::size_t>(at));
            if (!appended.contains(ref->key)) {
                _items.push_back(Item{.engine = static_cast<std::size_t>(at), .key = ref->key});
            }
        }
        std::erase_if(_appended, [this](table::RowId const& key) { return _source->row(key) == nullptr; });
        for (table::RowId const& key : _appended) {
            auto const found = inEngine.find(key);
            _items.push_back(Item{.engine = found == inEngine.end() ? std::nullopt : std::optional{found->second},
                                  .key = key});
        }
        for (std::size_t at = 0; at < _items.size(); ++at) {
            _positions.emplace(_items.at(at).key, at);
        }
    }

    // The record of an added row the filter rejects, built from the source's snapshot and kept until it changes.
    [[nodiscard]] RowRecord const* heldRecord(table::RowId const& key) const {
        if (auto const found = _records.find(key); found != _records.end()) {
            return &found->second;
        }
        auto const snapshot = _source->snapshot();
        for (std::size_t row = 0; row < snapshot->rowCount(); ++row) {
            if (snapshot->rowId(row) == key) {
                return &_records.insert_or_assign(key, _source->recordOf()(*snapshot, row)).first->second;
            }
        }
        return nullptr;
    }

    std::shared_ptr<FormRowsSource> _source;
    std::unique_ptr<EngineRows> _engine;
    std::function<void(table::ViewChange const&)> _onChange;
    std::vector<table::RowId> _appended;
    std::vector<Item> _items;
    std::unordered_map<table::RowId, std::size_t> _positions;
    mutable std::unordered_map<table::RowId, RowRecord> _records;
    RowStates _states;
    bool _sorted = false;
    bool _filtered = false;
};

/// @brief The formatting a form's values are shown in, read from its options when called: a later
///        `forms::FormSession::setFormat` is read by the next call.
/// @param form The form.
/// @return `{locale: options().bcp47, numeric: options().locale, datePattern, dateTimePattern}`.
[[nodiscard]] inline FormatContext formatOf(forms::FormSession const& form) {
    forms::FormSessionOptions const& options = form.options();
    FormatContext format;
    format.locale = options.bcp47;
    format.numeric = options.locale;
    format.datePattern = options.datePattern;
    format.dateTimePattern = options.dateTimePattern;
    return format;
}

/// @brief A grid of a form's `ObjectArray` field: its rows are the field's, and each cell is a member's field.
///
/// Every row of a form is fetched, so `keyAt` is never empty below `rowCount`. A cell's text is its draft while blank or
/// not encoding, else its value shown at the places its field takes.
class FormGridSource final : public GridSource {
public:
    /// @brief A grid of a form's collection.
    /// @param form      The form. Borrowed: it must outlive the source.
    /// @param arrayPath The collection's path.
    /// @param columns   The columns, each id once.
    /// @param format    How values are shown.
    /// @throws std::out_of_range when no field has @p arrayPath.
    /// @throws std::invalid_argument when the field is not a collection, or a column's `field` names no member.
    FormGridSource(forms::FormSession& form, std::string arrayPath, std::vector<GridColumn> columns,
                   FormatContext format)
        : _form{&form},
          _formAlive{form.alive()},
          _arrayPath{std::move(arrayPath)},
          _columns{std::move(columns)},
          _format{std::move(format)},
          _formatVersion{form.runtime(), std::uint64_t{0}} {
        forms::FieldState const& array = form.field(_arrayPath);
        if (array.spec().kind != forms::FieldKind::ObjectArray || array.spec().truncated) {
            throw std::invalid_argument{"morph::ui::FormGridSource: `" + _arrayPath + "` is not a collection"};
        }
        for (GridColumn const& column : _columns) {
            if (!column.field.empty() &&
                std::ranges::none_of(array.spec().children,
                                     [&column](forms::FieldSpec const& member) { return member.name == column.field; })) {
                throw std::invalid_argument{"morph::ui::FormGridSource: no member `" + column.field + "` in `" +
                                            _arrayPath + "`"};
            }
            _layout.push_back(column.id);
        }
        std::vector<table::ColumnInfo> infos;
        for (forms::FieldSpec const& member : array.spec().children) {
            infos.push_back(table::ColumnInfo{.id = member.name, .kind = detail::columnKindOf(member.kind)});
        }
        _rowsSource = std::make_shared<FormRowsSource>(form, _arrayPath, std::move(infos));
        _rows = std::make_unique<GridRows>(_rowsSource);
    }

    FormGridSource(FormGridSource const&) = delete;
    FormGridSource& operator=(FormGridSource const&) = delete;
    FormGridSource(FormGridSource&&) = delete;
    FormGridSource& operator=(FormGridSource&&) = delete;

    /// @brief Removes the commit `onBeforeSubmit` registered.
    ~FormGridSource() override {
        if (_hook && !_formAlive.expired()) {
            _form->removeBeforeSubmit(*_hook);
        }
    }

    /// @brief The rows of the view; the field's rows are taken in first.
    /// @return The count.
    [[nodiscard]] std::size_t rowCount() const override {
        _rows->sync();
        return static_cast<std::size_t>(_rows->total().rows);
    }

    /// @brief The key at a view position; the field's rows are taken in first.
    /// @param viewRow The position.
    /// @return The key; empty past the view.
    [[nodiscard]] std::optional<Key> keyAt(std::size_t viewRow) const override {
        _rows->sync();
        std::optional<TableRowRef> const ref = _rows->row(static_cast<std::int64_t>(viewRow));
        return ref ? std::optional<Key>{ref->key} : std::nullopt;
    }

    /// @brief Where a key is in the view; the field's rows are taken in first.
    /// @param key The key.
    /// @return The position, or nothing.
    [[nodiscard]] std::optional<std::size_t> positionOf(Key const& key) const override {
        _rows->sync();
        return _rows->positionOf(key);
    }

    /// @brief The visible columns in layout order.
    /// @return The ids, as `setLayout` gave them; every column in declared order before it.
    [[nodiscard]] std::vector<std::string> columns() const override { return _layout; }

    /// @brief Shows the cells in another formatting from now on: a mounted cell's text follows.
    /// @param format How values are shown.
    void setFormat(FormatContext format) {
        _format = std::move(format);
        _formatVersion.set(_formatVersion.peek() + 1);
    }

    /// @brief Sets the visible columns in layout order: what the walk follows.
    /// @param visible The column ids.
    void setLayout(std::vector<std::string> visible) { _layout = std::move(visible); }

    /// @brief A cell's field.
    /// @param row    The row's key.
    /// @param column The column's id.
    /// @return The field; null for a display column, an unknown column, or a row the field does not hold.
    [[nodiscard]] forms::FieldState* field(Key const& row, std::string_view column) const {
        GridColumn const* const declared = columnOf(column);
        if (declared == nullptr || declared->field.empty()) {
            return nullptr;
        }
        forms::RowState* const held = _rowsSource->row(row);
        return held == nullptr ? nullptr : held->field(declared->field);
    }

    /// @brief The forms control of the cell's field.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return The editor; a muted text for a cell with no field.
    [[nodiscard]] Node editor(Key const& key, std::string_view column) override {
        forms::FieldState* const state = field(key, column);
        if (state == nullptr) {
            return ui::text(Text{.text = std::string{}, .role = TextRole::Muted});
        }
        return forms::detail::engine::scalarControl(*_form, *state);
    }

    /// @brief Whether the cell's field is a Boolean.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return `true` for a Boolean.
    [[nodiscard]] bool toggles(Key const& key, std::string_view column) const override {
        forms::FieldState const* const state = field(key, column);
        return state != nullptr && state->spec().kind == forms::FieldKind::Boolean;
    }

    /// @brief `cellReadOnly` over the cell's field. Tracked.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return `true` when read-only.
    [[nodiscard]] bool readOnly(Key const& key, std::string_view column) const override {
        GridColumn const* const declared = columnOf(column);
        return cellReadOnly(CellFacts{.field = field(key, column), .hasEdit = declared != nullptr && !declared->field.empty()});
    }

    /// @brief No cell of a grid of a form is covered.
    /// @param key    Unused.
    /// @param column Unused.
    /// @return `false`.
    [[nodiscard]] bool covered(Key const& key, std::string_view column) const override {
        static_cast<void>(key);
        static_cast<void>(column);
        return false;
    }

    /// @brief The cell's draft. Untracked.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return The draft.
    [[nodiscard]] std::string draft(Key const& key, std::string_view column) const override {
        forms::FieldState const* const state = field(key, column);
        return state == nullptr ? std::string{} : _form->runtime().untracked([&] { return state->draft(); });
    }

    /// @brief The cell's text: a display column's value, a draft while blank or not encoding, else the value at the
    ///        field's places. Tracked.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return The text.
    [[nodiscard]] std::string text(Key const& key, std::string_view column) const override {
        GridColumn const* const declared = columnOf(column);
        if (declared == nullptr) {
            return {};
        }
        if (declared->field.empty()) {
            forms::RowState const* const held = _rowsSource->row(key);
            return held != nullptr && declared->value ? declared->value(*held) : std::string{};
        }
        forms::FieldState* const state = field(key, column);
        if (state == nullptr) {
            return {};
        }
        forms::Encoding const& encoding = state->encoded().get();
        if (!encoding.has_value() || !encoding->has_value()) {
            return state->draft();
        }
        forms::FieldSpec const& spec = state->spec();
        table::TableColumn shown;
        shown.info = table::ColumnInfo{.id = std::string{column}, .kind = detail::columnKindOf(spec.kind)};
        if (std::optional<std::uint32_t> const places = state->places().get()) {
            shown.display.decimals = places;
        } else if (spec.decimalPlaces) {
            shown.display.decimals = spec.decimalPlaces;
        } else if (spec.displayDecimals) {
            shown.display.decimals = spec.displayDecimals;
        } else if (spec.kind == forms::FieldKind::Integer) {
            shown.display.decimals = 0;
        }
        static_cast<void>(_formatVersion.get());
        return formatCell(detail::memberCell(*state, shown.info.kind), shown, _format);
    }

    /// @brief Why the cell's value is refused: its draft does not encode, or the last submission refused it. Tracked.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return The forms error's message, else the refusal's; empty for none.
    [[nodiscard]] std::optional<std::string> error(Key const& key, std::string_view column) const override {
        forms::FieldState const* const state = field(key, column);
        if (state == nullptr) {
            return std::nullopt;
        }
        std::optional<forms::FieldError> const& failed = state->error().get();
        return failed ? std::optional{failed->message} : _form->refusal(state->path());
    }

    /// @brief Writes a user's draft into the cell's field.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @param draft  The draft.
    void write(Key const& key, std::string_view column, std::string draft) override {
        if (forms::FieldState* const state = field(key, column)) {
            state->text().set(std::move(draft));
        }
    }

    /// @brief An edit is meant; tells the observer `onCommitted` gave.
    void committed() override {
        if (_committed) {
            _committed();
        }
    }

    /// @brief Calls @p observer at the end of each `committed`: a test seam.
    /// @param observer The observer; empty for none.
    void onCommitted(std::function<void()> observer) { _committed = std::move(observer); }

    /// @brief The cell's marks: read-only, modified, and not encoding. Tracked.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return The marks.
    [[nodiscard]] CellMarks marks(Key const& key, std::string_view column) const override {
        forms::FieldState const* const state = field(key, column);
        return CellMarks{.readOnly = readOnly(key, column),
                         .modified = state != nullptr && _form->modified(state->path()),
                         .error = error(key, column).has_value()};
    }

    /// @brief The key a grid's record holds in `#key`.
    /// @param record The record.
    /// @return The key; empty for a record without one.
    [[nodiscard]] std::optional<Key> keyOf(Value const& record) const override {
        Value const* const key = record.field(kGridKeyField);
        std::int64_t const* const number = key == nullptr ? nullptr : key->asInt();
        return number == nullptr ? std::nullopt : std::optional<Key>{Key{*number}};
    }

    /// @brief `forms::FormSession::rowModified` of the row. Tracked.
    /// @param key The row's key.
    /// @return `true` when modified.
    [[nodiscard]] bool rowModified(Key const& key) const override {
        std::int64_t const* const number = std::get_if<std::int64_t>(&key);
        return number != nullptr && *number >= 0 && _form->rowModified(_arrayPath, static_cast<std::uint64_t>(*number));
    }

    /// @brief The grid's rows.
    /// @return They.
    [[nodiscard]] GridRows& view() override { return *_rows; }

    /// @brief Restores the cell's loaded value.
    /// @param key    The row's key.
    /// @param column The column's id.
    void revertCell(Key const& key, std::string_view column) override {
        if (forms::FieldState const* const state = field(key, column)) {
            _form->revert(state->path());
        }
    }

    /// @brief Restores the row's loaded values; a row added since the load is removed.
    /// @param key The row's key.
    void revertRow(Key const& key) override {
        if (std::int64_t const* const number = std::get_if<std::int64_t>(&key)) {
            _form->revert(_arrayPath + "#" + std::to_string(*number));
        }
    }

    /// @brief Registers @p commit with the form's submit for the source's life, so the form's submit ends an open edit
    ///        first.
    /// @param commit The commit.
    void onBeforeSubmit(std::function<void()> commit) {
        if (_hook) {
            _form->removeBeforeSubmit(*_hook);
        }
        _hook = _form->addBeforeSubmit(std::move(commit));
    }

    /// @brief How many cells of the grid's columns are modified. Tracked.
    /// @return The count.
    [[nodiscard]] std::size_t modifiedCount() const {
        return countCells([this](forms::FieldState const& state) { return _form->modified(state.path()); });
    }

    /// @brief How many rows are modified. Tracked.
    /// @return The count.
    [[nodiscard]] std::size_t modifiedRows() const {
        std::size_t rows = 0;
        for (auto const& row : _rowsSource->array().rows()) {
            rows += _form->rowModified(_arrayPath, row->key()) ? 1 : 0;
        }
        return rows;
    }

    /// @brief How many cells of the grid's columns do not encode, or were refused. Tracked.
    /// @return The count.
    [[nodiscard]] std::size_t errorCount() const {
        return countCells([this](forms::FieldState const& state) {
            return state.error().get().has_value() || _form->refusal(state.path()).has_value();
        });
    }

    /// @brief The grid's columns.
    /// @return They, as given.
    [[nodiscard]] std::vector<GridColumn> const& gridColumns() const noexcept { return _columns; }

    /// @brief What "Filter by this value" on a cell filters by: the column's member and the cell's value as a filter
    ///        entry takes it, `isEmpty` for an empty cell. Tracked.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return The member and the entry; nothing for a display column, or a cell whose draft does not encode.
    [[nodiscard]] std::optional<std::pair<std::string, table::FilterEntry>> filterEntry(Key const& key,
                                                                                       std::string_view column) const {
        forms::FieldState const* const state = field(key, column);
        if (state == nullptr) {
            return std::nullopt;
        }
        table::TableColumn shown;
        shown.info = table::ColumnInfo{.id = state->spec().name, .kind = detail::columnKindOf(state->spec().kind)};
        table::Cell const cell = detail::memberCell(*state, shown.info.kind);
        if (cellLook(cell, shown) == CellLook::Empty) {
            return std::pair{shown.info.id, table::FilterEntry{.isEmpty = true}};
        }
        std::optional<std::string> const operand = operandOf(cell, shown);
        if (!operand) {
            return std::nullopt;
        }
        return std::pair{shown.info.id, table::FilterEntry{.oneOf = std::vector{*operand}}};
    }

private:
    [[nodiscard]] GridColumn const* columnOf(std::string_view id) const {
        auto const found = std::ranges::find(_columns, id, &GridColumn::id);
        return found == _columns.end() ? nullptr : &*found;
    }

    template <typename Test>
    [[nodiscard]] std::size_t countCells(Test test) const {
        std::size_t count = 0;
        for (auto const& row : _rowsSource->array().rows()) {
            for (GridColumn const& column : _columns) {
                forms::FieldState const* const state = column.field.empty() ? nullptr : row->field(column.field);
                count += state != nullptr && test(*state) ? 1 : 0;
            }
        }
        return count;
    }

    forms::FormSession* _form;
    std::weak_ptr<void const> _formAlive;
    std::string _arrayPath;
    std::vector<GridColumn> _columns;
    FormatContext _format;
    reactive::Signal<std::uint64_t> _formatVersion;
    std::vector<std::string> _layout;
    std::shared_ptr<FormRowsSource> _rowsSource;
    std::unique_ptr<GridRows> _rows;
    std::function<void()> _committed;
    std::optional<std::size_t> _hook;
};

/// @brief A grid's focus, its cell editor and its walk over a `GridSource`, and the table node that shows them.
///
/// The editor's text is the cell's draft, live: an opening keeps the draft the cell held, a commit closes the editor
/// and calls `GridSource::committed` once when the draft changed, and `Escape` writes the kept draft back.
class GridController : public std::enable_shared_from_this<GridController> {
public:
    /// @brief A controller over a source.
    /// @param runtime The runtime its state is tracked on. Borrowed.
    /// @param source  The source. Borrowed: it must outlive the controller.
    /// @param options The walk and the activation.
    GridController(reactive::Runtime& runtime, GridSource& source, GridOptions options)
        : _rt{&runtime},
          _source{&source},
          _options{std::move(options)},
          _place{runtime, std::optional<GridFocus>{}},
          _editing{runtime, std::optional<CellRef>{}},
          _message{runtime, std::string{}},
          _notices{runtime, std::uint64_t{0}},
          _messageEntries{runtime, std::vector<StatusEntry>{}},
          _standing{runtime, std::string{}},
          _standingEntries{runtime, std::vector<StatusEntry>{}} {}

    GridController(GridController const&) = delete;
    GridController& operator=(GridController const&) = delete;
    GridController(GridController&&) = delete;
    GridController& operator=(GridController&&) = delete;
    ~GridController() = default;

    /// @brief The focused cell. Tracked.
    /// @return The cell when its row is fetched; empty when nothing has focus or focus stands on a placeholder.
    [[nodiscard]] std::optional<CellRef> focus() const {
        std::optional<GridFocus> const shown = place();
        if (!shown || !shown->key) {
            return std::nullopt;
        }
        return CellRef{.key = *shown->key, .column = shown->column};
    }

    /// @brief The focus as the mount shows it, what `Table::gridFocus` binds. Tracked.
    /// @return `{key, view position, column}` for a focused cell, `{none, view row, column}` while the walk stopped at a
    ///         row not fetched yet, which takes its key once the source answers for it.
    [[nodiscard]] std::optional<GridFocus> place() const {
        std::optional<GridFocus> shown = _place.get();
        if (shown && !shown->key) {
            shown->key = _rt->untracked([&] { return _source->keyAt(shown->viewRow); });
        }
        return shown;
    }

    /// @brief Takes the cell a click or a key resolved to: `focusCell` for a fetched row, else the placeholder focus.
    /// @param target The cell.
    void focusPlace(GridFocus target) {
        if (target.key) {
            focusCell(CellRef{.key = *target.key, .column = target.column});
            return;
        }
        commit();
        _place.set(std::move(target));
        _message.set({});
    }

    /// @brief Focuses a cell, as a click or an arrow does; a covered cell, and a key the view does not hold, are
    ///        refused. An open editor on another cell is committed first. The status line shows the cell's error while
    ///        it has focus.
    /// @param cell The cell.
    void focusCell(CellRef cell) {
        if (_source->covered(cell.key, cell.column)) {
            return;
        }
        if (std::optional<CellRef> const open = _editing.peek(); open && *open != cell) {
            commit();
        }
        std::optional<std::size_t> const at = _source->positionOf(cell.key);
        if (!at) {
            return;
        }
        _place.set(GridFocus{.key = cell.key, .viewRow = *at, .column = cell.column});
        _message.set({});
    }

    /// @brief What `Enter` and `F2` with no editor, and a double click, do: focuses the cell, then opens its editor
    ///        holding the value on an editable cell (none for a Boolean), or runs `GridOptions::onActivate` on a
    ///        read-only one.
    /// @param cell The cell.
    void activate(CellRef cell) {
        focusCell(cell);
        if (_rt->untracked([&] { return _source->readOnly(cell.key, cell.column); })) {
            if (_options.onActivate) {
                _options.onActivate(cell.key);
            }
            return;
        }
        // A Boolean opens no editor: a click or `Space` flips it, so a double click is two flips.
        if (_source->toggles(cell.key, cell.column)) {
            return;
        }
        _kept = _source->draft(cell.key, cell.column);
        _editing.set(cell);
    }

    /// @brief The cell whose editor is open. Tracked.
    /// @return The cell, or none.
    [[nodiscard]] std::optional<CellRef> editing() const { return _editing.get(); }

    /// @brief The chords the table declares, what `Table::keyChords` binds. Tracked.
    /// @return With an editor open exactly `Enter`, `Tab`, `Shift+Tab`, `Escape` and `Ctrl+Enter`, so every other key
    ///         reaches the editor; with none, also `F2`, `Space` and `Delete`.
    [[nodiscard]] std::vector<std::string> keyChords() const {
        if (_editing.get()) {
            return {"Enter", "Tab", "Shift+Tab", "Escape", "Ctrl+Enter"};
        }
        return {"Tab", "Shift+Tab", "Enter", "F2", "Space", "Delete", "Escape", "Ctrl+Enter"};
    }

    /// @brief A chord pressed while the table has focus.
    /// @param chord The chord.
    /// @return `FocusNext` or `FocusPrevious` when `Tab` or `Shift+Tab` runs past the last or the first editable cell,
    ///         `Ignored` for a chord the grid does not take, else `Handled`.
    KeyOutcome press(std::string_view chord) {
        std::optional<CellRef> const open = _editing.peek();
        if (chord == "Tab" || chord == "Shift+Tab") {
            commit();
            return walkFrom(chord == "Tab");
        }
        if (chord == "Escape") {
            if (open) {
                _source->write(open->key, open->column, _kept);
                _editing.set(std::nullopt);
            }
            return KeyOutcome::Handled;
        }
        if (chord == "Ctrl+Enter") {
            commit();
            return KeyOutcome::Handled;
        }
        if (chord == "Enter" && open) {
            commit();
            enterMove(*open);
            return KeyOutcome::Handled;
        }
        std::optional<CellRef> const focused = _rt->untracked([&] { return focus(); });
        if (chord == "Enter" || chord == "F2") {
            if (focused) {
                activate(*focused);
            }
            return KeyOutcome::Handled;
        }
        if (chord == "Space") {
            if (focused && _source->toggles(focused->key, focused->column)) {
                toggle(*focused);
            }
            return KeyOutcome::Handled;
        }
        if (chord == "Delete") {
            if (focused && editable(*focused) && !_source->draft(focused->key, focused->column).empty()) {
                _source->write(focused->key, focused->column, std::string{});
                _source->committed();
            }
            return KeyOutcome::Handled;
        }
        return KeyOutcome::Ignored;
    }

    /// @brief Printable text: with an editor open, the editor's whole new text, as its input writes it; on a focused
    ///        cell with none, the cell's draft is kept, @p text written as the draft and the editor opened. A
    ///        read-only cell refuses it in the status line.
    /// @param text The text.
    void type(std::string_view text) {
        if (std::optional<CellRef> const open = _editing.peek()) {
            _source->write(open->key, open->column, std::string{text});
            return;
        }
        std::optional<CellRef> const focused = _rt->untracked([&] { return focus(); });
        if (!focused || _source->covered(focused->key, focused->column)) {
            return;
        }
        if (_rt->untracked([&] { return _source->readOnly(focused->key, focused->column); })) {
            _message.set(std::string{kReadOnlyText});
            _notices.set(_notices.peek() + 1);
            return;
        }
        if (_source->toggles(focused->key, focused->column)) {
            return;
        }
        _kept = _source->draft(focused->key, focused->column);
        _source->write(focused->key, focused->column, std::string{text});
        _editing.set(*focused);
    }

    /// @brief Focus left the table: an open editor is committed.
    void leave() { commit(); }

    /// @brief The latest transient refusal or notice, else the focused cell's error while it has one. Tracked.
    /// @return The text; empty for none.
    [[nodiscard]] std::string message() const {
        if (std::string const& notice = _message.get(); !notice.empty()) {
            return notice;
        }
        std::optional<CellRef> const focused = focus();
        return focused ? _source->error(focused->key, focused->column).value_or(std::string{}) : std::string{};
    }

    /// @brief How many refusals and notices were given: it moves on each, so one equal to the last is posted again.
    ///        Tracked.
    /// @return The count.
    [[nodiscard]] std::uint64_t notices() const { return _notices.get(); }

    /// @brief A click on a cell: focuses it, and flips a Boolean's.
    /// @param cell The cell.
    void click(CellRef cell) {
        focusCell(cell);
        if (_rt->untracked([&] { return focus(); }) == cell && _source->toggles(cell.key, cell.column)) {
            toggle(cell);
        }
    }

    /// @brief The choices the latest notice offers. Tracked.
    /// @return The entries.
    [[nodiscard]] std::vector<StatusEntry> const& messageEntries() const { return _messageEntries.get(); }

    /// @brief A state that lasts while its source holds. Tracked.
    /// @return The text; empty for none.
    [[nodiscard]] std::string standing() const { return _standing.get(); }

    /// @brief The choices the standing state offers. Tracked.
    /// @return The entries.
    [[nodiscard]] std::vector<StatusEntry> standingEntries() const { return _standingEntries.get(); }

    /// @brief An entry chosen in the status line.
    /// @param id The entry's id; one the grid does not offer is ignored.
    void chooseEntry(std::string_view id) { static_cast<void>(id); }

    /// @brief The grid's entries of a cell's menu: "Revert cell", enabled while the cell is modified, and "Revert row",
    ///        enabled while the row is modified, each restoring the loaded value and then committing once.
    /// @param row    The row's key.
    /// @param column The column's id.
    /// @return The entries.
    [[nodiscard]] std::vector<MenuItem> cellEntries(Key const& row, std::string_view column) {
        GridSource* const source = _source;
        std::string const id{column};
        MenuItem cell;
        cell.label = std::string{"Revert cell"};
        cell.enabled = Prop<bool>{[source, row, id] { return source->marks(row, id).modified; }};
        cell.onSelect = [source, row, id] {
            source->revertCell(row, id);
            source->committed();
        };
        MenuItem whole;
        whole.label = std::string{"Revert row"};
        whole.enabled = Prop<bool>{[source, row] { return source->rowModified(row); }};
        whole.onSelect = [source, row] {
            source->revertRow(row);
            source->committed();
        };
        return {std::move(cell), std::move(whole)};
    }

    /// @brief Keeps something alive as long as the controller: what a node it built owns through it.
    /// @param kept The thing.
    void keepAlive(std::shared_ptr<void const> kept) { _kept_alive = std::move(kept); }

    /// @brief The table node over @p columns, wired to this controller: its focus, keys, text, clicks, focus leaving,
    ///        editor and cell menu. A node built while the controller is owned by a `shared_ptr` keeps it alive.
    /// @param columns Every column, hidden ones included, each with its id.
    /// @param options The table's other options. Its status is the grid's own messages unless it binds one; its
    ///        `cellMenu`'s entries are followed by the grid's, before its first separator.
    /// @param base    A model's table's source, whose rows and cells the grid marks; none for the grid's own rows.
    /// @return The node.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): one wire per seam of the table.
    [[nodiscard]] Node node(std::vector<TableColumn> columns, TableOptions options,
                            std::optional<TableSource> base = std::nullopt) {
        std::shared_ptr<GridController> const keep = weak_from_this().lock();
        GridController* const self = this;
        GridSource* const source = _source;
        auto const ids = std::make_shared<std::vector<std::string>>();
        for (TableColumn const& column : columns) {
            ids->push_back(column.id);
        }
        auto const idOf = [ids](std::size_t column) {
            return column < ids->size() ? ids->at(column) : std::string{};
        };
        auto const marksOf = [source](reactive::Signal<Value> const* row, std::string id) {
            return Prop<CellMarks>{[source, row, id] {
                std::optional<Key> const key = source->keyOf(row->get());
                return key ? source->marks(*key, id) : CellMarks{};
            }};
        };
        TableSource made;
        if (base) {
            made = *base;
            made.cellView = [inner = base->cellView, idOf, marksOf](std::size_t column,
                                                                    reactive::Signal<Value> const& row) {
                return markedCell(inner(column, row), marksOf(&row, idOf(column)));
            };
        } else {
            made.rows = std::shared_ptr<TableRows>(keep, &source->view());
            made.cellView = [source, idOf, marksOf](std::size_t column, reactive::Signal<Value> const& row) {
                reactive::Signal<Value> const* const record = &row;
                std::string const id = idOf(column);
                Node content = ui::text(Text{.text = Prop<std::string>{[source, record, id] {
                    std::optional<Key> const key = source->keyOf(record->get());
                    return key ? source->text(*key, id) : std::string{};
                }}});
                Prop<CellAccent> accent{[source, record, id] {
                    std::optional<Key> const key = source->keyOf(record->get());
                    std::optional<std::string> const failed = key ? source->error(*key, id) : std::nullopt;
                    return CellAccent{.tone = std::nullopt, .tooltip = failed.value_or(std::string{}),
                                      .invalid = failed.has_value()};
                }};
                return markedCell(accentedCell(std::move(content), std::move(accent)), marksOf(record, id));
            };
            made.rowView = {};
            made.alwaysFetched = {};
            made.levels = {};
        }
        made.rowView = [inner = made.rowView, source](reactive::Signal<Value> const& row) {
            TableRowLook look = inner ? inner(row) : TableRowLook{};
            reactive::Signal<Value> const* const record = &row;
            look.modified = Prop<bool>{[source, record] {
                std::optional<Key> const key = source->keyOf(record->get());
                return key.has_value() && source->rowModified(*key);
            }};
            return look;
        };
        options.gridFocus = Prop<std::optional<GridFocus>>{[self] { return self->place(); }};
        options.onGridFocus = [self, keep](GridFocus target) { self->focusPlace(std::move(target)); };
        options.onKey = [self](std::string const& chord) { return self->press(chord); };
        options.keyChords = Prop<std::vector<std::string>>{[self] { return self->keyChords(); }};
        options.onText = [self](std::string text) { self->type(text); };
        options.onActivateCell = [self](CellRef cell) { self->activate(std::move(cell)); };
        options.onClickCell = [self](CellRef cell) { self->click(std::move(cell)); };
        options.onFocusOut = [self] { self->leave(); };
        options.editing = Prop<std::optional<CellRef>>{[self] { return self->editing(); }};
        options.editor = [source](CellRef const& cell) { return source->editor(cell.key, cell.column); };
        options.cellMenu = [self, idOf, outer = std::move(options.cellMenu)](Key const& row, std::size_t column) {
            std::vector<MenuItem> own = self->cellEntries(row, idOf(column));
            if (!outer) {
                return own;
            }
            std::vector<MenuItem> items = outer(row, column);
            auto const separator = std::ranges::find_if(items, [](MenuItem const& item) { return item.separator; });
            items.insert(separator, std::make_move_iterator(own.begin()), std::make_move_iterator(own.end()));
            return items;
        };
        if (!options.status.isBound() && options.status.constant() == TableStatus{}) {
            options.status = Prop<TableStatus>{[self] {
                TableStatus status{.message = self->message(),
                                   .entries = self->messageEntries(),
                                   .standing = self->standing(),
                                   .standingEntries = self->standingEntries()};
                status.shown = !status.message.empty() || !status.entries.empty() || !status.standing.empty() ||
                               !status.standingEntries.empty();
                return status;
            }};
            if (!options.onStatusEntry) {
                options.onStatusEntry = [self](std::string const& id) { self->chooseEntry(id); };
            }
        }
        return ui::table(std::move(columns), std::move(made), std::move(options));
    }

    /// @brief The status line's refusal of typing on a read-only cell.
    static constexpr std::string_view kReadOnlyText = "This cell is read-only";

private:
    // A cell a walk may stop on: editable and not covered.
    [[nodiscard]] bool editable(CellRef const& cell) const {
        return _rt->untracked([&] {
            return !_source->readOnly(cell.key, cell.column) && !_source->covered(cell.key, cell.column);
        });
    }

    // Commits an open editor: it closes, and an edit is meant when its draft differs from the one it opened on.
    void commit() {
        std::optional<CellRef> const open = _editing.peek();
        if (!open) {
            return;
        }
        bool const changed = _source->draft(open->key, open->column) != _kept;
        _editing.set(std::nullopt);
        if (changed) {
            _source->committed();
        }
    }

    // Flips a Boolean's cell and commits it.
    void toggle(CellRef const& cell) {
        if (!editable(cell)) {
            return;
        }
        std::string const now = _source->draft(cell.key, cell.column);
        _source->write(cell.key, cell.column,
                       forms::detail::engine::trim(now) == "true" ? std::string{"false"} : std::string{"true"});
        _source->committed();
    }

    // Where a walk lands: an editable cell, a row not fetched yet, or nowhere.
    struct Step {
        enum class Kind : std::uint8_t { Found, Stop, None } kind = Kind::None;
        std::size_t row = 0;
        std::string column;
        std::optional<Key> key;
    };

    // The next editable cell from @p from in the walk's order, by view positions at the moment of the call; a row not
    // fetched yet stops the walk.
    [[nodiscard]] Step next(std::optional<CellRef> const& from, bool forward) const {
        std::vector<std::string> const columns = _source->columns();
        std::size_t const rows = _source->rowCount();
        if (columns.empty() || rows == 0) {
            return {};
        }
        std::size_t const width = columns.size();
        std::size_t const total = rows * width;
        bool const byRows = _options.walk == Walk::Rows;
        auto const linear = [&](std::size_t row, std::size_t column) {
            return byRows ? row * width + column : column * rows + row;
        };
        std::int64_t at = forward ? -1 : static_cast<std::int64_t>(total);
        if (from) {
            std::optional<std::size_t> const row = _source->positionOf(from->key);
            auto const column = std::ranges::find(columns, from->column);
            if (row && column != columns.end()) {
                at = static_cast<std::int64_t>(linear(*row, static_cast<std::size_t>(column - columns.begin())));
            }
        }
        for (at += forward ? 1 : -1; at >= 0 && at < static_cast<std::int64_t>(total); at += forward ? 1 : -1) {
            auto const index = static_cast<std::size_t>(at);
            std::size_t const row = byRows ? index / width : index % rows;
            std::size_t const column = byRows ? index % width : index / rows;
            std::optional<Key> const key = _source->keyAt(row);
            if (!key) {
                return Step{.kind = Step::Kind::Stop, .row = row, .column = columns.at(column), .key = std::nullopt};
            }
            if (editable(CellRef{.key = *key, .column = columns.at(column)})) {
                return Step{.kind = Step::Kind::Found, .row = row, .column = columns.at(column), .key = key};
            }
        }
        return {};
    }

    // Lands on where a walk stopped.
    void land(Step const& step) {
        if (step.kind == Step::Kind::Found) {
            focusCell(CellRef{.key = *step.key, .column = step.column});
        } else if (step.kind == Step::Kind::Stop) {
            _place.set(GridFocus{.key = std::nullopt, .viewRow = step.row, .column = step.column});
            _message.set({});
        }
    }

    // Tab or Shift+Tab: the next editable cell, or out of the table at the ends. Focus on a row not fetched yet moves
    // nowhere until the row arrives.
    KeyOutcome walkFrom(bool forward) {
        std::optional<GridFocus> const shown = _rt->untracked([&] { return place(); });
        if (shown && !shown->key) {
            return KeyOutcome::Handled;
        }
        std::optional<CellRef> const from =
            shown ? std::optional<CellRef>{CellRef{.key = *shown->key, .column = shown->column}} : std::nullopt;
        Step const step = next(from, forward);
        if (step.kind == Step::Kind::None) {
            return forward ? KeyOutcome::FocusNext : KeyOutcome::FocusPrevious;
        }
        land(step);
        return KeyOutcome::Handled;
    }

    // Enter after a commit: down the column, along the row, or nowhere.
    void enterMove(CellRef const& from) {
        if (_options.enterMoves == EnterMoves::None) {
            return;
        }
        std::optional<std::size_t> const row = _source->positionOf(from.key);
        if (!row) {
            return;
        }
        if (_options.enterMoves == EnterMoves::Down) {
            std::size_t const rows = _source->rowCount();
            for (std::size_t below = *row + 1; below < rows; ++below) {
                std::optional<Key> const key = _source->keyAt(below);
                if (!key) {
                    land(Step{.kind = Step::Kind::Stop, .row = below, .column = from.column, .key = std::nullopt});
                    return;
                }
                if (editable(CellRef{.key = *key, .column = from.column})) {
                    focusCell(CellRef{.key = *key, .column = from.column});
                    return;
                }
            }
            return;
        }
        std::vector<std::string> const columns = _source->columns();
        auto const at = std::ranges::find(columns, from.column);
        if (at == columns.end()) {
            return;
        }
        for (auto column = std::next(at); column != columns.end(); ++column) {
            if (editable(CellRef{.key = from.key, .column = *column})) {
                focusCell(CellRef{.key = from.key, .column = *column});
                return;
            }
        }
    }

    reactive::Runtime* _rt;
    GridSource* _source;
    GridOptions _options;
    reactive::Signal<std::optional<GridFocus>> _place;
    reactive::Signal<std::optional<CellRef>> _editing;
    reactive::Signal<std::string> _message;
    reactive::Signal<std::uint64_t> _notices;
    reactive::Signal<std::vector<StatusEntry>> _messageEntries;
    reactive::Signal<std::string> _standing;
    reactive::Signal<std::vector<StatusEntry>> _standingEntries;
    // The draft the open editor's cell held when it opened.
    std::string _kept;
    std::shared_ptr<void const> _kept_alive;
};

/// @brief A grid of a form, owned whole: the source, the controller, and the node that shows them.
struct FormGrid {
    /// @brief The grid's source.
    std::shared_ptr<FormGridSource> source;
    /// @brief The grid's controller.
    std::shared_ptr<GridController> grid;
    /// @brief The table node; it keeps the controller and the source alive.
    Node node;
};

/// @brief Builds a grid of a form's collection: the source, the controller over the form's runtime, whose commit of an
///        open editor the form's submit runs first, and the node over one table column per grid column.
/// @param form    The form. Borrowed: it must outlive the grid.
/// @param array   The collection field.
/// @param columns The grid's columns.
/// @param options The walk and the activation.
/// @param format  How values are shown.
/// @param table   The table's other options.
/// @return The grid.
/// @throws std::invalid_argument when @p array is not a collection of @p form, or a column names no member.
[[nodiscard]] inline FormGrid makeFormGrid(forms::FormSession& form, forms::FieldState& array,
                                           std::vector<GridColumn> columns, GridOptions options, FormatContext format,
                                           TableOptions table = {}) {
    std::vector<TableColumn> shown;
    shown.reserve(columns.size());
    for (GridColumn const& column : columns) {
        shown.push_back(TableColumn{.id = column.id, .label = column.label});
    }
    auto source = std::make_shared<FormGridSource>(form, array.path(), std::move(columns), std::move(format));
    auto grid = std::make_shared<GridController>(form.runtime(), *source, std::move(options));
    grid->keepAlive(source);
    source->onBeforeSubmit([weak = std::weak_ptr<GridController>{grid}] {
        if (auto const live = weak.lock()) {
            live->leave();
        }
    });
    Node node = grid->node(std::move(shown), std::move(table));
    return FormGrid{.source = std::move(source), .grid = std::move(grid), .node = std::move(node)};
}

/// @brief The node of a grid of a form's collection (`makeFormGrid`), for a form's own view.
/// @param form    The form.
/// @param array   The collection field.
/// @param columns The grid's columns.
/// @param options The walk and the activation.
/// @param format  How values are shown.
/// @return The node; it keeps the grid alive.
[[nodiscard]] inline Node formGrid(forms::FormSession& form, forms::FieldState& array, std::vector<GridColumn> columns,
                                   GridOptions options, FormatContext format) {
    return makeFormGrid(form, array, std::move(columns), std::move(options), std::move(format)).node;
}

}  // namespace morph::ui
