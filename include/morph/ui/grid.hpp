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
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <morph/export.hpp>
#include <morph/table/edits.hpp>
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

#include "form_rows.hpp"
#include "mount.hpp"

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
    /// @brief The unit the row's member is entered in, an id of its `FieldSpec::units`: applied, converting the draft
    /// as the
    ///        editor's unit select does, when it first gives a unit for the row or gives another one, so a unit the
    ///        user picks afterwards stays; nothing, or an id the field does not have, after a unit restores the
    ///        field's entry unit. The first unit a loaded row's member takes, while the overlay had given none before,
    ///        counts as loaded
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
[[nodiscard]] MORPH_API std::string pasteMessage(PasteResult const& result);

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
    /// @brief The cell's value is in a unit its field cannot hold: a measure the table's units cannot convert into its
    ///        field's unit.
    bool foreignUnit = false;
};

/// @brief Whether a cell is read-only: what the walk skips, what refuses an edit, a paste and a fill, and what draws
///        on the read-only surface. Tracked through the field's `readonly()`.
/// @param facts The cell's facts.
/// @return `true` when the cell has no field or no edit, its field is read-only (`x-readonly`, `x-computed`,
///         `x-derived`, a `readonlyWhen` reaching it), or any other fact holds.
[[nodiscard]] MORPH_API bool cellReadOnly(CellFacts const& facts);

/// @brief What a grid's controller gives its source to keep the focus on its row's key. Where the focused row is in
///        the view, and the row that takes focus when it leaves, are the mount's, as for any table.
struct FocusLink {
    /// @brief A row's key changed from the first to the second: the focus and an open editor on it follow.
    std::function<void(Key const&, Key const&)> moved;
};

/// @brief What a grid edits: its rows by view index, which a model's table holds only a window of, and its cells by
///        row key and column id.
// NOLINTBEGIN(cppcoreguidelines-special-member-functions)
class MORPH_API GridSource {
public:
    GridSource() = default;
    GridSource(GridSource const&) = delete;
    GridSource& operator=(GridSource const&) = delete;
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
    [[nodiscard]] virtual std::vector<CellSpan> spans(Key const& key) const;

    /// @brief Whether any row may give spans: the mount then resolves them over every realized row.
    /// @return `false` by default.
    [[nodiscard]] virtual bool spanning() const { return false; }

    /// @brief A data row's cell content as a grid draws it, before its marks: its text, and its error as an accent.
    /// @param column The column's id.
    /// @param row    The signal holding the row's record; it must outlive the node.
    /// @return The cell's node.
    [[nodiscard]] Node cellContent(std::string const& column, reactive::Signal<Value> const& row);

    /// @brief A data row's cell as a grid draws it: `cellContent` with the cell's marks.
    /// @param column The column's id.
    /// @param row    The signal holding the row's record; it must outlive the node.
    /// @return The cell's node.
    [[nodiscard]] Node cellView(std::string const& column, reactive::Signal<Value> const& row);

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
    virtual void revertCell(Key const& key, std::string_view column);

    /// @brief Restores a row's loaded values. A source with no loaded values does nothing.
    /// @param key The row's key.
    virtual void revertRow(Key const& key) { static_cast<void>(key); }

    /// @brief Holds the row an editor is open on where it is in the view, whatever its values do to the sort and the
    ///        filter; nothing lets it go. A source whose rows never move under an edit does nothing.
    /// @param key The row's key, or nothing.
    // NOLINTNEXTLINE(performance-unnecessary-value-param): `key` is moved into the source by its overriding implementation, which takes it by value; this default only discards it
    virtual void holdRow(std::optional<Key> key) { static_cast<void>(key); }

    /// @brief A cell's editor opened: a choice in it fetches its options from now on. A source with nothing to fetch
    ///        does nothing.
    /// @param key    The row's key.
    /// @param column The column's id.
    virtual void editorOpened(Key const& key, std::string_view column);

    /// @brief Whether the derived values are the last ones loaded before a failed round, until a round succeeds: what
    /// the
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
    [[nodiscard]] virtual std::pair<std::size_t, std::optional<std::size_t>> bounds() const {
        return {0, std::nullopt};
    }

    /// @brief Appends a blank row to the record. A source whose rows are not its own adds none.
    /// @return The new row's key; nothing when no row was added.
    virtual std::optional<Key> appendRow() { return std::nullopt; }

    /// @brief Inserts a blank row before the row at a view position. A source whose rows are not its own adds none.
    /// @param position The view position; `rowCount()` appends.
    /// @return The new row's key; nothing when no row was added.
    virtual std::optional<Key> insertRow(std::size_t position);

    /// @brief Removes rows from the record in one change, kept so `undoRemove` can put them back.
    /// @param keys The rows' keys.
    /// @return `true` when one of them held a value.
    virtual bool removeRows(std::span<Key const> keys);

    /// @brief Puts the rows the last `removeRows` took back, each where it stood, in one change, while the record has
    ///        not changed since; does nothing after a change.
    virtual void undoRemove() {}

    /// @brief Why typing on a read-only cell is refused, where the source can say more than that it is read-only.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return The status line's text; nothing for the plain refusal.
    [[nodiscard]] virtual std::optional<std::string> refusal(Key const& key, std::string_view column) const;

    /// @brief Focus moved: to a row, or onto no fetched row or out of the table. A source that holds a row's edits
    ///        back until focus leaves the row commits them here.
    /// @param key The row focus is on now; nothing when it is on none.
    virtual void focusRow(std::optional<Key> const& key) { static_cast<void>(key); }

    /// @brief `Ctrl+Enter`: commits what the source holds back. A source that holds nothing does nothing.
    virtual void commitHeld() {}

    /// @brief Whether a cell's draft was committed and refused before it was sent, so committing it again sends it
    ///        even though the user did not change it.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return `false` unless a source holds such a draft.
    [[nodiscard]] virtual bool resends(Key const& key, std::string_view column) const;

    /// @brief Whether the grid selects rectangles of cells, and so takes a paste and a fill: a source whose rows are
    ///        selected as rows takes neither, and its `Shift`+arrows and `Ctrl+A` are the table's row selection.
    ///        `Delete` on the focused cell blanks it either way.
    /// @return `true` unless a source says otherwise.
    [[nodiscard]] virtual bool selectsCells() const { return true; }

    /// @brief Gives the source what follows the focused row's key when the key changes. A source whose keys never
    ///        change ignores it.
    /// @param link The controller's side; empty members when the controller goes.
    // NOLINTNEXTLINE(performance-unnecessary-value-param): `link` is moved into the source by its overriding implementation, which takes it by value; this default only discards it
    virtual void followFocusWith(FocusLink link) { static_cast<void>(link); }

    /// @brief Gives the source what moves focus to a cell while the table has focus: how a refusal that arrives after
    ///        focus left its row brings focus back. A source that never moves focus ignores it.
    /// @param refocus Focuses a cell while the table has focus; empty when the controller goes.
    // NOLINTNEXTLINE(performance-unnecessary-value-param): `refocus` is moved into the source by its overriding implementation, which takes it by value; this default only discards it
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
class MORPH_API GridRows final : public TableRows, private table::ChangeListener {
public:
    /// @brief Rows over a form's collection.
    /// @param source The collection's rows. Must not be null.
    /// @throws std::invalid_argument when @p source is null.
    explicit GridRows(std::shared_ptr<FormRowsSource> source);

    GridRows(GridRows const&) = delete;
    GridRows& operator=(GridRows const&) = delete;
    GridRows(GridRows&&) = delete;
    GridRows& operator=(GridRows&&) = delete;

    /// @brief Stops hearing the source; the engine goes first.
    ~GridRows() override;

    /// @brief The source.
    /// @return It.
    [[nodiscard]] FormRowsSource& source() const noexcept { return *_source; }

    /// @brief Takes in the field's rows as they are now, and the engine the change at once, so the view read next is
    ///        the field's.
    void sync();

    /// @brief Every row of the view, counted exactly.
    /// @return The total.
    [[nodiscard]] TableTotal total() const override;

    /// @brief The row at a view index.
    /// @param index The view index.
    /// @return The row; nothing past the view.
    [[nodiscard]] std::optional<TableRowRef> row(std::int64_t index) const override;

    /// @brief No level: every cell of a form is here.
    /// @param index Unused.
    /// @return Nothing unloaded.
    [[nodiscard]] table::UnloadedLevels unloadedLevels(std::int64_t index) const override;

