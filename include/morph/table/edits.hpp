// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file table/edits.hpp
/// @brief Committing edited cells: one row's edits in order, stale and error
///        state per cell, and the row patched with the server's reply.
///
/// A commit runs an injected function — the table's mutation — and hands it a
/// completion. A success carries the row as the server returned it, which
/// replaces the row through a `RowPatcher`; a failure carries the field error,
/// which the cell shows. Edits to one row run one at a time, in the order they
/// were committed, so a later edit never overtakes an earlier one. A cell is
/// stale from its commit until its edit settles.
///
/// With an engine attached, a row holds its place while it has an edit in
/// flight (`Engine::beginEdit`), which a `Deferred` reorder honours, and an
/// edit of a cell the engine's view holds as `Unloaded` is refused on the cell:
/// it would overwrite a value the user has not seen.
///
/// A model's table commits through `CellEdit` and `RowEdit` actions instead:
/// `CellEdits` built with a `RowEditCommit` sends one request per row commit,
/// one row's requests in order, and holds the cells' drafts, errors and
/// modified marks by row key and column, so a row that leaves the window and
/// comes back finds them where they were.
///
/// See `docs/spec/table/engine.md` and `docs/spec/table/editing.md`.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <map>
#include <morph/core/callback_scope.hpp>
#include <morph/forms/engine/field_model.hpp>
#include <morph/table/data_source.hpp>
#include <morph/table/description.hpp>
#include <morph/table/engine.hpp>
#include <optional>
#include <span>
#include <typeindex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace morph::table {

/// @brief One edited cell to commit.
struct EditRequest {
    /// @brief The row's key.
    RowId row;
    /// @brief The column's id.
    std::string column;
    /// @brief The value the user entered.
    Cell value;
};

/// @brief A server's refusal of an edit, attached to the cell.
struct FieldError {
    /// @brief The column the error is about.
    std::string column;
    /// @brief The message the cell shows.
    std::string message;
};

/// @brief Settles one commit, on the owner thread: the row as the server
///        returned it, or the field error.
using EditDone = std::function<void(std::expected<std::vector<Cell>, FieldError>)>;

/// @brief Runs a commit: the table's mutation for @p request, calling @p done
///        once with its outcome.
using EditCommit = std::function<void(EditRequest const& request, EditDone done)>;

/// @brief One cell's change on a model's table: the row and the new value of one member. Its schema carries
///        `x-cellEditOf` with the table's record at its root; which column it edits is the column's `EditInfo`.
///
/// An application's action derives from it, so each action is its own type:
/// `struct EditSpecimenCount : table::CellEdit<&SpecimensModel::specimens, std::int64_t> {};`.
/// @tparam Function The table's function, as `BRIDGE_REGISTER_TABLE` names it.
/// @tparam T        The member's type.
template <auto Function, typename T>
struct CellEdit {
    /// @brief The row's key.
    RowId key;
    /// @brief The member's new value.
    T value;

    /// @brief The schema keyword and its value that name the table, written at the schema's root.
    /// @return `{"x-cellEditOf", "<Model>.<id>"}`; the name is empty when no such record is registered.
    [[nodiscard]] static std::pair<std::string_view, std::string> schemaValue() {
        return {"x-cellEditOf", detail::recordNameOf(std::type_index{typeid(TableOf<Function>)})};
    }

    /// @brief The JSON of the action: `{"key": …, "value": …}`. Declared here so an action deriving from this type
    ///        reads and writes both members, which reflection over the derived type would count as one.
    struct glaze {
        /// @brief Each member after its JSON name.
        static constexpr auto value = glz::object("key", &CellEdit::key, "value", &CellEdit::value);
    };
};

/// @brief Several members of one row of a model's table changed together. `Values` is the application's struct of the
///        row's editable members, each optional: a member present is changed, an absent one is left as it is. Its
///        schema carries `x-rowEditOf` with the table's record at its root.
/// @tparam Function The table's function, as `BRIDGE_REGISTER_TABLE` names it.
/// @tparam Values   The struct of the row's editable members.
template <auto Function, typename Values>
struct RowEdit {
    /// @brief The row's key.
    RowId key;
    /// @brief The members changed.
    Values values;

