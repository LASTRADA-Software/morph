// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file ui/grid.hpp
/// @brief A grid: a table whose cells are edited. `GridController` holds a grid's focus, its cell editor and its walk
///        over a `GridSource`; `FormGridSource` is a grid of a form's `ObjectArray` field, and `makeFormGrid` the node
///        that shows one; `FixedGridSource` is a fixed grid, whose rows a standard fixes and whose cells are each a
///        field of their own; `ModelGridSource` is the cells of a model's table, edited through its edit actions.
///
/// Specified in `docs/spec/table/editing.md`.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
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
#include "../reactive/scheduler.hpp"
#include "../reactive/signal.hpp"
#include "../table/edits.hpp"
#include "cell_format.hpp"
#include "expression.hpp"
#include "form_rows.hpp"
#include "mount.hpp"
#include "table_config.hpp"
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
    /// @brief The member of the row type the column edits; empty for a display column, or one with `fieldOf`.
    std::string field;
    /// @brief A display column's text for a row; read only while `field` and `fieldOf` are empty.
    std::function<std::string(forms::RowState const&)> value;
    /// @brief The member the column edits in a row, when it varies by row: the record decides a value's kind. Tracked.
    ///        A name that is no member leaves the row's cell with no field, so it is read-only.
    std::function<std::string(forms::RowState const&)> fieldOf;
    /// @brief The entry places of the row's member, which are also its wire `dp`; nothing keeps the field's. Tracked.
    std::function<std::optional<std::uint32_t>(forms::RowState const&)> decimals;
    /// @brief The unit the row's member is entered in, an id of its `FieldSpec::units`: applied, converting the draft as the
    ///        editor's unit select does, when it first gives a unit for the row or gives another one, so a unit the user
    ///        picks afterwards stays; nothing, or an id the field does not have, after a unit restores the field's entry
    ///        unit. The first unit a loaded row's member takes, while the overlay had given none before, counts as loaded
    ///        (`forms::FormSession::switchUnitsAsLoaded`): the cell is not modified by it, and a revert returns to it.
    ///        Tracked.
    std::function<std::optional<std::string>(forms::RowState const&)> unit;
    /// @brief Whether the row's cell is read-only beyond its field (`CellFacts::overlayReadOnly`). Tracked.
    std::function<bool(forms::RowState const&)> readonly;
    /// @brief The options the row's member offers, as their values' JSON literals, narrowing a choice's; nothing
    ///        offers every option. Tracked.
    std::function<std::optional<std::vector<std::string>>(forms::RowState const&)> options;
    /// @brief The row's member's placeholder; empty keeps the field's. Tracked.
    std::function<std::string(forms::RowState const&)> placeholder;
};

/// @brief How a grid walks and what activating a read-only cell does.
struct GridOptions {
    /// @brief Where `Enter` moves after a commit.
    EnterMoves enterMoves = EnterMoves::Down;
    /// @brief The order `Tab` follows.
    Walk walk = Walk::Rows;
    /// @brief The table's `onActivate`: what `Enter` or a double click on a read-only cell runs, with its row's key.
    std::function<void(Key)> onActivate;
    /// @brief Whether the walk past the end and `Enter` below the last row add a row, and an "Add row" button under
    ///        the grid does, up to the field's `maxItems`.
    bool appendRows = false;
    /// @brief Whether the rows are the record's: no row is inserted, deleted or appended, and no entry offers it.
    bool rowsFixed = false;
    /// @brief The most values one paste writes or drops: a larger block is refused whole, and nothing is written.
    std::size_t maxPasteCells = 10'000;
    /// @brief The most rows one copy puts on the clipboard (`GridController::copyRefusal`).
    std::size_t maxCopyRows = 10'000;
    /// @brief The timers the offer to undo a deletion ends on; without one it ends only at the grid's next change.
    reactive::Scheduler* scheduler = nullptr;
    /// @brief How long the offer to undo a deletion lasts, unless a change of the grid ends it first: a status line
    ///        notice's hold.
    std::chrono::milliseconds undoHold{6000};
};

/// @brief What a paste did with each value of its block.
struct PasteResult {
    /// @brief The values written into editable cells.
    std::size_t written = 0;
    /// @brief The values dropped on read-only or covered cells.
    std::size_t readOnly = 0;
    /// @brief The values dropped on rows past the last one that could not be added.
    std::size_t pastRows = 0;
    /// @brief The values dropped past the last visible column.
    std::size_t pastColumns = 0;
};

/// @brief The status line's notice after a paste.
/// @param result What the paste did.
/// @return `Pasted {written} values`, then, when anything was dropped, `; `, the parts present joined by ` and ` —
///         `{readOnly} on read-only cells`, `{pastRows} past the last row`, `{pastColumns} past the last column` —
///         and ` were dropped`.
[[nodiscard]] inline std::string pasteMessage(PasteResult const& result) {
    std::string text = "Pasted " + std::to_string(result.written) + " values";
    std::vector<std::string> parts;
    if (result.readOnly > 0) {
        parts.push_back(std::to_string(result.readOnly) + " on read-only cells");
    }
    if (result.pastRows > 0) {
        parts.push_back(std::to_string(result.pastRows) + " past the last row");
    }
    if (result.pastColumns > 0) {
        parts.push_back(std::to_string(result.pastColumns) + " past the last column");
    }
    if (parts.empty()) {
        return text;
    }
    text += "; ";
    for (std::size_t at = 0; at < parts.size(); ++at) {
        text += (at == 0 ? "" : " and ") + parts.at(at);
    }
    return text + " were dropped";
}

/// @brief Which row operations a grid offers now.
struct RowOps {
    /// @brief Whether a row can be inserted or appended.
    bool insert = false;
    /// @brief Whether a row can be deleted.
    bool remove = false;
    /// @brief Why one is not: the bound it would pass; empty when both are offered, or the rows are fixed.
    std::string why;
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

    /// @brief The spans a row gives, for the mount to draw, while they apply (`covered` says when).
    /// @param key The row's key.
    /// @return Each span as `cutColumns` cuts it over the layout; none by default.
    [[nodiscard]] virtual std::vector<CellSpan> spans(Key const& key) const {
        static_cast<void>(key);
        return {};
    }

    /// @brief Whether any row may give spans: the mount then resolves them over every realized row.
    /// @return `false` by default.
    [[nodiscard]] virtual bool spanning() const { return false; }

    /// @brief A data row's cell content as a grid draws it, before its marks: its text, and its error as an accent.
    /// @param column The column's id.
    /// @param row    The signal holding the row's record; it must outlive the node.
    /// @return The cell's node.
    [[nodiscard]] Node cellContent(std::string const& column, reactive::Signal<Value> const& row) {
        reactive::Signal<Value> const* const record = &row;
        Node content = ui::text(Text{.text = Prop<std::string>{[this, record, column] {
            std::optional<Key> const key = keyOf(record->get());
            return key ? text(*key, column) : std::string{};
        }}});
        Prop<CellAccent> accent{[this, record, column] {
            std::optional<Key> const key = keyOf(record->get());
            std::optional<std::string> const failed = key ? error(*key, column) : std::nullopt;
            return CellAccent{.tone = std::nullopt, .tooltip = failed.value_or(std::string{}),
                              .invalid = failed.has_value()};
        }};
        return accentedCell(std::move(content), std::move(accent));
    }

    /// @brief A data row's cell as a grid draws it: `cellContent` with the cell's marks.
    /// @param column The column's id.
    /// @param row    The signal holding the row's record; it must outlive the node.
    /// @return The cell's node.
    [[nodiscard]] Node cellView(std::string const& column, reactive::Signal<Value> const& row) {
        reactive::Signal<Value> const* const record = &row;
        Prop<CellMarks> marked{[this, record, column] {
            std::optional<Key> const key = keyOf(record->get());
            return key ? marks(*key, column) : CellMarks{};
        }};
        return markedCell(cellContent(column, row), std::move(marked));
    }

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

    /// @brief Holds the row an editor is open on where it is in the view, whatever its values do to the sort and the
    ///        filter; nothing lets it go. A source whose rows never move under an edit does nothing.
    /// @param key The row's key, or nothing.
    virtual void holdRow(std::optional<Key> key) { static_cast<void>(key); }

    /// @brief A cell's editor opened: a choice in it fetches its options from now on. A source with nothing to fetch
    ///        does nothing.
    /// @param key    The row's key.
    /// @param column The column's id.
    virtual void editorOpened(Key const& key, std::string_view column) {
        static_cast<void>(key);
        static_cast<void>(column);
    }

    /// @brief Whether the derived values are the last ones loaded before a failed round, until a round succeeds: what the
    ///        status line offers to recalculate. Tracked.
    /// @return `true` while it holds; `false` for a source with no derived values.
    [[nodiscard]] virtual bool deriveFailed() const { return false; }

    /// @brief Makes one more round of derived values. A source with none does nothing.
    virtual void recalculate() {}

    /// @brief Whether the grid's rows are its own to insert, delete and append: a source that is not offers no row
    ///        operation, as `GridOptions::rowsFixed` does.
    /// @return `false` unless a source says otherwise.
    [[nodiscard]] virtual bool rowsEditable() const { return false; }

    /// @brief How many rows the record holds, whatever the filter shows: what `bounds` is compared with. Tracked.
    /// @return The count; `rowCount` for a source whose view is its record.
    [[nodiscard]] virtual std::size_t heldRows() const { return rowCount(); }

    /// @brief The fewest and the most rows the record holds.
    /// @return `{minItems, maxItems}`; `{0, nothing}` for a source with no bound.
    [[nodiscard]] virtual std::pair<std::size_t, std::optional<std::size_t>> bounds() const { return {0, std::nullopt}; }

    /// @brief Appends a blank row to the record. A source whose rows are not its own adds none.
    /// @return The new row's key; nothing when no row was added.
    virtual std::optional<Key> appendRow() { return std::nullopt; }

    /// @brief Inserts a blank row before the row at a view position. A source whose rows are not its own adds none.
    /// @param position The view position; `rowCount()` appends.
    /// @return The new row's key; nothing when no row was added.
    virtual std::optional<Key> insertRow(std::size_t position) {
        static_cast<void>(position);
        return std::nullopt;
    }

    /// @brief Removes rows from the record in one change, kept so `undoRemove` can put them back.
    /// @param keys The rows' keys.
    /// @return `true` when one of them held a value.
    virtual bool removeRows(std::span<Key const> keys) {
        static_cast<void>(keys);
        return false;
    }

    /// @brief Puts the rows the last `removeRows` took back, each where it stood, in one change, while the record has
    ///        not changed since; does nothing after a change.
    virtual void undoRemove() {}

    /// @brief Why typing on a read-only cell is refused, where the source can say more than that it is read-only.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return The status line's text; nothing for the plain refusal.
    [[nodiscard]] virtual std::optional<std::string> refusal(Key const& key, std::string_view column) const {
        static_cast<void>(key);
        static_cast<void>(column);
        return std::nullopt;
    }

    /// @brief Focus moved: to a row, or onto no fetched row or out of the table. A source that holds a row's edits
    ///        back until focus leaves the row commits them here.
    /// @param key The row focus is on now; nothing when it is on none.
    virtual void focusRow(std::optional<Key> const& key) { static_cast<void>(key); }

    /// @brief `Ctrl+Enter`: commits what the source holds back. A source that holds nothing does nothing.
    virtual void commitHeld() {}

    /// @brief Whether a paste, a fill and a clear write into the cells: a source whose rows are selected as rows, not
    ///        as cells, takes none.
    /// @return `true` unless a source says otherwise.
    [[nodiscard]] virtual bool takesBlocks() const { return true; }

    /// @brief Gives the source what moves focus to a cell while the table has focus: how a refusal that arrives after
    ///        focus left its row brings focus back. A source that never moves focus ignores it.
    /// @param refocus Focuses a cell while the table has focus; empty when the controller goes.
    virtual void refocusWith(std::function<void(CellRef const&)> refocus) { static_cast<void>(refocus); }
};
// NOLINTEND(cppcoreguidelines-special-member-functions)