    /// @brief Does nothing: every row is present.
    /// @param first   Unused.
    /// @param count   Unused.
    /// @param columns Unused.
    void visible(std::int64_t first, std::int64_t count, std::span<std::string const> columns) override;

    /// @brief The keys of a range of view rows, at once.
    /// @param first The first view row.
    /// @param count The rows.
    /// @return The keys; `InvalidSpec` for a range outside the view.
    async::Completion<std::expected<std::vector<table::RowId>, table::TableError>> keysOf(std::int64_t first,
                                                                                          std::int64_t count) override;

    /// @brief Finds a key's view index.
    /// @param key  The row's key.
    /// @param stop Once a stop is requested, @p done is not called.
    /// @param done Called on the owner with the index, or nothing when the view lacks the key.
    void locateRow(table::RowId key, ::core::async::StopToken stop,
                   std::function<void(RowLocation const&)> done) override;

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
    std::expected<void, table::TableError> setSort(SortChain sort) override;

    /// @brief Sets the filter: the rows added since the last sort or filter change join the view or leave it.
    /// @param filter The filter.
    /// @return The engine's error, or nothing.
    std::expected<void, table::TableError> setFilter(table::FilterSpec filter) override;

    /// @brief A column's distinct values, from the engine.
    /// @param request The request.
    /// @return The engine's answer.
    async::Completion<std::expected<table::DistinctValues, table::TableError>> distinctValues(
        table::DistinctRequest request) override {
        return _engine->distinctValues(std::move(request));
    }

    /// @brief The counts: the view's rows, and the engine's source and pending state.
    /// @return The counts.
    [[nodiscard]] TableCountsView counts() const override;

    /// @brief No row of a form is busy or failed.
    /// @return The empty states.
    [[nodiscard]] RowStates& states() override { return _states; }

    /// @brief No row of a form is busy or failed.
    /// @return The empty states.
    [[nodiscard]] RowStates const& states() const override { return _states; }

    /// @brief Does nothing: a form row's key never changes.
    /// @param from Unused.
    /// @param target   Unused.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): (`from`, `target`): the old key first, as every `moveKey` caller and override reads it
    void moveKey(table::RowId const& from, table::RowId const& target) override;

    /// @brief Sets the one receiver of the view's changes.
    /// @param handler The receiver; empty to stop receiving.
    void onChange(std::function<void(table::ViewChange const&)> handler) override { _onChange = std::move(handler); }

    /// @brief Holds a row where it is in the view, and in it, whatever its values do to the sort and the filter, until
    ///        another row is held or none: the row an editor is open on stays under the editor. Letting it go puts it
    ///        where the sort and the filter place it.
    /// @param key The row to hold; nothing lets the held row go.
    void hold(std::optional<table::RowId> key);

    /// @brief Whether a sort orders the view.
    /// @return `true` while the sort is not empty.
    [[nodiscard]] bool sorted() const noexcept { return _sorted; }

    /// @brief Groups the rows (`FormRowsSource::setGroup`) and takes the change in at once, so a sort that names
    ///        `kGroupRunColumn` is accepted from the next call.
    /// @param groupBy A row's group value; empty: no groups.
    void setGroup(std::function<Value(forms::RowState const&)> groupBy);

    /// @brief The row `hold` holds.
    /// @return Its key; nothing while none is held.
    [[nodiscard]] std::optional<table::RowId> const& held() const noexcept { return _held; }

    /// @brief Where a key is in the view.
    /// @param key The key.
    /// @return Its view index, or nothing.
    [[nodiscard]] std::optional<std::size_t> positionOf(table::RowId const& key) const;

private:
    // A view row: its key, and its place in the engine's view, if it has one.
    struct Item {
        std::optional<std::size_t> engine;
        table::RowId key;
    };

    [[nodiscard]] static std::shared_ptr<FormRowsSource> checked(std::shared_ptr<FormRowsSource> source);

    // A row added while the view is ordered is shown at its end until the order next changes.
    void rowsChanged(table::RowChange const& change) override;

    // The view's keys, in order.
    [[nodiscard]] std::vector<table::RowId> keys() const;

    // The view: the engine's rows but the added ones and the held one, then the added ones still in the source, and
    // the held one at its place.
    void rebuild();

    // The record of an added row the filter rejects, built from the source's snapshot and kept until it changes.
    [[nodiscard]] RowRecord const* heldRecord(table::RowId const& key) const;

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
[[nodiscard]] MORPH_API ValueList summaryScopeRows(TableRows const& rows, std::int64_t first, std::int64_t count);

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
class MORPH_API SummaryRows final : public TableRows {
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
                Runs runs);

    SummaryRows(SummaryRows const&) = delete;
    SummaryRows& operator=(SummaryRows const&) = delete;
    SummaryRows(SummaryRows&&) = delete;
    SummaryRows& operator=(SummaryRows&&) = delete;

    /// @brief Gives the data rows' change slot up.
    ~SummaryRows() override { _data->onChange({}); }

    /// @brief Draws a heading row before each run, from now on.
    /// @param heading A run's heading row by its index among the runs: its key and record; empty draws none.
    void setHeadings(std::function<RowRecord(std::size_t run)> heading);

    /// @brief Takes the runs and the data rows in again, as a change of the data rows would, and tells the mount.
    void relayout();

    /// @brief The data rows.
    /// @return They.
    [[nodiscard]] TableRows& data() const noexcept { return *_data; }

    /// @brief The data rows' total: a summary row is outside the count.
    /// @return It.
    [[nodiscard]] TableTotal total() const override { return _data->total(); }

    /// @brief Every row the mount realizes: data, summary and heading rows.
    /// @return The total.
    [[nodiscard]] TableTotal mountedTotal() const override;

    /// @brief The row at a mounted index.
    /// @param index The index.
    /// @return A data row as the data rows give it, a summary or heading row as built; nothing past the rows.
    [[nodiscard]] std::optional<TableRowRef> row(std::int64_t index) const override;

    /// @brief The mounted index of a data row.
    /// @param dataRow The data row's view index.
    /// @return Its mounted index; nothing past the data rows.
    [[nodiscard]] std::optional<std::int64_t> mounted(std::int64_t dataRow) const;

    /// @brief The data row at a mounted index, or the nearest one to a summary or heading row (the one above on a
    ///        tie).
    /// @param mountedRow The mounted index.
    /// @return The data row's view index; nothing with no data row or past the rows.
    [[nodiscard]] std::optional<std::int64_t> data(std::int64_t mountedRow) const;

    /// @brief Whether a mounted row is a summary row.
    /// @param mountedRow The mounted index.
    /// @return `true` for a summary row.
    [[nodiscard]] bool summary(std::int64_t mountedRow) const;

    /// @brief The data rows' levels of a data row; nothing for a summary or heading row.
    /// @param index The mounted index.
    /// @return The levels.
    [[nodiscard]] table::UnloadedLevels unloadedLevels(std::int64_t index) const override;

    /// @brief Tells the data rows the data rows of a mounted range.
    /// @param first   The first mounted row.
    /// @param count   The rows.
    /// @param columns The columns.
    void visible(std::int64_t first, std::int64_t count, std::span<std::string const> columns) override;

    /// @brief The keys of the data rows of a mounted range: summary and heading rows are outside the selection.
    /// @param first The first mounted row.
    /// @param count The rows.
    /// @return The data rows' answer.
    async::Completion<std::expected<std::vector<table::RowId>, table::TableError>> keysOf(std::int64_t first,
                                                                                          std::int64_t count) override;

    /// @brief Finds a key's mounted index.
    /// @param key  The key.
    /// @param stop Once a stop is requested, @p done is not called.
    /// @param done Called with the mounted index, or nothing.
    void locateRow(table::RowId key, ::core::async::StopToken stop,
                   std::function<void(RowLocation const&)> done) override;

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
        std::int64_t first, std::int64_t count, std::span<std::string const> columns) override;

    /// @brief The data rows' states.
    /// @return They.
    [[nodiscard]] RowStates& states() override { return _data->states(); }

    /// @brief The data rows' states.
    /// @return They.
    [[nodiscard]] RowStates const& states() const override { return _data->states(); }

    /// @brief Moves a data row's key.
    /// @param from The old key.
    /// @param target   The new key.
    void moveKey(table::RowId const& from, table::RowId const& target) override { _data->moveKey(from, target); }

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

    [[nodiscard]] static std::shared_ptr<TableRows> checked(std::shared_ptr<TableRows> data);

    // The data row exactly at a mounted index; nothing for a summary or heading row.
    [[nodiscard]] std::optional<std::int64_t> exact(std::int64_t index) const;

