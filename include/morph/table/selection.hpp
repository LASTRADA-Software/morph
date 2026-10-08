// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file table/selection.hpp
/// @brief A table's selection and activation, kept by row key.
///
/// Selection is a set of `RowId`s plus an active key (the row with the
/// cursor). Because it is kept by key, it survives sort, filter and refetch:
/// a row that leaves the view stays selected unless the table prunes on
/// filter. Activation (double-click, Enter) reports a row and does not change
/// the selection.
///
/// In `server` mode a selected row may be outside the fetched pages, so
/// "select all" and pruning are not available; the selection is the set of
/// keys the user chose.
///
/// See `docs/spec/table/engine.md`.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <morph/table/data_source.hpp>
#include <morph/table/engine.hpp>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

namespace morph::table {

// NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) -- row and column indices are bounded by the snapshot's own counts, and the sort and filter loops index once per comparison, where at() would check every access

/// @brief Where a table's rows are sorted and filtered.
enum class TableMode : std::uint8_t {
    Client,  ///< The client holds every row and the engine sorts and filters them.
    Server,  ///< The server sorts and filters; the client holds fetched pages.
    Hybrid,  ///< A server search whose result the client engine sorts and filters.
};

/// @brief How many rows a selection may hold.
enum class SelectionMode : std::uint8_t {
    None,      ///< Rows cannot be selected.
    Single,    ///< At most one row.
    Multiple,  ///< Any number of rows.
};

/// @brief A selection's configuration.
struct SelectionOptions {
    /// @brief How many rows may be selected.
    SelectionMode mode = SelectionMode::None;
    /// @brief Drop selected rows that leave the view.
    bool pruneOnFilter = false;
    /// @brief The table's mode; `Server` rules out select-all and pruning.
    TableMode tableMode = TableMode::Client;
};

/// @brief A table's selected keys and active key.
class Selection {
public:
    /// @brief Builds a selection.
    /// @param options The configuration.
    /// @return The selection, or `Unavailable` when @p options asks to prune
    ///         in `server` mode.
    [[nodiscard]] static std::expected<Selection, TableError> create(SelectionOptions options) {
        if (options.pruneOnFilter && options.tableMode == TableMode::Server) {
            return std::unexpected(TableError{.code = TableErrorCode::Unavailable,
                                              .column = {},
                                              .message = "pruneOnFilter is not available in server mode"});
        }
        return Selection{options};
    }

    /// @brief Selects a row; in `Single` mode it replaces the selection.
    /// @param key The row's key.
    /// @return `false` in `None` mode.
    bool select(RowId const& key) {
        if (_options.mode == SelectionMode::None) {
            return false;
        }
        if (_options.mode == SelectionMode::Single) {
            _keys.clear();
            _order.clear();
        }
        if (_keys.insert(key).second) {
            _order.push_back(key);
        }
        return true;
    }

    /// @brief Deselects a row.
    /// @param key The row's key.
    /// @return `true` when it was selected.
    bool deselect(RowId const& key) {
        if (_keys.erase(key) == 0) {
            return false;
        }
        std::erase(_order, key);
        return true;
    }

    /// @brief Selects an unselected row, or deselects a selected one.
    /// @param key The row's key.
    /// @return Whether the row is selected afterwards.
    bool toggle(RowId const& key) {
        if (deselect(key)) {
            return false;
        }
        return select(key);
    }

    /// @brief Deselects every row.
    void clear() {
        _keys.clear();
        _order.clear();
    }

    /// @brief Selects every row in the engine's view.
    /// @param engine The table's engine.
    /// @return `Unavailable` in `server` mode or unless the mode is `Multiple`.
    std::expected<void, TableError> selectAll(Engine const& engine) {
        if (_options.tableMode == TableMode::Server || _options.mode != SelectionMode::Multiple) {
            return std::unexpected(TableError{.code = TableErrorCode::Unavailable,
                                              .column = {},
                                              .message = "select all needs multiple selection over client rows"});
        }
        for (std::size_t row = 0; row < engine.viewRowCount(); ++row) {
            select(engine.rowIdAt(row));
        }
        return {};
    }

    /// @brief Brings the selection up to date after the view changed: with
    ///        `pruneOnFilter`, drops selected rows no longer in the view.
    /// @param engine The table's engine.
    void viewChanged(Engine const& engine) {
        if (!_options.pruneOnFilter) {
            return;
        }
        std::erase_if(_order, [&](RowId const& key) {
            if (engine.viewRowOfKey(key)) {
                return false;
            }
            _keys.erase(key);
            return true;
        });
    }

    /// @brief Moves the cursor.
    /// @param key The row's key, or nothing for no cursor.
    void setActiveKey(std::optional<RowId> key) { _active = std::move(key); }

    /// @brief The row with the cursor.
    /// @return Its key, or nothing.
    [[nodiscard]] std::optional<RowId> const& activeKey() const noexcept { return _active; }

    /// @brief Whether a row is selected.
    /// @param key The row's key.
    /// @return `true` when selected.
    [[nodiscard]] bool contains(RowId const& key) const { return _keys.contains(key); }

    /// @brief Selected rows.
    /// @return The count.
    [[nodiscard]] std::size_t selectedCount() const noexcept { return _keys.size(); }

    /// @brief The selected keys, in the order they were selected.
    /// @return The keys.
    [[nodiscard]] std::vector<RowId> const& keys() const noexcept { return _order; }

    /// @brief The configuration.
    /// @return The options.
    [[nodiscard]] SelectionOptions const& options() const noexcept { return _options; }

    /// @brief Activates the row at a view row: reports it and changes no selection.
    /// @param engine     The table's engine.
    /// @param viewRow    The view row.
    /// @param onActivate Receives the row's key.
    /// @return `false` when @p viewRow is past the view.
    static bool activate(Engine const& engine, std::size_t viewRow,
                         std::function<void(RowId const&)> const& onActivate) {
        if (viewRow >= engine.viewRowCount()) {
            return false;
        }
        if (onActivate) {
            onActivate(engine.rowIdAt(viewRow));
        }
        return true;
    }

private:
    explicit Selection(SelectionOptions options) : _options{options} {}

    SelectionOptions _options;
    std::unordered_set<RowId> _keys;
    std::vector<RowId> _order;
    std::optional<RowId> _active;
};

/// @brief The counts a status line shows.
struct TableCounts {
    /// @brief Rows in the view.
    std::size_t viewCount = 0;
    /// @brief Rows in the source.
    std::size_t sourceCount = 0;
    /// @brief Selected rows.
    std::size_t selectedCount = 0;
};

/// @brief A table's counts.
/// @param engine    The table's engine.
/// @param selection The table's selection.
/// @return View, source and selected counts.
[[nodiscard]] inline TableCounts counts(Engine const& engine, Selection const& selection) {
    return TableCounts{.viewCount = engine.viewRowCount(),
                       .sourceCount = engine.sourceRowCount(),
                       .selectedCount = selection.selectedCount()};
}

// NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)

}  // namespace morph::table