    /// @brief The schema keyword and its value that name the table, written at the schema's root.
    /// @return `{"x-rowEditOf", "<Model>.<id>"}`; the name is empty when no such record is registered.
    [[nodiscard]] static std::pair<std::string_view, std::string> schemaValue() {
        return {"x-rowEditOf", detail::recordNameOf(std::type_index{typeid(TableOf<Function>)})};
    }

    /// @brief The JSON of the action: `{"key": …, "values": …}`, for an action deriving from this type as for
    ///        `CellEdit`.
    struct glaze {
        /// @brief Each member after its JSON name.
        static constexpr auto value = glz::object("key", &RowEdit::key, "values", &RowEdit::values);
    };
};

namespace detail {

/// @brief Whether a form field of a kind can edit a column of a kind.
/// @param column The column's kind.
/// @param field  The field's kind.
/// @return `true` when the field's kind is one the column's kind is edited with.
[[nodiscard]] inline bool editFits(ColumnKind column, forms::FieldKind field) {
    using forms::FieldKind;
    auto const among = [field](std::initializer_list<FieldKind> kinds) { return std::ranges::contains(kinds, field); };
    switch (column) {
        case ColumnKind::Integer:
            return among({FieldKind::Integer, FieldKind::Slider, FieldKind::Enum, FieldKind::Choice});
        case ColumnKind::Key:
            return among({FieldKind::Integer, FieldKind::Choice});
        case ColumnKind::Decimal:
            return among({FieldKind::Decimal});
        case ColumnKind::Quantity:
        case ColumnKind::Measure:
            return among({FieldKind::Quantity});
        case ColumnKind::Number:
            return among({FieldKind::Number, FieldKind::Slider});
        case ColumnKind::Text:
            return among({FieldKind::Text, FieldKind::Multiline, FieldKind::Enum, FieldKind::Choice});
        case ColumnKind::Date:
            return among({FieldKind::Date, FieldKind::EpochDate});
        case ColumnKind::DateTime:
            return among({FieldKind::DateTime, FieldKind::EpochDateTime});
        case ColumnKind::Bool:
            return among({FieldKind::Boolean});
        case ColumnKind::Custom:
            return among({FieldKind::Text, FieldKind::Enum, FieldKind::Choice});
    }
    return false;
}

}  // namespace detail

inline std::expected<void, TableError> checkEdits(
    TableDescription const& description,
    std::function<std::optional<std::string>(std::string_view action)> const& schemaOf) {
    auto const refuse = [](std::string const& column, std::string message) -> std::expected<void, TableError> {
        return std::unexpected(
            TableError{.code = TableErrorCode::SourceContract, .column = column, .message = std::move(message)});
    };
    for (TableColumn const& column : description.columns) {
        if (!column.edit) {
            continue;
        }
        std::string const& id = column.info.id;
        EditInfo const& edit = *column.edit;
        std::optional<std::string> const schema = schemaOf ? schemaOf(edit.action) : std::nullopt;
        if (!schema) {
            return refuse(id, "column `" + id + "` is edited by `" + edit.action + "`, which is not an action here");
        }
        auto model = forms::FormModel::fromSchema(edit.action, *schema);
        if (!model) {
            return refuse(id, "column `" + id + "`'s edit action `" + edit.action +
                                  "` has an input schema that does not read: " + model.error().message);
        }
        std::span<forms::FieldSpec const> const fields = model->fields();
        forms::FieldSpec const* field = nullptr;
        if (edit.member.empty()) {
            auto const found = std::ranges::find(fields, std::string_view{"value"}, &forms::FieldSpec::name);
            field = found == fields.end() ? nullptr : &*found;
        } else {
            auto const values = std::ranges::find(fields, std::string_view{"values"}, &forms::FieldSpec::name);
            if (values != fields.end()) {
                auto const found = std::ranges::find(values->children, edit.member, &forms::FieldSpec::name);
                field = found == values->children.end() ? nullptr : &*found;
            }
        }
        if (field == nullptr) {
            return refuse(id, edit.member.empty()
                                  ? "column `" + id + "`'s edit action `" + edit.action + "` has no member `value`"
                                  : "column `" + id + "`'s edit action `" + edit.action + "` has no member `" +
                                        edit.member + "` in its `values`");
        }
        if (!detail::editFits(column.info.kind, field->kind)) {
            return refuse(id, "column `" + id + "` cannot be edited by `" + edit.action + "`'s field of kind `" +
                                  std::string{forms::fieldKindName(field->kind)} + "`");
        }
    }
    return {};
}