    // The data rows of a mounted range: contiguous in the data rows' view, since summary and heading rows only
    // interleave them. A range of summary rows alone is the empty range at the data row it stands before.
    [[nodiscard]] std::pair<std::int64_t, std::int64_t> dataRange(std::int64_t first, std::int64_t count) const;

    // A summary row of @p spec over the data rows [first, first + count).
    [[nodiscard]] Entry summaryEntry(SummarySpec const& spec, std::optional<std::size_t> run, std::int64_t first,
                                     std::int64_t count) const;

    // The mounted rows, from the data rows' count and runs now.
    void rebuild();

    // Whether the rows hold a row per run: a heading or a group's summary.
    [[nodiscard]] static bool perRun(std::vector<Entry> const& entries) {
        return std::ranges::any_of(entries, &Entry::perRun);
    }

    // The data rows changed: the layout follows, and the mount hears the change in mounted indices, with every summary
    // row changed after it. A change whose rows a run's rows move among is a reset.
    void heard(table::ViewChange const& change);

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
[[nodiscard]] MORPH_API FormatContext formatOf(forms::FormSession const& form);

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
MORPH_API void applyUnitOverlay(std::unordered_map<std::string, std::optional<std::string>>& applied,
                                forms::FieldState& state, std::optional<std::string> const& unit,
                                std::vector<std::pair<forms::FieldState*, std::size_t>>& asLoaded);

/// @brief A field's cell text as a grid shows it. Tracked.
/// @param state  The field.
/// @param column The column's id.
/// @param format How values are shown.
/// @return The draft while blank or not encoding, else the value at the places the field takes: its `places()`, its
///         `x-decimalPlaces`, its display places, or none for an integer.
[[nodiscard]] MORPH_API std::string fieldText(forms::FieldState& state, std::string_view column,
                                              FormatContext const& format);

}  // namespace detail

/// @brief A grid of a form's `ObjectArray` field: its rows are the field's, and each cell is a member's field.
///
/// Every row of a form is fetched, so `keyAt` is never empty below `rowCount`. A cell's text is its draft while blank
/// or not encoding, else its value shown at the places its field takes.
class MORPH_API FormGridSource final : public GridSource {
public:
    /// @brief A grid of a form's collection.
    /// @param form      The form. Borrowed: it must outlive the source.
    /// @param arrayPath The collection's path.
    /// @param columns   The columns, each id once.
    /// @param format    How values are shown.
    /// @throws std::out_of_range when no field has @p arrayPath.
    /// @throws std::invalid_argument when the field is not a collection, or a column's `field` names no member.
    FormGridSource(forms::FormSession& form, std::string arrayPath, std::vector<GridColumn> columns,
                   FormatContext format);

    FormGridSource(FormGridSource const&) = delete;
    FormGridSource& operator=(FormGridSource const&) = delete;
    FormGridSource(FormGridSource&&) = delete;
    FormGridSource& operator=(FormGridSource&&) = delete;

    /// @brief Removes the commit `onBeforeSubmit` registered.
    ~FormGridSource() override;

    /// @brief The rows of the view; the field's rows are taken in first.
    /// @return The count.
    [[nodiscard]] std::size_t rowCount() const override;

    /// @brief The key at a view position; the field's rows are taken in first.
    /// @param viewRow The position.
    /// @return The key; empty past the view.
    [[nodiscard]] std::optional<Key> keyAt(std::size_t viewRow) const override;

    /// @brief Where a key is in the view; the field's rows are taken in first.
    /// @param key The key.
    /// @return The position, or nothing.
    [[nodiscard]] std::optional<std::size_t> positionOf(Key const& key) const override;

    /// @brief The visible columns in layout order.
    /// @return The ids, as `setLayout` gave them; every column in declared order before it.
    [[nodiscard]] std::vector<std::string> columns() const override { return _layout; }

    /// @brief Shows the cells in another formatting from now on: a mounted cell's text follows.
    /// @param format How values are shown.
    void setFormat(FormatContext format);

    /// @brief Sets the visible columns in layout order: what the walk follows.
    /// @param visible The column ids.
    void setLayout(std::vector<std::string> visible);

    /// @brief A cell's field: the member its column names, or the one `GridColumn::fieldOf` names for the row.
    ///        Tracked through `fieldOf`.
    /// @param row    The row's key.
    /// @param column The column's id.
    /// @return The field; null for a display column, an unknown column, a row the field does not hold, or a name
    ///         `fieldOf` gives that is no member.
    [[nodiscard]] forms::FieldState* field(Key const& row, std::string_view column) const;

    /// @brief A row by its key.
    /// @param row The row's key.
    /// @return The row; null for one the field does not hold.
    [[nodiscard]] forms::RowState* rowOf(Key const& row) const { return _rowsSource->row(row); }

    /// @brief The forms control of the cell's field.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return The editor; a muted text for a cell with no field.
    [[nodiscard]] Node editor(Key const& key, std::string_view column) override;

    /// @brief Whether the cell's field is a Boolean.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return `true` for a Boolean.
    [[nodiscard]] bool toggles(Key const& key, std::string_view column) const override;

    /// @brief `cellReadOnly` over the cell's field. Tracked.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return `true` when read-only.
    [[nodiscard]] bool readOnly(Key const& key, std::string_view column) const override;

    /// @brief Whether another row's span covers a cell: the rows below the spanning cell's, up to its `rows`, and
    ///        the columns `cutColumns` gives it over the layout. Spans apply only while the grid is unsorted and under
    ///        its windowing thresholds (`Virtualization`'s defaults). Untracked.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return `true` when covered; the spanning cell itself is not.
    [[nodiscard]] bool covered(Key const& key, std::string_view column) const override;

    /// @brief The spans a row gives while spans apply, each cut over the layout.
    /// @param key The row's key.
    /// @return The spans; none while the grid is sorted or past its thresholds.
    [[nodiscard]] std::vector<CellSpan> spans(Key const& key) const override;

    /// @brief Whether `setSpans` gave spans.
    /// @return `true` once it did.
    [[nodiscard]] bool spanning() const override { return static_cast<bool>(_spansOf); }

    /// @brief Gives the rows' spans: each row's cells that cover the cells below and beside them.
    /// @param spansOf A row's spans, read untracked; empty: no spans.
    void setSpans(std::function<std::vector<CellSpan>(forms::RowState const&)> spansOf);

    /// @brief The cell's draft. Untracked.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return The draft.
    [[nodiscard]] std::string draft(Key const& key, std::string_view column) const override;

    /// @brief The cell's text: a display column's value, a draft while blank or not encoding, else the value at the
    ///        field's places. Tracked.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return The text.
    [[nodiscard]] std::string text(Key const& key, std::string_view column) const override;

    /// @brief Why the cell's value is refused: its draft does not encode, or the last submission refused it. Tracked.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return The forms error's message, else the refusal's; empty for none.
    [[nodiscard]] std::optional<std::string> error(Key const& key, std::string_view column) const override;

    /// @brief Writes a user's draft into the cell's field.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @param draft  The draft.
    void write(Key const& key, std::string_view column, std::string draft) override;

    /// @brief An edit is meant: the form's derived values follow (`forms::FormSession::noteCommit`), then the observer
    ///        `onCommitted` gave is told.
    void committed() override;

    /// @brief A cell's editor opened: its field fetches its options from now on (`forms::FieldState::openOptions`).
    /// @param key    The row's key.
    /// @param column The column's id.
    void editorOpened(Key const& key, std::string_view column) override;

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
    [[nodiscard]] std::pair<std::size_t, std::optional<std::size_t>> bounds() const override;

    /// @brief Appends a blank row to the collection (`forms::FieldState::addRow`), below `maxItems`.
    /// @return The new row's key; nothing at `maxItems`.
    std::optional<Key> appendRow() override;

    /// @brief Inserts a blank row into the collection before the row at a view position, below `maxItems`: at that
    ///        row's place in the record, which the view shows there while it is neither sorted nor filtered.
    /// @param position The view position; at or past `rowCount()` the row is appended.
    /// @return The new row's key; nothing at `maxItems`.
    std::optional<Key> insertRow(std::size_t position) override;

    /// @brief Removes rows from the collection in one change (`forms::FieldState::removeRows`), kept until the next
    ///        removal for `undoRemove`.
    /// @param keys The rows' keys.
    /// @return `true` when one of them was not blank.
    bool removeRows(std::span<Key const> keys) override;

    /// @brief Puts the rows the last `removeRows` took back where they stood (`forms::FieldState::restoreRows`), while
    ///        the collection holds what it held after the removal; after any change it does nothing.
    void undoRemove() override;

