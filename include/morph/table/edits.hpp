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
/// flight (`Engine::beginEdit`), which a `Deferred` reorder honours.
///
/// See `docs/spec/table/engine.md`.

#include <cstddef>
#include <deque>
#include <expected>
#include <functional>
#include <map>
#include <morph/core/callback_scope.hpp>
#include <morph/table/data_source.hpp>
#include <morph/table/engine.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
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
    /// @param request The edit.
    void commit(EditRequest request) {
        auto& queue = _queues[request.row];
        queue.push_back(std::move(request));
        if (queue.size() == 1) {
            start(queue.front().row);
        }
    }

    /// @brief Whether a cell has an edit committed and not yet settled.
    /// @param row    The row's key.
    /// @param column The column's id.
    /// @return `true` while stale.
    [[nodiscard]] bool stale(RowId const& row, std::string_view column) const {
        auto const found = _queues.find(row);
        if (found == _queues.end()) {
            return false;
        }
        return std::ranges::any_of(found->second,
                                   [&](EditRequest const& request) { return request.column == column; });
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

    RowPatcher* _patcher;
    EditCommit _commit;
    Engine* _engine;
    std::unordered_map<RowId, std::deque<EditRequest>> _queues;
    std::unordered_set<RowId> _ending;  // last edit settled, row being patched
    std::unordered_map<RowId, std::map<std::string, std::string, std::less<>>> _errors;
    async::CallbackScope _alive;
};

}  // namespace morph::table