/// @brief One cell of a row commit on a model's table.
struct EditCell {
    /// @brief The column's id.
    std::string column;
    /// @brief The value the action receives, as a JSON literal.
    std::string json;
    /// @brief The name the action's `FieldError` gives this cell: `value` for a `CellEdit`, the `EditInfo::member` for
    ///        a `RowEdit`; empty means the column's id.
    std::string member;
};

/// @brief A row commit on a model's table: the cells of one row, sent as one request.
struct RowEditRequest {
    /// @brief The row's key.
    RowId row;
    /// @brief The cells, in the order they were committed.
    std::vector<EditCell> cells;
};

/// @brief Settles one row commit, on the owner thread: nothing on success, else the refusals.
using RowEditDone = std::function<void(std::expected<void, std::vector<FieldError>>)>;

/// @brief Runs a row commit: the edit action for @p request, calling @p done once with its outcome.
using RowEditCommit = std::function<void(RowEditRequest const& request, RowEditDone done)>;

/// @brief Commits cell edits row by row, in order.
///
/// Owner-affine. A completion that arrives after the `CellEdits` is destroyed
/// is ignored, and destroying it ends the engine edit of every row it still
/// has in flight. The commit function, the patcher and the engine's handlers
/// may commit again, or destroy this object, from inside a call it makes.
class CellEdits {
public:
    /// @brief Builds the committer.
    /// @param patcher Replaces a row with the server's reply; must outlive this object.
    /// @param commit  Runs the table's mutation.
    /// @param engine  Optional: the engine whose rows hold their place while
    ///                an edit is in flight; must outlive this object.
    CellEdits(RowPatcher& patcher, EditCommit commit, Engine* engine = nullptr)
        : _patcher{&patcher}, _commit{std::move(commit)}, _engine{engine} {}

    /// @brief Builds the committer of a model's table: no patcher and no engine, its rows' commits sent through
    ///        @p commit.
    /// @param commit Runs a row commit.
    explicit CellEdits(RowEditCommit commit) : _patcher{nullptr}, _engine{nullptr}, _rowCommit{std::move(commit)} {}

    CellEdits(CellEdits const&) = delete;
    CellEdits& operator=(CellEdits const&) = delete;
    CellEdits(CellEdits&&) = delete;
    CellEdits& operator=(CellEdits&&) = delete;

    /// @brief Ends the engine edit of every row with an edit in flight or
    ///        settling: its completion will be ignored, so nothing else would.
    // NOLINTNEXTLINE(bugprone-exception-escape) -- a handler that throws from here terminates, as from any destructor
    ~CellEdits() {
        if (_engine == nullptr) {
            return;
        }
        std::vector<RowId> rows{_ending.begin(), _ending.end()};
        for (auto const& [row, queue] : _queues) {
            rows.push_back(row);
        }
        _queues.clear();
        _ending.clear();
        for (auto const& row : rows) {
            _engine->endEdit(row);
        }
    }

    /// @brief Commits an edit: now, or after the row's earlier edits settle.
    ///
    /// An edit of a cell that is unloaded in the engine's view is not
    /// committed: the cell's error becomes "not loaded yet" and the commit
    /// function is not called. Without an engine nothing is checked.
    /// @param request The edit.
    void commit(EditRequest request) {
        if (unloadedInView(request)) {
            _errors[request.row].insert_or_assign(request.column, "not loaded yet");
            return;
        }
        auto& queue = _queues[request.row];
        queue.push_back(std::move(request));
        if (queue.size() == 1) {
            start(queue.front().row);
        }
    }