    /// @brief Calls @p observer at the end of each `committed`: a test seam.
    /// @param observer The observer; empty for none.
    void onCommitted(std::function<void()> observer) { _committed = std::move(observer); }

    /// @brief The cell's marks: read-only, modified, and not encoding; a computed or derived member's cell muted while
    ///        a round of derived values is in flight, and stale from a failed round until a round succeeds. Tracked.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return The marks.
    [[nodiscard]] CellMarks marks(Key const& key, std::string_view column) const override;

    /// @brief The key a grid's record holds in `#key`.
    /// @param record The record.
    /// @return The key; empty for a record without one.
    [[nodiscard]] std::optional<Key> keyOf(Value const& record) const override;

    /// @brief `forms::FormSession::rowModified` of the row. Tracked.
    /// @param key The row's key.
    /// @return `true` when modified.
    [[nodiscard]] bool rowModified(Key const& key) const override;

    /// @brief The grid's rows.
    /// @return They.
    [[nodiscard]] GridRows& view() override { return *_rows; }

    /// @brief Holds the row an editor is open on in the view (`GridRows::hold`).
    /// @param key The row's key, or nothing to let it go.
    void holdRow(std::optional<Key> key) override { _rows->hold(std::move(key)); }

    /// @brief Restores the cell's loaded value.
    /// @param key    The row's key.
    /// @param column The column's id.
    void revertCell(Key const& key, std::string_view column) override;

    /// @brief Restores the row's loaded values; a row added since the load is removed.
    /// @param key The row's key.
    void revertRow(Key const& key) override;

    /// @brief Registers @p commit with the form's submit for the source's life, so the form's submit ends an open edit
    ///        first.
    /// @param commit The commit.
    void onBeforeSubmit(std::function<void()> commit);

    /// @brief How many cells of the grid's columns are modified. Tracked.
    /// @return The count.
    [[nodiscard]] std::size_t modifiedCount() const {
        return countCells([this](forms::FieldState const& state) { return _form->modified(state.path()); });
    }

    /// @brief How many rows are modified. Tracked.
    /// @return The count.
    [[nodiscard]] std::size_t modifiedRows() const;

    /// @brief How many cells of the grid's columns do not encode, or were refused. Tracked.
    /// @return The count.
    [[nodiscard]] std::size_t errorCount() const;

    /// @brief The grid's columns.
    /// @return They, as given.
    [[nodiscard]] std::vector<GridColumn> const& gridColumns() const noexcept { return _columns; }

    /// @brief A cell as its column's filter reads it: the cell of the column's kind and the column. Tracked.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return The cell and its column, the column's id that of the rows' column; nothing for a display column or a
    ///         row the field does not hold.
    [[nodiscard]] std::optional<std::pair<table::Cell, table::TableColumn>> filterCell(Key const& key,
                                                                                       std::string_view column) const;

private:
    // Whether the collection holds `maxItems` rows already.
    [[nodiscard]] bool atMost() const;

    [[nodiscard]] GridColumn const* columnOf(std::string_view columnId) const;

    // A column with an edit: a member it names, or one per row.
    [[nodiscard]] static bool edits(GridColumn const& column) { return column.fieldOf || !column.field.empty(); }

    // The member a column edits in a row; null for none.
    [[nodiscard]] static forms::FieldState* memberOf(GridColumn const& column, forms::RowState const& row);

    // The columns' overlays per row, onto each row's member: its places, its entry unit, its options and its
    // placeholder. Rerun when a row, an overlay's reads or the member a row's column edits changes.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): applies each column's overlay to each row's member in one pass over the rows; the places, entry unit, options and placeholder steps read the same row state
    void applyOverlays();

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
                count += state != nullptr && test(*state) ? 1U : 0U;
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
    [[nodiscard]] Coverage const& coverageNow() const;

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
    /// @brief Whether the cell is read-only beyond its field (`CellFacts::overlayReadOnly`), read with the block's
    /// row,
    ///        null outside a block. Tracked.
    std::function<bool(forms::RowState const*)> readonly;
    /// @brief The entry places of the cell's field, which are also its wire `dp`, read with the block's row, null
    ///        outside a block; nothing keeps the field's. Tracked.
    std::function<std::optional<std::uint32_t>(forms::RowState const*)> decimals;
    /// @brief The unit the cell's field is entered in, an id of its `FieldSpec::units`, read with the block's row,
    /// null
    ///        outside a block; applied as a grid column's `unit` overlay is (`GridColumn::unit`). Tracked.
    std::function<std::optional<std::string>(forms::RowState const*)> unit;
    /// @brief The options the cell's field offers, as their values' JSON literals, narrowing a choice's, read with the
    ///        block's row, null outside a block; nothing offers every option. Tracked.
    std::function<std::optional<std::vector<std::string>>(forms::RowState const*)> options;
    /// @brief The cell's field's placeholder, read with the block's row, null outside a block; empty keeps the
    /// field's.
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
/// row's is the collection row's key and the part's joined, `"<row key>/<key>"`, so it stays the same while rows of
/// the collection are added and removed. The rows are in the parts' order, a block's in its collection's order, and
/// are never sorted, filtered or rearranged; the columns may be hidden and resized. Spans always apply. A cell with no
/// field is read-only.
class MORPH_API FixedGridSource final : public GridSource {
public:
    /// @brief A part of a fixed grid: a row, or a block of rows.
    using Part = std::variant<FixedRow, FixedBlock>;

    /// @brief A fixed grid.
    /// @param runtime The runtime its state is tracked on, and whose owner its rows are delivered on. Borrowed.
    /// @param form    The form whose fields the cells edit; null for a grid of `value` and `text` cells only.
    /// Borrowed:
    ///                it must outlive the source.
    /// @param rows    The parts, in order.
    /// @param columns The columns, each id once: only their ids and labels are read.
    /// @param format  How values are shown.
    /// @throws std::invalid_argument when a key is empty, holds a `/` or is given twice, a cell names a field with
    ///         @p form null or a field the form does not have, or a block repeats over no collection of the form.
    FixedGridSource(reactive::Runtime& runtime, forms::FormSession* form, std::vector<Part> rows,
                    std::vector<GridColumn> columns, FormatContext format);

    FixedGridSource(FixedGridSource const&) = delete;
    FixedGridSource& operator=(FixedGridSource const&) = delete;
    FixedGridSource(FixedGridSource&&) = delete;
    FixedGridSource& operator=(FixedGridSource&&) = delete;

    /// @brief Stops watching the collections and removes the commit `onBeforeSubmit` registered.
    ~FixedGridSource() override;

    /// @brief The rows; the collections' rows are taken in first.
    /// @return The count.
    [[nodiscard]] std::size_t rowCount() const override;

    /// @brief The key at a position; the collections' rows are taken in first.
    /// @param viewRow The position.
    /// @return The key; empty past the rows.
    [[nodiscard]] std::optional<Key> keyAt(std::size_t viewRow) const override;

    /// @brief Where a key's row is; the collections' rows are taken in first.
    /// @param key The key.
    /// @return The position, or nothing.
    [[nodiscard]] std::optional<std::size_t> positionOf(Key const& key) const override;

    /// @brief The visible columns in layout order.
    /// @return The ids, as `setLayout` gave them; every column in declared order before it.
    [[nodiscard]] std::vector<std::string> columns() const override { return _layout; }

    /// @brief Shows the cells in another formatting from now on: a mounted cell's text follows.
    /// @param format How values are shown.
    void setFormat(FormatContext format);

    /// @brief Sets the visible columns in layout order: what the walk follows, and what a span is cut over.
    /// @param visible The column ids.
    void setLayout(std::vector<std::string> visible);

    /// @brief A cell's field. Tracked through the collections' rows.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return The field; null for a cell with none, an unknown row or column, or a block row's field its collection
    ///         row lacks.
    [[nodiscard]] forms::FieldState* field(Key const& key, std::string_view column) const;

    /// @brief A row's label.
    /// @param key The row's key.
    /// @return Its `FixedRow::label`; empty for an unknown row.
    [[nodiscard]] std::string label(Key const& key) const;

    /// @brief The forms control of the cell's field.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return The editor; a muted text for a cell with no field.
    [[nodiscard]] Node editor(Key const& key, std::string_view column) override;

    /// @brief Whether the cell's field is a Boolean.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return `true` for a Boolean.
    [[nodiscard]] bool toggles(Key const& key, std::string_view column) const override;

