// SPDX-License-Identifier: Apache-2.0

// The non-template definitions of <morph/ui/mount.hpp>.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <core/async/StopToken.hpp>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <morph/attributes.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/scope.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/table/view_change.hpp>
#include <morph/ui/backend.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/stretch_columns.hpp>
#include <morph/ui/table_rows.hpp>
#include <morph/ui/value.hpp>
#include <morph/ui/view.hpp>
#include <numeric>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace morph::ui::detail {

// NOLINTNEXTLINE(misc-no-recursion): a submenu is a menu one level down.
void Mounter::readMenuItems(std::vector<MenuItem> const& items, std::vector<MenuEntry>& shown,
                            std::vector<OpenItem>& kept) {
    for (MenuItem const& item : items) {
        if (item.separator) {
            if (!shown.empty() && !shown.back().separator) {
                shown.push_back(MenuEntry{.separator = true});
                kept.emplace_back();
            }
            continue;
        }
        if (!item.visible.evaluate()) {
            continue;
        }
        MenuEntry entry{.label = item.label.evaluate(),
                        .icon = item.icon,
                        .keys = item.keys,
                        .checked = item.checked.evaluate(),
                        .enabled = item.enabled.evaluate(),
                        .description = item.description.evaluate()};
        OpenItem open{.onSelect = item.onSelect, .items = {}};
        readMenuItems(item.items, entry.items, open.items);
        shown.push_back(std::move(entry));
        kept.push_back(std::move(open));
    }
    if (!shown.empty() && shown.back().separator) {
        shown.pop_back();
        kept.pop_back();
    }
}