    /// @brief Commits a row's cells as one request: now, or after the row's earlier row commits settle, in order.
    /// @param request The row and its cells.
    void commitRow(RowEditRequest request) {
        RowId const row = request.row;
        std::vector<std::optional<std::string>> drafts;
        for (EditCell const& cell : request.cells) {
            drafts.push_back(draft(row, cell.column));
        }
        auto& queue = _rowQueues[row];
        queue.push_back(RowFlight{
            .id = ++_nextFlight, .request = std::move(request), .sent = false, .discard = false, .drafts = std::move(drafts)});
        if (queue.size() == 1) {
            startRow(row);
        }
    }

    /// @brief Holds a cell's draft, as the user typed it, with the error it does not encode by.
    /// @param row    The row's key.
    /// @param column The column's id.
    /// @param text   The draft.
    /// @param error  Why it does not encode; nothing when it does.
    void setDraft(RowId const& row, std::string_view column, std::string text, std::optional<std::string> error) {
        CellState& cell = _cells[row][std::string{column}];
        cell.draft = std::move(text);
        cell.settled = false;
        if (error) {
            _errors[row].insert_or_assign(std::string{column}, std::move(*error));
        } else {
            eraseError(row, column);
        }
    }

    /// @brief A cell's draft.
    /// @param row    The row's key.
    /// @param column The column's id.
    /// @return The draft; nothing while none is held.
    [[nodiscard]] std::optional<std::string> draft(RowId const& row, std::string_view column) const {
        CellState const* const cell = cellOf(row, column);
        return cell == nullptr ? std::nullopt : cell->draft;
    }

    /// @brief Drops a cell's draft and its error.
    /// @param row    The row's key.
    /// @param column The column's id.
    void clearDraft(RowId const& row, std::string_view column) {
        if (auto const found = _cells.find(row); found != _cells.end()) {
            if (auto const cell = found->second.find(column); cell != found->second.end()) {
                cell->second.draft.reset();
                cell->second.settled = false;
                if (!cell->second.modified) {
                    found->second.erase(cell);
                }
            }
            if (found->second.empty()) {
                _cells.erase(found);
            }
        }
        eraseError(row, column);
    }

    /// @brief Whether a cell is modified: from its row commit's success until the row is next fetched.
    /// @param row    The row's key.
    /// @param column The column's id.
    /// @return `true` while modified.
    [[nodiscard]] bool modified(RowId const& row, std::string_view column) const {
        CellState const* const cell = cellOf(row, column);
        return cell != nullptr && cell->modified;
    }

    /// @brief Whether a row holds a modified mark or a draft in any of its cells.
    /// @param row The row's key.
    /// @return `true` when one does.
    [[nodiscard]] bool rowEdited(RowId const& row) const {
        auto const found = _cells.find(row);
        return found != _cells.end() && std::ranges::any_of(found->second, [](auto const& entry) {
                   return entry.second.modified || entry.second.draft.has_value();
               });
    }

    /// @brief The window fetched a row: its modified marks end, and so do the drafts its row commits settled. A cell
    ///        still stale is left as it is: its commit has not replied, so the row fetched does not hold it yet.
    /// @param row The row's key.
    void rowFetched(RowId const& row) {
        auto const found = _cells.find(row);
        if (found == _cells.end()) {
            return;
        }
        for (auto cell = found->second.begin(); cell != found->second.end();) {
            if (!cell->second.modified || stale(row, cell->first)) {
                ++cell;
                continue;
            }
            cell->second.modified = false;
            if (cell->second.settled) {
                cell->second.draft.reset();
                cell->second.settled = false;
            }
            cell = cell->second.draft ? std::next(cell) : found->second.erase(cell);
        }
        if (found->second.empty()) {
            _cells.erase(found);
        }
    }

    /// @brief A fetch that named a row returned none: it was deleted, or no longer matches the filter. Its drafts,
    ///        errors, modified marks and the row commits not yet sent go; one in flight settles as it would and
    ///        leaves nothing behind.
    /// @param row The row's key.
    void rowAbsent(RowId const& row) {
        _cells.erase(row);
        _errors.erase(row);
        auto const found = _rowQueues.find(row);
        if (found == _rowQueues.end()) {
            return;
        }
        std::erase_if(found->second, [](RowFlight const& flight) { return !flight.sent; });
        for (RowFlight& flight : found->second) {
            flight.discard = true;
        }
        if (found->second.empty()) {
            _rowQueues.erase(found);
        }
    }