    /// @brief `cellReadOnly` over the cell's field and its `readonly` overlay. Tracked.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return `true` when read-only: always for a `value` or a `text` cell.
    [[nodiscard]] bool readOnly(Key const& key, std::string_view column) const override;

    /// @brief Whether a span of another cell covers a cell: the rows below the spanning cell's, up to its `rows`, and
    ///        the columns `cutColumns` gives it over the layout. Untracked.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return `true` when covered; the spanning cell itself is not.
    [[nodiscard]] bool covered(Key const& key, std::string_view column) const override;

    /// @brief The spans a row's cells give, each cut over the layout.
    /// @param key The row's key.
    /// @return The spans.
    [[nodiscard]] std::vector<CellSpan> spans(Key const& key) const override;

    /// @brief Whether a cell spans more than itself.
    /// @return `true` when one does.
    [[nodiscard]] bool spanning() const override;

    /// @brief The cell's draft. Untracked.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return The draft; empty for a cell with no field.
    [[nodiscard]] std::string draft(Key const& key, std::string_view column) const override;

    /// @brief The cell's text: a `text` cell's text, a `value` cell's value, a field's draft while blank or not
    ///        encoding, else its value at the field's places. Tracked.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return The text.
    [[nodiscard]] std::string text(Key const& key, std::string_view column) const override;

    /// @brief Why the cell's value is refused: its draft does not encode, or the last submission refused it. Tracked.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return The forms error's message, else the refusal's; empty for none.
    [[nodiscard]] std::optional<std::string> error(Key const& key, std::string_view column) const override;

    /// @brief Writes a user's draft into the cell's field.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @param draft  The draft.
    void write(Key const& key, std::string_view column, std::string draft) override;

    /// @brief An edit is meant: the form's derived values follow (`forms::FormSession::noteCommit`).
    void committed() override;

    /// @brief A cell's editor opened: its field fetches its options from now on (`forms::FieldState::openOptions`).
    /// @param key    The row's key.
    /// @param column The column's id.
    void editorOpened(Key const& key, std::string_view column) override;

    /// @brief Whether the form's derived values are the last ones loaded before a failure. Tracked.
    /// @return `forms::FormSession::deriveFailing`; `false` with no form.
    [[nodiscard]] bool deriveFailed() const override { return _form != nullptr && _form->deriveFailing(); }

    /// @brief Makes one more round of the form's derived values (`forms::FormSession::recalculate`).
    void recalculate() override;

    /// @brief The cell's marks: read-only, modified and not encoding; a computed or derived field's cell muted while a
    ///        round of derived values is in flight, and stale from a failed round until a round succeeds. Tracked.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return The marks.
    [[nodiscard]] CellMarks marks(Key const& key, std::string_view column) const override;

    /// @brief The key a fixed grid's record holds in `#key`.
    /// @param record The record.
    /// @return The key; empty for a record without one.
    [[nodiscard]] std::optional<Key> keyOf(Value const& record) const override;

    /// @brief Whether a field cell of the row is modified (`forms::FormSession::modified`). Tracked.
    /// @param key The row's key.
    /// @return `true` when one is.
    [[nodiscard]] bool rowModified(Key const& key) const override;

    /// @brief The rows: one per row, in order, with no sort and no filter.
    /// @return They.
    [[nodiscard]] EngineRows& view() override;

    /// @brief Restores the cell's loaded value.
    /// @param key    The row's key.
    /// @param column The column's id.
    void revertCell(Key const& key, std::string_view column) override;

    /// @brief Restores the loaded values of the row's field cells.
    /// @param key The row's key.
    void revertRow(Key const& key) override;

    /// @brief Registers @p commit with the form's submit for the source's life, so the form's submit ends an open edit
    ///        first. Does nothing with no form.
    /// @param commit The commit.
    void onBeforeSubmit(std::function<void()> commit);

    /// @brief How many field cells are modified. Tracked.
    /// @return The count.
    [[nodiscard]] std::size_t modifiedCount() const {
        return countCells([this](forms::FieldState const& state) { return _form->modified(state.path()); });
    }

    /// @brief How many rows have a modified field cell. Tracked.
    /// @return The count.
    [[nodiscard]] std::size_t modifiedRows() const;

    /// @brief How many field cells do not encode, or were refused. Tracked.
    /// @return The count.
    [[nodiscard]] std::size_t errorCount() const;

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

    [[nodiscard]] static std::span<FixedRow const> rowsOf(Part const& part);

    // The field spec at a path below some fields' specs; null for none.
    [[nodiscard]] static forms::FieldSpec const* specAt(std::span<forms::FieldSpec const> fields,
                                                        std::string_view path);

    [[noreturn]] static void refuse(std::string why) {
        throw std::invalid_argument{"morph::ui::FixedGridSource: " + std::move(why)};
    }

    // Keys are given once and not empty, and every field a cell names is the form's.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): one refusal per rule of a part.
    void checkParts() const;

    // Each block's collection rows now. Tracked: a reader follows a change of a block's rows.
    [[nodiscard]] BlockRows blockRows() const;

    // The rows from the parts and the collections' rows now, given to the rows' source as one reset.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): rebuilds the rows from the parts and the collections in one pass, each part kind appending in the order the document lists them
    void expand() const;

    // Takes in the collections' rows when a block's changed, and the engine the reset at once.
    void sync() const;

    // A change of a block's rows is taken in at the next turn of the owner, so the mount hears it without a read.
    void watch();

    [[nodiscard]] Expanded const* expandedOf(Key const& key) const;

    // A cell's part and its block's collection row; null for no cell.
    [[nodiscard]] std::pair<FixedCell const*, forms::RowState const*> cellOf(Key const& key,
                                                                             std::string_view column) const;

    // Each field cell's overlays onto its field: its places, its entry unit, its options and its placeholder. Rerun
    // when a block's rows, an overlay's reads or the form's load count change. A load sets a field's unit afresh, so
    // the units the overlays gave an unmodified field before it are forgotten and given again as loaded. A switch as
    // loaded is no load and moves no load count, so no grid takes another grid's switch for a load, whatever units
    // they give.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): one section per overlay.
    void applyOverlays();

    template <typename Test>
    [[nodiscard]] std::size_t countCells(Test test) const {
        sync();
        std::size_t count = 0;
        for (Expanded const& row : _expanded) {
            for (auto const& [column, state] : row.fields) {
                count += state != nullptr && test(*state) ? 1U : 0U;
            }
        }
        return count;
    }

    // The coverage of the rows now, computed again once the rows or the layout changed.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): recomputes the coverage once from the rows and the layout; the span walk and the cache check share the running coverage
    [[nodiscard]] Coverage const& coverageNow() const;

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
class MORPH_API ModelGridSource final : public GridSource {
public:
    /// @brief A model's table as a grid.
    /// @param rows    The window's rows; the decorator `view()` owns their change slot from now on.
    /// @param edits   The table's drafts and commits. Borrowed: it must outlive the source.
    /// @param columns Every column the table shows, each with its id; an editable one with its form and field.
    /// @param mode    When a row's edits are sent.
    /// @param context What the cells are edited and shown with.
    ModelGridSource(std::shared_ptr<TableRows> rows, table::CellEdits& edits, std::vector<ModelColumn> columns,
                    EditMode mode, ModelGridContext context);

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
    void setDescription(table::TableDescription const& description);

    /// @brief Sets the visible columns in layout order: what the walk follows.
    /// @param visible The column ids.
    void setLayout(std::vector<std::string> visible);

    /// @brief Shows the cells in another formatting from now on.
    /// @param format How values are shown.
    void setFormat(FormatContext format);

    /// @brief Tells every reader of the cells that something they depend on and cannot track changed: the rows' busy
    ///        and failed states.
    void touch() { bump(); }

    /// @brief Rows a keyed fetch named and found none of: their drafts, errors and marks go
    ///        (`table::CellEdits::rowAbsent`).
    /// @param keys The rows' keys.
    void rowsAbsent(std::span<table::RowId const> keys);

    /// @brief A row's key changed: the rows, the drafts, the cells held for the row's request and the focus move
    ///        what they hold by it.
    /// @param from The row's old key.
    /// @param destination   The row's new key.
    void moveKey(Key const& from, Key const& destination);

    /// @brief A cell's draft while one is held, for a cell view that shows it in place of the row's value. Tracked.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return The draft; nothing while none is held.
    [[nodiscard]] std::optional<std::string> heldDraft(Key const& key, std::string_view column) const;

    /// @brief A cell's held draft as its cell shows it: a `Measure` column's with its field's unit, since the column's
    ///        other cells name their own. Tracked.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return The text; nothing while no draft is held.
    [[nodiscard]] std::optional<std::string> heldText(Key const& key, std::string_view column) const;