Mounter::OpenItem const* Mounter::openItemAt(std::vector<OpenItem> const& kept, std::span<std::size_t const> path) {
    OpenItem const* found = nullptr;
    std::vector<OpenItem> const* level = &kept;
    for (std::size_t const index : path) {
        if (index >= level->size()) {
            return nullptr;
        }
        found = &level->at(index);
        level = &found->items;
    }
    return found;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one seam per configuring callback.
void Mounter::bindTableConfig(reactive::Scope& scope, Table const& spec, TableWidget& widget,
                              std::function<void(Key const&, std::size_t)> focusCell) {
    auto const open = std::make_shared<OpenMenus>();
    if (spec.menu || spec.headerEntries) {
        // The entries are read when a menu opens, as a callback is: untracked, an exception reported. While the
        // header is disabled every built-in entry of a column's menu is, since each sorts, filters or arranges.
        widget.setMenu(
            [runtime = _rt, entries = spec.menu, extra = spec.headerEntries, enabled = spec.headerEnabled,
             open](std::optional<std::size_t> column) {
                std::vector<MenuEntry> shown;
                static_cast<void>(runCallback(*runtime, [&] {
                    if (entries) {
                        shown = entries(column);
                    }
                    if (!column) {
                        return;
                    }
                    if (!enabled.evaluate()) {
                        for (MenuEntry& entry : shown) {
                            entry.enabled = false;
                        }
                    }
                    open->headerColumn = column;
                    open->builtIn = shown.size();
                    open->header.clear();
                    if (!extra) {
                        return;
                    }
                    std::vector<MenuEntry> added;
                    readMenuItems(extra(*column), added, open->header);
                    if (added.empty()) {
                        return;
                    }
                    if (!shown.empty()) {
                        shown.push_back(MenuEntry{.separator = true});
                    }
                    open->headerOffset = shown.size();
                    std::ranges::move(added, std::back_inserter(shown));
                }));
                return shown;
            },
            [runtime = _rt, chosen = event(spec.onMenu), open](std::optional<std::size_t> column,
                                                               std::vector<std::size_t> path) {
                bool const builtIn = !column || open->headerColumn != column || path.empty() ||
                                     path.front() < open->builtIn || open->header.empty();
                if (builtIn) {
                    if (chosen) {
                        chosen(column, std::move(path));
                    }
                    return;
                }
                if (path.front() < open->headerOffset) {
                    return;
                }
                path.front() -= open->headerOffset;
                OpenItem const* const item = openItemAt(open->header, path);
                if (item != nullptr && item->onSelect && item->items.empty()) {
                    Action const run = item->onSelect;
                    static_cast<void>(runCallback(*runtime, run));
                }
            });
    }
    if (spec.cellMenu) {
        widget.setCellMenu(
            [runtime = _rt, entries = spec.cellMenu, open, focusCell](Key const& row, std::size_t column) {
                std::vector<MenuEntry> shown;
                static_cast<void>(runCallback(*runtime, [&] {
                    if (focusCell) {
                        focusCell(row, column);
                    }
                    open->cell = std::pair{row, column};
                    open->cellItems.clear();
                    readMenuItems(entries(row, column), shown, open->cellItems);
                }));
                return shown;
            },
            [runtime = _rt, open](Key const& row, std::size_t column, std::vector<std::size_t> const& path) {
                if (open->cell != std::pair{row, column}) {
                    return;
                }
                OpenItem const* const item = openItemAt(open->cellItems, path);
                if (item != nullptr && item->onSelect && item->items.empty()) {
                    Action const run = item->onSelect;
                    static_cast<void>(runCallback(*runtime, run));
                }
            });
        widget.setCellMenuKeys(spec.cellMenuKeys);
    }
    if (spec.onColumnMove) {
        widget.setOnColumnMove(event(spec.onColumnMove));
    }
    if (spec.onColumnResize) {
        widget.setOnColumnResize(event(spec.onColumnResize));
    }
    if (spec.onFilter) {
        widget.setOnFilter(event(spec.onFilter));
    }
    if (spec.filtered.isBound() || !spec.filtered.constant().empty()) {
        bind(scope, spec.filtered,
             [&widget](std::vector<std::size_t> const& columns) { widget.setFiltered(columns); });
    }
    if (spec.columnGroups.isBound() || !spec.columnGroups.constant().empty()) {
        bind(scope, spec.columnGroups,
             [&widget](std::vector<TableColumnGroup> const& groups) { widget.setColumnGroups(groups); });
    }
    if (spec.onGroupFilter) {
        widget.setOnGroupFilter(event(spec.onGroupFilter));
    }
    if (spec.onCopy) {
        widget.setOnCopy(event(spec.onCopy));
    }
}

void Mounter::bindSelectionView(reactive::Scope& scope, Table const& spec, TableWidget& widget, bool& viewing) {
    if (spec.onStatusEntry) {
        widget.setOnStatusEntry(event(spec.onStatusEntry));
    }
    if (!spec.selectionView.isBound() && !spec.selectionView.constant()) {
        return;
    }
    bind(scope, spec.selectionView,
         [this, &widget, &viewing, selection = spec.selection](std::optional<std::vector<Key>> const& except) {
             if (except) {
                 viewing = true;
                 widget.setSelectionView(*except);
                 return;
             }
             if (!viewing) {
                 return;
             }
             viewing = false;
             widget.setSelection(selection.isBound() ? _rt->untracked([&] { return selection.binding()(); })
                                                     : selection.constant());
         });
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): mounts each table spec kind (rows, source, model) against one widget, in the order the rows then the selection must bind
Widget& Mounter::mountKind(reactive::Scope& scope, Table const& spec, ContainerWidget* parent) {
    if (spec.source) {
        return mountSourceTable(scope, spec, parent);
    }
    TableWidget& widget = scope.adopt(_backend->createTable(parent));
    applyCommon(scope, widget, spec.common);
    checkNoShare(spec.columns);
    widget.setColumns(spec.columns);
    widget.setShowHeader(spec.showHeader);
    widget.setSelectionMode(spec.selectionMode);
    if (spec.selectColumn) {
        widget.setSelectColumn(true);
        bind(scope, spec.selectAllLabel, [&widget](std::string const& label) { widget.setSelectAllLabel(label); });
    }
    widget.setOnActivate(event(spec.onActivate));
    if (spec.onSelectAll) {
        widget.setOnSelectAll(event(spec.onSelectAll));
    }
    bool const sortable = std::ranges::any_of(spec.columns, &TableColumn::sortable);
    reactive::Computed<std::vector<TableSort>> const* sortValue = nullptr;
    if (sortable) {
        sortValue = bind(scope, spec.sort, [&widget](std::vector<TableSort> const& chain) { widget.setSort(chain); });
        widget.setOnSort(event(spec.onSort));
    }
    bindTableConfig(scope, spec, widget);
    auto& spans = scope.make<SpanTable>();
    spans.columns = spec.columns.size();
    spans.threshold = spec.virtualize.threshold;
    // The sort counts where it is wired: a table with no sortable column shows no sort. It is read inside the
    // rows Effect, not in an Effect of its own, so that two Effects cannot resolve the spans in either order,
    // one of them against the wrong rows. A sort bound to a server query can still clear before the rows reorder.
    spans.sorted = sortable && sortValue == nullptr && !spec.sort.constant().empty();
    mountRows(
        scope, spec.rows, widget,
        [this, &widget, &spans](reactive::Scope& rowScope, Node const& node, Widget& rowWidget, Key const& key) {
            widget.setRowKey(rowWidget, key);
            // A row's tone binds in the row's scope, after its key and whether it is a heading.
            Row const* const view = node ? std::get_if<Row>(&node->kind) : nullptr;
            if (view != nullptr && view->heading) {
                widget.setRowHeading(rowWidget);
            }
            if (view != nullptr && (view->heading || view->spans)) {
                spans.rows[key].heading = view->heading;
                if (view->spans) {
                    bind(rowScope, *view->spans, [this, &widget, &spans, key](std::vector<RowSpan> const& asked) {
                        spans.rows[key].requested = asked;
                        spans.anyAsked = spans.anyAsked || !asked.empty();
                        resolveSpans(widget, spans);
                    });
                }
            }
            if (view != nullptr && view->tone) {
                bind(rowScope, *view->tone,
                     [&widget, &rowWidget](std::optional<Tone> const& tone) { widget.setRowTone(rowWidget, tone); });
            }
            if (view != nullptr && view->label) {
                bind(rowScope, *view->label,
                     [&widget, &rowWidget](std::string const& label) { widget.setRowLabel(rowWidget, label); });
            }
        },
        &spec.virtualize,
        RowsObserver{.track =
                         [&spans, sortValue] {
                             if (sortValue != nullptr) {
                                 spans.sorted = !sortValue->get().empty();
                             }
                         },
                     .begin = [&spans] { ++spans.reconciling; },
                     .end =
                         [this, &widget, &spans](std::vector<KeyedRow> const& rows) {
                             --spans.reconciling;
                             spans.order.clear();
                             for (KeyedRow const& row : rows) {
                                 spans.order.push_back(
                                     {.key = row.key,
                                      .widget = row.widgetState == RowWidgetState::Realized ? row.widget : nullptr});
                             }
                             std::unordered_set<Key> present;
                             for (KeyedRow const& row : rows) {
                                 present.insert(row.key);
                             }
                             std::erase_if(spans.rows,
                                           [&present](auto const& entry) { return !present.contains(entry.first); });
                             resolveSpans(widget, spans);
                         }});
    // While the selection is of the view the backend marks it, and the key list it holds is not sent over it.
    auto& viewing = scope.make<bool>(false);
    widget.setOnSelectionChange(inputEvent(
        spec.onSelectionChange, controlled(scope, spec.selection, [&widget, &viewing](std::vector<Key> const& keys) {
            if (!viewing) {
                widget.setSelection(keys);
            }
        })));
    bindSelectionView(scope, spec, widget, viewing);
    return widget;
}

void Mounter::reconcileWindowed(std::size_t depth, ForEachSession& session, KeyedRows& rows,
                                ContainerWidget& container, TableWidget& table, std::vector<Key> const& keys,
                                RowHook const& onRow, VisibleRange const& window, Virtualization const& setting,
                                bool windowed) {
    if (keys.size() < setting.threshold) {
        reconcile(depth, session, rows.list, container, keys, onRow);
        if (windowed) {
            // Rows released while the table was windowed come back: below the threshold every row is realized.
            applyWindow(depth, rows.list, container, {.first = 0, .count = rows.list.size()}, 0, onRow);
        }
        return;
    }
    std::pair<VisibleRange, std::size_t> const windowAndOverscan{window, setting.overscan};
    reconcile(depth, session, rows.list, container, keys, onRow, &windowAndOverscan);
    // The kept rows have new view positions, and reconcile realizes only new keys: apply the window again so the
    // realized rows are the ones now at the window's view positions.
    applyWindow(depth, rows.list, container, window, setting.overscan, onRow);
    // Only above threshold: a backend's own selection-filtering logic may need to know a key still exists even
    // without a realized row for it right now (backend.hpp's setExistingKeys). Below threshold every key already
    // has a realized row, so this would add nothing.
    table.setExistingKeys(keys);
}

void Mounter::publishRealizedWindow(TableWidget& table, KeyedRows const& rows, std::size_t keyCount,
                                    Virtualization const& setting, bool& windowed) {
    if (keyCount >= setting.threshold) {
        auto const [first, count] = realizedExtent(rows.list);
        table.setRealizedWindow(first, count, rows.list.size());
        windowed = true;
    } else if (windowed) {
        table.setRealizedWindow(0, rows.list.size(), rows.list.size());
        windowed = false;
    }
}

Mounter::SourceTable::~SourceTable() {
    // The rows' widgets go one by one, and the backend measures the table as they do: `closing` ends every
    // window pass that asks, since the span table and whatever else the mount made after this one is gone.
    closing = true;
    focusLocate.request_stop();
    while (!realized.empty()) {
        realized.pop_back();
    }
}

CellLoad Mounter::cellLoadOf(SourceTable const& table, table::UnloadedLevels const& levels, std::size_t column) {
    if (column >= table.source.levels.size() || table.source.levels.at(column).empty()) {
        return CellLoad::Loaded;
    }
    std::string const& level = table.source.levels.at(column);
    if (std::ranges::contains(levels.failed, level)) {
        return CellLoad::Failed;
    }
    return std::ranges::contains(levels.levels, level) ? CellLoad::Unloaded : CellLoad::Loaded;
}

Mounter::SourceTable::PoolKey Mounter::poolOf(SourceTable const& table, std::size_t column) {
    bool const own = !table.source.defaultCellView ||
                     (column < table.source.ownCell.size() ? table.source.ownCell.at(column) : true);
    if (own) {
        return {false, column};
    }
    return {true, static_cast<std::size_t>(table.columns.at(column).role)};
}

int Mounter::contentWidthOf(ColumnMeasure const& known, TableMeasure const& measure) {
    int const measured = std::max(known.natural, known.header);
    return measured > 0 ? measured : measure.contentEstimate;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): takes a new arrangement: the visible columns, their places and their widths, declared and user's, each step reading the previous one's result
void Mounter::arrange(SourceTable& table, TableArrangement arrangement) {
    table.arrangement = std::move(arrangement);
    std::vector<std::size_t> const before = std::exchange(table.visible, {});
    table.placeOf.assign(table.columns.size(), std::nullopt);
    auto const add = [&table](std::size_t column) {
        if (column < table.columns.size() && !table.placeOf.at(column)) {
            table.placeOf.at(column) = table.visible.size();
            table.visible.push_back(column);
        }
    };
    if (table.arrangement.visible.empty()) {
        for (std::size_t column = 0; column < table.columns.size(); ++column) {
            add(column);
        }
    } else {
        for (std::size_t const column : table.arrangement.visible) {
            add(column);
        }
    }
    if (table.visible != before || !table.visibleTold) {
        table.visibleTold = true;
        table.widget->setVisibleColumns(table.visible);
    }
    // Widths in the backend's measure: a fixed column's units and a user's width scaled by it, with the padding a
    // column takes beyond its content, a content column from the backend's estimate. Monospace characters are
    // rounded up, so a column of n of them never cuts the last.
    TableMeasure const measure = table.widget->tableMeasure();
    table.measure = measure;
    table.measures.resize(table.columns.size());
    auto const scaled = [&measure, &table](std::size_t column, int units) {
        bool const mono = table.columns.at(column).role == TextRole::Mono;
        double const content = units * (mono ? measure.monoUnit : measure.unit);
        return (mono ? static_cast<int>(std::ceil(content)) : static_cast<int>(std::lround(content))) +
               measure.padding;
    };
    std::vector<int> widths;
    widths.reserve(table.visible.size());
    for (std::size_t const column : table.visible) {
        Sizing const& width = table.columns.at(column).width;
        ColumnMeasure const& known = table.measures.at(column);
        widths.push_back(width.kind == Sizing::Kind::Fixed ? scaled(column, width.amount)
                                                           : contentWidthOf(known, measure));
    }
    table.extents->assign(widths);
    table.stretchPlaces.clear();
    for (std::size_t place = 0; place < table.visible.size(); ++place) {
        std::size_t const column = table.visible.at(place);
        if (table.columns.at(column).width.kind == Sizing::Kind::Stretch &&
            !table.arrangement.widths.contains(column)) {
            table.stretchPlaces.push_back(place);
        }
    }
    table.stretchDirty = true;
    if (table.spans != nullptr) {
        table.spans->placeOf = table.placeOf;
    }
    for (auto const& [column, width] : table.arrangement.widths) {
        if (column < table.placeOf.size()) {
            if (auto const place = table.placeOf.at(column)) {
                table.extents->set(*place, scaled(column, width));
            }
        }
    }
    table.windowDirty = true;
}

std::vector<std::string> Mounter::fetchedOf(SourceTable const& table) {
    std::vector<std::string> ids = table.source.alwaysFetched;
    auto const add = [&ids](std::string const& columnId) {
        if (!columnId.empty() && !std::ranges::contains(ids, columnId)) {
            ids.push_back(columnId);
        }
    };
    for (std::size_t const column : table.window) {
        add(table.columns.at(column).id);
        if (column < table.source.cellReads.size()) {
            for (std::string const& columnId : table.source.cellReads.at(column)) {
                add(columnId);
            }
        }
    }
    return ids;
}

VisibleRange Mounter::shownOf(SourceTable const& table) {
    return table.range.count > 0 || !table.source.alwaysWindowed
               ? table.range
               : VisibleRange{.first = 0, .count = table.virtualize.overscan};
}

void Mounter::tellVisible(SourceTable& table) {
    VisibleRange const shown = shownOf(table);
    table.source.rows->visible(static_cast<std::int64_t>(shown.first), static_cast<std::int64_t>(shown.count),
                               table.fetched);
}

void Mounter::refreshColumns(SourceTable& table, std::size_t rows) {
    if (table.stretchDirty) {
        fitStretch(table);
    }
    bool const windowed =
        table.horizontal && windowingOf(table.virtualize, rows, table.visible.size(), /*serverMode*/ true).columns;
    if (!table.windowDirty && windowed == table.windowedColumns) {
        return;
    }
    table.windowDirty = false;
    table.windowedColumns = windowed;
    ColumnExtents const& extents = *table.extents;
    std::size_t pinned = std::min(table.arrangement.pinned, table.visible.size());
    // Pinned columns wider than half the viewport leave the last of them to scroll with the rest; the
    // arrangement keeps its count.
    if (table.horizontal) {
        while (pinned > 0 && extents.offsetOf(pinned) > table.viewport / 2) {
            --pinned;
        }
    }
    std::vector<std::size_t> places;
    if (windowed) {
        places = columnWindow(extents, pinned, table.scrollX, table.viewport, table.virtualize.columnOverscan).columns;
    } else {
        places.resize(table.visible.size());
        // NOLINTNEXTLINE(modernize-use-ranges): std::ranges::iota is missing from Emscripten's libc++.
        std::iota(places.begin(), places.end(), std::size_t{0});
    }
    std::vector<std::size_t> window;
    window.reserve(places.size() + 1);
    for (std::size_t const place : places) {
        window.push_back(table.visible.at(place));
    }
    tellBands(table, places, pinned);
    // The focused column stays realized whatever the scroll.
    std::optional<std::size_t> const focusPlace =
        table.focused && table.focusColumn < table.placeOf.size() ? table.placeOf.at(table.focusColumn) : std::nullopt;
    if (focusPlace && !std::ranges::contains(window, table.focusColumn)) {
        window.push_back(table.focusColumn);
        places.push_back(*focusPlace);
    }
    bool const changed = window != table.window || pinned != table.pinnedShown;
    table.window = std::move(window);
    table.pinnedShown = pinned;
    if (arranged(table)) {
        std::vector<int> offsets;
        std::vector<int> widths;
        offsets.reserve(places.size());
        widths.reserve(places.size());
        for (std::size_t const place : places) {
            offsets.push_back(extents.offsetOf(place));
            widths.push_back(extents.widthOf(place));
        }
        table.widget->setColumnWindow(table.window, offsets, widths, extents.totalWidth(), table.pinnedShown);
    }
    if (changed) {
        std::vector<std::string> fetched = fetchedOf(table);
        if (fetched != table.fetched) {
            table.fetched = std::move(fetched);
            if (table.mounted) {
                tellVisible(table);
            }
        }
    }
}

std::size_t Mounter::firstFullPlace(SourceTable const& table) {
    ColumnExtents const& extents = *table.extents;
    std::size_t firstFull = extents.columnsAt(table.scrollX, 1).first;
    if (extents.offsetOf(firstFull) < table.scrollX) {
        ++firstFull;
    }
    return std::max(firstFull, table.pinnedShown);
}

void Mounter::fitStretch(SourceTable& table) {
    table.stretchDirty = false;
    if (!table.horizontal || table.stretchPlaces.empty()) {
        return;
    }
    ColumnExtents& extents = *table.extents;
    int const shrinkFloor = table.measure.shrinkFloor;
    std::vector<StretchColumn> columns;
    columns.reserve(table.stretchPlaces.size());
    std::int64_t held = 0;
    for (std::size_t const place : table.stretchPlaces) {
        TableColumn const& column = table.columns.at(table.visible.at(place));
        ColumnMeasure const& known = table.measures.at(table.visible.at(place));
        int const natural = std::max(known.header, known.natural);
        columns.push_back(StretchColumn{.natural = natural,
                                        .least = column.wrap ? std::max(known.header, known.least) : natural,
                                        .floor = column.shrink ? std::min(known.header, shrinkFloor) : known.header,
                                        .weight = column.width.amount,
                                        .shrink = column.shrink});
        held += extents.offsetOf(place + 1) - extents.offsetOf(place);
    }
    std::int64_t const room = std::int64_t{table.viewport} - (extents.totalWidth() - held);
    std::vector<int> const widths =
        stretchWidths(columns, static_cast<int>(std::clamp<std::int64_t>(room, 0, std::numeric_limits<int>::max())));
    std::size_t const firstFull = firstFullPlace(table);
    int moved = 0;
    bool changed = false;
    for (auto const [place, width] : std::views::zip(table.stretchPlaces, widths)) {
        int const before = extents.offsetOf(place + 1) - extents.offsetOf(place);
        if (extents.fit(place, width)) {
            changed = true;
            if (place >= table.pinnedShown && place < firstFull) {
                moved += width - before;
            }
        }
    }
    if (!changed) {
        return;
    }
    table.windowDirty = true;
    if (moved != 0) {
        table.scrollX = std::max(table.scrollX + moved, 0);
        table.widget->setHorizontalOffset(table.scrollX);
    }
}

void Mounter::tellBands(SourceTable& table, std::vector<std::size_t> const& places, std::size_t pinned) {
    if (table.bands.empty()) {
        return;
    }
    std::vector<std::string> ids;
    ids.reserve(table.visible.size());
    for (std::size_t const column : table.visible) {
        ids.push_back(table.columns.at(column).id);
    }
    std::vector<bool> inWindow(table.visible.size(), false);
    for (std::size_t const place : places) {
        inWindow.at(place) = true;
    }
    std::vector<BandRun> runs;
    for (BandRun const& run : bandRuns(table.bands, ids, pinned)) {
        std::optional<std::size_t> first;
        for (std::size_t at = run.first; at <= run.first + run.count; ++at) {
            bool const shown = at < run.first + run.count && inWindow.at(at);
            if (shown && !first) {
                first = at;
            } else if (!shown && first) {
                BandRun part = run;
                part.first = *first;
                part.count = at - *first;
                runs.push_back(std::move(part));
                first.reset();
            }
        }
    }
    if (!table.bandsTold || *table.bandsTold != runs) {
        table.widget->setBands(runs);
        table.bandsTold = std::move(runs);
    }
}

Mounter::SourceTable::Cell Mounter::makeCell(SourceTable& table, SourceTable::Realized& row, std::size_t column,
                                             Value const& record) {
    SourceTable::Cell cell;
    cell.pool = poolOf(table, column);
    cell.scope = std::make_unique<reactive::Scope>(*_rt, table.depth + 1);
    reactive::Scope& scope = *cell.scope;
    cell.row = &scope.make<reactive::Signal<Value>>(*_rt, record);
    Node made;
    if (cell.pool.first) {
        cell.column = &scope.make<reactive::Signal<std::size_t>>(*_rt, column);
        made = table.source.defaultCellView(*cell.column, *cell.row);
    } else {
        made = table.source.cellView(column, *cell.row);
    }
    Node const& node = scope.make<Node>(std::move(made));
    auto* const stack = dynamic_cast<ContainerWidget*>(row.widget);
    // A marked cell's content is the cell, which may be accented; an accented cell mounts its content as the
    // cell. The marks and the accent are bound in the cell's scope and told once the cell is attached to a row
    // (`realizeCell`), so a cell built for a pool tells nothing yet.
    MarkedCell const* const marked = node ? std::get_if<MarkedCell>(&node->kind) : nullptr;
    Node const& inner = marked != nullptr ? marked->content : node;
    AccentedCell const* const accented = inner ? std::get_if<AccentedCell>(&inner->kind) : nullptr;
    Widget* const widget = inner ? mount(scope, accented != nullptr ? accented->content : inner, stack)
                                 : &scope.adopt(_backend->createSpacer(stack));
    if (marked != nullptr) {
        auto& marks = scope.make<SourceTable::Marks>();
        cell.marks = &marks;
        bind(scope, marked->marks, [&table, &marks](CellMarks const& value) {
            marks.last = value;
            if (marks.row != nullptr) {
                table.widget->setCellMarks(*marks.row, marks.column, value);
            }
        });
    }
    if (accented != nullptr) {
        auto& accent = scope.make<SourceTable::Accent>();
        cell.accent = &accent;
        bind(scope, accented->accent, [&table, &accent](CellAccent const& value) {
            accent.last = value;
            if (accent.row != nullptr) {
                table.widget->setCellAccent(*accent.row, accent.column, value);
            }
        });
    }
    cell.widget = widget;
    row.children.push_back(widget);
    ++_stats.cellsCreated;
    return cell;
}

void Mounter::setRecord(reactive::Signal<Value>& signal, Value const& record) {
    if (signal.peek() != record) {
        ++_stats.cellBinds;
        signal.set(record);
    }
}

void Mounter::assignRow(SourceTable& table, SourceTable::Realized& slot, std::int64_t index, TableRowRef const& ref) {
    if (slot.key && *slot.key != ref.key) {
        closeEditor(table, slot);
        dropRowScoped(table, slot);
    }
    slot.index = index;
    slot.key = ref.key;
    if (slot.row->peek() != ref.row) {
        ++_stats.rowBinds;
        slot.row->set(ref.row);
    }
    if (slot.headingWidget != nullptr) {
        std::erase(slot.children, slot.headingWidget);
        slot.headingWidget = nullptr;
    }
    slot.look.reset();
    slot.look = std::make_unique<reactive::Scope>(*_rt, table.depth + 1);
    reactive::Scope& lookScope = *slot.look;
    TableRowLook look;
    if (table.source.rowView) {
        look = table.source.rowView(*slot.row);
        lookScope.make<std::shared_ptr<void const>>(look.keep);
    }
    table.widget->setRowKey(*slot.widget, *slot.key);
    slot.summary = look.summary;
    if (look.summary) {
        table.widget->setRowSummary(*slot.widget);
    }
    slot.heading = look.heading != nullptr;
    if (slot.heading) {
        Node const& heading = lookScope.make<Node>(look.heading);
        if (Widget* const made = mount(lookScope, heading, dynamic_cast<ContainerWidget*>(slot.widget))) {
            slot.children.push_back(made);
            slot.headingWidget = made;
        }
        table.widget->setRowHeading(*slot.widget);
    }
    if (table.spans != nullptr && (look.heading || look.spans)) {
        SpanTable& spans = *table.spans;
        spans.rows[*slot.key].heading = static_cast<bool>(look.heading);
        if (look.spans) {
            bind(lookScope, *look.spans, [this, &table, &spans, key = *slot.key](std::vector<RowSpan> const& asked) {
                spans.rows[key].requested = asked;
                spans.anyAsked = spans.anyAsked || !asked.empty();
                resolveSpans(*table.widget, spans);
            });
        }
    }
    if (look.tone) {
        bind(lookScope, *look.tone, [&table, widget = slot.widget](std::optional<Tone> const& tone) {
            table.widget->setRowTone(*widget, tone);
        });
    }
    if (look.label) {
        bind(lookScope, *look.label,
             [&table, widget = slot.widget](std::string const& label) { table.widget->setRowLabel(*widget, label); });
    }
    if (look.marks) {
        bind(lookScope, *look.marks,
             [&table, widget = slot.widget](RowMarks const& marks) { table.widget->setRowMarks(*widget, marks); });
    }
    if (look.modified) {
        bind(lookScope, *look.modified,
             [&table, widget = slot.widget](bool modified) { table.widget->setRowModified(*widget, modified); });
    }
    for (auto& [column, placed] : slot.cells) {
        placed.fresh = false;
    }
}

void Mounter::closeEditor(SourceTable& table, SourceTable::Realized& slot) {
    if (slot.editor == nullptr) {
        return;
    }
    Widget const* const editor = std::exchange(slot.editorWidget, nullptr);
    table.widget->setCellEditor(*slot.widget, slot.editorColumn, nullptr);
    std::erase(slot.children, editor);
    slot.editor.reset();
}

void Mounter::syncEditor(SourceTable& table) {
    std::optional<std::size_t> column;
    if (table.editWanted) {
        auto const found = std::ranges::find(table.columns, table.editWanted->column, &TableColumn::id);
        if (found != table.columns.end()) {
            column = static_cast<std::size_t>(found - table.columns.begin());
        }
    }
    for (SourceTable::Realized& slot : table.realized) {
        bool const wanted =
            column && slot.key && table.editWanted && *slot.key == table.editWanted->key && !slot.heading;
        if (slot.editor != nullptr && (!wanted || slot.editorColumn != *column)) {
            closeEditor(table, slot);
        }
        if (!wanted || slot.editor != nullptr || !table.editor) {
            continue;
        }
        auto scope = std::make_unique<reactive::Scope>(*_rt, table.depth + 1);
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access): `wanted` holds only while an edit is wanted.
        Node const& node = scope->make<Node>(table.editor(*table.editWanted));
        Widget* const made =
            mountPass([&] { return mount(*scope, node, dynamic_cast<ContainerWidget*>(slot.widget)); });
        if (made == nullptr) {
            continue;
        }
        slot.editor = std::move(scope);
        slot.editorWidget = made;
        slot.editorColumn = *column;
        slot.children.push_back(made);
        table.widget->setCellEditor(*slot.widget, *column, made);
        try {
            made->focus();
        } catch (...) {  // NOLINT(bugprone-empty-catch)
            // Focus is a request: a backend that cannot move it leaves it where it was.
        }
    }
}