    /// @brief A row's key changed: its drafts, errors, modified marks and row commits move to the new key. Where the
    ///        new key holds a cell of its own, the new key's is kept.
    /// @param from The row's old key.
    /// @param to   The row's new key.
    void moveRow(RowId const& from, RowId const& to) {
        if (from == to) {
            return;
        }
        if (auto found = _cells.find(from); found != _cells.end()) {
            auto moved = std::move(found->second);
            _cells.erase(found);
            auto& target = _cells[to];
            for (auto& [column, cell] : moved) {
                target.try_emplace(column, std::move(cell));
            }
        }
        if (auto found = _errors.find(from); found != _errors.end()) {
            auto moved = std::move(found->second);
            _errors.erase(found);
            auto& target = _errors[to];
            for (auto& [column, message] : moved) {
                target.try_emplace(column, std::move(message));
            }
        }
        if (auto found = _rowQueues.find(from); found != _rowQueues.end()) {
            auto moved = std::move(found->second);
            _rowQueues.erase(found);
            auto& target = _rowQueues[to];
            for (RowFlight& flight : moved) {
                flight.request.row = to;
                target.push_back(std::move(flight));
            }
        }
    }

    /// @brief Keeps the drafts, errors and modified marks of these columns only: a new description dropped the others.
    /// @param columns The ids of the columns kept.
    void keepColumns(std::span<std::string const> columns) {
        auto const gone = [columns](std::string const& column) { return !std::ranges::contains(columns, column); };
        for (auto row = _cells.begin(); row != _cells.end();) {
            std::erase_if(row->second, [&gone](auto const& entry) { return gone(entry.first); });
            row = row->second.empty() ? _cells.erase(row) : std::next(row);
        }
        for (auto row = _errors.begin(); row != _errors.end();) {
            std::erase_if(row->second, [&gone](auto const& entry) { return gone(entry.first); });
            row = row->second.empty() ? _errors.erase(row) : std::next(row);
        }
    }

    /// @brief Cells that show an error, wherever their row is.
    /// @return The count.
    [[nodiscard]] std::size_t errorCount() const {
        std::size_t out = 0;
        for (auto const& [row, cells] : _errors) {
            out += cells.size();
        }
        return out;
    }

    /// @brief Cells marked modified, wherever their row is.
    /// @return The count.
    [[nodiscard]] std::size_t modifiedCount() const {
        std::size_t out = 0;
        for (auto const& [row, cells] : _cells) {
            out += static_cast<std::size_t>(
                std::ranges::count_if(cells, [](auto const& entry) { return entry.second.modified; }));
        }
        return out;
    }

    /// @brief Sets the one receiver of each row commit's settling, called once the cells took its outcome and the
    ///        row's next commit was sent, with the row's key now and whether the commit was refused. A commit whose
    ///        row went absent settles unheard.
    /// @param handler The receiver; empty for none.
    void onRowSettled(std::function<void(RowId const& row, bool refused)> handler) { _rowSettled = std::move(handler); }

    /// @brief Whether a cell has an edit committed and not yet settled.
    /// @param row    The row's key.
    /// @param column The column's id.
    /// @return `true` while stale.
    [[nodiscard]] bool stale(RowId const& row, std::string_view column) const {
        if (auto const found = _queues.find(row);
            found != _queues.end() &&
            std::ranges::any_of(found->second, [&](EditRequest const& request) { return request.column == column; })) {
            return true;
        }
        auto const flights = _rowQueues.find(row);
        return flights != _rowQueues.end() && std::ranges::any_of(flights->second, [&](RowFlight const& flight) {
                   return !flight.discard && std::ranges::any_of(flight.request.cells, [&](EditCell const& cell) {
                       return cell.column == column;
                   });
               });
    }

    /// @brief The error a cell shows, until its next edit succeeds.
    /// @param row    The row's key.
    /// @param column The column's id.
    /// @return The message, or nothing.
    [[nodiscard]] std::optional<std::string> error(RowId const& row, std::string_view column) const {
        auto const found = _errors.find(row);
        if (found == _errors.end()) {
            return std::nullopt;
        }
        auto const cell = found->second.find(column);
        return cell == found->second.end() ? std::nullopt : std::optional{cell->second};
    }