    /// @brief The view's rows, fetched or not.
    /// @return `TableRows::total().rows`. Tracked.
    [[nodiscard]] std::size_t rowCount() const override;

    /// @brief The key of the row at a view position. Tracked: it answers once the row arrives.
    /// @param viewRow The position.
    /// @return The key; empty while the row is not fetched.
    [[nodiscard]] std::optional<Key> keyAt(std::size_t viewRow) const override;

    /// @brief Where a key's row is in the view among the rows held. The index is read from the rows' held ranges, a
    ///        bounded window of a model's table, once per change of the rows.
    /// @param key The key.
    /// @return The position; empty when no held row has the key.
    [[nodiscard]] std::optional<std::size_t> positionOf(Key const& key) const override;

    /// @brief The visible columns in layout order.
    /// @return The ids.
    [[nodiscard]] std::vector<std::string> columns() const override { return _layout; }

    /// @brief The forms control of the cell's field, its session loaded with the cell's draft or the row's value.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return The editor; a muted text for a column with no edit.
    [[nodiscard]] Node editor(Key const& key, std::string_view column) override;

    /// @brief Whether the cell's field is a Boolean.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return `true` for a Boolean.
    [[nodiscard]] bool toggles(Key const& key, std::string_view column) const override;

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
    [[nodiscard]] bool covered(Key const& key, std::string_view column) const override;

    /// @brief The cell's draft: the open editor's text, else the draft held, else the row's value as its field shows
    ///        it for editing. Untracked.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return The draft; empty for a column with no edit.
    [[nodiscard]] std::string draft(Key const& key, std::string_view column) const override;

    /// @brief The cell's text: its draft while one is held, else the row's value as the column shows it. Tracked.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return The text.
    [[nodiscard]] std::string text(Key const& key, std::string_view column) const override;

    /// @brief Why the cell's value is refused: its draft does not encode, or the edit action refused it. Tracked.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return The message; empty for none.
    [[nodiscard]] std::optional<std::string> error(Key const& key, std::string_view column) const override;

    /// @brief Writes a user's draft into the cell: its field takes it, and the draft is held with the field's error.
    ///        A draft that is the row's own value again is no draft.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @param draft  The draft.
    void write(Key const& key, std::string_view column, std::string draft) override;

    /// @brief An edit is meant: each cell written since the last commit is held with its encoding and, when it
    ///        encodes and the cell takes edits, sent as its own request (`EditMode::Cell`) or kept for its row's
    ///        request (`EditMode::Row`).
    void committed() override;

    /// @brief A cell's editor opened: its session is loaded with the cell, and a choice fetches its options.
    /// @param key    The row's key.
    /// @param column The column's id.
    void editorOpened(Key const& key, std::string_view column) override;

    /// @brief The cell's marks: read-only, modified from its commit's success until its row's next fetch, refused,
    ///        and stale while its commit is out. Tracked.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return The marks.
    [[nodiscard]] CellMarks marks(Key const& key, std::string_view column) const override;

    /// @brief The key a record holds in the description's key column.
    /// @param record The record.
    /// @return The key; empty for a record without one, or before a description.
    [[nodiscard]] std::optional<Key> keyOf(Value const& record) const override;

    /// @brief Whether some cell of the row is modified or holds a draft. Tracked.
    /// @param key The row's key.
    /// @return `true` when one does.
    [[nodiscard]] bool rowModified(Key const& key) const override;

    /// @brief The decorator over the rows the mount is handed.
    /// @return It.
    [[nodiscard]] TableRows& view() override { return *_view; }

    /// @brief Why typing on a read-only cell is refused: the cell is not loaded, the row is being saved, or the row
    ///        is locked, in that order.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return The status line's text; nothing for any other read-only cell.
    [[nodiscard]] std::optional<std::string> refusal(Key const& key, std::string_view column) const override;

    /// @brief Focus moved: in `EditMode::Row` the cells held for every other row are sent, a request per row.
    /// @param key The row focus is on now; nothing when it is on none.
    void focusRow(std::optional<Key> const& key) override;

    /// @brief `Ctrl+Enter`: every row's held cells are sent.
    void commitHeld() override;

    /// @brief A model's table selects rows, not rectangles of cells: it takes no paste and no fill.
    /// @return `false`.
    [[nodiscard]] bool selectsCells() const override { return false; }

    /// @brief Keeps what moves the focus with a row's key.
    /// @param link The controller's side.
    void followFocusWith(FocusLink link) override { _focus = std::move(link); }

    /// @brief Keeps what brings focus back to the first refused cell of a row commit in `EditMode::Row`.
    /// @param refocus Focuses a cell while the table has focus.
    void refocusWith(std::function<void(CellRef const&)> refocus) override { _refocus = std::move(refocus); }

    /// @brief Whether a cell's draft was refused when its row's held cells were sent, so its next commit sends it.
    /// @param key    The row's key.
    /// @param column The column's id.
    /// @return `true` until the cell is committed again.
    [[nodiscard]] bool resends(Key const& key, std::string_view column) const override;

    /// @brief A held cell's refusal when its row became read-only for no reason the cell can name.
    static constexpr std::string_view kReadOnlyText = "This cell is read-only";
    /// @brief The status line's refusal of typing on a cell not loaded yet.
    static constexpr std::string_view kUnloadedText = "This value is not loaded yet";
    /// @brief The status line's refusal of typing on a measure in a unit its field cannot hold.
    static constexpr std::string_view kForeignUnitText = "This value's unit cannot be edited here";
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
        // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members): a view of a record the source holds for the length of the call that returns it; copying the record would defeat the lookup
        Value const& record;
    };

    // The rows the mount reads: the window's, with their one change slot set here.
    class MORPH_API Rows final : public TableRows {
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
        void locateRow(table::RowId key, ::core::async::StopToken stop,
                       std::function<void(RowLocation const&)> done) override {
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
        [[nodiscard]] std::vector<std::pair<std::int64_t, std::int64_t>> heldRanges() const override {
            return _inner->heldRanges();
        }
        void moveKey(table::RowId const& from, table::RowId const& target) override { _source->moveKey(from, target); }
        void onChange(std::function<void(table::ViewChange const&)> handler) override {
            _handler = std::move(handler);
        }

    private:
        // The rows a change brought are fetched: their modified marks end before anything reads the rows, then the
        // mount hears the change.
        void changed(table::ViewChange const& change);

        ModelGridSource* _source;
        TableRows* _inner;
        std::function<void(table::ViewChange const&)> _handler;
    };

    // The view indices, after the whole change, of the rows its `Inserted` and `Changed` operations brought.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): walks the change's operations once, shifting the collected indices as each insert or removal passes; the shift depends on the operation kind
    [[nodiscard]] static std::vector<std::size_t> broughtRows(table::ViewChange const& change);

    void bump() { _version.set(_version.peek() + 1); }

    [[nodiscard]] ModelColumn const* columnOf(std::string_view columnId) const;

    [[nodiscard]] table::TableColumn const* describedColumn(std::string_view columnId) const;

    [[nodiscard]] Session const* sessionOf(std::string_view column) const;

    [[nodiscard]] forms::FieldState* fieldOf(std::string_view column) const;

    // The held row with a key: its view index and record. Tracked through the source's version.
    [[nodiscard]] std::optional<Located> locate(Key const& key) const;

    // Every fact the cell's read-only state follows from. Tracked.
    [[nodiscard]] CellFacts factsOf(Key const& key, std::string_view column) const;

    // The column's session loaded with the cell: with the row's value, and then the draft held, unless it holds the
    // cell already. Null for a column with no edit.
    Session* loaded(Key const& key, std::string_view column);

    // Holds the field's text as the cell's draft, with the field's error; the row's own value again is no draft.
    void hold(Key const& key, std::string_view column);

    // The cell's draft, held, and its cell of a request when it encodes and the cell takes edits. A cell that became
    // read-only while it was edited is not sent: an edit of a value the user has not seen, or of a row another run is
    // saving, would overwrite what arrives.
    std::optional<table::EditCell> encode(Key const& key, std::string const& column);

    // Sends a row's held cells as one request. A cell that became read-only since it was held (its row busy with
    // another run, locked, or not loaded) is not sent: it keeps its draft, as an edit refused at the cell does.
    void send(Key const& row);

    // A row commit settled: the cells follow, and in `EditMode::Row` a refusal brings focus back to the row's first
    // refused cell in layout order.
    void rowSettled(table::RowId const& row, bool refused);