Mounter::SourceTable::Realized Mounter::makeRow(SourceTable& table, std::int64_t index) {
    SourceTable::Realized made;
    made.index = index;
    made.scope = std::make_unique<reactive::Scope>(*_rt, table.depth);
    StackWidget& stack = made.scope->adopt(_backend->createStack(table.widget, Axis::Horizontal));
    made.scope->make<OffChain>(_offChain, &stack);
    made.widget = &stack;
    std::optional<TableRowRef> const ref = table.source.rows->row(index);
    made.row = &made.scope->make<reactive::Signal<Value>>(*_rt, ref ? ref->row : Value{});
    if (ref) {
        assignRow(table, made, index, *ref);
    }
    return made;
}

void Mounter::park(SourceTable& table, SourceTable::Realized& slot, std::size_t column) {
    auto const found = slot.cells.find(column);
    if (found == slot.cells.end()) {
        return;
    }
    SourceTable::Cell cell = std::move(found->second.cell);
    slot.cells.erase(found);
    if (cell.accent != nullptr) {
        cell.accent->row = nullptr;
    }
    if (cell.marks != nullptr) {
        cell.marks->row = nullptr;
    }
    table.widget->setCellColumn(*slot.widget, *cell.widget, std::nullopt);
    slot.pool[cell.pool].push_back(std::move(cell));
}