    /// @brief Edits committed and not yet settled, across rows.
    /// @return The count.
    [[nodiscard]] std::size_t unsettled() const {
        std::size_t out = 0;
        for (auto const& [row, queue] : _queues) {
            out += queue.size();
        }
        return out;
    }

private:
    // Whether the engine's view shows the edited cell as unloaded.
    [[nodiscard]] bool unloadedInView(EditRequest const& request) const {
        if (_engine == nullptr) {
            return false;
        }
        auto const viewRow = _engine->viewRowOfKey(request.row);
        if (!viewRow) {
            return false;
        }
        auto const columns = _engine->columns();
        auto const column = std::ranges::find(columns, request.column, &ColumnInfo::id);
        if (column == columns.end()) {
            return false;
        }
        auto const index = static_cast<std::size_t>(column - columns.begin());
        return std::holds_alternative<Unloaded>(_engine->cellAt(*viewRow, index));
    }

    void start(RowId const& row) {
        if (_engine != nullptr) {
            _engine->beginEdit(row);
        }
        // A copy: a commit that settles at once pops the queue's front, and the
        // commit may still read its request afterwards.
        auto const request = _queues.at(row).front();
        // The commit runs last: it may settle, commit again or destroy this object.
        _commit(request, _alive.guard([this, row](std::expected<std::vector<Cell>, FieldError> outcome) {
            settle(row, std::move(outcome));
        }));
    }

    // The queue is brought up to date before the patcher is called: patching
    // notifies the source's listeners at once, and their handlers may commit
    // to the same row or destroy this object.
    void settle(RowId const& row, std::expected<std::vector<Cell>, FieldError> outcome) {
        auto const found = _queues.find(row);
        if (found == _queues.end() || found->second.empty()) {
            return;
        }
        auto const request = std::move(found->second.front());
        found->second.pop_front();
        bool const more = !found->second.empty();
        if (!more) {
            _queues.erase(found);
            _ending.insert(row);
        }
        if (outcome) {
            if (auto const errors = _errors.find(row); errors != _errors.end()) {
                errors->second.erase(request.column);
            }
            auto const alive = _alive.token();
            _patcher->patchRow(row, std::move(*outcome));
            if (alive.expired()) {
                return;
            }
        } else {
            auto const column = outcome.error().column.empty() ? request.column : outcome.error().column;
            _errors[row].insert_or_assign(column, std::move(outcome.error().message));
        }
        if (more) {
            start(row);
            return;
        }
        _ending.erase(row);
        if (_queues.contains(row)) {
            // A commit made while the row was patched started its next edit.
            return;
        }
        if (_engine != nullptr) {
            _engine->endEdit(row);
        }
    }

    // A row commit: sent once it is at the front of its row's queue, and discarded on settling when its row went
    // absent meanwhile.
    struct RowFlight {
        std::uint64_t id = 0;
        RowEditRequest request;
        bool sent = false;
        bool discard = false;
        // Each cell's draft when the commit was made, in the request's order: a success settles a cell's draft only
        // while it is still this one, so a draft typed after the commit outlives the next fetch.
        std::vector<std::optional<std::string>> drafts;
    };

    // A cell of a model's table: its draft; whether its row commit succeeded and the row was not fetched since; and
    // whether its draft is the one that commit sent, which the next fetch replaces with the row's own value.
    struct CellState {
        std::optional<std::string> draft;
        bool modified = false;
        bool settled = false;
    };

    [[nodiscard]] CellState const* cellOf(RowId const& row, std::string_view column) const {
        auto const found = _cells.find(row);
        if (found == _cells.end()) {
            return nullptr;
        }
        auto const cell = found->second.find(column);
        return cell == found->second.end() ? nullptr : &cell->second;
    }

    void eraseError(RowId const& row, std::string_view column) {
        if (auto const found = _errors.find(row); found != _errors.end()) {
            if (auto const cell = found->second.find(column); cell != found->second.end()) {
                found->second.erase(cell);
            }
            if (found->second.empty()) {
                _errors.erase(found);
            }
        }
    }