    // A record's value as its field's wire form. A `Date` cell is `YYYY-MM-DD` text and a `DateTime` cell a timestamp,
    // which a `Date` or `DateTime` field reads as it is; an `EpochDate` field reads days and an `EpochDateTime` field
    // seconds, so those are counted from the epoch. A measure is an amount in its own unit and a `Quantity` field's
    // value an amount in its canonical unit: the amount is converted through the description's units, and one the
    // units cannot convert opens blank rather than as a figure in the wrong unit.
    [[nodiscard]] std::string wireOf(Value const& value, forms::FieldSpec const& spec) const;

    // A measure's amount in its `Quantity` field's canonical unit, through the description's units; nothing when they
    // cannot convert it (a unit they lack, another dimension), so the cell is read-only.
    [[nodiscard]] std::optional<math::Rational> inFieldUnit(Quantity const& measure,
                                                            forms::FieldSpec const& spec) const;

    // A record's value as a cell of its column.
    [[nodiscard]] static table::Cell cellOf(Value const* value);

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
    FocusLink _focus;
    // Cells refused when their row's held cells were sent, which their next commit sends.
    std::vector<CellRef> _resend;
    // The description's units, through which a measure is converted into its field's unit.
    std::shared_ptr<table::UnitRegistry const> _units;
    // The held rows' view indices by key, as of a count of the rows' changes: the cells' own changes do not move rows.
    std::uint64_t _rowsVersion = 0;
    mutable std::unordered_map<Key, std::size_t> _positions;
    mutable std::optional<std::uint64_t> _positionsVersion;
    async::CallbackScope _alive;
    std::unique_ptr<Rows> _view;
};

/// @brief A grid's focus, its cell editor and its walk over a `GridSource`, and the table node that shows them.
///
/// The editor's text is the cell's draft, live: an opening keeps the draft the cell held, a commit closes the editor
/// and calls `GridSource::committed` once when the draft changed, and `Escape` writes the kept draft back. A paste, a
/// fill, a clear and each row operation is one change too: it calls `GridSource::committed` once, however many cells
/// or rows it touches.
class MORPH_API GridController : public std::enable_shared_from_this<GridController> {
public:
    /// @brief A controller over a source.
    /// @param runtime The runtime its state is tracked on. Borrowed.
    /// @param source  The source. Borrowed: it must outlive the controller.
    /// @param options The walk and the activation.
    GridController(reactive::Runtime& runtime, GridSource& source, GridOptions options);

    GridController(GridController const&) = delete;
    GridController& operator=(GridController const&) = delete;
    GridController(GridController&&) = delete;
    GridController& operator=(GridController&&) = delete;
    /// @brief Takes back what the source was given to move focus with.
    ~GridController();

    /// @brief The focused cell. Tracked.
    /// @return The cell when its row is fetched; empty when nothing has focus or focus stands on a placeholder.
    [[nodiscard]] std::optional<CellRef> focus() const;

    /// @brief The focus as the mount shows it, what `Table::gridFocus` binds. Tracked.
    /// @return `{key, view position, column}` for a focused cell, `{none, view row, column}` while the walk stopped at
    /// a
    ///         row not fetched yet, which takes its key once the source answers for it.
    [[nodiscard]] std::optional<GridFocus> place() const;

    /// @brief Takes the cell a click or a key resolved to: `focusCell` for a fetched row, else the placeholder focus.
    /// @param target The cell.
    void focusPlace(GridFocus target);

    /// @brief Focuses a cell, as a click or an arrow does; a covered cell, and a key the view does not hold, are
    ///        refused. An open editor on another cell is committed first. The status line shows the cell's error while
    ///        it has focus. A cell outside the cell selection ends it.
    /// @param cell The cell.
    void focusCell(CellRef cell);

    /// @brief What `Enter` and `F2` with no editor, and a double click, do: focuses the cell, then opens its editor
    ///        holding the value on an editable cell (none for a Boolean), or runs `GridOptions::onActivate` on a
    ///        read-only one.
    /// @param cell The cell.
    void activate(CellRef cell);

    /// @brief The cell whose editor is open. Tracked.
    /// @return The cell, or none.
    [[nodiscard]] std::optional<CellRef> editing() const { return _editing.get(); }

    /// @brief The chords the table declares, what `Table::keyChords` binds. Tracked.
    /// @return With an editor open exactly `Enter`, `Tab`, `Shift+Tab`, `Escape` and `Ctrl+Enter`, so every other key
    ///         reaches the editor; with none, also `F2`, `Space`, `Delete`, `Shift` with an arrow, `Ctrl+A`, `Ctrl+V`,
    ///         `Ctrl+D`, `Ctrl+Shift+=` and `Ctrl+-`. `Ctrl+C` is the table's own copy key, which copies `copy()`.
    [[nodiscard]] std::vector<std::string> keyChords() const;

    /// @brief A chord pressed while the table has focus.
    /// @param chord The chord.
    /// @return `FocusNext` or `FocusPrevious` when `Tab` or `Shift+Tab` runs past the last or the first editable cell,
    ///         `Ignored` for a chord the grid does not take, else `Handled`.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): dispatches one chord across the focus, editing, selection and clipboard states; each branch checks the state the one before left
    KeyOutcome press(std::string_view chord);

    /// @brief Printable text: with an editor open, the editor's whole new text, as its input writes it; on a focused
    ///        cell with none, the cell's draft is kept, @p text written as the draft and the editor opened. A
    ///        read-only cell refuses it in the status line.
    /// @param text The text.
    void type(std::string_view text);

    /// @brief Focus left the table: an open editor is committed, and the source hears that focus is on no row.
    void leave();

    /// @brief The latest transient refusal or notice, else the focused cell's error while it has one. Tracked.
    /// @return The text; empty for none.
    [[nodiscard]] std::string message() const;

    /// @brief How many refusals and notices were given: it moves on each, so one equal to the last is posted again.
    ///        Tracked.
    /// @return The count.
    [[nodiscard]] std::uint64_t notices() const { return _notices.get(); }

    /// @brief A click on a cell: focuses it, and flips a Boolean's.
    /// @param cell The cell.
    void click(CellRef const& cell);

    /// @brief The choices the latest notice offers. Tracked.
    /// @return The entries.
    [[nodiscard]] std::vector<StatusEntry> const& messageEntries() const { return _messageEntries.get(); }

    /// @brief A state that lasts while its source holds: `kDeriveFailedText` from a failed round of derived values
    /// until
    ///        a round succeeds, a retry in flight included. Tracked.
    /// @return The text; empty for none.
    [[nodiscard]] std::string standing() const;

    /// @brief The choices the standing state offers: "Recalculate" (`recalculate`) while a round of derived values
    ///        failed. Tracked.
    /// @return The entries.
    [[nodiscard]] std::vector<StatusEntry> standingEntries() const;

    /// @brief An entry chosen in the status line: `recalculate` makes one more round of derived values, `undo` puts
    ///        back the rows the last deletion removed (`undo`).
    /// @param entryId The entry's id; one the grid does not offer is ignored.
    void chooseEntry(std::string_view entryId);

    /// @brief The grid's entries of a cell's menu: "Revert cell", enabled while the cell is modified, and "Revert
    /// row",
    ///        enabled while the row is modified, each restoring the loaded value and then committing once; then,
    ///        unless the rows are fixed, a separator and the row entries (`rowEntries`).
    /// @param row    The row's key.
    /// @param column The column's id.
    /// @return The entries.
    [[nodiscard]] std::vector<MenuItem> cellEntries(Key const& row, std::string_view column);

    /// @brief The row entries of a row's menus: "Insert row above", "Insert row below" and "Delete row", or "Delete
    ///        rows" when the cell selection spans more than one row and @p row is among them, deleting every row it
    ///        spans. An insertion is disabled at `maxItems` and a deletion where it would leave fewer than `minItems`,
    ///        each with the bound as its description. Each is one change.
    /// @param row The row's key.
    /// @return The entries; none while the rows are fixed.
    [[nodiscard]] std::vector<MenuItem> rowEntries(Key const& row);

    /// @brief The cell selection. Tracked.
    /// @return The range by its corners; none while no cell range is selected.
    [[nodiscard]] std::optional<CellRange> selection() const { return _selection.get(); }

    /// @brief Copies the summary rows a rectangle spans with the data rows (`copy`): the rows the grid is mounted
    /// over,
    ///        and a summary cell's text.
    /// @param rows The summary rows over this grid's rows; null copies data rows only.
    /// @param text A summary row's cell text, by its record and its column's id.
    void copySummaries(std::shared_ptr<SummaryRows> rows,
                       std::function<std::string(Value const& record, std::string const& column)> text);