void Mounter::realizeCell(SourceTable& table, SourceTable::Realized& slot, std::size_t column, Value const& record) {
    if (auto const placed = slot.cells.find(column); placed != slot.cells.end()) {
        setRecord(*placed->second.cell.row, record);
        placed->second.fresh = true;
        ++_stats.cellsRebound;
        return;
    }
    SourceTable::PoolKey const key = poolOf(table, column);
    SourceTable::Cell cell;
    if (auto const pooled = slot.pool.find(key); pooled != slot.pool.end() && !pooled->second.empty()) {
        cell = std::move(pooled->second.back());
        pooled->second.pop_back();
        if (cell.accent != nullptr) {
            cell.accent->row = nullptr;
        }
        if (cell.marks != nullptr) {
            cell.marks->row = nullptr;
        }
        if (cell.column != nullptr && cell.column->peek() != column) {
            cell.column->set(column);
        }
        setRecord(*cell.row, record);
        ++_stats.cellsRebound;
    } else {
        cell = mountPass([&] { return makeCell(table, slot, column, record); });
    }
    table.widget->setCellColumn(*slot.widget, *cell.widget, column);
    // Attached to this row at this column: the accent is told here whether or not it changed.
    if (cell.accent != nullptr) {
        cell.accent->row = slot.widget;
        cell.accent->column = column;
        table.widget->setCellAccent(*slot.widget, column, cell.accent->last);
    }
    // The marks likewise: a cell rebound to another row re-evaluates them with its content, and this row is told
    // them even when they equal the ones it showed.
    if (cell.marks != nullptr) {
        cell.marks->row = slot.widget;
        cell.marks->column = column;
        table.widget->setCellMarks(*slot.widget, column, cell.marks->last);
    }
    slot.cells.emplace(column, SourceTable::Placed{.cell = std::move(cell), .fresh = true});
}