    // Sends the front of a row's queue unless it is out already. The commit runs last: it may settle, commit again
    // or destroy this object.
    void startRow(RowId const& row) {
        auto const found = _rowQueues.find(row);
        if (found == _rowQueues.end() || found->second.empty() || found->second.front().sent) {
            return;
        }
        RowFlight& front = found->second.front();
        front.sent = true;
        // A copy: a commit that settles at once pops the queue's front, and the commit may still read its request.
        RowEditRequest const request = front.request;
        _rowCommit(request, _alive.guard([this, id = front.id](std::expected<void, std::vector<FieldError>> outcome) {
            settleRow(id, std::move(outcome));
        }));
    }

    // A refusal lands by name: an error naming a cell's member (its column when it has none) is that cell's; one that
    // names no cell of the request, or nothing, is every other cell's. A cell no error reached takes none.
    void landErrors(RowEditRequest const& request, std::vector<FieldError> const& errors) {
        auto const nameOf = [](EditCell const& cell) -> std::string const& {
            return cell.member.empty() ? cell.column : cell.member;
        };
        std::map<std::string, std::string, std::less<>> landed;
        std::optional<std::string> general;
        for (FieldError const& error : errors) {
            auto const named = std::ranges::find_if(request.cells, [&](EditCell const& cell) {
                return !error.column.empty() && nameOf(cell) == error.column;
            });
            if (named != request.cells.end()) {
                landed.insert_or_assign(named->column, error.message);
            } else if (!general) {
                general = error.message;
            }
        }
        for (EditCell const& cell : request.cells) {
            if (auto const found = landed.find(cell.column); found != landed.end()) {
                _errors[request.row].insert_or_assign(cell.column, found->second);
            } else if (general) {
                _errors[request.row].insert_or_assign(cell.column, *general);
            } else {
                eraseError(request.row, cell.column);
            }
        }
    }

    // A row commit settled. The cells take its outcome, and the row's next commit is sent, before the receiver hears
    // it: it may commit again or destroy this object.
    void settleRow(std::uint64_t id, std::expected<void, std::vector<FieldError>> outcome) {
        auto queue = _rowQueues.begin();
        auto flight = std::deque<RowFlight>::iterator{};
        for (; queue != _rowQueues.end(); ++queue) {
            flight = std::ranges::find(queue->second, id, &RowFlight::id);
            if (flight != queue->second.end()) {
                break;
            }
        }
        if (queue == _rowQueues.end()) {
            return;
        }
        RowId const row = queue->first;
        RowFlight const settled = std::move(*flight);
        queue->second.erase(flight);
        if (queue->second.empty()) {
            _rowQueues.erase(queue);
        }
        if (!settled.discard) {
            if (outcome) {
                for (std::size_t at = 0; at < settled.request.cells.size(); ++at) {
                    EditCell const& cell = settled.request.cells.at(at);
                    eraseError(row, cell.column);
                    CellState& state = _cells[row][cell.column];
                    state.modified = true;
                    state.settled = state.draft.has_value() && at < settled.drafts.size() &&
                                    state.draft == settled.drafts.at(at);
                }
            } else {
                RowEditRequest landing = settled.request;
                landing.row = row;
                landErrors(landing, outcome.error());
            }
        }
        auto const alive = _alive.token();
        startRow(row);
        if (alive.expired() || settled.discard || !_rowSettled) {
            return;
        }
        std::function<void(RowId const&, bool)> const heard = _rowSettled;
        heard(row, !outcome.has_value());
    }

    RowPatcher* _patcher;
    EditCommit _commit;
    Engine* _engine;
    std::unordered_map<RowId, std::deque<EditRequest>> _queues;
    std::unordered_set<RowId> _ending;  // last edit settled, row being patched
    std::unordered_map<RowId, std::map<std::string, std::string, std::less<>>> _errors;
    RowEditCommit _rowCommit;
    std::unordered_map<RowId, std::deque<RowFlight>> _rowQueues;
    std::unordered_map<RowId, std::map<std::string, CellState, std::less<>>> _cells;
    std::function<void(RowId const&, bool)> _rowSettled;
    std::uint64_t _nextFlight = 0;
    async::CallbackScope _alive;
};

}  // namespace morph::table