    /// @brief Selects a range of cells, or none; focus stays where it is.
    /// @param range The range by its corners, or nothing.
    void select(std::optional<CellRange> range) { setSelection(std::move(range)); }

    /// @brief The cells of the selection, else the focused cell, as clipboard text.
    /// @return Each cell's display text, tab-separated, a line per row in view order and the columns in layout order;
    ///         a tab or a line break inside a cell written as a space. Empty when no cell is selected or focused.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): builds the clipboard text for the selection or the focus in one walk over the rows and the columns in layout order
    [[nodiscard]] std::string copy() const;

    /// @brief Why a copy of the selection, else of the focused cell, is refused: it spans more than
    ///        `GridOptions::maxCopyRows` rows.
    /// @param separator The grouping separator the bound is written with.
    /// @return `A copy holds at most {maxCopyRows} rows`; nothing when the copy may go ahead.
    [[nodiscard]] std::optional<std::string> copyRefusal(std::string_view separator = ",") const;

    /// @brief Writes tab-separated text into the grid from the focused cell right and down, the block keeping its
    ///        shape, as one change; the status line says how many values were written and dropped.
    ///
    /// Lines split on a line feed, a trailing carriage return removed from each and a final empty line dropped; values
    /// split on tabs, each trimmed of spaces. With no focus the block starts at the first row and the first visible
    /// column, in a grid with no rows at the first row the paste adds. A line past the last row appends a row while
    /// `appendRows` allows and the record holds fewer than `maxItems`, else each of its values is dropped. A value
    /// past the last visible column, or on a read-only or covered cell, is dropped. Every other value is written as
    /// the cell's draft (`GridSource::write`), all in one batch, then `GridSource::committed` is called once. A block
    /// of more than `GridOptions::maxPasteCells` values is refused whole, and nothing is written.
    /// @param text The text.
    /// @return What the paste did with each value.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): parses the block, writes each value and counts the written and the dropped in one pass; both tallies depend on each cell's outcome
    PasteResult paste(std::string_view text);

    /// @brief Copies the first row of the selection, else of the focused cell, into its other rows, editable cells
    ///        only, as one change.
    void fillDown();

    /// @brief Blanks the editable cells of the selection, else the focused cell, as one change.
    void clear();

    /// @brief Inserts a blank row above or below the focused row, as one change; focus stays where it is.
    /// @param below Whether the row goes below the focused one.
    void insertRow(bool below);

    /// @brief Deletes the rows the cell selection spans, else the focused row, as one change, at once. When one of
    /// them
    ///        held a value the status line says `Deleted {n} rows` (`Deleted 1 row`) with the entry "Undo" (`undo`).
    void deleteRows();

    /// @brief Puts back the rows the last deletion removed, each where it stood, as one change, while the offer to
    /// undo
    ///        it stands: until the grid's next change, or `GridOptions::undoHold` after the deletion, whichever comes
    ///        first. Once the offer ends it does nothing.
    void undo();

    /// @brief Which row operations the grid offers now. Tracked.
    /// @return Insert and delete of one row, with the bound that stops one; neither while the rows are fixed.
    [[nodiscard]] RowOps rowOps() const;

    /// @brief Appends a row and focuses its first editable cell, as one change: what "Add row" under the grid does.
    void addRow();

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
                            std::optional<TableSource> base = std::nullopt);

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
    [[nodiscard]] std::optional<Block> block() const;

    // Whether a cell lies in the selection's rectangle.
    [[nodiscard]] bool inBlock(CellRef const& cell) const;

    // The rows the selection spans, in view order; none without a selection.
    [[nodiscard]] std::vector<Key> selectedRows() const;

    // Shift with an arrow: the selection's focus corner moves by a row or a column from where it is, its anchor the
    // focused cell when nothing was selected.
    void extend(int rows, int columns);

    // Ctrl+A: every cell of the view.
    void selectAll();

    // A paste's lines and values: lines split on LF, a trailing CR removed from each, a final empty line dropped;
    // values split on tabs and trimmed of spaces.
    [[nodiscard]] static std::vector<std::vector<std::string>> pasteLines(std::string_view text);

    // Why no row can be inserted: the record holds `maxItems`. Empty when one can. Tracked.
    [[nodiscard]] std::string insertRefusal() const;

    // Why @p count rows cannot be deleted: fewer than `minItems` would be left. Empty when they can. Tracked.
    [[nodiscard]] std::string removeRefusal(std::size_t count) const;

    // Inserts a blank row above or below a row, as one change.
    void insertAt(Key const& row, bool below);

    // Deletes rows by key, as one change; focus moves to the row now at the focused row's place, and a selection
    // whose corner is gone ends. Rows that held a value are said in the status line, with "Undo".
    void removeKeys(std::vector<Key> const& keys);

    // Focuses the first editable cell of a row in layout order.
    void focusFirstEditable(Key const& key);

    // Selects a range, and tells the observer when it changed.
    void setSelection(std::optional<CellRange> range);

    // Posts a transient notice, with the choices it offers. One that offers none keeps a live offer to undo a
    // deletion: such a notice (a refusal, a paste that wrote nothing) is no change, so the offer still stands.
    void notice(std::string text, std::vector<StatusEntry> entries = {});

    // A focus move clears a notice, but not one that offers a choice, which lasts until the next change.
    void clearNotice();

    // Ends the offer to undo a deletion, and a notice that offers a choice.
    void clearUndo();

    // A change was made: an earlier deletion is no longer offered for undoing.
    void noteChange() { clearUndo(); }

    // Whether no row operation is offered: the rows are fixed, or not the source's to change.
    [[nodiscard]] bool fixedRows() const { return _options.rowsFixed || !_source->rowsEditable(); }

    // A row's key changed: the focus and an open editor on it take the new key, at the row's place now.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): (`from`, `target`): the old key first, which the one call site (the source's move callback) spells out
    void keyMoved(Key const& from, Key const& target);

    // Moves the focus, and tells the source when the row it is on changed.
    void setPlace(std::optional<GridFocus> place);

    // Lets go of the row an editor was open on, once no editor is open: the sort and the filter place it again.
    void letGo();

    // A cell a walk may stop on: editable and not covered.
    [[nodiscard]] bool editable(CellRef const& cell) const;

    // Commits an open editor: it closes, and an edit is meant when its draft differs from the one it opened on.
    void commit();

    // Flips a Boolean's cell and commits it.
    void toggle(CellRef const& cell);

    // Where a walk lands: an editable cell, a row not fetched yet, or nowhere.
    struct Step {
        enum class Kind : std::uint8_t { Found, Stop, None } kind = Kind::None;
        std::size_t row = 0;
        std::string column;
        std::optional<Key> key;
    };

    // The next editable cell from @p from in the walk's order, by view positions at the moment of the call; a row not
    // fetched yet stops the walk.
    [[nodiscard]] Step next(std::optional<CellRef> const& from, bool forward) const;

    // Lands on where a walk stopped.
    void land(Step const& step);

    // Tab or Shift+Tab: the next editable cell, or out of the table at the ends. Focus on a row not fetched yet moves
    // nowhere until the row arrives.
    KeyOutcome walkFrom(bool forward);

    // Tab past the last editable cell with `appendRows`: a row is added, as one change, and focus goes to its first
    // editable cell. Nothing is added at `maxItems`, or while the rows are fixed.
    bool appendOnWalk();

    // Enter after a commit: down the column, along the row, or nowhere.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): the three Enter modes each walk a different axis and share the focus and the editable-cell test
    void enterMove(CellRef const& from);

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
[[nodiscard]] MORPH_API FormGrid makeFormGrid(forms::FormSession& form, forms::FieldState const& array,
                                              std::vector<GridColumn> columns, GridOptions options,
                                              FormatContext format, TableOptions table = {},
                                              std::vector<TableColumn> shown = {});

/// @brief The node of a grid of a form's collection (`makeFormGrid`), for a form's own view.
/// @param form    The form.
/// @param array   The collection field.
/// @param columns The grid's columns.
/// @param options The walk and the activation.
/// @param format  How values are shown.
/// @return The node; it keeps the grid alive.
[[nodiscard]] inline Node formGrid(forms::FormSession& form, forms::FieldState const& array,
                                   std::vector<GridColumn> columns, GridOptions options, FormatContext format) {
    return makeFormGrid(form, array, std::move(columns), std::move(options), std::move(format)).node;
}

}  // namespace morph::ui