void Mounter::tellLoads(SourceTable& table, SourceTable::Realized& slot) {
    table::UnloadedLevels const levels = table.source.rows->unloadedLevels(slot.index);
    for (auto const& [column, placed] : slot.cells) {
        if (!placed.fresh) {
            continue;
        }
        CellLoad const load = cellLoadOf(table, levels, column);
        auto const [was, made] = slot.loads.try_emplace(column, CellLoad::Loaded);
        if (was->second != load) {
            was->second = load;
            table.widget->setCellLoad(*slot.widget, column, load);
        }
    }
}

void Mounter::realizeWindow(SourceTable& table, std::function<bool(std::int64_t)> const& updated) {
    if (table.closing) {
        return;
    }
    if (table.realizing) {
        table.realizeAgain = true;
        return;
    }
    table.realizing = true;
    try {
        realizeWindowPass(table, updated);
        while (std::exchange(table.realizeAgain, false) && !table.closing) {
            realizeWindowPass(table, [](std::int64_t) { return true; });
        }
    } catch (...) {
        table.realizing = false;
        table.realizeAgain = false;
        throw;
    }
    table.realizing = false;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): rows, cells, the budget, focus and the reports.
void Mounter::realizeWindowPass(SourceTable& table, std::function<bool(std::int64_t)> const& updated) {
    TableRows const& rows = *table.source.rows;
    TableTotal const total = rows.mountedTotal();
    auto const count = static_cast<std::size_t>(std::max<std::int64_t>(total.rows, 0));
    refreshColumns(table, count);
    // A short table over a document's rows realizes every row. A windowed one realizes the rows in view and
    // `overscan` either side.
    bool const everyRow = !table.source.alwaysWindowed && count < table.virtualize.threshold;
    VisibleRange const shown = shownOf(table);
    std::size_t const overscan = table.virtualize.overscan;
    std::size_t const low = everyRow || shown.first <= overscan ? 0 : shown.first - overscan;
    std::size_t const high = everyRow ? count : std::min(count, shown.first + shown.count + overscan);
    std::vector<std::int64_t> targets;
    targets.reserve(high > low ? high - low + 1 : 1);
    for (std::size_t at = low; at < high; ++at) {
        targets.push_back(static_cast<std::int64_t>(at));
    }
    // The focused row stays realized: one row beyond the window at most.
    if (table.focused && table.focusIndex >= 0 && std::cmp_less(table.focusIndex, count) &&
        (std::cmp_less(table.focusIndex, low) || std::cmp_greater_equal(table.focusIndex, high))) {
        targets.insert(std::ranges::upper_bound(targets, table.focusIndex), table.focusIndex);
    }
    if (table.spans != nullptr) {
        ++table.spans->reconciling;
    }
    std::vector<SourceTable::Realized> old = std::move(table.realized);
    table.realized.clear();
    std::unordered_map<Key, std::size_t> keyed;
    std::unordered_map<std::int64_t, std::size_t> placeholders;
    for (std::size_t at = 0; at < old.size(); ++at) {
        if (old.at(at).key) {
            keyed.emplace(*old.at(at).key, at);
        } else {
            placeholders.emplace(old.at(at).index, at);
        }
    }
    std::vector<Widget*> current;
    current.reserve(old.size());
    for (SourceTable::Realized const& row : old) {
        current.push_back(row.widget);
    }
    std::vector<std::optional<std::size_t>> match(targets.size());
    std::vector<bool> taken(old.size(), false);
    for (std::size_t at = 0; at < targets.size(); ++at) {
        std::optional<TableRowRef> const ref = rows.row(targets.at(at));
        if (ref) {
            if (auto const found = keyed.find(ref->key); found != keyed.end() && !taken.at(found->second)) {
                match.at(at) = found->second;
            }
        } else if (auto const found = placeholders.find(targets.at(at));
                   found != placeholders.end() && !taken.at(found->second)) {
            match.at(at) = found->second;
        }
        if (match.at(at)) {
            taken.at(*match.at(at)) = true;
        }
    }
    // Slots no row kept, for the rows that enter: keyed ones only for a keyed row, so no stack keeps a key it no
    // longer shows; a placeholder's for either. A heading's or a summary row's slot is never given another row:
    // the backend has no call that makes a row a data row again, so it goes, and the entering row gets a new one.
    std::vector<std::size_t> freeKeyed;
    std::vector<std::size_t> freePlain;
    for (std::size_t at = old.size(); at-- > 0;) {
        if (!taken.at(at) && !old.at(at).heading && !old.at(at).summary) {
            (old.at(at).key ? freeKeyed : freePlain).push_back(at);
        }
    }
    std::exception_ptr failure;
    std::vector<SourceTable::Realized> next;
    next.reserve(targets.size());
    std::vector<bool> const moved;
    for (std::size_t at = 0; at < targets.size(); ++at) {
        std::int64_t const index = targets.at(at);
        std::optional<TableRowRef> const ref = rows.row(index);
        if (match.at(at)) {
            SourceTable::Realized row = std::move(old.at(*match.at(at)));
            bool const shifted = row.index != index;
            row.index = index;
            if (ref && (shifted || updated(index))) {
                if (row.row->peek() != ref->row) {
                    ++_stats.rowBinds;
                    row.row->set(ref->row);
                }
                for (auto& [column, placed] : row.cells) {
                    if (placed.fresh) {
                        setRecord(*placed.cell.row, ref->row);
                    }
                }
            }
            next.push_back(std::move(row));
            continue;
        }
        try {
            std::vector<std::size_t>* pool = !freePlain.empty() ? &freePlain : nullptr;
            if (ref && !freeKeyed.empty()) {
                pool = &freeKeyed;
            }
            if (pool != nullptr) {
                SourceTable::Realized row = std::move(old.at(pool->back()));
                pool->pop_back();
                row.index = index;
                if (ref) {
                    mountPass([&] {
                        assignRow(table, row, index, *ref);
                        return 0;
                    });
                }
                next.push_back(std::move(row));
            } else {
                next.push_back(mountPass([&] { return makeRow(table, index); }));
                current.push_back(next.back().widget);
            }
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
    }
    // What was not taken goes, last first; the container forgets its widgets.
    for (SourceTable::Realized& row : old | std::views::reverse) {
        if (row.scope) {
            std::erase(current, row.widget);
            row.editor.reset();
            row.pool.clear();
            row.cells.clear();
            row.look.reset();
            row.scope.reset();
        }
    }
    std::vector<Widget*> target;
    target.reserve(next.size());
    for (SourceTable::Realized const& row : next) {
        target.push_back(row.widget);
    }
    try {
        reorderChildren(*table.widget, current, target);
    } catch (...) {
        if (!failure) {
            failure = std::current_exception();
        }
    }
    table.realized = std::move(next);
    for (SourceTable::Realized& row : table.realized) {
        if (row.toldIndex != row.index) {
            row.toldIndex = row.index;
            table.widget->setRowIndex(*row.widget, static_cast<std::size_t>(row.index));
        }
    }
    if (!table.source.alwaysWindowed && !everyRow) {
        // A backend's own selection needs to know a key exists without a realized row for it.
        std::vector<Key> keys;
        keys.reserve(count);
        for (std::size_t at = 0; at < count; ++at) {
            if (std::optional<TableRowRef> const ref = rows.row(static_cast<std::int64_t>(at))) {
                keys.push_back(ref->key);
            }
        }
        table.widget->setExistingKeys(keys);
    }
    if (table.spans != nullptr) {
        SpanTable& spans = *table.spans;
        --spans.reconciling;
        spans.windowed = !everyRow;
        spans.order.clear();
        std::unordered_set<Key> present;
        for (SourceTable::Realized const& row : table.realized) {
            if (row.key) {
                spans.order.push_back({.key = *row.key, .widget = row.widget});
                present.insert(*row.key);
            }
        }
        std::erase_if(spans.rows, [&present](auto const& entry) { return !present.contains(entry.first); });
        resolveSpans(*table.widget, spans);
    }
    realizeCells(table, shown);
    syncEditor(table);
    // The contiguous range only: a focused row kept beyond it is placed by its own `setRowIndex`. Every source
    // table is told it, one that realizes every row as the whole view: a backend takes the view's count from it.
    // Whether spans are drawn is the span table's (`SpanTable::windowed`), not this call's.
    table.widget->setRealizedWindow(low, high > low ? high - low : 0, count);
    afterPass(table, total);
    table.lastCount = count;
    if (failure) {
        std::rethrow_exception(failure);
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): pooling, the budget, and the order in each row.
void Mounter::realizeCells(SourceTable& table, VisibleRange const& shown) {
    TableRows const& rows = *table.source.rows;
    std::unordered_set<std::size_t> const inWindow(table.window.begin(), table.window.end());
    auto const centreRow = static_cast<std::int64_t>(shown.first + (shown.count / 2));
    std::size_t const centreColumn = table.window.empty() ? 0 : table.window.size() / 2;
    std::vector<CellWork> work;
    for (std::size_t at = 0; at < table.realized.size(); ++at) {
        SourceTable::Realized& slot = table.realized.at(at);
        bool const keyed = slot.key.has_value() && !slot.heading;
        std::vector<std::size_t> leaving;
        for (auto const& [column, placed] : slot.cells) {
            if (!keyed || !inWindow.contains(column)) {
                leaving.push_back(column);
            }
        }
        for (std::size_t const column : leaving) {
            park(table, slot, column);
        }
        // A row the rows do not hold now takes none of the budget: its cells wait for its arrival, which is a
        // change of the rows and so a pass of its own.
        if (!keyed || !rows.row(slot.index)) {
            continue;
        }
        auto const rowDistance = static_cast<std::size_t>(std::abs(slot.index - centreRow));
        for (std::size_t place = 0; place < table.window.size(); ++place) {
            std::size_t const column = table.window.at(place);
            auto const placed = slot.cells.find(column);
            if (placed != slot.cells.end() && placed->second.fresh) {
                continue;
            }
            std::size_t const columnDistance = place > centreColumn ? place - centreColumn : centreColumn - place;
            work.push_back(
                CellWork{.slot = at, .column = column, .distance = (rowDistance * 1'000'000) + columnDistance});
        }
    }
    std::ranges::stable_sort(work, {}, &CellWork::distance);
    std::size_t const budget = table.virtualize.realizePerFrame;
    std::exception_ptr failure;
    std::size_t done = 0;
    std::unordered_set<std::size_t> waiting;
    for (std::size_t at = 0; at < work.size(); ++at) {
        SourceTable::Realized& slot = table.realized.at(work.at(at).slot);
        std::size_t const column = work.at(at).column;
        if (at >= budget) {
            // Past the budget: a stale cell leaves its column blank rather than show another row.
            park(table, slot, column);
            waiting.insert(work.at(at).slot);
            ++_stats.placeholders;
            continue;
        }
        std::optional<TableRowRef> const ref = rows.row(slot.index);
        if (!ref) {
            continue;
        }
        try {
            realizeCell(table, slot, column, ref->row);
            ++done;
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
    }
    for (std::size_t at = 0; at < table.realized.size(); ++at) {
        SourceTable::Realized& slot = table.realized.at(at);
        // A row with nothing left to realize keeps no pooled cell: the widgets alive are the cells in the window.
        if (!waiting.contains(at)) {
            dropPools(slot);
        }
        tellLoads(table, slot);
        orderCells(table, slot);
    }
    // A pass that realized nothing (every build it tried threw) posts no other: it would fail the same way.
    if (work.size() > budget && done > 0) {
        continueLater(table);
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
}

void Mounter::dropRowScoped(SourceTable const& table, SourceTable::Realized& slot) {
    std::erase_if(slot.cells, [&table, &slot](auto const& entry) {
        if (!rowScoped(table, entry.first)) {
            return false;
        }
        std::erase(slot.children, entry.second.cell.widget);
        return true;
    });
    std::erase_if(slot.pool, [&table, &slot](auto const& entry) {
        if (entry.first.first || !rowScoped(table, entry.first.second)) {
            return false;
        }
        for (SourceTable::Cell const& cell : entry.second) {
            std::erase(slot.children, cell.widget);
        }
        return true;
    });
}

void Mounter::dropPools(SourceTable::Realized& slot) {
    for (auto& [key, cells] : slot.pool) {
        for (SourceTable::Cell const& cell : cells) {
            std::erase(slot.children, cell.widget);
        }
    }
    slot.pool.clear();
}

void Mounter::orderCells(SourceTable const& table, SourceTable::Realized& slot) {
    if (slot.heading || slot.children.empty()) {
        return;
    }
    std::vector<Widget*> target;
    target.reserve(slot.children.size());
    for (std::size_t const column : table.window) {
        if (auto const placed = slot.cells.find(column); placed != slot.cells.end()) {
            target.push_back(placed->second.cell.widget);
        }
    }
    std::ranges::copy_if(slot.children, std::back_inserter(target),
                         [&target](Widget const* child) { return !std::ranges::contains(target, child); });
    if (target != slot.children) {
        reorderChildren(*dynamic_cast<ContainerWidget*>(slot.widget), slot.children, target);
    }
}

void Mounter::continueLater(SourceTable& table) {
    if (table.continuing) {
        return;
    }
    table.continuing = true;
    try {
        _rt->owner().post([this, &table, alive = std::weak_ptr{table.alive}] {
            if (alive.expired()) {
                return;
            }
            table.continuing = false;
            static_cast<void>(runCallback(*_rt, [&] { realizeWindow(table, [](std::int64_t) { return false; }); }));
        });
    } catch (...) {
        table.continuing = false;
        throw;
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity, readability-convert-member-functions-to-static): the pass's reports (counts, focused cell, highlighted row) each read the same table state in turn
void Mounter::afterPass(SourceTable& table, TableTotal const& total) {
    std::pair<std::optional<std::int64_t>, std::size_t> const counts{
        total.known && total.kind == table::TotalKind::Exact ? std::optional{total.rows} : std::nullopt,
        table.visible.size()};
    if (table.counts != counts) {
        table.counts = counts;
        table.widget->setCounts(counts.first, counts.second);
    }
    if (table.highlightKey && table.highlightSearching) {
        if (std::optional<std::int64_t> const now = placeOfKey(table, *table.highlightKey, table.highlightIndex)) {
            table.highlightSearching = false;
            table.highlightIndex = *now;
            table.widget->setHighlightedRow(static_cast<std::size_t>(*now));
        }
    }
    if (table.focused && table.focusSearching && table.focusKey) {
        if (std::optional<std::int64_t> const now = placeOfKey(table, *table.focusKey, table.focusIndex)) {
            table.focusLocate.request_stop();
            table.focusSearching = false;
            table.focusIndex = *now;
            table.widget->setFocusCell(static_cast<std::size_t>(*now), table.focusColumn);
        }
        return;
    }
    if (table.focused && table.focusUntold && total.known) {
        if (table.focusLeft) {
            table.focusIndex = std::min(table.focusIndex, std::max<std::int64_t>(total.rows - 1, 0));
        }
        if (table.focusIndex < total.rows) {
            table.focusUntold = false;
            tellLocatedFocus(table);
        }
    }
    if (table.focused && table.focusUnconfirmed && table.focusKey) {
        std::optional<TableRowRef> const ref = table.source.rows->row(table.focusIndex);
        std::optional<std::int64_t> const now = ref && ref->key == *table.focusKey
                                                    ? std::optional{table.focusIndex}
                                                    : placeOfKey(table, *table.focusKey, table.focusIndex);
        if (now) {
            table.focusUnconfirmed = false;
            table.focusIndex = *now;
            table.widget->setFocusCell(static_cast<std::size_t>(*now), table.focusColumn);
        }
    }
    if (table.focused) {
        std::optional<TableRowRef> const ref = table.source.rows->row(table.focusIndex);
        // The row at the focused index arrived, or took the place of the focused row that left the view; a row
        // that is there for any other reason (a reset that moved the focused key away) does not take focus.
        if (ref && table.focusKey != ref->key && (!table.focusKey || table.focusLeft)) {
            table.focusKey = ref->key;
            reportFocus(table);
        }
        table.focusLeft = table.focusLeft && !ref;
    }
}

void Mounter::locateFocus(SourceTable& table) {
    if (!table.focusKey) {
        return;
    }
    table.focusLocate.request_stop();
    table.focusLocate = ::core::async::StopSource{};
    ::core::async::StopToken const stop = table.focusLocate.get_token();
    table.source.rows->locateRow(*table.focusKey, stop,
                                 [this, &table, stop, alive = std::weak_ptr{table.alive}](RowLocation const& found) {
                                     if (alive.expired() || stop.stop_requested()) {
                                         return;
                                     }
                                     static_cast<void>(runCallback(*_rt, [&] { focusLocated(table, found); }));
                                 });
}

void Mounter::focusLocated(SourceTable& table, RowLocation const& found) {
    if (!table.focused || !table.focusSearching || !found) {
        return;
    }
    table.focusSearching = false;
    table.focusUntold = true;
    if (*found && **found >= 0) {
        table.focusIndex = **found;
    } else {
        table.focusWas = table.focusKey;
        table.focusLeft = true;
    }
    realizeWindow(table, [](std::int64_t) { return false; });
}

void Mounter::tellLocatedFocus(SourceTable& table) {
    auto const index = static_cast<std::size_t>(table.focusIndex);
    // A row held at the located index with another key is of the view before the change that moved the key
    // there: the index is scrolled to, so its rows are realized, and focus waits for a realized row to hold the
    // key. A row not held names no key, and is fetched.
    std::optional<TableRowRef> const ref = table.source.rows->row(table.focusIndex);
    bool const other = !table.focusLeft && table.focusKey && ref && ref->key != *table.focusKey;
    if (!table.focusLeft && table.focusKey) {
        table.focusUnconfirmed = !ref || other;
    }
    VisibleRange range = focusRangeOf(table);
    if ((!ref || other) && (index < range.first || index >= range.first + range.count)) {
        range.first = index < range.first ? index : index + 1 - range.count;
        table.range = range;
        table.widget->scrollToRow(index);
        tellVisible(table);
    }
    if (!other) {
        table.widget->setFocusCell(index, table.focusColumn);
    }
}

void Mounter::reportFocus(SourceTable const& table) {
    // A grid holds its own focus: the key that took focus is handed to it, as a gesture's cell is.
    if (table.grid) {
        if (table.onGridFocus && table.focusColumn < table.columns.size()) {
            table.onGridFocus(
                GridFocus{.key = table.focusKey,
                          .viewRow = static_cast<std::size_t>(std::max<std::int64_t>(table.focusIndex, 0)),
                          .column = table.columns.at(table.focusColumn).id});
        }
        return;
    }
    if (!table.onFocusCell || !table.focusKey) {
        return;
    }
    std::string const& columnId =
        table.focusColumn < table.columns.size() ? table.columns.at(table.focusColumn).id : std::string{};
    table.onFocusCell(CellRef{.key = *table.focusKey, .column = columnId});
}

}  // namespace morph::ui::detail