/// @brief The rows of a grid of a form: the engine's view over a `FormRowsSource`, less the rows added since the sort
///        or the filter last changed, which follow it in the order they were added and pass the filter until it
///        changes, and less the row `hold` holds, which keeps the place it had and passes the filter until it is let
///        go. With no sort and no filter the view is the array's order, an inserted row in its place.
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
        // The engine's change is the view's while both views were the engine's own; a view that kept its keys in
        // order changed only in content; anything else is a reset.
        _engine->onChange([this](table::ViewChange const& change) {
            bool const alignedBefore = _aligned;
            std::vector<table::RowId> const before = keys();
            rebuild();
            if (!_onChange) {
                return;
            }
            if (alignedBefore && _aligned) {
                _onChange(change);
                return;
            }
            table::ViewChange out;
            if (keys() == before) {
                out.ops.push_back(table::ViewOp{
                    .kind = table::ViewOp::Kind::Changed, .first = 0, .count = _items.size(), .to = 0});
            } else {
                out.ops.push_back(table::ViewOp{.kind = table::ViewOp::Kind::Reset, .first = 0, .count = 0, .to = 0});
            }
            _onChange(out);
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

    /// @brief Holds a row where it is in the view, and in it, whatever its values do to the sort and the filter, until
    ///        another row is held or none: the row an editor is open on stays under the editor. Letting it go puts it
    ///        where the sort and the filter place it.
    /// @param key The row to hold; nothing lets the held row go.
    void hold(std::optional<table::RowId> key) {
        if (key == _held) {
            return;
        }
        sync();
        std::vector<table::RowId> const before = keys();
        _held = std::move(key);
        _heldAt = _held ? positionOf(*_held).value_or(_items.size()) : 0;
        rebuild();
        if (_onChange && keys() != before) {
            table::ViewChange reset;
            reset.ops.push_back(table::ViewOp{.kind = table::ViewOp::Kind::Reset, .first = 0, .count = 0, .to = 0});
            _onChange(reset);
        }
    }

    /// @brief Whether a sort orders the view.
    /// @return `true` while the sort is not empty.
    [[nodiscard]] bool sorted() const noexcept { return _sorted; }

    /// @brief Groups the rows (`FormRowsSource::setGroup`) and takes the change in at once, so a sort that names
    ///        `kGroupRunColumn` is accepted from the next call.
    /// @param by A row's group value; empty: no groups.
    void setGroup(std::function<Value(forms::RowState const&)> by) {
        _source->setGroup(std::move(by));
        _engine->engine().flush();
        rebuild();
    }

    /// @brief The row `hold` holds.
    /// @return Its key; nothing while none is held.
    [[nodiscard]] std::optional<table::RowId> const& held() const noexcept { return _held; }

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

    // The view's keys, in order.
    [[nodiscard]] std::vector<table::RowId> keys() const {
        std::vector<table::RowId> out;
        out.reserve(_items.size());
        for (Item const& item : _items) {
            out.push_back(item.key);
        }
        return out;
    }

    // The view: the engine's rows but the added ones and the held one, then the added ones still in the source, and
    // the held one at its place.
    void rebuild() {
        _items.clear();
        _positions.clear();
        std::unordered_set<table::RowId> appended(_appended.begin(), _appended.end());
        if (_held && _source->row(*_held) == nullptr) {
            _held.reset();
        }
        if (_held) {
            appended.insert(*_held);
        }
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
        auto const itemOf = [&inEngine](table::RowId const& key) {
            auto const found = inEngine.find(key);
            return Item{.engine = found == inEngine.end() ? std::nullopt : std::optional{found->second}, .key = key};
        };
        for (table::RowId const& key : _appended) {
            if (key != _held) {
                _items.push_back(itemOf(key));
            }
        }
        if (_held) {
            auto const place = static_cast<std::ptrdiff_t>(std::min(_heldAt, _items.size()));
            _items.insert(_items.begin() + place, itemOf(*_held));
        }
        _aligned = _items.size() == static_cast<std::size_t>(std::max<std::int64_t>(engineTotal.rows, 0));
        for (std::size_t at = 0; at < _items.size(); ++at) {
            _positions.emplace(_items.at(at).key, at);
            _aligned = _aligned && _items.at(at).engine == at;
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
    std::optional<table::RowId> _held;
    std::size_t _heldAt = 0;
    bool _aligned = true;
};

/// @brief The record member naming a summary row's spec: its key. A data record never holds it.
inline constexpr std::string_view kSummaryField = "#summary";
/// @brief The record member holding the first data row a summary row's scope covers.
inline constexpr std::string_view kSummaryFirstField = "#first";
/// @brief The record member holding how many data rows a summary row's scope covers.
inline constexpr std::string_view kSummaryCountField = "#count";
/// @brief The record member holding a group summary's run ordinal; absent for a table's own summary.
inline constexpr std::string_view kSummaryRunField = "#run";
/// @brief The record member a summary row's record changes in whenever the data rows change, so its cells are
///        evaluated again.
inline constexpr std::string_view kSummaryVersionField = "#version";

/// @brief The records of view rows of the data rows, a summary row's `rows` scope.
/// @param rows  The data rows (a grid's `GridSource::view()`, or a `rows` table's rows), never a `SummaryRows`.
/// @param first The first view row.
/// @param count How many view rows.
/// @return The records, in view order; a row not fetched is left out. A grid row's record holds each member's encoded
///         value, `null` when blank or not encoding.
[[nodiscard]] inline ValueList summaryScopeRows(TableRows const& rows, std::int64_t first, std::int64_t count) {
    ValueList out;
    std::int64_t const last = std::min(first + std::max<std::int64_t>(count, 0), rows.total().rows);
    for (std::int64_t at = std::max<std::int64_t>(first, 0); at < last; ++at) {
        if (std::optional<TableRowRef> const ref = rows.row(at)) {
            out.push_back(ref->row);
        }
    }
    return out;
}

/// @brief A summary row a table or a group declares: its key, and whether it is drawn before the rows it summarises.
struct SummarySpec {
    /// @brief The summary's key, unique among the table's (or the group's) summaries.
    std::string key;
    /// @brief Whether it is drawn before the rows (`position: "top"`); after them otherwise.
    bool top = false;
};

/// @brief The rows a mount is handed for a table with summary rows: the data rows, with the table's summaries before
///        or after them and each run's after (or before) the run, and, once `setHeadings` is given, each run's
///        heading before it.
///
/// A forwarding decorator: it holds the data rows' one change slot and tells the mount each change in its own indices.
/// Summary and heading rows are outside the count, the selection, the sort and the filter: `total()` and `counts()`
/// are the data rows', `keysOf` skips them, and `mountedTotal()` is what the mount positions rows by. A summary row's
/// key is `"\x1Fsummary:" + key` (a group's `"\x1Fsummary:" + run ordinal + "/" + key`) and its record
/// `{"#summary": key, "#first": first data row, "#count": data rows in scope, "#version": n}`, a group's with
/// `"#run"` too; `#version` changes whenever the data rows change, so the summary's cells are evaluated again. A data
/// change the layout keeps is told as its own operations and a change of every summary and heading row.
class SummaryRows final : public TableRows {
public:
    /// @brief The runs of the data rows, each `{first, count}` in the data rows' view.
    using Runs = std::function<std::vector<std::pair<std::int64_t, std::int64_t>>()>;

    /// @brief Summary rows over data rows.
    /// @param data   The data rows. Must not be null.
    /// @param table  The table's own summaries, in order.
    /// @param perRun Each run's summaries, in order.
    /// @param runs   The runs of the data rows' view; empty or answering none for an ungrouped table.
    /// @throws std::invalid_argument when @p data is null.
    SummaryRows(std::shared_ptr<TableRows> data, std::vector<SummarySpec> table, std::vector<SummarySpec> perRun,
                Runs runs)
        : _data{checked(std::move(data))}, _table{std::move(table)}, _perRun{std::move(perRun)}, _runs{std::move(runs)} {
        rebuild();
        _data->onChange([this](table::ViewChange const& change) { heard(change); });
    }

    SummaryRows(SummaryRows const&) = delete;
    SummaryRows& operator=(SummaryRows const&) = delete;
    SummaryRows(SummaryRows&&) = delete;
    SummaryRows& operator=(SummaryRows&&) = delete;

    /// @brief Gives the data rows' change slot up.
    ~SummaryRows() override { _data->onChange({}); }

    /// @brief Draws a heading row before each run, from now on.
    /// @param heading A run's heading row by its index among the runs: its key and record; empty draws none.
    void setHeadings(std::function<RowRecord(std::size_t run)> heading) {
        _heading = std::move(heading);
        relayout();
    }

    /// @brief Takes the runs and the data rows in again, as a change of the data rows would, and tells the mount.
    void relayout() {
        ++_version;
        rebuild();
        if (_onChange) {
            table::ViewChange reset;
            reset.ops.push_back(table::ViewOp{.kind = table::ViewOp::Kind::Reset, .first = 0, .count = 0, .to = 0});
            _onChange(reset);
        }
    }

    /// @brief The data rows.
    /// @return They.
    [[nodiscard]] TableRows& data() const noexcept { return *_data; }

    /// @brief The data rows' total: a summary row is outside the count.
    /// @return It.
    [[nodiscard]] TableTotal total() const override { return _data->total(); }

    /// @brief Every row the mount realizes: data, summary and heading rows.
    /// @return The total.
    [[nodiscard]] TableTotal mountedTotal() const override {
        TableTotal out = _data->total();
        out.rows = static_cast<std::int64_t>(_entries.size());
        return out;
    }

    /// @brief The row at a mounted index.
    /// @param index The index.
    /// @return A data row as the data rows give it, a summary or heading row as built; nothing past the rows.
    [[nodiscard]] std::optional<TableRowRef> row(std::int64_t index) const override {
        if (index < 0 || static_cast<std::size_t>(index) >= _entries.size()) {
            return std::nullopt;
        }
        Entry const& entry = _entries.at(static_cast<std::size_t>(index));
        if (entry.kind == Entry::Kind::Data) {
            return _data->row(entry.data);
        }
        return TableRowRef{.key = entry.key, .row = entry.record};
    }

    /// @brief The mounted index of a data row.
    /// @param dataRow The data row's view index.
    /// @return Its mounted index; nothing past the data rows.
    [[nodiscard]] std::optional<std::int64_t> mounted(std::int64_t dataRow) const {
        if (dataRow < 0 || static_cast<std::size_t>(dataRow) >= _mounted.size()) {
            return std::nullopt;
        }
        return _mounted.at(static_cast<std::size_t>(dataRow));
    }

    /// @brief The data row at a mounted index, or the nearest one to a summary or heading row (the one above on a
    ///        tie).
    /// @param mountedRow The mounted index.
    /// @return The data row's view index; nothing with no data row or past the rows.
    [[nodiscard]] std::optional<std::int64_t> data(std::int64_t mountedRow) const {
        if (mountedRow < 0 || static_cast<std::size_t>(mountedRow) >= _entries.size()) {
            return std::nullopt;
        }
        auto const at = static_cast<std::size_t>(mountedRow);
        for (std::size_t distance = 0; distance < _entries.size(); ++distance) {
            if (distance <= at && _entries.at(at - distance).kind == Entry::Kind::Data) {
                return _entries.at(at - distance).data;
            }
            if (at + distance < _entries.size() && _entries.at(at + distance).kind == Entry::Kind::Data) {
                return _entries.at(at + distance).data;
            }
        }
        return std::nullopt;
    }

    /// @brief Whether a mounted row is a summary row.
    /// @param mountedRow The mounted index.
    /// @return `true` for a summary row.
    [[nodiscard]] bool summary(std::int64_t mountedRow) const {
        return mountedRow >= 0 && static_cast<std::size_t>(mountedRow) < _entries.size() &&
               _entries.at(static_cast<std::size_t>(mountedRow)).kind == Entry::Kind::Summary;
    }

    /// @brief The data rows' levels of a data row; nothing for a summary or heading row.
    /// @param index The mounted index.
    /// @return The levels.
    [[nodiscard]] table::UnloadedLevels unloadedLevels(std::int64_t index) const override {
        std::optional<std::int64_t> const at = exact(index);
        return at ? _data->unloadedLevels(*at) : table::UnloadedLevels{};
    }

    /// @brief Tells the data rows the data rows of a mounted range.
    /// @param first   The first mounted row.
    /// @param count   The rows.
    /// @param columns The columns.
    void visible(std::int64_t first, std::int64_t count, std::span<std::string const> columns) override {
        auto const [from, rows] = dataRange(first, count);
        _data->visible(from, rows, columns);
    }

    /// @brief The keys of the data rows of a mounted range: summary and heading rows are outside the selection.
    /// @param first The first mounted row.
    /// @param count The rows.
    /// @return The data rows' answer.
    async::Completion<std::expected<std::vector<table::RowId>, table::TableError>> keysOf(std::int64_t first,
                                                                                       std::int64_t count) override {
        auto const [from, rows] = dataRange(first, count);
        return _data->keysOf(from, rows);
    }

    /// @brief Finds a key's mounted index.
    /// @param key  The key.
    /// @param stop Once a stop is requested, @p done is not called.
    /// @param done Called with the mounted index, or nothing.
    void locateRow(table::RowId key, std::stop_token stop, std::function<void(RowLocation const&)> done) override {
        _data->locateRow(std::move(key), std::move(stop), [this, done = std::move(done)](RowLocation const& found) {
            if (found && *found) {
                done(RowLocation{mounted(**found)});
                return;
            }
            done(found);
        });
    }

    /// @brief Rows by key, from the data rows.
    /// @param keys    The keys.
    /// @param columns The columns.
    /// @return The data rows' answer.
    async::Completion<std::expected<std::vector<FetchedRow>, table::TableError>> fetchRows(
        std::vector<table::RowId> keys, std::vector<std::string> columns) override {
        return _data->fetchRows(std::move(keys), std::move(columns));
    }

    /// @brief Sorts the data rows.
    /// @param sort The keys.
    /// @return The data rows' answer.
    std::expected<void, table::TableError> setSort(SortChain sort) override { return _data->setSort(std::move(sort)); }

    /// @brief Filters the data rows.
    /// @param filter The filter.
    /// @return The data rows' answer.
    std::expected<void, table::TableError> setFilter(table::FilterSpec filter) override {
        return _data->setFilter(std::move(filter));
    }

    /// @brief A column's distinct values, from the data rows.
    /// @param request The request.
    /// @return The data rows' answer.
    async::Completion<std::expected<table::DistinctValues, table::TableError>> distinctValues(
        table::DistinctRequest request) override {
        return _data->distinctValues(std::move(request));
    }

    /// @brief The data rows' counts.
    /// @return They.
    [[nodiscard]] TableCountsView counts() const override { return _data->counts(); }

    /// @brief The data rows' view as a selection takes it.
    /// @return Their answer.
    [[nodiscard]] std::expected<ViewSelection, SelectAllRefusal> selectView() const override {
        return _data->selectView();
    }

    /// @brief The data rows' count of a selection.
    /// @param selection The selection.
    /// @return Their answer.
    [[nodiscard]] SelectedCount selectedCount(TableSelection const& selection) const override {
        return _data->selectedCount(selection);
    }

    /// @brief Whether the data rows' view left a key.
    /// @param key The key.
    /// @return Their answer.
    [[nodiscard]] bool leftView(table::RowId const& key) const override { return _data->leftView(key); }

    /// @brief Retains data rows.
    /// @param keys The keys.
    /// @return Their answer.
    [[nodiscard]] RetainResult retain(std::span<table::RowId const> keys) override { return _data->retain(keys); }

    /// @brief Drops the data rows' retained rows.
    void clearRetained() override { _data->clearRetained(); }

    /// @brief Whether a data row is retained and unmatched.
    /// @param key The key.
    /// @return Their answer.
    [[nodiscard]] bool unmatched(table::RowId const& key) const override { return _data->unmatched(key); }

    /// @brief Holds the data rows' refetches.
    /// @param run The run.
    void holdRefetches(std::uint64_t run) override { _data->holdRefetches(run); }

    /// @brief Releases the data rows' refetches.
    /// @param run The run.
    void releaseRefetches(std::uint64_t run) override { _data->releaseRefetches(run); }

    /// @brief The data rows of a mounted range, with their cells.
    /// @param first   The first mounted row.
    /// @param count   The rows.
    /// @param columns The columns.
    /// @return The data rows' answer, in their view's indices.
    async::Completion<std::expected<std::vector<FetchedRow>, table::TableError>> rowsOf(
        std::int64_t first, std::int64_t count, std::span<std::string const> columns) override {
        auto const [from, rows] = dataRange(first, count);
        return _data->rowsOf(from, rows, columns);
    }

    /// @brief The data rows' states.
    /// @return They.
    [[nodiscard]] RowStates& states() override { return _data->states(); }

    /// @brief The data rows' states.
    /// @return They.
    [[nodiscard]] RowStates const& states() const override { return _data->states(); }

    /// @brief Moves a data row's key.
    /// @param from The old key.
    /// @param to   The new key.
    void moveKey(table::RowId const& from, table::RowId const& to) override { _data->moveKey(from, to); }

    /// @brief Sets the one receiver of the changes, in mounted indices.
    /// @param handler The receiver; empty to stop receiving.
    void onChange(std::function<void(table::ViewChange const&)> handler) override { _onChange = std::move(handler); }

private:
    struct Entry {
        enum class Kind : std::uint8_t { Data, Summary, Heading } kind = Kind::Data;
        std::int64_t data = 0;
        bool perRun = false;
        Key key;
        Value record;
    };

    [[nodiscard]] static std::shared_ptr<TableRows> checked(std::shared_ptr<TableRows> data) {
        if (data == nullptr) {
            throw std::invalid_argument{"morph::ui::SummaryRows: the data rows must not be null"};
        }
        return data;
    }

    // The data row exactly at a mounted index; nothing for a summary or heading row.
    [[nodiscard]] std::optional<std::int64_t> exact(std::int64_t index) const {
        if (index < 0 || static_cast<std::size_t>(index) >= _entries.size()) {
            return std::nullopt;
        }
        Entry const& entry = _entries.at(static_cast<std::size_t>(index));
        return entry.kind == Entry::Kind::Data ? std::optional{entry.data} : std::nullopt;
    }

    // The data rows of a mounted range: contiguous in the data rows' view, since summary and heading rows only
    // interleave them. A range of summary rows alone is the empty range at the data row it stands before.
    [[nodiscard]] std::pair<std::int64_t, std::int64_t> dataRange(std::int64_t first, std::int64_t count) const {
        std::optional<std::int64_t> low;
        std::int64_t high = -1;
        auto const size = static_cast<std::int64_t>(_entries.size());
        std::int64_t const begin = std::clamp<std::int64_t>(first, 0, size);
        std::int64_t const end = std::clamp<std::int64_t>(first + std::max<std::int64_t>(count, 0), begin, size);
        for (std::int64_t at = begin; at < end; ++at) {
            if (std::optional<std::int64_t> const data = exact(at)) {
                low = low.value_or(*data);
                high = *data;
            }
        }
        if (low) {
            return {*low, high - *low + 1};
        }
        std::int64_t before = 0;
        for (std::int64_t at = 0; at < begin; ++at) {
            before += exact(at) ? 1 : 0;
        }
        return {before, 0};
    }

    // A summary row of @p spec over the data rows [first, first + count).
    [[nodiscard]] Entry summaryEntry(SummarySpec const& spec, std::optional<std::size_t> run, std::int64_t first,
                                     std::int64_t count) const {
        ValueRecord record;
        record.insert_or_assign(std::string{kSummaryField}, Value{spec.key});
        record.insert_or_assign(std::string{kSummaryFirstField}, Value{first});
        record.insert_or_assign(std::string{kSummaryCountField}, Value{count});
        record.insert_or_assign(std::string{kSummaryVersionField}, Value{static_cast<std::int64_t>(_version)});
        std::string key = "\x1Fsummary:";
        if (run) {
            record.insert_or_assign(std::string{kSummaryRunField}, Value{static_cast<std::int64_t>(*run)});
            key += std::to_string(*run) + "/";
        }
        key += spec.key;
        return Entry{.kind = Entry::Kind::Summary,
                     .data = 0,
                     .perRun = run.has_value(),
                     .key = Key{std::move(key)},
                     .record = Value{std::move(record)}};
    }

    // The mounted rows, from the data rows' count and runs now.
    void rebuild() {
        std::int64_t const total = std::max<std::int64_t>(_data->total().rows, 0);
        std::vector<std::pair<std::int64_t, std::int64_t>> const runs =
            _runs ? _runs() : std::vector<std::pair<std::int64_t, std::int64_t>>{};
        _entries.clear();
        auto const data = [this](std::int64_t at) {
            _entries.push_back(Entry{.kind = Entry::Kind::Data, .data = at, .perRun = false, .key = {}, .record = {}});
        };
        auto const summaries = [this](std::vector<SummarySpec> const& specs, bool top, std::optional<std::size_t> run,
                                      std::int64_t first, std::int64_t count) {
            for (SummarySpec const& spec : specs) {
                if (spec.top == top) {
                    _entries.push_back(summaryEntry(spec, run, first, count));
                }
            }
        };
        summaries(_table, true, std::nullopt, 0, total);
        std::int64_t next = 0;
        for (std::size_t run = 0; run < runs.size(); ++run) {
            std::int64_t const first = std::clamp<std::int64_t>(runs.at(run).first, next, total);
            std::int64_t const last = std::clamp<std::int64_t>(first + runs.at(run).second, first, total);
            for (; next < first; ++next) {
                data(next);
            }
            if (_heading) {
                RowRecord heading = _heading(run);
                _entries.push_back(Entry{.kind = Entry::Kind::Heading,
                                         .data = 0,
                                         .perRun = true,
                                         .key = std::move(heading.key),
                                         .record = std::move(heading.row)});
            }
            summaries(_perRun, true, run, first, last - first);
            for (; next < last; ++next) {
                data(next);
            }
            summaries(_perRun, false, run, first, last - first);
        }
        for (; next < total; ++next) {
            data(next);
        }
        summaries(_table, false, std::nullopt, 0, total);
        _mounted.assign(static_cast<std::size_t>(total), std::int64_t{0});
        for (std::size_t at = 0; at < _entries.size(); ++at) {
            if (_entries.at(at).kind == Entry::Kind::Data) {
                _mounted.at(static_cast<std::size_t>(_entries.at(at).data)) = static_cast<std::int64_t>(at);
            }
        }
    }

    // Whether the rows hold a row per run: a heading or a group's summary.
    [[nodiscard]] static bool perRun(std::vector<Entry> const& entries) {
        return std::ranges::any_of(entries, &Entry::perRun);
    }

    // The data rows changed: the layout follows, and the mount hears the change in mounted indices, with every summary
    // row changed after it. A change whose rows a run's rows move among is a reset.
    void heard(table::ViewChange const& change) {
        std::vector<Entry> const before = std::move(_entries);
        auto const lead = static_cast<std::size_t>(
            std::ranges::count_if(_table, [](SummarySpec const& spec) { return spec.top; }));
        ++_version;
        rebuild();
        if (!_onChange) {
            return;
        }
        bool const reset = std::ranges::any_of(
            change.ops, [](table::ViewOp const& op) { return op.kind == table::ViewOp::Kind::Reset; });
        table::ViewChange out;
        if (!reset && !perRun(before) && !perRun(_entries)) {
            // Ungrouped: every data row sits `lead` rows down, before and after.
            for (table::ViewOp op : change.ops) {
                op.first += lead;
                if (op.kind == table::ViewOp::Kind::Moved) {
                    op.to += lead;
                }
                out.ops.push_back(op);
            }
        } else {
            bool const sameShape =
                !reset && before.size() == _entries.size() &&
                std::ranges::equal(before, _entries,
                                   [](Entry const& left, Entry const& right) {
                                       return left.kind == right.kind && left.data == right.data && left.key == right.key;
                                   }) &&
                std::ranges::all_of(change.ops,
                                    [](table::ViewOp const& op) { return op.kind == table::ViewOp::Kind::Changed; });
            if (!sameShape) {
                out.ops.push_back(table::ViewOp{.kind = table::ViewOp::Kind::Reset, .first = 0, .count = 0, .to = 0});
                _onChange(out);
                return;
            }
            for (table::ViewOp const& op : change.ops) {
                for (std::size_t at = op.first; at < op.first + op.count; ++at) {
                    if (std::optional<std::int64_t> const now = mounted(static_cast<std::int64_t>(at))) {
                        out.ops.push_back(table::ViewOp{.kind = table::ViewOp::Kind::Changed,
                                                        .first = static_cast<std::size_t>(*now),
                                                        .count = 1,
                                                        .to = 0});
                    }
                }
            }
        }
        for (std::size_t at = 0; at < _entries.size(); ++at) {
            if (_entries.at(at).kind != Entry::Kind::Data) {
                out.ops.push_back(table::ViewOp{.kind = table::ViewOp::Kind::Changed, .first = at, .count = 1, .to = 0});
            }
        }
        _onChange(out);
    }

    std::shared_ptr<TableRows> _data;
    std::vector<SummarySpec> _table;
    std::vector<SummarySpec> _perRun;
    Runs _runs;
    std::function<RowRecord(std::size_t run)> _heading;
    std::function<void(table::ViewChange const&)> _onChange;
    std::vector<Entry> _entries;
    std::vector<std::int64_t> _mounted;
    std::uint64_t _version = 0;
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

namespace detail {

/// @brief Applies a `unit` overlay to a field: only when it first gives a unit, or gives another one than it last did,
///        since the overlay is read again on every change and a unit the user then picks in the editor must stay. It
///        switches as the editor does, converting the draft; no unit, or one the field does not have, after a unit
///        restores the field's entry unit. The first unit a field is given is part of how it is shown as loaded: it
///        goes to @p asLoaded, so the cell is not modified by it and a revert returns to it. A unit the field already
///        holds is not switched to, so applying the overlay again writes nothing.
/// @param applied  The unit the overlay last gave each field, by path; updated.
/// @param state    The field.
/// @param unit     The unit the overlay gives now, an id of the field's units; nothing for none.
/// @param asLoaded The fields whose first unit is switched as loaded, with the unit's index, after the caller's walk.
inline void applyUnitOverlay(std::unordered_map<std::string, std::optional<std::string>>& applied,
                             forms::FieldState& state, std::optional<std::string> const& unit,
                             std::vector<std::pair<forms::FieldState*, std::size_t>>& asLoaded) {
    auto const last = applied.find(state.path());
    bool const first = last == applied.end() || !last->second.has_value();
    bool const changed = first ? unit.has_value() : last->second != unit;
    applied.insert_or_assign(state.path(), unit);
    if (!changed) {
        return;
    }
    std::vector<forms::UnitOption> const& units = state.spec().units;
    auto const found = unit ? std::ranges::find(units, *unit, &forms::UnitOption::id) : units.end();
    std::size_t const index =
        found != units.end() ? static_cast<std::size_t>(found - units.begin()) : state.spec().entryUnit;
    if (state.unit().peek() == index) {
        return;
    }
    if (first) {
        asLoaded.emplace_back(&state, index);
    } else {
        state.switchUnit(index);
    }
}

/// @brief A field's cell text as a grid shows it. Tracked.
/// @param state  The field.
/// @param column The column's id.
/// @param format How values are shown.
/// @return The draft while blank or not encoding, else the value at the places the field takes: its `places()`, its
///         `x-decimalPlaces`, its display places, or none for an integer.
[[nodiscard]] inline std::string fieldText(forms::FieldState& state, std::string_view column,
                                           FormatContext const& format) {
    forms::Encoding const& encoding = state.encoded().get();
    if (!encoding.has_value() || !encoding->has_value()) {
        return state.draft();
    }
    forms::FieldSpec const& spec = state.spec();
    table::TableColumn shown;
    shown.info = table::ColumnInfo{.id = std::string{column}, .kind = columnKindOf(spec.kind)};
    if (std::optional<std::uint32_t> const places = state.places().get()) {
        shown.display.decimals = places;
    } else if (spec.decimalPlaces) {
        shown.display.decimals = spec.decimalPlaces;
    } else if (spec.displayDecimals) {
        shown.display.decimals = spec.displayDecimals;
    } else if (spec.kind == forms::FieldKind::Integer) {
        shown.display.decimals = 0;
    }
    return formatCell(memberCell(state, shown.info.kind), shown, format);
}

}  // namespace detail

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
          _formatVersion{form.runtime(), std::uint64_t{0}},
          _layoutVersion{form.runtime(), std::uint64_t{0}} {
        forms::FieldState const& array = form.field(_arrayPath);
        if (array.spec().kind != forms::FieldKind::ObjectArray || array.spec().truncated) {
            throw std::invalid_argument{"morph::ui::FormGridSource: `" + _arrayPath + "` is not a collection"};
        }
        for (GridColumn const& column : _columns) {
            if (!column.fieldOf && !column.field.empty() &&
                std::ranges::none_of(array.spec().children,
                                     [&column](forms::FieldSpec const& member) { return member.name == column.field; })) {
                throw std::invalid_argument{"morph::ui::FormGridSource: no member `" + column.field + "` in `" +
                                            _arrayPath + "`"};
            }
            _layout.push_back(column.id);
        }
        // The rows' columns are the edit columns, by their own ids, so a sort or a filter names a column as the
        // table does; a display column has no cell there and neither sorts nor filters.
        std::vector<table::ColumnInfo> infos;
        std::vector<std::string> members;
        std::vector<std::function<std::string(forms::RowState const&)>> perRow;
        for (GridColumn const& column : _columns) {
            if (column.fieldOf) {
                // A member per row: the kind varies, so the column sorts and filters the drafts as text.
                infos.push_back(table::ColumnInfo{.id = column.id, .kind = table::ColumnKind::Text});
                members.emplace_back();
                perRow.push_back(column.fieldOf);
                continue;
            }
            auto const member = std::ranges::find(array.spec().children, column.field, &forms::FieldSpec::name);
            if (!column.field.empty() && member != array.spec().children.end()) {
                infos.push_back(table::ColumnInfo{.id = column.id, .kind = detail::columnKindOf(member->kind)});
                members.push_back(column.field);
                perRow.emplace_back();
            }
        }
        _rowsSource =
            std::make_shared<FormRowsSource>(form, _arrayPath, std::move(infos), std::move(members), std::move(perRow));
        _rows = std::make_unique<GridRows>(_rowsSource);
        if (std::ranges::any_of(_columns, [](GridColumn const& column) {
                return column.decimals || column.unit || column.options || column.placeholder;
            })) {
            _overlays = std::make_unique<reactive::Effect>(form.runtime(), [this] { applyOverlays(); });
        }
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
    void setLayout(std::vector<std::string> visible) {
        if (visible != _layout) {
            _layout = std::move(visible);
            _layoutVersion.set(_layoutVersion.peek() + 1);
        }
    }

    /// @brief A cell's field: the member its column names, or the one `GridColumn::fieldOf` names for the row.
    ///        Tracked through `fieldOf`.
    /// @param row    The row's key.
    /// @param column The column's id.
    /// @return The field; null for a display column, an unknown column, a row the field does not hold, or a name
    ///         `fieldOf` gives that is no member.
    [[nodiscard]] forms::FieldState* field(Key const& row, std::string_view column) const {
        GridColumn const* const declared = columnOf(column);
        if (declared == nullptr || !edits(*declared)) {
            return nullptr;
        }
        forms::RowState* const held = _rowsSource->row(row);
        return held == nullptr ? nullptr : memberOf(*declared, *held);
    }

    /// @brief A row by its key.
    /// @param row The row's key.
    /// @return The row; null for one the field does not hold.
    [[nodiscard]] forms::RowState* rowOf(Key const& row) const { return _rowsSource->row(row); }

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
        bool overlay = false;
        if (declared != nullptr && declared->readonly) {
            forms::RowState const* const held = _rowsSource->row(key);
            overlay = held != nullptr && declared->readonly(*held);
        }
        return cellReadOnly(CellFacts{.field = field(key, column),
                                      .hasEdit = declared != nullptr && edits(*declared),
                                      .overlayReadOnly = overlay});
    }

    /// @brief Whether another row's span covers a cell: the rows below the spanning cell's, up to its `rows`, and
    ///        the columns `cutColumns` gives it over the layout. Spans apply only while the grid is unsorted and under
    ///        its windowing thresholds (`Virtualization`'s defaults). Untracked.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return `true` when covered; the spanning cell itself is not.
    [[nodiscard]] bool covered(Key const& key, std::string_view column) const override {
        if (!_spansOf) {
            return false;
        }
        Coverage const& coverage = coverageNow();
        auto const found = coverage.covered.find(key);
        return found != coverage.covered.end() && std::ranges::contains(found->second, column);
    }

    /// @brief The spans a row gives while spans apply, each cut over the layout.
    /// @param key The row's key.
    /// @return The spans; none while the grid is sorted or past its thresholds.
    [[nodiscard]] std::vector<CellSpan> spans(Key const& key) const override {
        if (!_spansOf) {
            return {};
        }
        static_cast<void>(_layoutVersion.get());
        Coverage const& coverage = coverageNow();
        auto const found = coverage.given.find(key);
        return found == coverage.given.end() ? std::vector<CellSpan>{} : found->second;
    }

    /// @brief Whether `setSpans` gave spans.
    /// @return `true` once it did.
    [[nodiscard]] bool spanning() const override { return static_cast<bool>(_spansOf); }

    /// @brief Gives the rows' spans: each row's cells that cover the cells below and beside them.
    /// @param spansOf A row's spans, read untracked; empty: no spans.
    void setSpans(std::function<std::vector<CellSpan>(forms::RowState const&)> spansOf) {
        _spansOf = std::move(spansOf);
        _coverage.reset();
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
        if (!edits(*declared)) {
            forms::RowState const* const held = _rowsSource->row(key);
            return held != nullptr && declared->value ? declared->value(*held) : std::string{};
        }
        forms::FieldState* const state = field(key, column);
        if (state == nullptr) {
            return {};
        }
        static_cast<void>(_formatVersion.get());
        return detail::fieldText(*state, column, _format);
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

    /// @brief An edit is meant: the form's derived values follow (`forms::FormSession::noteCommit`), then the observer
    ///        `onCommitted` gave is told.
    void committed() override {
        _form->noteCommit();
        if (_committed) {
            _committed();
        }
    }

    /// @brief A cell's editor opened: its field fetches its options from now on (`forms::FieldState::openOptions`).
    /// @param key    The row's key.
    /// @param column The column's id.
    void editorOpened(Key const& key, std::string_view column) override {
        if (forms::FieldState* const state = field(key, column)) {
            state->openOptions();
        }
    }

    /// @brief Whether the form's derived values are the last ones loaded before a failure. Tracked.
    /// @return `forms::FormSession::deriveFailing`: from a failed round until a round succeeds, a retry in flight
    ///         included.
    [[nodiscard]] bool deriveFailed() const override { return _form->deriveFailing(); }

    /// @brief Makes one more round of the form's derived values (`forms::FormSession::recalculate`).
    void recalculate() override { _form->recalculate(); }

    /// @brief A form's collection takes rows.
    /// @return `true`.
    [[nodiscard]] bool rowsEditable() const override { return true; }

    /// @brief How many rows the collection holds. Tracked.
    /// @return The count.
    [[nodiscard]] std::size_t heldRows() const override { return _rowsSource->array().rows().size(); }

    /// @brief The collection's `minItems` and `maxItems`.
    /// @return `{minItems or 0, maxItems}`.
    [[nodiscard]] std::pair<std::size_t, std::optional<std::size_t>> bounds() const override {
        forms::FieldSpec const& spec = _rowsSource->array().spec();
        return {spec.minItems.value_or(0), spec.maxItems};
    }

    /// @brief Appends a blank row to the collection (`forms::FieldState::addRow`), below `maxItems`.
    /// @return The new row's key; nothing at `maxItems`.
    std::optional<Key> appendRow() override {
        if (atMost()) {
            return std::nullopt;
        }
        std::uint64_t const key = _rowsSource->array().addRow();
        _rows->sync();
        return key == 0 ? std::nullopt : std::optional<Key>{Key{static_cast<std::int64_t>(key)}};
    }

    /// @brief Inserts a blank row into the collection before the row at a view position, below `maxItems`: at that
    ///        row's place in the record, which the view shows there while it is neither sorted nor filtered.
    /// @param position The view position; at or past `rowCount()` the row is appended.
    /// @return The new row's key; nothing at `maxItems`.
    std::optional<Key> insertRow(std::size_t position) override {
        if (atMost()) {
            return std::nullopt;
        }
        forms::FieldState& array = _rowsSource->array();
        std::size_t place = _form->runtime().untracked([&] { return array.rows().size(); });
        if (std::optional<Key> const before = keyAt(position)) {
            std::int64_t const* const number = std::get_if<std::int64_t>(&*before);
            std::vector<std::shared_ptr<forms::RowState>> const& rows = _form->runtime().untracked([&] -> auto const& {
                return array.rows();
            });
            auto const found = std::ranges::find_if(rows, [number](auto const& row) {
                return number != nullptr && std::cmp_equal(row->key(), *number);
            });
            place = static_cast<std::size_t>(found - rows.begin());
        }
        std::uint64_t const key = array.insertRow(place);
        _rows->sync();
        return key == 0 ? std::nullopt : std::optional<Key>{Key{static_cast<std::int64_t>(key)}};
    }

    /// @brief Removes rows from the collection in one change (`forms::FieldState::removeRows`), kept until the next
    ///        removal for `undoRemove`.
    /// @param keys The rows' keys.
    /// @return `true` when one of them was not blank.
    bool removeRows(std::span<Key const> keys) override {
        std::vector<std::uint64_t> numbers;
        for (Key const& key : keys) {
            if (std::int64_t const* const number = std::get_if<std::int64_t>(&key); number != nullptr && *number > 0) {
                numbers.push_back(static_cast<std::uint64_t>(*number));
            }
        }
        forms::FieldState& array = _rowsSource->array();
        _removed = array.removeRows(numbers);
        _rows->sync();
        _afterRemove = array.snapshot();
        return _form->runtime().untracked([&] {
            return std::ranges::any_of(_removed, [](forms::RemovedRow const& row) { return !row.row->blank(); });
        });
    }

    /// @brief Puts the rows the last `removeRows` took back where they stood (`forms::FieldState::restoreRows`), while
    ///        the collection holds what it held after the removal; after any change it does nothing.
    void undoRemove() override {
        forms::FieldState& array = _rowsSource->array();
        std::vector<forms::RemovedRow> rows = std::exchange(_removed, {});
        if (rows.empty() || array.snapshot() != _afterRemove) {
            return;
        }
        array.restoreRows(std::move(rows));
        _rows->sync();
    }

    /// @brief Calls @p observer at the end of each `committed`: a test seam.
    /// @param observer The observer; empty for none.
    void onCommitted(std::function<void()> observer) { _committed = std::move(observer); }

    /// @brief The cell's marks: read-only, modified, and not encoding; a computed or derived member's cell muted while
    ///        a round of derived values is in flight, and stale from a failed round until a round succeeds. Tracked.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return The marks.
    [[nodiscard]] CellMarks marks(Key const& key, std::string_view column) const override {
        forms::FieldState const* const state = field(key, column);
        bool const derived = state != nullptr && (state->spec().computed || state->spec().derived);
        return CellMarks{.readOnly = readOnly(key, column),
                         .modified = state != nullptr && _form->modified(state->path()),
                         .error = error(key, column).has_value(),
                         .stale = derived && _form->deriveFailing(),
                         .muted = derived && _form->deriveState() == forms::DeriveState::Running};
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

    /// @brief Holds the row an editor is open on in the view (`GridRows::hold`).
    /// @param key The row's key, or nothing to let it go.
    void holdRow(std::optional<Key> key) override { _rows->hold(std::move(key)); }

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

    /// @brief A cell as its column's filter reads it: the cell of the column's kind and the column. Tracked.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return The cell and its column, the column's id that of the rows' column; nothing for a display column or a
    ///         row the field does not hold.
    [[nodiscard]] std::optional<std::pair<table::Cell, table::TableColumn>> filterCell(Key const& key,
                                                                                      std::string_view column) const {
        forms::FieldState const* const state = field(key, column);
        if (state == nullptr) {
            return std::nullopt;
        }
        table::TableColumn shown;
        GridColumn const* const declared = columnOf(column);
        if (declared != nullptr && declared->fieldOf) {
            // The rows' column holds text, whatever the row's member is.
            shown.info = table::ColumnInfo{.id = std::string{column}, .kind = table::ColumnKind::Text};
            return std::pair{detail::perRowCell(*state), shown};
        }
        shown.info = table::ColumnInfo{.id = std::string{column}, .kind = detail::columnKindOf(state->spec().kind)};
        return std::pair{detail::memberCell(*state, shown.info.kind), shown};
    }

private:
    // Whether the collection holds `maxItems` rows already.
    [[nodiscard]] bool atMost() const {
        std::optional<std::size_t> const most = bounds().second;
        return most && _form->runtime().untracked([&] { return heldRows(); }) >= *most;
    }

    [[nodiscard]] GridColumn const* columnOf(std::string_view id) const {
        auto const found = std::ranges::find(_columns, id, &GridColumn::id);
        return found == _columns.end() ? nullptr : &*found;
    }

    // A column with an edit: a member it names, or one per row.
    [[nodiscard]] static bool edits(GridColumn const& column) { return column.fieldOf || !column.field.empty(); }

    // The member a column edits in a row; null for none.
    [[nodiscard]] static forms::FieldState* memberOf(GridColumn const& column, forms::RowState const& row) {
        if (column.fieldOf) {
            return row.field(column.fieldOf(row));
        }
        return column.field.empty() ? nullptr : row.field(column.field);
    }

    // The columns' overlays per row, onto each row's member: its places, its entry unit, its options and its
    // placeholder. Rerun when a row, an overlay's reads or the member a row's column edits changes.
    void applyOverlays() {
        // Forget the units applied to members of rows that are gone: a reload builds every row anew.
        if (!_unitApplied.empty()) {
            std::unordered_set<std::string> present;
            for (auto const& row : _rowsSource->array().rows()) {
                for (auto const& member : row->fields()) {
                    present.insert(member->path());
                }
            }
            std::erase_if(_unitApplied, [&present](auto const& entry) { return !present.contains(entry.first); });
        }
        // The first units a load's rows take, switched together after the walk (one batch, one pass over the form).
        std::vector<std::pair<forms::FieldState*, std::size_t>> asLoaded;
        for (auto const& row : _rowsSource->array().rows()) {
            for (GridColumn const& column : _columns) {
                forms::FieldState* const state = memberOf(column, *row);
                if (state == nullptr) {
                    continue;
                }
                std::optional<std::uint32_t> const places = column.decimals ? column.decimals(*row) : std::nullopt;
                std::optional<std::string> const unit = column.unit ? column.unit(*row) : std::nullopt;
                std::optional<std::vector<std::string>> options =
                    column.options ? column.options(*row) : std::nullopt;
                std::string placeholder = column.placeholder ? column.placeholder(*row) : std::string{};
                _form->runtime().untracked([&] {
                    if (column.decimals && state->places().peek() != places) {
                        state->places().set(places);
                    }
                    if (column.unit) {
                        applyUnit(*state, unit, asLoaded);
                    }
                    if (column.options) {
                        state->narrowOptions(std::move(options));
                    }
                    if (column.placeholder) {
                        state->overridePlaceholder(std::move(placeholder));
                    }
                });
            }
        }
        _form->runtime().untracked([&] { _form->switchUnitsAsLoaded(asLoaded); });
    }

    // A row's `unit` overlay onto its member (`detail::applyUnitOverlay`).
    void applyUnit(forms::FieldState& state, std::optional<std::string> const& unit,
                   std::vector<std::pair<forms::FieldState*, std::size_t>>& asLoaded) {
        detail::applyUnitOverlay(_unitApplied, state, unit, asLoaded);
    }

    template <typename Test>
    [[nodiscard]] std::size_t countCells(Test test) const {
        std::size_t count = 0;
        for (auto const& row : _rowsSource->array().rows()) {
            for (GridColumn const& column : _columns) {
                forms::FieldState const* const state = memberOf(column, *row);
                count += state != nullptr && test(*state) ? 1 : 0;
            }
        }
        return count;
    }

    // The cells the spans cover and the spans each row gives, as of a snapshot of the rows, the layout and the sort.
    struct Coverage {
        std::shared_ptr<table::RowSnapshot const> snapshot;
        std::vector<std::string> layout;
        bool sorted = false;
        std::unordered_map<Key, std::vector<std::string>> covered;
        std::unordered_map<Key, std::vector<CellSpan>> given;
    };

    // The coverage of the rows now, computed again once the rows, the layout or the sort changed.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): rows, their spans, and the cells each covers.
    [[nodiscard]] Coverage const& coverageNow() const {
        _rows->sync();
        std::shared_ptr<table::RowSnapshot const> const snapshot = _rowsSource->snapshot();
        bool const sorted = _rows->sorted();
        if (_coverage && _coverage->snapshot == snapshot && _coverage->layout == _layout && _coverage->sorted == sorted) {
            return *_coverage;
        }
        Coverage next{.snapshot = snapshot, .layout = _layout, .sorted = sorted, .covered = {}, .given = {}};
        Virtualization const limits{};
        auto const rows = static_cast<std::size_t>(std::max<std::int64_t>(_rows->total().rows, 0));
        bool const applies = !sorted && rows < limits.threshold && _layout.size() < limits.columnThreshold &&
                             rows * _layout.size() < limits.cellThreshold;
        if (applies) {
            std::vector<std::string> declared;
            declared.reserve(_columns.size());
            for (GridColumn const& column : _columns) {
                declared.push_back(column.id);
            }
            _form->runtime().untracked([&] {
                for (std::size_t row = 0; row < rows; ++row) {
                    std::optional<TableRowRef> const ref = _rows->row(static_cast<std::int64_t>(row));
                    forms::RowState const* const held = ref ? _rowsSource->row(ref->key) : nullptr;
                    if (held == nullptr) {
                        continue;
                    }
                    for (CellSpan span : _spansOf(*held)) {
                        span.columns = cutColumns(span, declared, _layout);
                        auto const place = std::ranges::find(_layout, span.column);
                        // A span past the last row is refused by the mount and drawn as nothing, so it covers
                        // nothing here either.
                        if (span.columns == 0 || span.rows == 0 || place == _layout.end() || span.rows > rows - row) {
                            continue;
                        }
                        auto const first = static_cast<std::size_t>(place - _layout.begin());
                        for (std::size_t down = 0; down < span.rows; ++down) {
                            std::optional<TableRowRef> const below = _rows->row(static_cast<std::int64_t>(row + down));
                            if (!below) {
                                continue;
                            }
                            for (std::size_t right = 0; right < span.columns; ++right) {
                                if (down != 0 || right != 0) {
                                    next.covered[below->key].push_back(_layout.at(first + right));
                                }
                            }
                        }
                        next.given[ref->key].push_back(std::move(span));
                    }
                }
            });
        }
        _coverage = std::move(next);
        return *_coverage;
    }

    forms::FormSession* _form;
    std::weak_ptr<void const> _formAlive;
    std::string _arrayPath;
    std::vector<GridColumn> _columns;
    std::function<std::vector<CellSpan>(forms::RowState const&)> _spansOf;
    mutable std::optional<Coverage> _coverage;
    FormatContext _format;
    reactive::Signal<std::uint64_t> _formatVersion;
    // Moves with each change of `_layout`, which a drawn span's columns follow.
    reactive::Signal<std::uint64_t> _layoutVersion;
    std::vector<std::string> _layout;
    std::shared_ptr<FormRowsSource> _rowsSource;
    std::unique_ptr<GridRows> _rows;
    std::function<void()> _committed;
    std::optional<std::size_t> _hook;
    // The unit each member's `unit` overlay last gave, by the member's path; a member it never reached has none.
    std::unordered_map<std::string, std::optional<std::string>> _unitApplied;
    std::unique_ptr<reactive::Effect> _overlays;
    // The rows the last removal took, and what the collection held right after it: an undo puts them back only while
    // it still holds that.
    std::vector<forms::RemovedRow> _removed;
    std::string _afterRemove;
};

/// @brief One cell of a fixed grid's row: a field of the form it edits, an expression it shows, or a literal text.
///
/// A cell with `field` edits that field; a cell without one is read-only and shows `value`, else `text`.
struct FixedCell {
    /// @brief The path of the form field the cell edits (`limits.density.min`); in a block, relative to the block's
    ///        row, so `mass` is `specimens#<key>.mass`. Empty for a cell that shows `value` or `text`.
    std::string field;
    /// @brief What a `{value}` cell shows, read with the block's row, null outside a block. Tracked.
    std::function<std::string(forms::RowState const*)> value;
    /// @brief What a `{text}` cell shows.
    std::string text;
    /// @brief How many rows the cell covers, its own included, downwards.
    std::size_t rows = 1;
    /// @brief How many columns the cell covers, its own included, to the right in the layout.
    std::size_t columns = 1;
    /// @brief Whether the cell is read-only beyond its field (`CellFacts::overlayReadOnly`), read with the block's row,
    ///        null outside a block. Tracked.
    std::function<bool(forms::RowState const*)> readonly;
    /// @brief The entry places of the cell's field, which are also its wire `dp`, read with the block's row, null
    ///        outside a block; nothing keeps the field's. Tracked.
    std::function<std::optional<std::uint32_t>(forms::RowState const*)> decimals;
    /// @brief The unit the cell's field is entered in, an id of its `FieldSpec::units`, read with the block's row, null
    ///        outside a block; applied as a grid column's `unit` overlay is (`GridColumn::unit`). Tracked.
    std::function<std::optional<std::string>(forms::RowState const*)> unit;
    /// @brief The options the cell's field offers, as their values' JSON literals, narrowing a choice's, read with the
    ///        block's row, null outside a block; nothing offers every option. Tracked.
    std::function<std::optional<std::vector<std::string>>(forms::RowState const*)> options;
    /// @brief The cell's field's placeholder, read with the block's row, null outside a block; empty keeps the field's.
    ///        Tracked.
    std::function<std::string(forms::RowState const*)> placeholder;
};

/// @brief One row of a fixed grid: its key, its label and its cells by column id.
struct FixedRow {
    /// @brief The row's key, unique among the grid's parts.
    std::string key;
    /// @brief The row's name: what an assistive technology reads for it.
    std::string label;
    /// @brief The row's cells, by column id; a column with no cell here shows nothing and takes no edit.
    std::map<std::string, FixedCell, std::less<>> cells;
};

/// @brief Rows of a fixed grid repeated for each row of a form's `ObjectArray` field.
struct FixedBlock {
    /// @brief The path of the collection the rows repeat over.
    std::string forEach;
    /// @brief The rows repeated for each of its rows, in order; a cell's `field` is relative to that row.
    std::vector<FixedRow> rows;
};

/// @brief A fixed grid: rows a standard or a specification fixes, each cell its own field of a form, of its own kind,
///        places and editor.
///
/// Every row is held, so `keyAt` is never empty below `rowCount`. A fixed row's key is its `FixedRow::key`; a block
/// row's is the collection row's key and the part's joined, `"<row key>/<key>"`, so it stays the same while rows of the
/// collection are added and removed. The rows are in the parts' order, a block's in its collection's order, and are
/// never sorted, filtered or rearranged; the columns may be hidden and resized. Spans always apply. A cell with no
/// field is read-only.
class FixedGridSource final : public GridSource {
public:
    /// @brief A part of a fixed grid: a row, or a block of rows.
    using Part = std::variant<FixedRow, FixedBlock>;

    /// @brief A fixed grid.
    /// @param runtime The runtime its state is tracked on, and whose owner its rows are delivered on. Borrowed.
    /// @param form    The form whose fields the cells edit; null for a grid of `value` and `text` cells only. Borrowed:
    ///                it must outlive the source.
    /// @param rows    The parts, in order.
    /// @param columns The columns, each id once: only their ids and labels are read.
    /// @param format  How values are shown.
    /// @throws std::invalid_argument when a key is empty, holds a `/` or is given twice, a cell names a field with
    ///         @p form null or a field the form does not have, or a block repeats over no collection of the form.
    FixedGridSource(reactive::Runtime& runtime, forms::FormSession* form, std::vector<Part> rows,
                    std::vector<GridColumn> columns, FormatContext format)
        : _rt{&runtime},
          _form{form},
          _formAlive{form != nullptr ? form->alive() : std::weak_ptr<void const>{}},
          _parts{std::move(rows)},
          _columns{std::move(columns)},
          _format{std::move(format)},
          _formatVersion{runtime, std::uint64_t{0}},
          _layoutVersion{runtime, std::uint64_t{0}},
          _data{std::make_shared<table::VectorSource>(
              std::vector<table::ColumnInfo>{table::ColumnInfo{.id = std::string{kGridKeyField},
                                                               .kind = table::ColumnKind::Text}})} {
        for (GridColumn const& column : _columns) {
            _layout.push_back(column.id);
        }
        checkParts();
        expand();
        table::EngineOptions options;
        options.owner = &runtime.owner();
        _rows = std::make_unique<EngineRows>(
            _data,
            [](table::RowSnapshot const& snapshot, std::size_t row) {
                table::RowId key = snapshot.rowId(row);
                ValueRecord record;
                record.insert_or_assign(std::string{kGridKeyField}, Value{std::get<std::string>(key)});
                return RowRecord{.key = std::move(key), .row = Value{std::move(record)}};
            },
            std::move(options));
        _rows->engine().flush();
        if (std::ranges::any_of(_parts, [](Part const& part) { return std::holds_alternative<FixedBlock>(part); })) {
            _watch = std::make_unique<reactive::Effect>(runtime, [this] { watch(); });
        }
        if (std::ranges::any_of(_parts, [](Part const& part) {
                return std::ranges::any_of(rowsOf(part), [](FixedRow const& row) {
                    return std::ranges::any_of(row.cells, [](auto const& cell) {
                        FixedCell const& given = cell.second;
                        return !given.field.empty() &&
                               (given.decimals || given.unit || given.options || given.placeholder);
                    });
                });
            })) {
            _overlays = std::make_unique<reactive::Effect>(runtime, [this] { applyOverlays(); });
        }
    }

    FixedGridSource(FixedGridSource const&) = delete;
    FixedGridSource& operator=(FixedGridSource const&) = delete;
    FixedGridSource(FixedGridSource&&) = delete;
    FixedGridSource& operator=(FixedGridSource&&) = delete;

    /// @brief Stops watching the collections and removes the commit `onBeforeSubmit` registered.
    ~FixedGridSource() override {
        _overlays.reset();
        _watch.reset();
        if (_hook && _form != nullptr && !_formAlive.expired()) {
            _form->removeBeforeSubmit(*_hook);
        }
    }

    /// @brief The rows; the collections' rows are taken in first.
    /// @return The count.
    [[nodiscard]] std::size_t rowCount() const override {
        sync();
        return _expanded.size();
    }

    /// @brief The key at a position; the collections' rows are taken in first.
    /// @param viewRow The position.
    /// @return The key; empty past the rows.
    [[nodiscard]] std::optional<Key> keyAt(std::size_t viewRow) const override {
        sync();
        return viewRow < _expanded.size() ? std::optional<Key>{_expanded.at(viewRow).key} : std::nullopt;
    }

    /// @brief Where a key's row is; the collections' rows are taken in first.
    /// @param key The key.
    /// @return The position, or nothing.
    [[nodiscard]] std::optional<std::size_t> positionOf(Key const& key) const override {
        sync();
        auto const found = _index.find(key);
        return found == _index.end() ? std::nullopt : std::optional{found->second};
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

    /// @brief Sets the visible columns in layout order: what the walk follows, and what a span is cut over.
    /// @param visible The column ids.
    void setLayout(std::vector<std::string> visible) {
        if (visible != _layout) {
            _layout = std::move(visible);
            _layoutVersion.set(_layoutVersion.peek() + 1);
        }
    }

    /// @brief A cell's field. Tracked through the collections' rows.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return The field; null for a cell with none, an unknown row or column, or a block row's field its collection
    ///         row lacks.
    [[nodiscard]] forms::FieldState* field(Key const& key, std::string_view column) const {
        Expanded const* const row = expandedOf(key);
        if (row == nullptr) {
            return nullptr;
        }
        auto const found = row->fields.find(column);
        return found == row->fields.end() ? nullptr : found->second;
    }

    /// @brief A row's label.
    /// @param key The row's key.
    /// @return Its `FixedRow::label`; empty for an unknown row.
    [[nodiscard]] std::string label(Key const& key) const {
        Expanded const* const row = expandedOf(key);
        return row == nullptr ? std::string{} : row->row->label;
    }

    /// @brief The forms control of the cell's field.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return The editor; a muted text for a cell with no field.
    [[nodiscard]] Node editor(Key const& key, std::string_view column) override {
        forms::FieldState* const state = field(key, column);
        if (state == nullptr || _form == nullptr) {
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

    /// @brief `cellReadOnly` over the cell's field and its `readonly` overlay. Tracked.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return `true` when read-only: always for a `value` or a `text` cell.
    [[nodiscard]] bool readOnly(Key const& key, std::string_view column) const override {
        auto const [cell, specimen] = cellOf(key, column);
        bool const overlay = cell != nullptr && cell->readonly && cell->readonly(specimen);
        return cellReadOnly(CellFacts{.field = field(key, column),
                                      .hasEdit = cell != nullptr && !cell->field.empty(),
                                      .overlayReadOnly = overlay});
    }

    /// @brief Whether a span of another cell covers a cell: the rows below the spanning cell's, up to its `rows`, and
    ///        the columns `cutColumns` gives it over the layout. Untracked.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return `true` when covered; the spanning cell itself is not.
    [[nodiscard]] bool covered(Key const& key, std::string_view column) const override {
        Coverage const& coverage = coverageNow();
        auto const found = coverage.covered.find(key);
        return found != coverage.covered.end() && std::ranges::contains(found->second, column);
    }

    /// @brief The spans a row's cells give, each cut over the layout.
    /// @param key The row's key.
    /// @return The spans.
    [[nodiscard]] std::vector<CellSpan> spans(Key const& key) const override {
        static_cast<void>(_layoutVersion.get());
        Coverage const& coverage = coverageNow();
        auto const found = coverage.given.find(key);
        return found == coverage.given.end() ? std::vector<CellSpan>{} : found->second;
    }

    /// @brief Whether a cell spans more than itself.
    /// @return `true` when one does.
    [[nodiscard]] bool spanning() const override {
        return std::ranges::any_of(_parts, [](Part const& part) {
            return std::ranges::any_of(rowsOf(part), [](FixedRow const& row) {
                return std::ranges::any_of(row.cells, [](auto const& cell) {
                    return cell.second.rows > 1 || cell.second.columns > 1;
                });
            });
        });
    }

    /// @brief The cell's draft. Untracked.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return The draft; empty for a cell with no field.
    [[nodiscard]] std::string draft(Key const& key, std::string_view column) const override {
        forms::FieldState const* const state = field(key, column);
        return state == nullptr ? std::string{} : _rt->untracked([&] { return state->draft(); });
    }

    /// @brief The cell's text: a `text` cell's text, a `value` cell's value, a field's draft while blank or not
    ///        encoding, else its value at the field's places. Tracked.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return The text.
    [[nodiscard]] std::string text(Key const& key, std::string_view column) const override {
        auto const [cell, specimen] = cellOf(key, column);
        if (cell == nullptr) {
            return {};
        }
        if (cell->field.empty()) {
            return cell->value ? cell->value(specimen) : cell->text;
        }
        forms::FieldState* const state = field(key, column);
        if (state == nullptr) {
            return {};
        }
        static_cast<void>(_formatVersion.get());
        return detail::fieldText(*state, column, _format);
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

    /// @brief An edit is meant: the form's derived values follow (`forms::FormSession::noteCommit`).
    void committed() override {
        if (_form != nullptr) {
            _form->noteCommit();
        }
    }

    /// @brief A cell's editor opened: its field fetches its options from now on (`forms::FieldState::openOptions`).
    /// @param key    The row's key.
    /// @param column The column's id.
    void editorOpened(Key const& key, std::string_view column) override {
        if (forms::FieldState* const state = field(key, column)) {
            state->openOptions();
        }
    }

    /// @brief Whether the form's derived values are the last ones loaded before a failure. Tracked.
    /// @return `forms::FormSession::deriveFailing`; `false` with no form.
    [[nodiscard]] bool deriveFailed() const override { return _form != nullptr && _form->deriveFailing(); }

    /// @brief Makes one more round of the form's derived values (`forms::FormSession::recalculate`).
    void recalculate() override {
        if (_form != nullptr) {
            _form->recalculate();
        }
    }

    /// @brief The cell's marks: read-only, modified and not encoding; a computed or derived field's cell muted while a
    ///        round of derived values is in flight, and stale from a failed round until a round succeeds. Tracked.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return The marks.
    [[nodiscard]] CellMarks marks(Key const& key, std::string_view column) const override {
        forms::FieldState const* const state = field(key, column);
        bool const derived = state != nullptr && (state->spec().computed || state->spec().derived);
        return CellMarks{.readOnly = readOnly(key, column),
                         .modified = state != nullptr && _form->modified(state->path()),
                         .error = error(key, column).has_value(),
                         .stale = derived && _form->deriveFailing(),
                         .muted = derived && _form->deriveState() == forms::DeriveState::Running};
    }

    /// @brief The key a fixed grid's record holds in `#key`.
    /// @param record The record.
    /// @return The key; empty for a record without one.
    [[nodiscard]] std::optional<Key> keyOf(Value const& record) const override {
        Value const* const key = record.field(kGridKeyField);
        std::string const* const text = key == nullptr ? nullptr : key->asString();
        return text == nullptr ? std::nullopt : std::optional<Key>{Key{*text}};
    }

    /// @brief Whether a field cell of the row is modified (`forms::FormSession::modified`). Tracked.
    /// @param key The row's key.
    /// @return `true` when one is.
    [[nodiscard]] bool rowModified(Key const& key) const override {
        Expanded const* const row = expandedOf(key);
        return row != nullptr && std::ranges::any_of(row->fields, [this](auto const& entry) {
                   return entry.second != nullptr && _form->modified(entry.second->path());
               });
    }

    /// @brief The rows: one per row, in order, with no sort and no filter.
    /// @return They.
    [[nodiscard]] EngineRows& view() override {
        sync();
        return *_rows;
    }

    /// @brief Restores the cell's loaded value.
    /// @param key    The row's key.
    /// @param column The column's id.
    void revertCell(Key const& key, std::string_view column) override {
        if (forms::FieldState const* const state = field(key, column)) {
            _form->revert(state->path());
        }
    }

    /// @brief Restores the loaded values of the row's field cells.
    /// @param key The row's key.
    void revertRow(Key const& key) override {
        Expanded const* const row = expandedOf(key);
        if (row == nullptr) {
            return;
        }
        std::vector<std::string> paths;
        for (auto const& [column, state] : row->fields) {
            if (state != nullptr) {
                paths.push_back(state->path());
            }
        }
        for (std::string const& path : paths) {
            _form->revert(path);
        }
    }

    /// @brief Registers @p commit with the form's submit for the source's life, so the form's submit ends an open edit
    ///        first. Does nothing with no form.
    /// @param commit The commit.
    void onBeforeSubmit(std::function<void()> commit) {
        if (_form == nullptr) {
            return;
        }
        if (_hook) {
            _form->removeBeforeSubmit(*_hook);
        }
        _hook = _form->addBeforeSubmit(std::move(commit));
    }

    /// @brief How many field cells are modified. Tracked.
    /// @return The count.
    [[nodiscard]] std::size_t modifiedCount() const {
        return countCells([this](forms::FieldState const& state) { return _form->modified(state.path()); });
    }

    /// @brief How many rows have a modified field cell. Tracked.
    /// @return The count.
    [[nodiscard]] std::size_t modifiedRows() const {
        sync();
        return static_cast<std::size_t>(
            std::ranges::count_if(_expanded, [this](Expanded const& row) { return rowModified(row.key); }));
    }

    /// @brief How many field cells do not encode, or were refused. Tracked.
    /// @return The count.
    [[nodiscard]] std::size_t errorCount() const {
        return countCells([this](forms::FieldState const& state) {
            return state.error().get().has_value() || _form->refusal(state.path()).has_value();
        });
    }

private:
    // A row as shown: its key, its part, the collection row of a block's row, and its cells' fields by column id.
    struct Expanded {
        Key key;
        FixedRow const* row = nullptr;
        forms::RowState const* specimen = nullptr;
        std::map<std::string, forms::FieldState*, std::less<>> fields;
    };

    // The cells the spans cover and the spans each row gives, as of an expansion and a layout.
    struct Coverage {
        std::uint64_t expansion = 0;
        std::vector<std::string> layout;
        std::unordered_map<Key, std::vector<std::string>> covered;
        std::unordered_map<Key, std::vector<CellSpan>> given;
    };

    // Each block's collection rows, by key and identity: a change of either is a change of the block's rows.
    using BlockRows = std::vector<std::vector<std::pair<std::uint64_t, forms::RowState const*>>>;

    [[nodiscard]] static std::span<FixedRow const> rowsOf(Part const& part) {
        if (FixedRow const* const row = std::get_if<FixedRow>(&part)) {
            return {row, 1};
        }
        return std::get<FixedBlock>(part).rows;
    }

    // The field spec at a path below some fields' specs; null for none.
    [[nodiscard]] static forms::FieldSpec const* specAt(std::span<forms::FieldSpec const> fields,
                                                        std::string_view path) {
        forms::FieldSpec const* found = nullptr;
        while (!path.empty()) {
            std::size_t const dot = path.find('.');
            std::string_view const name = path.substr(0, dot);
            path = dot == std::string_view::npos ? std::string_view{} : path.substr(dot + 1);
            auto const member = std::ranges::find(fields, name, &forms::FieldSpec::name);
            if (member == fields.end()) {
                return nullptr;
            }
            found = &*member;
            fields = member->children;
        }
        return found;
    }

    [[noreturn]] static void refuse(std::string why) {
        throw std::invalid_argument{"morph::ui::FixedGridSource: " + std::move(why)};
    }

    // Keys are given once and not empty, and every field a cell names is the form's.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): one refusal per rule of a part.
    void checkParts() const {
        std::unordered_set<std::string> keys;
        for (Part const& part : _parts) {
            FixedBlock const* const block = std::get_if<FixedBlock>(&part);
            std::span<forms::FieldSpec const> members;
            if (block != nullptr) {
                if (_form == nullptr) {
                    refuse("a block repeats rows of a form's collection, and there is no form");
                }
                forms::FieldState const* array = nullptr;
                try {
                    array = &_form->field(block->forEach);
                } catch (std::out_of_range const&) {
                    refuse("`" + block->forEach + "` is not a field of the form");
                }
                if (array->spec().kind != forms::FieldKind::ObjectArray || array->spec().truncated) {
                    refuse("`" + block->forEach + "` is not a collection");
                }
                members = array->spec().children;
            }
            for (FixedRow const& row : rowsOf(part)) {
                if (row.key.find('/') != std::string::npos) {
                    refuse("the key `" + row.key + "` holds a `/`, which joins a block row's keys");
                }
                if (row.key.empty() || !keys.insert(row.key).second) {
                    refuse(row.key.empty() ? std::string{"a row has no key"}
                                           : "the key `" + row.key + "` is given twice");
                }
                for (auto const& [column, cell] : row.cells) {
                    if (cell.field.empty()) {
                        continue;
                    }
                    if (_form == nullptr) {
                        refuse("the cell `" + row.key + "`, `" + column + "` edits a field, and there is no form");
                    }
                    bool const known = block != nullptr ? specAt(members, cell.field) != nullptr : [&] {
                        try {
                            static_cast<void>(_form->field(cell.field));
                            return true;
                        } catch (std::out_of_range const&) {
                            return false;
                        }
                    }();
                    if (!known) {
                        refuse("`" + cell.field + "` is not a field of the form");
                    }
                }
            }
        }
    }

    // Each block's collection rows now. Tracked: a reader follows a change of a block's rows.
    [[nodiscard]] BlockRows blockRows() const {
        BlockRows out;
        for (Part const& part : _parts) {
            if (FixedBlock const* const block = std::get_if<FixedBlock>(&part)) {
                std::vector<std::pair<std::uint64_t, forms::RowState const*>> rows;
                for (auto const& row : _form->field(block->forEach).rows()) {
                    rows.emplace_back(row->key(), row.get());
                }
                out.push_back(std::move(rows));
            }
        }
        return out;
    }

    // The rows from the parts and the collections' rows now, given to the rows' source as one reset.
    void expand() const {
        std::vector<Expanded> next;
        _blockRows = _rt->untracked([&] { return blockRows(); });
        std::size_t block = 0;
        for (Part const& part : _parts) {
            if (FixedRow const* const row = std::get_if<FixedRow>(&part)) {
                Expanded made{.key = Key{row->key}, .row = row, .specimen = nullptr, .fields = {}};
                for (auto const& [column, cell] : row->cells) {
                    if (!cell.field.empty()) {
                        made.fields.insert_or_assign(column, &_form->field(cell.field));
                    }
                }
                next.push_back(std::move(made));
                continue;
            }
            FixedBlock const& repeated = std::get<FixedBlock>(part);
            for (auto const& [rowKey, specimen] : _blockRows.at(block)) {
                for (FixedRow const& row : repeated.rows) {
                    Expanded made{.key = Key{std::to_string(rowKey) + "/" + row.key},
                                  .row = &row,
                                  .specimen = specimen,
                                  .fields = {}};
                    for (auto const& [column, cell] : row.cells) {
                        if (cell.field.empty()) {
                            continue;
                        }
                        std::string const path = repeated.forEach + "#" + std::to_string(rowKey) + "." + cell.field;
                        try {
                            made.fields.insert_or_assign(column, &_form->field(path));
                        } catch (std::out_of_range const&) {
                            made.fields.insert_or_assign(column, nullptr);
                        }
                    }
                    next.push_back(std::move(made));
                }
            }
            ++block;
        }
        _expanded = std::move(next);
        _index.clear();
        std::vector<table::RowId> ids;
        std::vector<std::vector<table::Cell>> cells;
        for (std::size_t at = 0; at < _expanded.size(); ++at) {
            Key const& key = _expanded.at(at).key;
            _index.emplace(key, at);
            ids.push_back(key);
            cells.push_back({table::Cell{std::get<std::string>(key)}});
        }
        ++_expansion;
        _data->setRows(std::move(ids), std::move(cells));
    }

    // Takes in the collections' rows when a block's changed, and the engine the reset at once.
    void sync() const {
        if (_blockRows.empty() || blockRows() == _blockRows) {
            return;
        }
        _rt->untracked([&] { expand(); });
        _rows->engine().flush();
    }

    // A change of a block's rows is taken in at the next turn of the owner, so the mount hears it without a read.
    void watch() {
        static_cast<void>(blockRows());
        if (_watching && !_posted) {
            _posted = true;
            _rt->owner().post(_alive.guard([this] {
                _posted = false;
                sync();
            }));
        }
        _watching = true;
    }

    [[nodiscard]] Expanded const* expandedOf(Key const& key) const {
        sync();
        auto const found = _index.find(key);
        return found == _index.end() ? nullptr : &_expanded.at(found->second);
    }

    // A cell's part and its block's collection row; null for no cell.
    [[nodiscard]] std::pair<FixedCell const*, forms::RowState const*> cellOf(Key const& key,
                                                                            std::string_view column) const {
        Expanded const* const row = expandedOf(key);
        if (row == nullptr) {
            return {nullptr, nullptr};
        }
        auto const found = row->row->cells.find(column);
        return {found == row->row->cells.end() ? nullptr : &found->second, row->specimen};
    }

    // Each field cell's overlays onto its field: its places, its entry unit, its options and its placeholder. Rerun
    // when a block's rows, an overlay's reads or the form's load count change. A load sets a field's unit afresh, so
    // the units the overlays gave an unmodified field before it are forgotten and given again as loaded. A switch as
    // loaded is no load and moves no load count, so no grid takes another grid's switch for a load, whatever units
    // they give.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): one section per overlay.
    void applyOverlays() {
        sync();
        if (std::uint64_t const generation = _form->loadGeneration(); generation != _unitGeneration) {
            // A field the user changed keeps the unit they left it in.
            _rt->untracked([&] {
                std::erase_if(_unitApplied, [this](auto const& entry) { return !_form->modified(entry.first); });
            });
            _unitGeneration = generation;
        }
        std::vector<std::pair<forms::FieldState*, std::size_t>> asLoaded;
        for (Expanded const& row : _expanded) {
            for (auto const& [column, state] : row.fields) {
                auto const found = row.row->cells.find(column);
                if (state == nullptr || found == row.row->cells.end()) {
                    continue;
                }
                FixedCell const& cell = found->second;
                std::optional<std::uint32_t> const places = cell.decimals ? cell.decimals(row.specimen) : std::nullopt;
                std::optional<std::string> const unit = cell.unit ? cell.unit(row.specimen) : std::nullopt;
                std::optional<std::vector<std::string>> options =
                    cell.options ? cell.options(row.specimen) : std::nullopt;
                std::string placeholder = cell.placeholder ? cell.placeholder(row.specimen) : std::string{};
                _rt->untracked([&] {
                    if (cell.decimals && state->places().peek() != places) {
                        state->places().set(places);
                    }
                    if (cell.unit) {
                        detail::applyUnitOverlay(_unitApplied, *state, unit, asLoaded);
                    }
                    if (cell.options) {
                        state->narrowOptions(std::move(options));
                    }
                    if (cell.placeholder) {
                        state->overridePlaceholder(std::move(placeholder));
                    }
                });
            }
        }
        if (!asLoaded.empty()) {
            _rt->untracked([&] { _form->switchUnitsAsLoaded(asLoaded); });
        }
    }

    template <typename Test>
    [[nodiscard]] std::size_t countCells(Test test) const {
        sync();
        std::size_t count = 0;
        for (Expanded const& row : _expanded) {
            for (auto const& [column, state] : row.fields) {
                count += state != nullptr && test(*state) ? 1 : 0;
            }
        }
        return count;
    }

    // The coverage of the rows now, computed again once the rows or the layout changed.
    [[nodiscard]] Coverage const& coverageNow() const {
        sync();
        if (_coverage && _coverage->expansion == _expansion && _coverage->layout == _layout) {
            return *_coverage;
        }
        Coverage next{.expansion = _expansion, .layout = _layout, .covered = {}, .given = {}};
        std::vector<std::string> declared;
        declared.reserve(_columns.size());
        for (GridColumn const& column : _columns) {
            declared.push_back(column.id);
        }
        for (std::size_t at = 0; at < _expanded.size(); ++at) {
            Expanded const& row = _expanded.at(at);
            for (auto const& [column, cell] : row.row->cells) {
                if (cell.rows <= 1 && cell.columns <= 1) {
                    continue;
                }
                CellSpan span{.column = column, .rows = cell.rows, .columns = cell.columns};
                span.columns = cutColumns(span, declared, _layout);
                auto const place = std::ranges::find(_layout, span.column);
                // A span past the last row is refused by the mount and drawn as nothing, so it covers nothing here
                // either.
                if (span.columns == 0 || span.rows == 0 || place == _layout.end() ||
                    span.rows > _expanded.size() - at) {
                    continue;
                }
                auto const first = static_cast<std::size_t>(place - _layout.begin());
                for (std::size_t down = 0; down < span.rows; ++down) {
                    for (std::size_t right = 0; right < span.columns; ++right) {
                        if (down != 0 || right != 0) {
                            next.covered[_expanded.at(at + down).key].push_back(_layout.at(first + right));
                        }
                    }
                }
                next.given[row.key].push_back(std::move(span));
            }
        }
        _coverage = std::move(next);
        return *_coverage;
    }

    reactive::Runtime* _rt;
    forms::FormSession* _form;
    std::weak_ptr<void const> _formAlive;
    std::vector<Part> _parts;
    std::vector<GridColumn> _columns;
    FormatContext _format;
    reactive::Signal<std::uint64_t> _formatVersion;
    // Moves with each change of `_layout`, which a drawn span's columns follow.
    reactive::Signal<std::uint64_t> _layoutVersion;
    std::vector<std::string> _layout;
    std::shared_ptr<table::VectorSource> _data;
    std::unique_ptr<EngineRows> _rows;
    // The rows as shown, their positions by key, the collections' rows they were expanded from, and a count that moves
    // with each expansion.
    mutable std::vector<Expanded> _expanded;
    mutable std::unordered_map<Key, std::size_t> _index;
    mutable BlockRows _blockRows;
    mutable std::uint64_t _expansion = 0;
    mutable std::optional<Coverage> _coverage;
    std::optional<std::size_t> _hook;
    // The unit each field's `unit` overlay last gave, by the field's path, and the form's load count it was given at.
    std::unordered_map<std::string, std::optional<std::string>> _unitApplied;
    std::uint64_t _unitGeneration = 0;
    bool _watching = false;
    bool _posted = false;
    async::CallbackScope _alive;
    std::unique_ptr<reactive::Effect> _watch;
    std::unique_ptr<reactive::Effect> _overlays;
};

/// @brief One column of a model's table as a grid edits it.
struct ModelColumn {
    /// @brief The column's id.
    std::string id;
    /// @brief The action that edits the column's cells; none leaves the column read-only.
    std::optional<table::EditInfo> edit;
    /// @brief The edit action's form, `forms::FormModel::fromSchema` over the action's input schema; null for a
    ///        column with no edit.
    std::shared_ptr<forms::FormModel const> model;
    /// @brief The field of `model` the column's cells are: `value` for a `CellEdit`, `values.<member>` for a
    ///        `RowEdit`; empty for a column with no edit.
    std::string path;
};

/// @brief When a model's table commits its edits.
enum class EditMode : std::uint8_t {
    Cell,  ///< Each committed cell is its own request.
    Row,   ///< A row's committed cells are collected while focus stays in the row, and sent as one request.
};

/// @brief What a model's table's cells are edited and shown with.
struct ModelGridContext {
    /// @brief The runtime the source's state and its edit sessions are tracked on. Borrowed.
    reactive::Runtime* runtime = nullptr;
    /// @brief Fetches a choice field's options.
    forms::ChoiceFetcher choices;
    /// @brief The edit sessions' locale and patterns.
    forms::FormSessionOptions session;
    /// @brief How a cell's value is shown.
    FormatContext format;
    /// @brief Whether the principal may run an action: an edit action it may not run leaves its column read-only.
    ///        Tracked.
    std::function<bool(std::string_view action)> may;
};

/// @brief The cells of a model's table as a grid edits them: rows the window holds, each editable column's cells the
///        field of its edit action, drafts and their errors held by `table::CellEdits` by row key and column.
///
/// The rows are the window's view: `keyAt` is empty for a row not fetched yet, which stops a walk there. `view()` is a
/// decorator over the rows that sets their one change slot itself: on each change it first ends the modified marks of
/// every row the change brought (`table::CellEdits::rowFetched`), then calls the handler the mount set on it. The
/// table's own edits are sent through the `CellEdits`' commit function, never as runs of the session, so they make no
/// row busy: a cell being saved is stale, and its row takes further edits, queued behind it.
class ModelGridSource final : public GridSource {
public:
    /// @brief A model's table as a grid.
    /// @param rows    The window's rows; the decorator `view()` owns their change slot from now on.
    /// @param edits   The table's drafts and commits. Borrowed: it must outlive the source.
    /// @param columns Every column the table shows, each with its id; an editable one with its form and field.
    /// @param mode    When a row's edits are sent.
    /// @param context What the cells are edited and shown with.
    ModelGridSource(std::shared_ptr<TableRows> rows, table::CellEdits& edits, std::vector<ModelColumn> columns,
                    EditMode mode, ModelGridContext context)
        : _rows{std::move(rows)},
          _edits{&edits},
          _columns{std::move(columns)},
          _mode{mode},
          _context{std::move(context)},
          _rt{_context.runtime},
          _version{*_context.runtime, std::uint64_t{0}},
          _open{*_context.runtime, std::optional<CellRef>{}} {
        for (ModelColumn const& column : _columns) {
            _layout.push_back(column.id);
            if (column.edit && column.model != nullptr && !column.path.empty()) {
                // The submitter is never called: an edit is sent as a row commit, by the table's own `CellEdits`.
                exec::IExecutor* const owner = &_rt->owner();
                auto session = std::make_unique<forms::FormSession>(
                    *_rt, *column.model,
                    [owner](std::string_view, std::string) {
                        return async::Completion<std::string>::makeSettleable(owner).first;
                    },
                    _context.choices, _context.session);
                _sessions.insert_or_assign(column.id, Session{.form = std::move(session), .loadedFor = {}, .base = {}});
            }
        }
        _view = std::make_unique<Rows>(*this);
        _edits->onRowSettled(_alive.guard([this](table::RowId const& row, bool refused) { rowSettled(row, refused); }));
    }

    ModelGridSource(ModelGridSource const&) = delete;
    ModelGridSource& operator=(ModelGridSource const&) = delete;
    ModelGridSource(ModelGridSource&&) = delete;
    ModelGridSource& operator=(ModelGridSource&&) = delete;
    /// @brief Leaves the rows' change slot and the edits' receiver to whatever set them last: a source mounted after
    ///        this one may hold them already, and what this one set is refused once it is gone.
    ~ModelGridSource() override = default;

    /// @brief The description the rows follow: its key names a record's key, its columns are shown and edited, and
    ///        the drafts of a column it no longer has are dropped (`table::CellEdits::keepColumns`).
    /// @param description The description.
    void setDescription(table::TableDescription const& description) {
        _description = description;
        std::vector<std::string> ids;
        for (table::TableColumn const& column : description.columns) {
            ids.push_back(column.info.id);
        }
        _edits->keepColumns(ids);
        _positionsVersion.reset();
        bump();
    }

    /// @brief Sets the visible columns in layout order: what the walk follows.
    /// @param visible The column ids.
    void setLayout(std::vector<std::string> visible) {
        if (visible != _layout) {
            _layout = std::move(visible);
            bump();
        }
    }

    /// @brief Shows the cells in another formatting from now on.
    /// @param format How values are shown.
    void setFormat(FormatContext format) {
        _context.format = std::move(format);
        bump();
    }

    /// @brief Tells every reader of the cells that something they depend on and cannot track changed: the rows' busy
    ///        and failed states.
    void touch() { bump(); }

    /// @brief Rows a keyed fetch named and found none of: their drafts, errors and marks go
    ///        (`table::CellEdits::rowAbsent`).
    /// @param keys The rows' keys.
    void rowsAbsent(std::span<table::RowId const> keys) {
        for (table::RowId const& key : keys) {
            _edits->rowAbsent(key);
        }
        bump();
    }

    /// @brief A row's key changed: the rows and the drafts move what they hold by it.
    /// @param from The row's old key.
    /// @param to   The row's new key.
    void moveKey(Key const& from, Key const& to) {
        _rows->moveKey(from, to);
        _edits->moveRow(from, to);
        _positionsVersion.reset();
        bump();
    }

    /// @brief A cell's draft while one is held, for a cell view that shows it in place of the row's value. Tracked.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return The draft; nothing while none is held.
    [[nodiscard]] std::optional<std::string> heldDraft(Key const& key, std::string_view column) const {
        static_cast<void>(_version.get());
        if (std::optional<CellRef> const open = _open.get(); open && open->key == key && open->column == column) {
            if (Session const* const session = sessionOf(column); session != nullptr && session->loadedFor == key) {
                return fieldOf(column)->text().get();
            }
        }
        return _edits->draft(key, column);
    }

    /// @brief The view's rows, fetched or not.
    /// @return `TableRows::total().rows`. Tracked.
    [[nodiscard]] std::size_t rowCount() const override {
        static_cast<void>(_version.get());
        return static_cast<std::size_t>(std::max<std::int64_t>(_rows->total().rows, 0));
    }

    /// @brief The key of the row at a view position. Tracked: it answers once the row arrives.
    /// @param viewRow The position.
    /// @return The key; empty while the row is not fetched.
    [[nodiscard]] std::optional<Key> keyAt(std::size_t viewRow) const override {
        static_cast<void>(_version.get());
        std::optional<TableRowRef> const row = _rows->row(static_cast<std::int64_t>(viewRow));
        return row ? std::optional<Key>{row->key} : std::nullopt;
    }

    /// @brief Where a key's row is in the view among the rows held.
    /// @param key The key.
    /// @return The position; empty when no held row has the key.
    [[nodiscard]] std::optional<std::size_t> positionOf(Key const& key) const override {
        if (_positionsVersion != _version.peek()) {
            _positions.clear();
            std::int64_t const total = _rows->total().rows;
            for (std::int64_t at = 0; at < total; ++at) {
                if (std::optional<TableRowRef> const row = _rows->row(at)) {
                    _positions.insert_or_assign(row->key, static_cast<std::size_t>(at));
                }
            }
            _positionsVersion = _version.peek();
        }
        auto const found = _positions.find(key);
        return found == _positions.end() ? std::nullopt : std::optional{found->second};
    }

    /// @brief The visible columns in layout order.
    /// @return The ids.
    [[nodiscard]] std::vector<std::string> columns() const override { return _layout; }

    /// @brief The forms control of the cell's field, its session loaded with the cell's draft or the row's value.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return The editor; a muted text for a column with no edit.
    [[nodiscard]] Node editor(Key const& key, std::string_view column) override {
        Session* const session = loaded(key, column);
        if (session == nullptr) {
            return ui::text(Text{.text = std::string{}, .role = TextRole::Muted});
        }
        return forms::detail::engine::scalarControl(*session->form, *fieldOf(column));
    }

    /// @brief Whether the cell's field is a Boolean.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return `true` for a Boolean.
    [[nodiscard]] bool toggles(Key const& key, std::string_view column) const override {
        static_cast<void>(key);
        forms::FieldState const* const field = fieldOf(column);
        return field != nullptr && field->spec().kind == forms::FieldKind::Boolean;
    }

    /// @brief `cellReadOnly` over the cell's field, whether the principal may run its edit action, its
    ///        `EditInfo::readonly` companion, the row's busy and locked states and whether the cell is loaded.
    ///        Tracked.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return `true` when read-only.
    [[nodiscard]] bool readOnly(Key const& key, std::string_view column) const override {
        return cellReadOnly(factsOf(key, column));
    }

    /// @brief No cell of a model's table is covered.
    /// @param key    Unused.
    /// @param column Unused.
    /// @return `false`.
    [[nodiscard]] bool covered(Key const& key, std::string_view column) const override {
        static_cast<void>(key);
        static_cast<void>(column);
        return false;
    }

    /// @brief The cell's draft: the open editor's text, else the draft held, else the row's value as its field shows
    ///        it for editing. Untracked.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return The draft; empty for a column with no edit.
    [[nodiscard]] std::string draft(Key const& key, std::string_view column) const override {
        return _rt->untracked([&] {
            if (std::optional<std::string> held = heldDraft(key, column)) {
                return std::move(*held);
            }
            // With no draft held the cell is the row's value now: a session still holding an earlier edit of the
            // cell, whose draft a fetch since ended, is loaded again.
            auto* const self = const_cast<ModelGridSource*>(this);
            if (auto const found = self->_sessions.find(column); found != self->_sessions.end()) {
                found->second.loadedFor.reset();
            }
            Session* const session = self->loaded(key, column);
            return session == nullptr ? std::string{} : fieldOf(column)->draft();
        });
    }

    /// @brief The cell's text: its draft while one is held, else the row's value as the column shows it. Tracked.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return The text.
    [[nodiscard]] std::string text(Key const& key, std::string_view column) const override {
        if (std::optional<std::string> held = heldDraft(key, column)) {
            return std::move(*held);
        }
        table::TableColumn const* const described = describedColumn(column);
        std::optional<Located> const row = locate(key);
        if (described == nullptr || !row) {
            return {};
        }
        return formatCell(cellOf(row->record.field(std::string{column})), *described, _context.format);
    }

    /// @brief Why the cell's value is refused: its draft does not encode, or the edit action refused it. Tracked.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return The message; empty for none.
    [[nodiscard]] std::optional<std::string> error(Key const& key, std::string_view column) const override {
        static_cast<void>(_version.get());
        return _edits->error(key, column);
    }

    /// @brief Writes a user's draft into the cell: its field takes it, and the draft is held with the field's error.
    ///        A draft that is the row's own value again is no draft.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @param draft  The draft.
    void write(Key const& key, std::string_view column, std::string draft) override {
        Session* const session = loaded(key, column);
        if (session == nullptr) {
            return;
        }
        _open.set(CellRef{.key = key, .column = std::string{column}});
        fieldOf(column)->text().set(std::move(draft));
        CellRef const cell{.key = key, .column = std::string{column}};
        if (!std::ranges::contains(_dirty, cell)) {
            _dirty.push_back(cell);
        }
        hold(key, column);
    }

    /// @brief An edit is meant: each cell written since the last commit is held with its encoding and, when it
    ///        encodes and the cell takes edits, sent as its own request (`EditMode::Cell`) or kept for its row's
    ///        request (`EditMode::Row`).
    void committed() override {
        std::vector<CellRef> cells = std::exchange(_dirty, {});
        // An editor's input writes its field without `write`: the open cell is committed too.
        if (std::optional<CellRef> const open = _open.peek(); open && !std::ranges::contains(cells, *open)) {
            cells.push_back(*open);
        }
        for (CellRef const& cell : cells) {
            std::optional<table::EditCell> const made = encode(cell.key, cell.column);
            if (!made) {
                continue;
            }
            if (_mode == EditMode::Cell) {
                _edits->commitRow(table::RowEditRequest{.row = cell.key, .cells = {*made}});
                continue;
            }
            std::vector<table::EditCell>& held = _held[cell.key];
            std::erase_if(held, [&](table::EditCell const& other) { return other.column == made->column; });
            held.push_back(*made);
        }
        _open.set(std::nullopt);
        bump();
    }

    /// @brief A cell's editor opened: its session is loaded with the cell, and a choice fetches its options.
    /// @param key    The row's key.
    /// @param column The column's id.
    void editorOpened(Key const& key, std::string_view column) override {
        if (loaded(key, column) == nullptr) {
            return;
        }
        _open.set(CellRef{.key = key, .column = std::string{column}});
        fieldOf(column)->openOptions();
    }

    /// @brief The cell's marks: read-only, modified from its commit's success until its row's next fetch, refused,
    ///        and stale while its commit is out. Tracked.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return The marks.
    [[nodiscard]] CellMarks marks(Key const& key, std::string_view column) const override {
        static_cast<void>(_version.get());
        return CellMarks{.readOnly = readOnly(key, column),
                         .modified = _edits->modified(key, column),
                         .error = _edits->error(key, column).has_value(),
                         .stale = _edits->stale(key, column)};
    }

    /// @brief The key a record holds in the description's key column.
    /// @param record The record.
    /// @return The key; empty for a record without one, or before a description.
    [[nodiscard]] std::optional<Key> keyOf(Value const& record) const override {
        if (!_description) {
            return std::nullopt;
        }
        Value const* const key = record.field(_description->key);
        if (key == nullptr) {
            return std::nullopt;
        }
        if (std::int64_t const* const number = key->asInt()) {
            return Key{*number};
        }
        if (std::string const* const text = key->asString()) {
            return Key{*text};
        }
        return std::nullopt;
    }

    /// @brief Whether some cell of the row is modified or holds a draft. Tracked.
    /// @param key The row's key.
    /// @return `true` when one does.
    [[nodiscard]] bool rowModified(Key const& key) const override {
        static_cast<void>(_version.get());
        return _edits->rowEdited(key);
    }

    /// @brief The decorator over the rows the mount is handed.
    /// @return It.
    [[nodiscard]] TableRows& view() override { return *_view; }

    /// @brief Why typing on a read-only cell is refused: the cell is not loaded, the row is being saved, or the row
    ///        is locked, in that order.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return The status line's text; nothing for any other read-only cell.
    [[nodiscard]] std::optional<std::string> refusal(Key const& key, std::string_view column) const override {
        CellFacts const facts = factsOf(key, column);
        if (facts.unloaded) {
            return std::string{kUnloadedText};
        }
        if (facts.busy) {
            return std::string{kBusyText};
        }
        if (facts.locked) {
            std::optional<Located> const row = locate(key);
            if (row && _description) {
                return lockText(TableRowRef{.key = key, .row = row->record}, *_description);
            }
        }
        return std::nullopt;
    }

    /// @brief Focus moved: in `EditMode::Row` the cells held for every other row are sent, a request per row.
    /// @param key The row focus is on now; nothing when it is on none.
    void focusRow(std::optional<Key> const& key) override {
        if (_mode != EditMode::Row) {
            return;
        }
        std::vector<Key> leaving;
        for (auto const& [row, cells] : _held) {
            if (!key || row != *key) {
                leaving.push_back(row);
            }
        }
        for (Key const& row : leaving) {
            send(row);
        }
    }

    /// @brief `Ctrl+Enter`: every row's held cells are sent.
    void commitHeld() override {
        std::vector<Key> rows;
        for (auto const& [row, cells] : _held) {
            rows.push_back(row);
        }
        for (Key const& row : rows) {
            send(row);
        }
    }

    /// @brief A model's table selects rows: it takes no paste, fill or clear.
    /// @return `false`.
    [[nodiscard]] bool takesBlocks() const override { return false; }

    /// @brief Keeps what brings focus back to the first refused cell of a row commit in `EditMode::Row`.
    /// @param refocus Focuses a cell while the table has focus.
    void refocusWith(std::function<void(CellRef const&)> refocus) override { _refocus = std::move(refocus); }

    /// @brief The status line's refusal of typing on a cell not loaded yet.
    static constexpr std::string_view kUnloadedText = "This value is not loaded yet";
    /// @brief The status line's refusal of typing in a row another run is saving.
    static constexpr std::string_view kBusyText = "This row is being saved";

private:
    // An editable column's form: its session, the row it is loaded with, and its field's text for that row's value.
    struct Session {
        std::unique_ptr<forms::FormSession> form;
        std::optional<Key> loadedFor;
        std::string base;
    };

    // A held row: its view index and its record, valid until the rows' next change.
    struct Located {
        std::int64_t index = 0;
        Value const& record;
    };

    // The rows the mount reads: the window's, with their one change slot set here.
    class Rows final : public TableRows {
    public:
        explicit Rows(ModelGridSource& source) : _source{&source}, _inner{source._rows.get()} {
            _inner->onChange(source._alive.guard([this](table::ViewChange const& change) { changed(change); }));
        }

        Rows(Rows const&) = delete;
        Rows& operator=(Rows const&) = delete;
        Rows(Rows&&) = delete;
        Rows& operator=(Rows&&) = delete;
        ~Rows() override = default;

        [[nodiscard]] TableTotal total() const override { return _inner->total(); }
        [[nodiscard]] TableTotal mountedTotal() const override { return _inner->mountedTotal(); }
        [[nodiscard]] std::optional<TableRowRef> row(std::int64_t index) const override { return _inner->row(index); }
        [[nodiscard]] table::UnloadedLevels unloadedLevels(std::int64_t index) const override {
            return _inner->unloadedLevels(index);
        }
        void visible(std::int64_t first, std::int64_t count, std::span<std::string const> columns) override {
            _inner->visible(first, count, columns);
        }
        async::Completion<std::expected<std::vector<table::RowId>, table::TableError>> keysOf(
            std::int64_t first, std::int64_t count) override {
            return _inner->keysOf(first, count);
        }
        void locateRow(table::RowId key, std::stop_token stop, std::function<void(RowLocation const&)> done) override {
            _inner->locateRow(std::move(key), std::move(stop), std::move(done));
        }
        async::Completion<std::expected<std::vector<FetchedRow>, table::TableError>> fetchRows(
            std::vector<table::RowId> keys, std::vector<std::string> columns) override {
            return _inner->fetchRows(std::move(keys), std::move(columns));
        }
        std::expected<void, table::TableError> setSort(SortChain sort) override {
            return _inner->setSort(std::move(sort));
        }
        std::expected<void, table::TableError> setFilter(table::FilterSpec filter) override {
            return _inner->setFilter(std::move(filter));
        }
        async::Completion<std::expected<table::DistinctValues, table::TableError>> distinctValues(
            table::DistinctRequest request) override {
            return _inner->distinctValues(std::move(request));
        }
        [[nodiscard]] TableCountsView counts() const override { return _inner->counts(); }
        [[nodiscard]] std::expected<ViewSelection, SelectAllRefusal> selectView() const override {
            return _inner->selectView();
        }
        [[nodiscard]] SelectedCount selectedCount(TableSelection const& selection) const override {
            return _inner->selectedCount(selection);
        }
        [[nodiscard]] bool leftView(table::RowId const& key) const override { return _inner->leftView(key); }
        [[nodiscard]] RetainResult retain(std::span<table::RowId const> keys) override { return _inner->retain(keys); }
        void clearRetained() override { _inner->clearRetained(); }
        [[nodiscard]] bool unmatched(table::RowId const& key) const override { return _inner->unmatched(key); }
        void holdRefetches(std::uint64_t run) override { _inner->holdRefetches(run); }
        void releaseRefetches(std::uint64_t run) override { _inner->releaseRefetches(run); }
        async::Completion<std::expected<std::vector<FetchedRow>, table::TableError>> rowsOf(
            std::int64_t first, std::int64_t count, std::span<std::string const> columns) override {
            return _inner->rowsOf(first, count, columns);
        }
        [[nodiscard]] RowStates& states() override { return _inner->states(); }
        [[nodiscard]] RowStates const& states() const override { return _inner->states(); }
        void moveKey(table::RowId const& from, table::RowId const& to) override { _source->moveKey(from, to); }
        void onChange(std::function<void(table::ViewChange const&)> handler) override { _handler = std::move(handler); }

    private:
        // The rows a change brought are fetched: their modified marks end before anything reads the rows, then the
        // mount hears the change.
        void changed(table::ViewChange const& change) {
            for (std::size_t const index : broughtRows(change)) {
                if (std::optional<TableRowRef> const row = _inner->row(static_cast<std::int64_t>(index))) {
                    _source->_edits->rowFetched(row->key);
                }
            }
            _source->bump();
            if (_handler) {
                std::function<void(table::ViewChange const&)> const handler = _handler;
                handler(change);
            }
        }

        ModelGridSource* _source;
        TableRows* _inner;
        std::function<void(table::ViewChange const&)> _handler;
    };

    // The view indices, after the whole change, of the rows its `Inserted` and `Changed` operations brought.
    [[nodiscard]] static std::vector<std::size_t> broughtRows(table::ViewChange const& change) {
        std::vector<std::size_t> out;
        for (table::ViewOp const& op : change.ops) {
            switch (op.kind) {
                case table::ViewOp::Kind::Reset:
                    out.clear();
                    break;
                case table::ViewOp::Kind::Removed:
                    std::erase_if(out, [&op](std::size_t at) { return at >= op.first && at < op.first + op.count; });
                    for (std::size_t& at : out) {
                        at -= at >= op.first + op.count ? op.count : 0;
                    }
                    break;
                case table::ViewOp::Kind::Inserted:
                    for (std::size_t& at : out) {
                        at += at >= op.first ? op.count : 0;
                    }
                    for (std::size_t at = op.first; at < op.first + op.count; ++at) {
                        out.push_back(at);
                    }
                    break;
                case table::ViewOp::Kind::Changed:
                    for (std::size_t at = op.first; at < op.first + op.count; ++at) {
                        if (!std::ranges::contains(out, at)) {
                            out.push_back(at);
                        }
                    }
                    break;
                case table::ViewOp::Kind::Moved:
                    for (std::size_t& at : out) {
                        if (at == op.first) {
                            at = op.to;
                            continue;
                        }
                        std::size_t const removed = at > op.first ? at - 1 : at;
                        at = removed >= op.to ? removed + 1 : removed;
                    }
                    break;
            }
        }
        return out;
    }

    void bump() { _version.set(_version.peek() + 1); }

    [[nodiscard]] ModelColumn const* columnOf(std::string_view id) const {
        auto const found = std::ranges::find(_columns, id, &ModelColumn::id);
        return found == _columns.end() ? nullptr : &*found;
    }

    [[nodiscard]] table::TableColumn const* describedColumn(std::string_view id) const {
        if (!_description) {
            return nullptr;
        }
        auto const found = std::ranges::find_if(_description->columns,
                                                [id](table::TableColumn const& column) { return column.info.id == id; });
        return found == _description->columns.end() ? nullptr : &*found;
    }

    [[nodiscard]] Session const* sessionOf(std::string_view column) const {
        auto const found = _sessions.find(column);
        return found == _sessions.end() ? nullptr : &found->second;
    }

    [[nodiscard]] forms::FieldState* fieldOf(std::string_view column) const {
        Session const* const session = sessionOf(column);
        ModelColumn const* const described = columnOf(column);
        return session == nullptr || described == nullptr ? nullptr : &session->form->field(described->path);
    }

    // The held row with a key: its view index and record. Tracked through the source's version.
    [[nodiscard]] std::optional<Located> locate(Key const& key) const {
        static_cast<void>(_version.get());
        std::optional<std::size_t> const index = positionOf(key);
        if (!index) {
            return std::nullopt;
        }
        std::optional<TableRowRef> const row = _rows->row(static_cast<std::int64_t>(*index));
        if (!row || row->key != key) {
            return std::nullopt;
        }
        return Located{.index = static_cast<std::int64_t>(*index), .record = row->row};
    }

    // Every fact the cell's read-only state follows from. Tracked.
    [[nodiscard]] CellFacts factsOf(Key const& key, std::string_view column) const {
        static_cast<void>(_version.get());
        ModelColumn const* const described = columnOf(column);
        CellFacts facts;
        facts.field = fieldOf(column);
        facts.hasEdit = described != nullptr && described->edit && (!_context.may || _context.may(described->edit->action));
        std::optional<Located> const row = locate(key);
        if (!row) {
            facts.unloaded = true;
            return facts;
        }
        if (described != nullptr && described->edit && !described->edit->readonly.empty()) {
            Value const* const companion = row->record.field(described->edit->readonly);
            bool const* const frozen = companion == nullptr ? nullptr : companion->asBool();
            facts.companionReadOnly = frozen != nullptr && *frozen;
        }
        facts.busy = _rows->states().isBusy(key);
        facts.locked = _description && lockText(TableRowRef{.key = key, .row = row->record}, *_description).has_value();
        table::TableColumn const* const shown = describedColumn(column);
        table::UnloadedLevels const levels = _rows->unloadedLevels(row->index);
        facts.unloaded = row->record.field(std::string{column}) == nullptr ||
                         (shown != nullptr && !shown->info.level.empty() &&
                          std::ranges::contains(levels.levels, shown->info.level));
        return facts;
    }

    // The column's session loaded with the cell: with the row's value, and then the draft held, unless it holds the
    // cell already. Null for a column with no edit.
    Session* loaded(Key const& key, std::string_view column) {
        auto const found = _sessions.find(column);
        ModelColumn const* const described = columnOf(column);
        if (found == _sessions.end() || described == nullptr) {
            return nullptr;
        }
        Session& session = found->second;
        if (session.loadedFor == key) {
            return &session;
        }
        _rt->untracked([&] {
            std::optional<Located> const row = locate(key);
            Value const* const value = row ? row->record.field(std::string{column}) : nullptr;
            std::string const json = value == nullptr ? std::string{"null"}
                                                      : glz::write_json(valueToJson(*value)).value_or("null");
            std::string const body = described->edit->member.empty()
                                         ? R"({"value":)" + json + "}"
                                         : R"({"values":{")" + described->edit->member + R"(":)" + json + "}}";
            _rt->batch([&] {
                session.form->prefill(body);
                forms::FieldState& field = *fieldOf(column);
                session.base = field.draft();
                if (std::optional<std::string> held = _edits->draft(key, column)) {
                    field.text().set(std::move(*held));
                }
            });
        });
        session.loadedFor = key;
        return &session;
    }

    // Holds the field's text as the cell's draft, with the field's error; the row's own value again is no draft.
    void hold(Key const& key, std::string_view column) {
        Session const* const session = sessionOf(column);
        forms::FieldState const* const field = fieldOf(column);
        if (session == nullptr || field == nullptr) {
            return;
        }
        _rt->untracked([&] {
            std::string const text = field->draft();
            if (text == session->base && !_edits->stale(key, column) && !_edits->modified(key, column)) {
                _edits->clearDraft(key, column);
                return;
            }
            std::optional<forms::FieldError> const& failed = field->error().get();
            _edits->setDraft(key, column, text, failed ? std::optional{failed->message} : std::nullopt);
        });
        bump();
    }

    // The cell's draft, held, and its cell of a request when it encodes and the cell takes edits. A cell that became
    // read-only while it was edited is not sent: an edit of a value the user has not seen, or of a row another run is
    // saving, would overwrite what arrives.
    std::optional<table::EditCell> encode(Key const& key, std::string const& column) {
        if (loaded(key, column) == nullptr) {
            return std::nullopt;
        }
        hold(key, column);
        if (!_edits->draft(key, column) || _edits->error(key, column) ||
            _rt->untracked([&] { return readOnly(key, column); })) {
            return std::nullopt;
        }
        forms::Encoding const encoding = _rt->untracked([&] { return fieldOf(column)->encoded().get(); });
        ModelColumn const* const described = columnOf(column);
        return table::EditCell{.column = column,
                               .json = encoding && encoding->has_value() ? (*encoding)->json : std::string{"null"},
                               .member = described->edit->member.empty() ? std::string{"value"}
                                                                         : described->edit->member};
    }

    // Sends a row's held cells as one request.
    void send(Key const& row) {
        auto const found = _held.find(row);
        if (found == _held.end()) {
            return;
        }
        std::vector<table::EditCell> cells = std::move(found->second);
        _held.erase(found);
        if (!cells.empty()) {
            _edits->commitRow(table::RowEditRequest{.row = row, .cells = std::move(cells)});
        }
        bump();
    }

    // A row commit settled: the cells follow, and in `EditMode::Row` a refusal brings focus back to the row's first
    // refused cell in layout order.
    void rowSettled(table::RowId const& row, bool refused) {
        bump();
        if (_mode != EditMode::Row || !refused || !_refocus) {
            return;
        }
        for (std::string const& column : _layout) {
            if (_edits->error(row, column)) {
                std::function<void(CellRef const&)> const refocus = _refocus;
                refocus(CellRef{.key = row, .column = column});
                return;
            }
        }
    }

    // A record's value as a cell of its column.
    [[nodiscard]] static table::Cell cellOf(Value const* value) {
        if (value == nullptr || value->isNull()) {
            return std::monostate{};
        }
        if (std::int64_t const* const integer = value->asInt()) {
            return *integer;
        }
        if (Decimal const* const decimal = value->asDecimal()) {
            return decimal->value;
        }
        if (bool const* const flag = value->asBool()) {
            return *flag;
        }
        if (Quantity const* const quantity = value->asQuantity()) {
            return quantity->value;
        }
        if (std::string const* const text = value->asString()) {
            return *text;
        }
        return std::monostate{};
    }

    std::shared_ptr<TableRows> _rows;
    table::CellEdits* _edits;
    std::vector<ModelColumn> _columns;
    EditMode _mode;
    ModelGridContext _context;
    reactive::Runtime* _rt;
    reactive::Signal<std::uint64_t> _version;
    // The cell whose editor is open, or whose text was written last: its field holds its text.
    reactive::Signal<std::optional<CellRef>> _open;
    std::vector<std::string> _layout;
    std::optional<table::TableDescription> _description;
    std::map<std::string, Session, std::less<>> _sessions;
    // Cells written since the last commit, and the cells each row holds for its request in `EditMode::Row`.
    std::vector<CellRef> _dirty;
    std::map<Key, std::vector<table::EditCell>> _held;
    std::function<void(CellRef const&)> _refocus;
    // The held rows' view indices by key, as of a version.
    mutable std::unordered_map<Key, std::size_t> _positions;
    mutable std::optional<std::uint64_t> _positionsVersion;
    async::CallbackScope _alive;
    std::unique_ptr<Rows> _view;
};

/// @brief A grid's focus, its cell editor and its walk over a `GridSource`, and the table node that shows them.
///
/// The editor's text is the cell's draft, live: an opening keeps the draft the cell held, a commit closes the editor
/// and calls `GridSource::committed` once when the draft changed, and `Escape` writes the kept draft back. A paste, a
/// fill, a clear and each row operation is one change too: it calls `GridSource::committed` once, however many cells or
/// rows it touches.
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
          _standingEntries{runtime, std::vector<StatusEntry>{}},
          _selection{runtime, std::optional<CellRange>{}} {
        _source->refocusWith([this](CellRef const& cell) {
            if (_hasFocus) {
                focusCell(cell);
            }
        });
    }

    GridController(GridController const&) = delete;
    GridController& operator=(GridController const&) = delete;
    GridController(GridController&&) = delete;
    GridController& operator=(GridController&&) = delete;
    /// @brief Takes back what the source was given to move focus with.
    ~GridController() { _source->refocusWith({}); }

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
            // Tracked: a binding over the place runs again when the row arrives, and names its key.
            shown->key = _source->keyAt(shown->viewRow);
        }
        return shown;
    }

    /// @brief Takes the cell a click or a key resolved to: `focusCell` for a fetched row, else the placeholder focus.
    /// @param target The cell.
    void focusPlace(GridFocus target) {
        _hasFocus = true;
        if (target.key) {
            focusCell(CellRef{.key = *target.key, .column = target.column});
            return;
        }
        commit();
        letGo();
        setPlace(std::move(target));
        setSelection(std::nullopt);
        clearNotice();
    }

    /// @brief Focuses a cell, as a click or an arrow does; a covered cell, and a key the view does not hold, are
    ///        refused. An open editor on another cell is committed first. The status line shows the cell's error while
    ///        it has focus. A cell outside the cell selection ends it.
    /// @param cell The cell.
    void focusCell(CellRef cell) {
        _hasFocus = true;
        if (_source->covered(cell.key, cell.column)) {
            return;
        }
        if (std::optional<CellRef> const open = _editing.peek(); open && *open != cell) {
            commit();
            letGo();
        }
        std::optional<std::size_t> const at = _source->positionOf(cell.key);
        if (!at) {
            return;
        }
        setPlace(GridFocus{.key = cell.key, .viewRow = *at, .column = cell.column});
        if (_selection.peek() && !inBlock(cell)) {
            setSelection(std::nullopt);
        }
        clearNotice();
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
        // A Boolean opens no editor: a click or `Space` flips it, and the second click of a double click, which
        // activates, flips nothing more.
        if (_source->toggles(cell.key, cell.column)) {
            return;
        }
        _kept = _source->draft(cell.key, cell.column);
        _source->holdRow(cell.key);
        _editing.set(cell);
        _source->editorOpened(cell.key, cell.column);
    }

    /// @brief The cell whose editor is open. Tracked.
    /// @return The cell, or none.
    [[nodiscard]] std::optional<CellRef> editing() const { return _editing.get(); }

    /// @brief The chords the table declares, what `Table::keyChords` binds. Tracked.
    /// @return With an editor open exactly `Enter`, `Tab`, `Shift+Tab`, `Escape` and `Ctrl+Enter`, so every other key
    ///         reaches the editor; with none, also `F2`, `Space`, `Delete`, `Shift` with an arrow, `Ctrl+A`, `Ctrl+V`,
    ///         `Ctrl+D`, `Ctrl+Shift+=` and `Ctrl+-`. `Ctrl+C` is the table's own copy key, which copies `copy()`.
    [[nodiscard]] std::vector<std::string> keyChords() const {
        if (_editing.get()) {
            return {"Enter", "Tab", "Shift+Tab", "Escape", "Ctrl+Enter"};
        }
        return {"Tab",         "Shift+Tab", "Enter",  "F2",     "Space",  "Delete",       "Escape",
                "Ctrl+Enter",  "Shift+Up",  "Shift+Down", "Shift+Left", "Shift+Right", "Ctrl+A", "Ctrl+V",
                "Ctrl+D",      "Ctrl+Shift+=", "Ctrl+-"};
    }

    /// @brief A chord pressed while the table has focus.
    /// @param chord The chord.
    /// @return `FocusNext` or `FocusPrevious` when `Tab` or `Shift+Tab` runs past the last or the first editable cell,
    ///         `Ignored` for a chord the grid does not take, else `Handled`.
    KeyOutcome press(std::string_view chord) {
        // The row an editor was open on is let go once the key is handled: the walk moves by view position at the
        // moment of the commit, so the edited row stays where it was while focus moves on.
        struct LetGo {
            GridController* grid;
            explicit LetGo(GridController* held) : grid{held} {}
            LetGo(LetGo const&) = delete;
            LetGo& operator=(LetGo const&) = delete;
            LetGo(LetGo&&) = delete;
            LetGo& operator=(LetGo&&) = delete;
            ~LetGo() { grid->letGo(); }
        } const letting{this};
        _hasFocus = true;
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
            _source->commitHeld();
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
            clear();
            return KeyOutcome::Handled;
        }
        if (chord == "Shift+Up" || chord == "Shift+Down" || chord == "Shift+Left" || chord == "Shift+Right") {
            extend(chord == "Shift+Up" ? -1 : (chord == "Shift+Down" ? 1 : 0),
                   chord == "Shift+Left" ? -1 : (chord == "Shift+Right" ? 1 : 0));
            return KeyOutcome::Handled;
        }
        if (chord == "Ctrl+A") {
            selectAll();
            return KeyOutcome::Handled;
        }
        if (chord == "Ctrl+V") {
            if (_readClipboard) {
                if (std::optional<std::string> const text = _readClipboard()) {
                    static_cast<void>(paste(*text));
                }
            }
            return KeyOutcome::Handled;
        }
        if (chord == "Ctrl+D") {
            fillDown();
            return KeyOutcome::Handled;
        }
        if (chord == "Ctrl+Shift+=") {
            insertRow(true);
            return KeyOutcome::Handled;
        }
        if (chord == "Ctrl+-") {
            deleteRows();
            return KeyOutcome::Handled;
        }
        return KeyOutcome::Ignored;
    }

    /// @brief Printable text: with an editor open, the editor's whole new text, as its input writes it; on a focused
    ///        cell with none, the cell's draft is kept, @p text written as the draft and the editor opened. A
    ///        read-only cell refuses it in the status line.
    /// @param text The text.
    void type(std::string_view text) {
        _hasFocus = true;
        if (std::optional<CellRef> const open = _editing.peek()) {
            _source->write(open->key, open->column, std::string{text});
            return;
        }
        std::optional<CellRef> const focused = _rt->untracked([&] { return focus(); });
        if (!focused || _source->covered(focused->key, focused->column)) {
            return;
        }
        if (_rt->untracked([&] { return _source->readOnly(focused->key, focused->column); })) {
            notice(_rt->untracked([&] { return _source->refusal(focused->key, focused->column); })
                       .value_or(std::string{kReadOnlyText}));
            return;
        }
        if (_source->toggles(focused->key, focused->column)) {
            return;
        }
        _kept = _source->draft(focused->key, focused->column);
        _source->holdRow(focused->key);
        _source->write(focused->key, focused->column, std::string{text});
        _editing.set(*focused);
        _source->editorOpened(focused->key, focused->column);
    }

    /// @brief Focus left the table: an open editor is committed, and the source hears that focus is on no row.
    void leave() {
        commit();
        letGo();
        _hasFocus = false;
        _focusedRow.reset();
        _source->focusRow(std::nullopt);
    }

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

    /// @brief A state that lasts while its source holds: `kDeriveFailedText` from a failed round of derived values until
    ///        a round succeeds, a retry in flight included. Tracked.
    /// @return The text; empty for none.
    [[nodiscard]] std::string standing() const {
        if (_source->deriveFailed()) {
            return std::string{kDeriveFailedText};
        }
        return _standing.get();
    }

    /// @brief The choices the standing state offers: "Recalculate" (`recalculate`) while a round of derived values
    ///        failed. Tracked.
    /// @return The entries.
    [[nodiscard]] std::vector<StatusEntry> standingEntries() const {
        if (_source->deriveFailed()) {
            return {StatusEntry{.id = "recalculate", .label = "Recalculate"}};
        }
        return _standingEntries.get();
    }

    /// @brief An entry chosen in the status line: `recalculate` makes one more round of derived values, `undo` puts
    ///        back the rows the last deletion removed (`undo`).
    /// @param id The entry's id; one the grid does not offer is ignored.
    void chooseEntry(std::string_view id) {
        if (id == "recalculate") {
            _source->recalculate();
        } else if (id == "undo") {
            undo();
        }
    }

    /// @brief The grid's entries of a cell's menu: "Revert cell", enabled while the cell is modified, and "Revert row",
    ///        enabled while the row is modified, each restoring the loaded value and then committing once; then, unless
    ///        the rows are fixed, a separator and the row entries (`rowEntries`).
    /// @param row    The row's key.
    /// @param column The column's id.
    /// @return The entries.
    [[nodiscard]] std::vector<MenuItem> cellEntries(Key const& row, std::string_view column) {
        GridController* const self = this;
        GridSource* const source = _source;
        std::string const id{column};
        MenuItem cell;
        cell.label = std::string{"Revert cell"};
        cell.enabled = Prop<bool>{[source, row, id] { return source->marks(row, id).modified; }};
        cell.onSelect = [self, source, row, id] {
            self->noteChange();
            source->revertCell(row, id);
            source->committed();
        };
        MenuItem whole;
        whole.label = std::string{"Revert row"};
        whole.enabled = Prop<bool>{[source, row] { return source->rowModified(row); }};
        whole.onSelect = [self, source, row] {
            self->noteChange();
            source->revertRow(row);
            source->committed();
        };
        std::vector<MenuItem> entries;
        entries.push_back(std::move(cell));
        entries.push_back(std::move(whole));
        std::vector<MenuItem> rows = rowEntries(row);
        if (!rows.empty()) {
            MenuItem separator;
            separator.separator = true;
            entries.push_back(std::move(separator));
            std::ranges::move(rows, std::back_inserter(entries));
        }
        return entries;
    }

    /// @brief The row entries of a row's menus: "Insert row above", "Insert row below" and "Delete row", or "Delete
    ///        rows" when the cell selection spans more than one row and @p row is among them, deleting every row it
    ///        spans. An insertion is disabled at `maxItems` and a deletion where it would leave fewer than `minItems`,
    ///        each with the bound as its description. Each is one change.
    /// @param row The row's key.
    /// @return The entries; none while the rows are fixed.
    [[nodiscard]] std::vector<MenuItem> rowEntries(Key const& row) {
        if (fixedRows()) {
            return {};
        }
        GridController* const self = this;
        std::vector<Key> doomed = _rt->untracked([&] { return selectedRows(); });
        if (doomed.size() < 2 || !std::ranges::contains(doomed, row)) {
            doomed = {row};
        }
        std::size_t const count = doomed.size();
        auto const insertEntry = [self, row](std::string label, bool below) {
            MenuItem item;
            item.label = std::move(label);
            item.enabled = Prop<bool>{[self] { return self->rowOps().insert; }};
            item.description = Prop<std::string>{[self] { return self->insertRefusal(); }};
            item.onSelect = [self, row, below] { self->insertAt(row, below); };
            return item;
        };
        MenuItem removal;
        removal.label = std::string{count > 1 ? "Delete rows" : "Delete row"};
        removal.enabled = Prop<bool>{[self, count] { return self->removeRefusal(count).empty(); }};
        removal.description = Prop<std::string>{[self, count] { return self->removeRefusal(count); }};
        removal.onSelect = [self, doomed = std::move(doomed)] { self->removeKeys(doomed); };
        std::vector<MenuItem> entries;
        entries.push_back(insertEntry("Insert row above", false));
        entries.push_back(insertEntry("Insert row below", true));
        entries.push_back(std::move(removal));
        return entries;
    }

    /// @brief The cell selection. Tracked.
    /// @return The range by its corners; none while no cell range is selected.
    [[nodiscard]] std::optional<CellRange> selection() const { return _selection.get(); }

    /// @brief Copies the summary rows a rectangle spans with the data rows (`copy`): the rows the grid is mounted over,
    ///        and a summary cell's text.
    /// @param rows The summary rows over this grid's rows; null copies data rows only.
    /// @param text A summary row's cell text, by its record and its column's id.
    void copySummaries(std::shared_ptr<SummaryRows> rows,
                       std::function<std::string(Value const& record, std::string const& column)> text) {
        _summaries = std::move(rows);
        _summaryText = std::move(text);
    }

    /// @brief Selects a range of cells, or none; focus stays where it is.
    /// @param range The range by its corners, or nothing.
    void select(std::optional<CellRange> range) { setSelection(std::move(range)); }

    /// @brief The cells of the selection, else the focused cell, as clipboard text.
    /// @return Each cell's display text, tab-separated, a line per row in view order and the columns in layout order;
    ///         a tab or a line break inside a cell written as a space. Empty when no cell is selected or focused.
    [[nodiscard]] std::string copy() const {
        return _rt->untracked([&] {
            std::optional<Block> const cells = block();
            if (!cells) {
                return std::string{};
            }
            std::vector<std::vector<std::optional<std::string>>> rows;
            if (_summaries != nullptr && _summaryText && !cells->keys.empty()) {
                std::optional<std::size_t> const first = _source->positionOf(cells->keys.front());
                std::optional<std::size_t> const last = _source->positionOf(cells->keys.back());
                std::optional<std::int64_t> const from =
                    first ? _summaries->mounted(static_cast<std::int64_t>(*first)) : std::nullopt;
                std::optional<std::int64_t> const to =
                    last ? _summaries->mounted(static_cast<std::int64_t>(*last)) : std::nullopt;
                if (from && to) {
                    // The rectangle's rows as the table draws them: a summary row between its corners is copied with
                    // its cells' text, a heading row is not a row of cells.
                    for (std::int64_t at = *from; at <= *to; ++at) {
                        std::optional<TableRowRef> const ref = _summaries->row(at);
                        if (!ref) {
                            continue;
                        }
                        std::vector<std::optional<std::string>> line;
                        if (_summaries->summary(at)) {
                            for (std::string const& column : cells->columns) {
                                line.emplace_back(_summaryText(ref->row, column));
                            }
                        } else if (_source->positionOf(ref->key)) {
                            for (std::string const& column : cells->columns) {
                                line.emplace_back(_source->text(ref->key, column));
                            }
                        } else {
                            continue;
                        }
                        rows.push_back(std::move(line));
                    }
                    return copyText({}, rows).text;
                }
            }
            for (Key const& key : cells->keys) {
                std::vector<std::optional<std::string>> line;
                for (std::string const& column : cells->columns) {
                    line.emplace_back(_source->text(key, column));
                }
                rows.push_back(std::move(line));
            }
            return copyText({}, rows).text;
        });
    }

    /// @brief Why a copy of the selection, else of the focused cell, is refused: it spans more than
    ///        `GridOptions::maxCopyRows` rows.
    /// @param separator The grouping separator the bound is written with.
    /// @return `A copy holds at most {maxCopyRows} rows`; nothing when the copy may go ahead.
    [[nodiscard]] std::optional<std::string> copyRefusal(std::string_view separator = ",") const {
        std::optional<Block> const cells = _rt->untracked([&] { return block(); });
        if (cells && cells->keys.size() > _options.maxCopyRows) {
            return tableText::copyBound(_options.maxCopyRows, separator);
        }
        return std::nullopt;
    }

    /// @brief Writes tab-separated text into the grid from the focused cell right and down, the block keeping its
    ///        shape, as one change; the status line says how many values were written and dropped.
    ///
    /// Lines split on a line feed, a trailing carriage return removed from each and a final empty line dropped; values
    /// split on tabs, each trimmed of spaces. With no focus the block starts at the first row and the first visible
    /// column, in a grid with no rows at the first row the paste adds. A line past the last row appends a row while
    /// `appendRows` allows and the record holds fewer than `maxItems`, else each of its values is dropped. A value past
    /// the last visible column, or on a read-only or covered cell, is dropped. Every other value is written as the
    /// cell's draft (`GridSource::write`), all in one batch, then `GridSource::committed` is called once. A block of
    /// more than `GridOptions::maxPasteCells` values is refused whole, and nothing is written.
    /// @param text The text.
    /// @return What the paste did with each value.
    PasteResult paste(std::string_view text) {
        commit();
        letGo();
        if (!_source->takesBlocks()) {
            return {};
        }
        std::vector<std::vector<std::string>> const lines = pasteLines(text);
        std::size_t values = 0;
        for (std::vector<std::string> const& line : lines) {
            values += line.size();
        }
        if (values > _options.maxPasteCells) {
            notice("A paste holds at most " + groupedCount(static_cast<std::int64_t>(_options.maxPasteCells)) +
                   " values");
            return {};
        }
        PasteResult result;
        std::vector<std::string> const columns = _source->columns();
        std::size_t const rows = _source->rowCount();
        std::size_t firstRow = 0;
        std::size_t firstColumn = 0;
        // The focused cell, else the selection's top left corner.
        std::optional<CellRef> start = _rt->untracked([&] { return focus(); });
        if (!start) {
            if (std::optional<Block> const cells = _rt->untracked([&] { return block(); });
                cells && !cells->keys.empty() && !cells->columns.empty()) {
                start = CellRef{.key = cells->keys.front(), .column = cells->columns.front()};
            }
        }
        if (start) {
            firstRow = _source->positionOf(start->key).value_or(0);
            auto const at = std::ranges::find(columns, start->column);
            firstColumn = at == columns.end() ? 0 : static_cast<std::size_t>(at - columns.begin());
        }
        // The keys each line lands on: the rows there, then the rows appended for the lines past the last.
        std::vector<std::optional<Key>> keys;
        bool appended = false;
        for (std::size_t line = 0; line < lines.size(); ++line) {
            std::size_t const viewRow = firstRow + line;
            if (viewRow < rows) {
                keys.push_back(_source->keyAt(viewRow));
                continue;
            }
            std::optional<Key> added;
            if (_options.appendRows && !fixedRows() && _rt->untracked([&] { return insertRefusal(); }).empty()) {
                added = _source->appendRow();
            }
            appended = appended || added.has_value();
            keys.push_back(std::move(added));
        }
        _rt->batch([&] {
            for (std::size_t line = 0; line < lines.size(); ++line) {
                std::vector<std::string> const& cells = lines.at(line);
                if (!keys.at(line)) {
                    result.pastRows += cells.size();
                    continue;
                }
                for (std::size_t at = 0; at < cells.size(); ++at) {
                    std::size_t const column = firstColumn + at;
                    if (column >= columns.size()) {
                        ++result.pastColumns;
                        continue;
                    }
                    CellRef const cell{.key = *keys.at(line), .column = columns.at(column)};
                    if (!editable(cell)) {
                        ++result.readOnly;
                        continue;
                    }
                    _source->write(cell.key, cell.column, cells.at(at));
                    ++result.written;
                }
            }
        });
        if (result.written > 0 || appended) {
            noteChange();
            _source->committed();
        }
        notice(pasteMessage(result));
        return result;
    }

    /// @brief Copies the first row of the selection, else of the focused cell, into its other rows, editable cells
    ///        only, as one change.
    void fillDown() {
        commit();
        letGo();
        if (!_source->takesBlocks()) {
            return;
        }
        std::optional<Block> const cells = _rt->untracked([&] { return block(); });
        if (!cells || cells->keys.size() < 2) {
            return;
        }
        bool changed = false;
        _rt->batch([&] {
            for (std::string const& column : cells->columns) {
                std::string const first = _source->draft(cells->keys.front(), column);
                for (std::size_t row = 1; row < cells->keys.size(); ++row) {
                    CellRef const cell{.key = cells->keys.at(row), .column = column};
                    if (editable(cell) && _source->draft(cell.key, column) != first) {
                        _source->write(cell.key, column, first);
                        changed = true;
                    }
                }
            }
        });
        if (changed) {
            noteChange();
            _source->committed();
        }
    }

    /// @brief Blanks the editable cells of the selection, else the focused cell, as one change.
    void clear() {
        commit();
        letGo();
        if (!_source->takesBlocks()) {
            return;
        }
        std::optional<Block> const cells = _rt->untracked([&] { return block(); });
        if (!cells) {
            return;
        }
        bool changed = false;
        _rt->batch([&] {
            for (Key const& key : cells->keys) {
                for (std::string const& column : cells->columns) {
                    CellRef const cell{.key = key, .column = column};
                    if (editable(cell) && !_source->draft(key, column).empty()) {
                        _source->write(key, column, std::string{});
                        changed = true;
                    }
                }
            }
        });
        if (changed) {
            noteChange();
            _source->committed();
        }
    }

    /// @brief Inserts a blank row above or below the focused row, as one change; focus stays where it is.
    /// @param below Whether the row goes below the focused one.
    void insertRow(bool below) {
        if (std::optional<CellRef> const focused = _rt->untracked([&] { return focus(); })) {
            insertAt(focused->key, below);
        }
    }

    /// @brief Deletes the rows the cell selection spans, else the focused row, as one change, at once. When one of them
    ///        held a value the status line says `Deleted {n} rows` (`Deleted 1 row`) with the entry "Undo" (`undo`).
    void deleteRows() {
        std::vector<Key> doomed = _rt->untracked([&] { return selectedRows(); });
        if (doomed.empty()) {
            if (std::optional<CellRef> const focused = _rt->untracked([&] { return focus(); })) {
                doomed.push_back(focused->key);
            }
        }
        removeKeys(doomed);
    }

    /// @brief Puts back the rows the last deletion removed, each where it stood, as one change, while the offer to undo
    ///        it stands: until the grid's next change, or `GridOptions::undoHold` after the deletion, whichever comes
    ///        first. Once the offer ends it does nothing.
    void undo() {
        commit();
        letGo();
        if (!_undoOffered) {
            return;
        }
        std::size_t const before = _rt->untracked([&] { return _source->heldRows(); });
        _source->undoRemove();
        clearUndo();
        if (_rt->untracked([&] { return _source->heldRows(); }) != before) {
            _source->committed();
        }
    }

    /// @brief Which row operations the grid offers now. Tracked.
    /// @return Insert and delete of one row, with the bound that stops one; neither while the rows are fixed.
    [[nodiscard]] RowOps rowOps() const {
        if (fixedRows()) {
            return {};
        }
        std::string const insert = insertRefusal();
        std::string const remove = removeRefusal(1);
        bool const anyRow = _source->heldRows() > 0;
        return RowOps{.insert = insert.empty(), .remove = remove.empty() && anyRow, .why = !insert.empty() ? insert : remove};
    }

    /// @brief Appends a row and focuses its first editable cell, as one change: what "Add row" under the grid does.
    void addRow() {
        commit();
        letGo();
        if (fixedRows()) {
            return;
        }
        if (std::optional<Key> const key = _source->appendRow()) {
            noteChange();
            _source->committed();
            focusFirstEditable(*key);
        }
    }

    /// @brief Calls @p observer with the cell selection after each change of it, whatever made it.
    /// @param observer The observer; empty for none.
    void onSelectionChange(std::function<void(std::optional<CellRange> const&)> observer) {
        _selectionObserver = std::move(observer);
    }

    /// @brief Gives the grid the clipboard `Ctrl+V` reads.
    /// @param read Reads the clipboard's text; nothing when it holds none.
    void useClipboard(std::function<std::optional<std::string>()> read) { _readClipboard = std::move(read); }

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
        if (base && base->cellView) {
            made = *base;
            made.cellView = [inner = base->cellView, idOf, marksOf](std::size_t column,
                                                                    reactive::Signal<Value> const& row) {
                return markedCell(inner(column, row), marksOf(&row, idOf(column)));
            };
        } else {
            if (base) {
                made = *base;
            } else {
                made.rows = std::shared_ptr<TableRows>(keep, &source->view());
                made.rowView = {};
                made.alwaysFetched = {};
                made.levels = {};
            }
            made.cellView = [source, idOf](std::size_t column, reactive::Signal<Value> const& row) {
                return source->cellView(idOf(column), row);
            };
        }
        if (source->spanning()) {
            // A grid's rows are all on the client: under the threshold every row is realized, and the spans drawn.
            made.alwaysWindowed = false;
        }
        made.rowView = [inner = made.rowView, source, ids](reactive::Signal<Value> const& row) {
            TableRowLook look = inner ? inner(row) : TableRowLook{};
            reactive::Signal<Value> const* const record = &row;
            look.modified = Prop<bool>{[source, record] {
                std::optional<Key> const key = source->keyOf(record->get());
                return key.has_value() && source->rowModified(*key);
            }};
            if (source->spanning() && !look.spans) {
                look.spans = Prop<std::vector<RowSpan>>{[source, record, ids] {
                    std::vector<RowSpan> out;
                    std::optional<Key> const key = source->keyOf(record->get());
                    if (!key) {
                        return out;
                    }
                    for (CellSpan const& span : source->spans(*key)) {
                        auto const at = std::ranges::find(*ids, span.column);
                        if (at != ids->end()) {
                            out.push_back(RowSpan{.column = static_cast<std::size_t>(at - ids->begin()),
                                                  .rows = span.rows,
                                                  .columns = span.columns});
                        }
                    }
                    return out;
                }};
            }
            return look;
        };
        // Over summary rows the mount counts mounted rows and the grid data rows: a focus's view row is mapped
        // between the two, and a click, a move or a rectangle's corner that lands on a summary or heading row goes to
        // the nearest data row.
        auto const summaries = std::dynamic_pointer_cast<SummaryRows>(made.rows);
        options.gridFocus = Prop<std::optional<GridFocus>>{[self, summaries] {
            std::optional<GridFocus> shown = self->place();
            if (shown && summaries != nullptr) {
                if (std::optional<std::int64_t> const mounted =
                        summaries->mounted(static_cast<std::int64_t>(shown->viewRow))) {
                    shown->viewRow = static_cast<std::size_t>(*mounted);
                }
            }
            return shown;
        }};
        auto const toData = [source, summaries](Key const& key) -> std::optional<Key> {
            if (summaries == nullptr || source->positionOf(key)) {
                return key;
            }
            std::int64_t const count = summaries->mountedTotal().rows;
            for (std::int64_t at = 0; at < count; ++at) {
                std::optional<TableRowRef> const ref = summaries->row(at);
                if (ref && ref->key == key) {
                    std::optional<std::int64_t> const data = summaries->data(at);
                    return data ? source->keyAt(static_cast<std::size_t>(*data)) : std::nullopt;
                }
            }
            return std::nullopt;
        };
        // The table's `onFocusCell` hears where a click or a move put the grid's focus, as a table's does.
        options.onGridFocus = [self, keep, source, summaries, toData,
                               heard = std::move(options.onFocusCell)](GridFocus target) {
            if (summaries != nullptr) {
                if (target.key) {
                    target.key = toData(*target.key);
                    if (!target.key) {
                        return;
                    }
                }
                std::optional<std::int64_t> const data = summaries->data(static_cast<std::int64_t>(target.viewRow));
                target.viewRow = data ? static_cast<std::size_t>(*data) : target.viewRow;
                if (target.key) {
                    target.viewRow = source->positionOf(*target.key).value_or(target.viewRow);
                }
            }
            self->focusPlace(std::move(target));
            if (heard) {
                heard(self->_rt->untracked([&] { return self->focus(); }));
            }
        };
        options.onKey = [self](std::string const& chord) { return self->press(chord); };
        options.keyChords = Prop<std::vector<std::string>>{[self] { return self->keyChords(); }};
        options.onText = [self](std::string text) { self->type(text); };
        options.onActivateCell = [self](CellRef cell) { self->activate(std::move(cell)); };
        options.onClickCell = [self, toData](CellRef cell) {
            if (std::optional<Key> const key = toData(cell.key)) {
                self->click(CellRef{.key = *key, .column = std::move(cell.column)});
            }
        };
        options.onFocusOut = [self] { self->leave(); };
        options.editing = Prop<std::optional<CellRef>>{[self] { return self->editing(); }};
        options.editor = [source](CellRef const& cell) { return source->editor(cell.key, cell.column); };
        options.cellSelection = Prop<std::optional<CellRange>>{[self] { return self->selection(); }};
        options.onCellSelect = [self, toData](std::optional<CellRange> range) {
            if (range) {
                std::optional<Key> const anchor = toData(range->anchor.key);
                std::optional<Key> const focus = toData(range->focus.key);
                if (!anchor || !focus) {
                    return;
                }
                range->anchor.key = *anchor;
                range->focus.key = *focus;
            }
            self->select(std::move(range));
        };
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
        if (!_options.appendRows || fixedRows()) {
            return ui::table(std::move(columns), std::move(made), std::move(options));
        }
        // "Add row" under the grid, disabled at `maxItems` with the bound as its tooltip.
        Button add;
        add.label = std::string{"Add row"};
        add.onClick = [self, keep] { self->addRow(); };
        add.common.testId = options.common.testId.empty() ? std::string{} : options.common.testId + "_addRow";
        add.common.enabled = Prop<bool>{[self] { return self->rowOps().insert; }};
        add.common.tooltip = Prop<std::string>{[self] { return self->insertRefusal(); }};
        std::vector<Node> under;
        under.push_back(ui::button(std::move(add)));
        return besideTable(options.common, std::move(under), [&] {
            return ui::table(std::move(columns), std::move(made), std::move(options));
        });
    }

    /// @brief The status line's refusal of typing on a read-only cell.
    static constexpr std::string_view kReadOnlyText = "This cell is read-only";
    /// @brief The standing state while a round of derived values failed.
    static constexpr std::string_view kDeriveFailedText = "Derived values could not be recalculated";

private:
    // The cells a selection, else the focused cell, covers: keys in view order, column ids in layout order.
    struct Block {
        std::vector<Key> keys;
        std::vector<std::string> columns;
    };

    // The selection's rectangle, else the focused cell; nothing when a corner's row is not in the view or its column
    // is not visible. Untracked reads of the source.
    [[nodiscard]] std::optional<Block> block() const {
        std::optional<CellRange> range = _selection.peek();
        if (!range) {
            std::optional<CellRef> const focused = focus();
            if (!focused) {
                return std::nullopt;
            }
            range = CellRange{.anchor = *focused, .focus = *focused};
        }
        std::vector<std::string> const columns = _source->columns();
        std::optional<std::size_t> const anchorRow = _source->positionOf(range->anchor.key);
        std::optional<std::size_t> const focusRow = _source->positionOf(range->focus.key);
        auto const anchorColumn = std::ranges::find(columns, range->anchor.column);
        auto const focusColumn = std::ranges::find(columns, range->focus.column);
        if (!anchorRow || !focusRow || anchorColumn == columns.end() || focusColumn == columns.end()) {
            return std::nullopt;
        }
        Block out;
        for (std::size_t row = std::min(*anchorRow, *focusRow); row <= std::max(*anchorRow, *focusRow); ++row) {
            if (std::optional<Key> const key = _source->keyAt(row)) {
                out.keys.push_back(*key);
            }
        }
        out.columns.assign(std::min(anchorColumn, focusColumn), std::max(anchorColumn, focusColumn) + 1);
        return out;
    }

    // Whether a cell lies in the selection's rectangle.
    [[nodiscard]] bool inBlock(CellRef const& cell) const {
        std::optional<Block> const cells = _rt->untracked([&] { return block(); });
        return cells && std::ranges::contains(cells->keys, cell.key) && std::ranges::contains(cells->columns, cell.column);
    }

    // The rows the selection spans, in view order; none without a selection.
    [[nodiscard]] std::vector<Key> selectedRows() const {
        if (!_selection.peek()) {
            return {};
        }
        std::optional<Block> const cells = block();
        return cells ? cells->keys : std::vector<Key>{};
    }

    // Shift with an arrow: the selection's focus corner moves by a row or a column from where it is, its anchor the
    // focused cell when nothing was selected.
    void extend(int rows, int columns) {
        std::optional<CellRef> const focused = _rt->untracked([&] { return focus(); });
        if (!focused) {
            return;
        }
        CellRange range = _selection.peek().value_or(CellRange{.anchor = *focused, .focus = *focused});
        std::vector<std::string> const ids = _source->columns();
        std::optional<std::size_t> const row = _source->positionOf(range.focus.key);
        auto const column = std::ranges::find(ids, range.focus.column);
        std::size_t const count = _source->rowCount();
        if (!row || column == ids.end() || count == 0) {
            return;
        }
        auto const clamp = [](std::size_t at, int by, std::size_t size) {
            auto const moved = static_cast<std::int64_t>(at) + by;
            return static_cast<std::size_t>(std::clamp<std::int64_t>(moved, 0, static_cast<std::int64_t>(size) - 1));
        };
        std::optional<Key> const key = _source->keyAt(clamp(*row, rows, count));
        if (!key) {
            return;
        }
        range.focus = CellRef{.key = *key,
                              .column = ids.at(clamp(static_cast<std::size_t>(column - ids.begin()), columns, ids.size()))};
        setSelection(std::move(range));
    }

    // Ctrl+A: every cell of the view.
    void selectAll() {
        std::vector<std::string> const ids = _source->columns();
        std::size_t const count = _source->rowCount();
        if (ids.empty() || count == 0) {
            return;
        }
        std::optional<Key> const first = _source->keyAt(0);
        std::optional<Key> const last = _source->keyAt(count - 1);
        if (first && last) {
            setSelection(CellRange{.anchor = CellRef{.key = *first, .column = ids.front()},
                                     .focus = CellRef{.key = *last, .column = ids.back()}});
        }
    }

    // A paste's lines and values: lines split on LF, a trailing CR removed from each, a final empty line dropped;
    // values split on tabs and trimmed of spaces.
    [[nodiscard]] static std::vector<std::vector<std::string>> pasteLines(std::string_view text) {
        std::vector<std::string_view> lines;
        for (std::size_t start = 0;;) {
            std::size_t const end = text.find('\n', start);
            lines.push_back(text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
            if (end == std::string_view::npos) {
                break;
            }
            start = end + 1;
        }
        for (std::string_view& line : lines) {
            if (line.ends_with('\r')) {
                line.remove_suffix(1);
            }
        }
        if (!lines.empty() && lines.back().empty()) {
            lines.pop_back();
        }
        std::vector<std::vector<std::string>> out;
        for (std::string_view const line : lines) {
            std::vector<std::string> values;
            for (std::size_t start = 0;;) {
                std::size_t const end = line.find('\t', start);
                std::string_view value =
                    line.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
                value.remove_prefix(std::min(value.find_first_not_of(' '), value.size()));
                std::size_t const last = value.find_last_not_of(' ');
                value = last == std::string_view::npos ? std::string_view{} : value.substr(0, last + 1);
                values.emplace_back(value);
                if (end == std::string_view::npos) {
                    break;
                }
                start = end + 1;
            }
            out.push_back(std::move(values));
        }
        return out;
    }

    // Why no row can be inserted: the record holds `maxItems`. Empty when one can. Tracked.
    [[nodiscard]] std::string insertRefusal() const {
        std::optional<std::size_t> const most = _source->bounds().second;
        if (most && _source->heldRows() >= *most) {
            return "A grid holds at most " + std::to_string(*most) + " rows";
        }
        return {};
    }

    // Why @p count rows cannot be deleted: fewer than `minItems` would be left. Empty when they can. Tracked.
    [[nodiscard]] std::string removeRefusal(std::size_t count) const {
        std::size_t const fewest = _source->bounds().first;
        if (_source->heldRows() < count + fewest) {
            return "A grid holds at least " + std::to_string(fewest) + " rows";
        }
        return {};
    }

    // Inserts a blank row above or below a row, as one change.
    void insertAt(Key const& row, bool below) {
        commit();
        letGo();
        if (fixedRows() || !_rt->untracked([&] { return insertRefusal(); }).empty()) {
            return;
        }
        std::optional<std::size_t> const at = _source->positionOf(row);
        if (!at) {
            return;
        }
        if (_source->insertRow(below ? *at + 1 : *at)) {
            noteChange();
            _source->committed();
        }
    }

    // Deletes rows by key, as one change; focus moves to the row now at the focused row's place, and a selection
    // whose corner is gone ends. Rows that held a value are said in the status line, with "Undo".
    void removeKeys(std::vector<Key> const& keys) {
        commit();
        letGo();
        if (fixedRows() || keys.empty() ||
            !_rt->untracked([&] { return removeRefusal(keys.size()); }).empty()) {
            return;
        }
        std::optional<GridFocus> const shown = _rt->untracked([&] { return place(); });
        std::optional<std::size_t> const at = shown && shown->key ? _source->positionOf(*shown->key) : std::nullopt;
        noteChange();
        bool const held = _source->removeRows(keys);
        _source->committed();
        if (std::optional<CellRange> const range = _selection.peek();
            range && (!_source->positionOf(range->anchor.key) || !_source->positionOf(range->focus.key))) {
            setSelection(std::nullopt);
        }
        if (shown && shown->key && !_source->positionOf(*shown->key)) {
            std::size_t const rows = _source->rowCount();
            std::optional<Key> const next =
                rows == 0 ? std::nullopt : _source->keyAt(std::min(at.value_or(0), rows - 1));
            if (next) {
                focusCell(CellRef{.key = *next, .column = shown->column});
            } else {
                setPlace(std::nullopt);
            }
        }
        if (held) {
            std::size_t const count = keys.size();
            notice("Deleted " + std::to_string(count) + (count == 1 ? " row" : " rows"),
                   {StatusEntry{.id = "undo", .label = "Undo"}});
            _undoOffered = true;
            if (_options.scheduler != nullptr) {
                _undoTimer = _options.scheduler->after(_options.undoHold, [this] { clearUndo(); });
            }
        }
    }

    // Focuses the first editable cell of a row in layout order.
    void focusFirstEditable(Key const& key) {
        for (std::string const& column : _source->columns()) {
            if (editable(CellRef{.key = key, .column = column})) {
                focusCell(CellRef{.key = key, .column = column});
                return;
            }
        }
    }

    // Selects a range, and tells the observer when it changed.
    void setSelection(std::optional<CellRange> range) {
        if (_selection.peek() == range) {
            return;
        }
        _selection.set(std::move(range));
        if (_selectionObserver) {
            std::function<void(std::optional<CellRange> const&)> const observer = _selectionObserver;
            observer(_selection.peek());
        }
    }

    // Posts a transient notice, with the choices it offers. One that offers none keeps a live offer to undo a
    // deletion: such a notice (a refusal, a paste that wrote nothing) is no change, so the offer still stands.
    void notice(std::string text, std::vector<StatusEntry> entries = {}) {
        if (entries.empty() && _undoOffered) {
            entries.push_back(StatusEntry{.id = "undo", .label = "Undo"});
        }
        _message.set(std::move(text));
        _messageEntries.set(std::move(entries));
        _notices.set(_notices.peek() + 1);
    }

    // A focus move clears a notice, but not one that offers a choice, which lasts until the next change.
    void clearNotice() {
        if (_messageEntries.peek().empty()) {
            _message.set({});
        }
    }

    // Ends the offer to undo a deletion, and a notice that offers a choice.
    void clearUndo() {
        _undoOffered = false;
        _undoTimer.cancel();
        if (!_messageEntries.peek().empty()) {
            _messageEntries.set({});
            _message.set({});
        }
    }

    // A change was made: an earlier deletion is no longer offered for undoing.
    void noteChange() { clearUndo(); }

    // Whether no row operation is offered: the rows are fixed, or not the source's to change.
    [[nodiscard]] bool fixedRows() const { return _options.rowsFixed || !_source->rowsEditable(); }

    // Moves the focus, and tells the source when the row it is on changed.
    void setPlace(std::optional<GridFocus> place) {
        std::optional<Key> const row = place ? place->key : std::nullopt;
        _place.set(std::move(place));
        if (row != _focusedRow) {
            _focusedRow = row;
            _source->focusRow(row);
        }
    }

    // Lets go of the row an editor was open on, once no editor is open: the sort and the filter place it again.
    void letGo() {
        if (!_editing.peek()) {
            _source->holdRow(std::nullopt);
        }
    }

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
            noteChange();
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
        noteChange();
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
            setPlace(GridFocus{.key = std::nullopt, .viewRow = step.row, .column = step.column});
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
        if (step.kind == Step::Kind::None && forward && from && appendOnWalk()) {
            return KeyOutcome::Handled;
        }
        if (step.kind == Step::Kind::None) {
            return forward ? KeyOutcome::FocusNext : KeyOutcome::FocusPrevious;
        }
        land(step);
        return KeyOutcome::Handled;
    }

    // Tab past the last editable cell with `appendRows`: a row is added, as one change, and focus goes to its first
    // editable cell. Nothing is added at `maxItems`, or while the rows are fixed.
    bool appendOnWalk() {
        if (!_options.appendRows || fixedRows() || !_rt->untracked([&] { return insertRefusal(); }).empty()) {
            return false;
        }
        std::optional<Key> const key = _source->appendRow();
        if (!key) {
            return false;
        }
        noteChange();
        _source->committed();
        focusFirstEditable(*key);
        return true;
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
            // Enter down from the last row adds a row with `appendRows`, focused in the same column when it is editable.
            if (*row + 1 >= rows && _options.appendRows && !fixedRows() &&
                _rt->untracked([&] { return insertRefusal(); }).empty()) {
                if (std::optional<Key> const key = _source->appendRow()) {
                    noteChange();
                    _source->committed();
                    if (editable(CellRef{.key = *key, .column = from.column})) {
                        focusCell(CellRef{.key = *key, .column = from.column});
                    } else {
                        focusFirstEditable(*key);
                    }
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
    reactive::Signal<std::optional<CellRange>> _selection;
    std::function<std::optional<std::string>()> _readClipboard;
    // The summary rows a copy draws its rows from, and a summary cell's text; empty copies data rows only.
    std::shared_ptr<SummaryRows> _summaries;
    std::function<std::string(Value const& record, std::string const& column)> _summaryText;
    std::function<void(std::optional<CellRange> const&)> _selectionObserver;
    // Whether the last deletion is offered for undoing, and the timer that ends the offer.
    bool _undoOffered = false;
    reactive::TimerHandle _undoTimer;
    // The draft the open editor's cell held when it opened.
    std::string _kept;
    // Whether the table has focus: since a click, a key or a move inside it, until focus left it.
    bool _hasFocus = false;
    // The row the source last heard focus is on.
    std::optional<Key> _focusedRow;
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
/// @param shown   The table's columns, one per grid column in its order, each with the grid column's id; empty: each
///                with the grid column's id and label alone.
/// @return The grid.
/// @throws std::invalid_argument when @p array is not a collection of @p form, a column names no member, or @p shown
///         is neither empty nor one per grid column.
[[nodiscard]] inline FormGrid makeFormGrid(forms::FormSession& form, forms::FieldState& array,
                                           std::vector<GridColumn> columns, GridOptions options, FormatContext format,
                                           TableOptions table = {}, std::vector<TableColumn> shown = {}) {
    if (shown.empty()) {
        for (GridColumn const& column : columns) {
            shown.push_back(TableColumn{.id = column.id, .label = column.label});
        }
    }
    if (shown.size() != columns.size()) {
        throw std::invalid_argument{"morph::ui::makeFormGrid: one table column per grid column"};
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
