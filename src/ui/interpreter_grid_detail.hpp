// SPDX-License-Identifier: Apache-2.0

// What the interpreter's grid lowering shares: the grid tables and the helpers over them.

#pragma once

#include <morph/forms/engine/collection_view.hpp>
#include <morph/forms/engine/form_view.hpp>
#include <morph/forms/engine/wizard_view.hpp>
#include <morph/ui/grid.hpp>

#include "interpreter_table_detail.hpp"

namespace morph::ui::detail {

/// A grid of a form: the grid it shows, and its status line's message, held for 6 s as a table's is.
struct GridTable {
    explicit GridTable(reactive::Runtime& runtime)
        : config{runtime},
          filterVersion{runtime, std::uint64_t{0}},
          message{runtime, std::string{}},
          entries{runtime, std::vector<StatusEntry>{}} {}

    GridTable(GridTable const&) = delete;
    GridTable& operator=(GridTable const&) = delete;
    GridTable(GridTable&&) = delete;
    GridTable& operator=(GridTable&&) = delete;
    ~GridTable() = default;

    /// The grid: its source, its controller and its table node. A fixed grid's source is `fixed`, and `grid.source`
    /// is null.
    FormGrid grid;
    /// A fixed grid's source; null for a grid of a form.
    std::shared_ptr<FixedGridSource> fixed;
    /// The summary rows over the grid's rows, which the mount is handed in their place; null for a grid with no
    /// summaries and no group.
    std::shared_ptr<SummaryRows> summaries;
    /// Whether a `group` part groups the rows.
    bool grouped = false;
    /// Each run of the view, by ordinal: the source's run index and how many runs of that index came before it in the
    /// view (rows added while sorted show at the end, after their run).
    std::vector<std::pair<std::size_t, std::size_t>> runOrder;
    /// The places a summary of each edit column shows at, by column id: its member's.
    std::map<std::string, std::optional<std::uint32_t>, std::less<>> places;
    /// The table's columns, one per grid column.
    std::vector<TableColumn> shown;
    /// The table's configuring: its columns, layout, menus and filters.
    TableConfig config;
    /// Sets the slots of `config` a mount owns (`filterTarget`, `heldCell`, `gather`); run before each mount begins,
    /// so a mount after an unmount has them again.
    std::function<void()> setMountSlots;
    /// The filters the user set, and a count that moves with each.
    table::FilterSpec filter;
    reactive::Signal<std::uint64_t> filterVersion;
    /// The status line's message and the choices it offers, with what each runs.
    reactive::Signal<std::string> message;
    reactive::Signal<std::vector<StatusEntry>> entries;
    std::map<std::string, std::function<void()>, std::less<>> entryHandlers;
    reactive::TimerHandle messageTimer;
    /// Last, so what the message's timer posts does nothing once the grid is gone.
    async::CallbackScope scope;
};

std::vector<std::pair<std::int64_t, std::int64_t>> gridRuns(GridTable& held);

RowRecord gridHeading(GridTable& held, std::size_t ordinal);

}  // namespace morph::ui::detail
