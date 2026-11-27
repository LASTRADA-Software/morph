// SPDX-License-Identifier: Apache-2.0

// A model-backed table's columns and configuration.

#include "interpreter_table_detail.hpp"

namespace morph::ui::detail {

namespace {

/// Makes each of a table's menu entries clear the status line's message before it runs, as every gesture on the table
/// does.
// NOLINTNEXTLINE(misc-no-recursion): a submenu is a menu one level down.
void clearsMessageFirst(TableConfig& config, std::vector<MenuItem>& items) {
    for (MenuItem& item : items) {
        clearsMessageFirst(config, item.items);
        if (item.onSelect) {
            item.onSelect = [&config, run = std::move(item.onSelect)] {
                if (config.message) {
                    config.message({});
                }
                run();
            };
        }
    }
}

/// The chords menu entry parts declare in `keys`, their submenus' included, each once.
// NOLINTNEXTLINE(misc-no-recursion): a submenu is a menu one level down.
std::vector<std::string> chordsOf(std::span<DocNodePtr const> parts, std::vector<std::string> out = {}) {
    for (DocNodePtr const& part : parts) {
        if (std::string const chord{part->name("keys")}; !chord.empty() && !std::ranges::contains(out, chord)) {
            out.push_back(chord);
        }
        out = chordsOf(part->partList("items"), std::move(out));
    }
    return out;
}

}  // namespace

[[nodiscard]] Value layoutValueOf(table::ColumnLayoutValue const& layout) {
    auto const ids = [](std::span<std::string const> list) {
        ValueList out;
        for (std::string const& columnId : list) {
            out.emplace_back(columnId);
        }
        return Value{std::move(out)};
    };
    ValueRecord out;
    out.insert_or_assign("order", ids(layout.order));
    out.insert_or_assign("hidden", ids(layout.hidden));
    out.insert_or_assign("pinned", Value{static_cast<std::int64_t>(layout.pinned)});
    ValueRecord widths;
    for (auto const& [columnId, width] : layout.widths) {
        widths.insert_or_assign(columnId, Value{static_cast<std::int64_t>(width)});
    }
    out.insert_or_assign("widths", Value{std::move(widths)});
    if (!layout.groups.empty() || !layout.units.empty()) {
        auto const json = parseJson(glz::write_json(layout).value_or("null"));
        auto read = json ? valueFromJson(*json) : std::expected<Value, std::string>{Value{}};
        if (ValueRecord const* const whole = read ? read->asRecord() : nullptr) {
            if (auto const groups = whole->find("groups"); groups != whole->end() && !layout.groups.empty()) {
                out.insert_or_assign("groups", groups->second);
            }
            if (auto const units = whole->find("units"); units != whole->end() && !layout.units.empty()) {
                out.insert_or_assign("units", units->second);
            }
        }
    }
    return Value{std::move(out)};
}

namespace {

/// The arrangement a mount takes from a layout, by index into @p columns; the default one while the layout is the
/// declared order with every column shown, none pinned and no width set, so such a table is laid out as one with
/// no layout is.
[[nodiscard]] TableArrangement arrangementOf(std::span<LayoutColumn const> columns,
                                             table::ColumnLayout const& layout) {
    std::vector<std::string> const visible = layout.visible();
    bool plain = visible.size() == columns.size() && layout.pinned() == 0 && layout.widths().empty();
    for (std::size_t at = 0; plain && at < visible.size(); ++at) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): `at` is below `visible.size()` and `columns.size()` by the loop condition above
        plain = visible[at] == columns[at].id;
    }
    TableArrangement out;
    if (plain) {
        return out;
    }
    auto const indexOf = [columns](std::string const& columnId) -> std::optional<std::size_t> {
        auto const found = std::ranges::find(columns, columnId, &LayoutColumn::id);
        return found == columns.end() ? std::nullopt
                                      : std::optional{static_cast<std::size_t>(found - columns.begin())};
    };
    for (std::string const& columnId : visible) {
        if (std::optional<std::size_t> const index = indexOf(columnId)) {
            out.visible.push_back(*index);
        }
    }
    out.pinned = layout.pinned();
    for (auto const& [columnId, width] : layout.widths()) {
        if (std::optional<std::size_t> const index = indexOf(columnId)) {
            out.widths.insert_or_assign(*index, width);
        }
    }
    return out;
}

/// The entries of a menu whose commands the lowering carries out: the sort, filter and arranging entries, "Display
/// unit", "Clear all filters", "Reset columns", "Columns…", "Sort…", "Copy", "Copy with headers", and an entry that
/// does nothing itself ("Views", whose views commands `Table::onMenu` carries out). Any other is not offered; a
/// separator left at either end or beside another goes with it.
[[nodiscard]] std::vector<TableMenuEntry> offeredEntries(std::vector<TableMenuEntry> entries) {
    auto const offered = [](TableMenuEntry const& entry) {
        if (entry.entry.separator || entry.command == MenuCommand::None) {
            return true;
        }
        switch (entry.command) {
            case MenuCommand::SortAscending:
            case MenuCommand::SortDescending:
            case MenuCommand::AddToSort:
            case MenuCommand::RemoveFromSort:
            case MenuCommand::MoveLeft:
            case MenuCommand::MoveRight:
            case MenuCommand::PinColumn:
            case MenuCommand::UnpinColumn:
            case MenuCommand::HideColumn:
            case MenuCommand::ResetColumns:
            case MenuCommand::Columns:
            case MenuCommand::SortDialog:
            case MenuCommand::Filter:
            case MenuCommand::FilterGroup:
            case MenuCommand::ClearAllFilters:
            case MenuCommand::DisplayUnit:
            case MenuCommand::Copy:
            case MenuCommand::CopyWithHeaders:
                return true;
            case MenuCommand::ApplyView:
            case MenuCommand::SaveView:
            case MenuCommand::RenameView:
            case MenuCommand::RemoveView:
            case MenuCommand::SetDefaultView:
            case MenuCommand::DefaultOnMount:
            case MenuCommand::ApplySharedView:
            case MenuCommand::PublishView:
            case MenuCommand::SaveAsMyView:
            case MenuCommand::None:
            default:
                return false;
        }
    };
    std::vector<TableMenuEntry> kept;
    for (TableMenuEntry& entry : entries) {
        if (!offered(entry) || (entry.entry.separator && (kept.empty() || kept.back().entry.separator))) {
            continue;
        }
        kept.push_back(std::move(entry));
    }
    if (!kept.empty() && kept.back().entry.separator) {
        kept.pop_back();
    }
    return kept;
}

}  // namespace

void ScreenCore::modelLayoutColumns(ElementState& element) const {
    ModelTable& model = *element.modelTable;
    TableConfig& config = model.config;
    DocNode const& node = *element.node;
    RtScope& scope = *element.scope;
    auto const parts = node.partList("columns");
    config.columns.clear();
    config.declaredHidden.clear();
    config.sortable.clear();
    if (table::TableDescription const* const description = model.window->description()) {
        for (table::TableColumn const* const column : arrangedColumns(node, *description)) {
            auto const part = std::ranges::find(parts, column->info.id,
                                                [](DocNodePtr const& given) -> auto const& { return given->id; });
            bool const named = part != parts.end();
            std::string label = column->label.empty() ? column->info.id : column->label;
            if (ExprPtr const given = named ? (*part)->prop("label") : nullptr) {
                label = runtime().untracked([&] { return textOf(evalIn(scope, nullptr, given), scope.formatting()); });
            }
            config.columns.push_back(LayoutColumn{.id = column->info.id,
                                                  .label = std::move(label),
                                                  .locked = named && (*part)->flag("locked"),
                                                  .kind = column->info.kind});
            config.sortable.push_back(column->sortable);
            // A column the description does not show starts hidden; a column part shows the column it names.
            if (!named && !column->shown) {
                config.declaredHidden.push_back(column->info.id);
            }
        }
    }
    config.version.set(config.version.peek() + 1);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity, readability-function-size): the sort, the layout, then the menus that reach both; each option wires a handler that reads the same config and nodes
Node ScreenCore::configureTable(TableConfig& config, LowerCtx const& ctx, TableOptions& options,
                                std::shared_ptr<void const> const& keep) {
    ElementState& element = *config.element;
    DocNode const& node = *element.node;
    // This mount's columns: a new description mounts the table again, with its own.
    auto const columns = std::make_shared<std::vector<LayoutColumn> const>(config.columns);
    auto const sortable = std::make_shared<std::vector<bool> const>(config.sortable);
    auto const hidden = std::make_shared<std::vector<std::string> const>(config.declaredHidden);
    auto const layoutHeld = [&config, columns, hidden] { return layoutOver(*columns, *hidden, layoutNow(config)); };
    // Every key of the chain marks its header, in the chain's order; a sort that is not a list of keys naming
    // sortable columns, each with a direction, marks none, as the rows keep their order under it.
    options.sort = slotProp<std::vector<TableSort>>([&element, columns, sortable] {
        std::vector<TableSort> out;
        Value const sort = tableSort(element);
        if (sort.isNull()) {
            return out;
        }
        ValueList const* const keys = sort.asList();
        if (keys == nullptr) {
            return out;
        }
        for (Value const& key : *keys) {
            Value const* const column = key.field("column");
            Value const* const direction = key.field("direction");
            std::string const* const columnId = column == nullptr ? nullptr : column->asString();
            std::string const* const way = direction == nullptr ? nullptr : direction->asString();
            auto const found =
                columnId == nullptr ? columns->end() : std::ranges::find(*columns, *columnId, &LayoutColumn::id);
            if (found == columns->end() || way == nullptr || (*way != "asc" && *way != "desc")) {
                return std::vector<TableSort>{};
            }
            auto const index = static_cast<std::size_t>(found - columns->begin());
            if (!sortable->at(index)) {
                return std::vector<TableSort>{};
            }
            out.push_back(TableSort{
                .column = index, .direction = *way == "desc" ? SortDirection::Descending : SortDirection::Ascending});
        }
        return out;
    });
    options.arrangement =
        slotProp<TableArrangement>([columns, layoutHeld] { return arrangementOf(*columns, layoutHeld()); });
    // A copy: the selected rows in view order by the visible columns in layout order, or with no row selected the
    // focused cell alone; over `maxCopyRows` keys it is refused and nothing is copied. The rows say how each cell
    // shows; an unloaded cell is copied as empty and counted in the message.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): a copy: the selected rows in view order by the visible columns in layout order, or the focused cell alone, refusing past the key limit
    auto const copy = [this, &config, &element, layoutHeld, keep, slots = config.mountSlots(),
                       scope = ctx.scope](bool headers) {
        static_cast<void>(keep);
        static_cast<void>(slots);
        if (!config.gather) {
            return;
        }
        std::vector<Key> keys;
        std::vector<std::string> copyColumns = runtime().untracked([&] {
            if (Value const itemsValue = tableSelection(element); ValueList const* const items = itemsValue.asList()) {
                for (Value const& item : *items) {
                    keys.push_back(keyOf(item));
                }
            }
            return layoutHeld().visible();
        });
        std::string separator{scope->formatting().numeric.groupSeparator};
        std::size_t const bound = session.options().maxCopyRows;
        std::optional<std::int64_t> const viewing =
            config.viewSelected ? runtime().untracked([&] { return config.viewSelected(); }) : std::nullopt;
        if (keys.size() > bound || (viewing && std::cmp_greater(*viewing, bound))) {
            config.message(tableText::copyBound(bound, separator));
            return;
        }
        if (keys.empty() && !viewing) {
            if (!config.focused || !std::ranges::contains(copyColumns, config.focused->column)) {
                return;
            }
            keys.push_back(config.focused->key);
            copyColumns = {config.focused->column};
        }
        // A copy started after this one wins: this one's rows, arriving late, write nothing.
        std::uint64_t const started = ++config.copies;
        auto gather = [&config, viewing, keys = std::move(keys)](std::vector<std::string> const& named,
                                                                 CopyDone done) mutable {
            if (viewing && config.gatherView) {
                config.gatherView(named, std::move(done));
            } else {
                config.gather(std::move(keys), named, std::move(done));
            }
        };
        gather(copyColumns, [this, &config, &element, headers, started,
                             separator = std::move(separator)](std::expected<CopyGathered, std::string> gathered) {
            if (started != config.copies) {
                return;
            }
            if (!gathered) {
                tableMessage(element, std::move(gathered.error()));
                return;
            }
            // None of the rows is in the view any more: the clipboard keeps what it had.
            if (gathered->rows.empty()) {
                return;
            }
            std::ranges::stable_sort(gathered->rows, {}, &CopyRow::viewRow);
            std::vector<std::vector<std::optional<std::string>>> rows;
            rows.reserve(gathered->rows.size());
            for (CopyRow& row : gathered->rows) {
                rows.push_back(std::move(row.cells));
            }
            CopiedText const text = ui::copyText(headers ? gathered->headers : std::vector<std::string>{}, rows);
            if (!clipboard || !clipboard(text.text)) {
                tableMessage(element, "The rows could not be copied: there is no clipboard");
                return;
            }
            tableMessage(element, tableText::copied(rows.size(), text.unloaded, separator));
        });
    };
    options.onCopy = [&config, copy] {
        config.touch();
        copy(false);
    };
    options.onFocusCell = [&config](std::optional<CellRef> cell) { config.focused = std::move(cell); };
    // A sort gesture: held to the table's bound, a key past it refused in the status line and the chain kept. The
    // table's own sort takes the new chain when no key binds one, and is saved; then `onSortChange` runs with
    // `event` the new sort.
    auto const resort = [this, ctx, &config, &element, keep, commands = node.event("onSortChange")](
                            std::function<SortChange(SortChain const&)> const& gesture) {
        static_cast<void>(keep);
        SortChain const current = runtime().untracked([&] { return sortChainOf(tableSort(element)); });
        SortChange const change = gesture(current);
        if (change.refusal) {
            config.message(*change.refusal);
            return;
        }
        if (change.chain == current) {
            return;
        }
        Value const next = sortValueOf(change.chain);
        runtime().batch([&] {
            if (element.sortBound == nullptr) {
                session.asUser([&] {
                    session.noteWrite();
                    element.sort.set(next);
                });
                if (config.save) {
                    table::StateDocument changed;
                    changed.sort = change.chain;
                    config.save(changed, StateParts::Sort);
                }
            }
            runEvent(commands, ctx, next);
        });
    };
    auto const sortBy = [&config, columns, sortable, resort](std::size_t index, SortGesture gesture) {
        config.touch();
        if (index >= columns->size() || !sortable->at(index)) {
            return;
        }
        resort([&](SortChain const& current) {
            return applySortGesture(current, columns->at(index).id, gesture, config.maxSortKeys());
        });
    };
    // A layout gesture: refused in the status line when its column is locked or the table does not allow it. The
    // table's own layout takes the change when no key binds one, and is saved; then `onLayoutChange` runs with
    // `event` the new layout. The arrangement follows the layout, so the mount fetches the columns it shows.
    auto const relayout = [this, ctx, &config, &element, keep,
                           commands = node.event("onLayoutChange")](table::ColumnLayoutValue value) {
        static_cast<void>(keep);
        Value const event = layoutValueOf(value);
        runtime().batch([&] {
            if (config.bound == nullptr) {
                session.asUser([&] {
                    session.noteWrite();
                    config.held = value;
                    config.version.set(config.version.peek() + 1);
                });
                if (config.save) {
                    table::StateDocument changed;
                    changed.layout = std::move(value);
                    config.save(changed, StateParts::Layout);
                }
                tablePoints(element);
            }
            runEvent(commands, ctx, event);
        });
    };
    auto const arrange = [this, &config, columns, layoutHeld, relayout](LayoutGesture const& gesture) {
        config.touch();
        table::ColumnLayout layout = runtime().untracked(layoutHeld);
        LayoutResult const result = applyLayoutGesture(layout, *columns, config.allowed, gesture);
        if (result.refusal) {
            config.message(*result.refusal);
            return;
        }
        if (result.changed) {
            relayout(layout.value());
        }
    };
    options.onSort = [sortBy](std::size_t index, HeaderClick click) {
        sortBy(index, click == HeaderClick::Shift ? SortGesture::Extend : SortGesture::Click);
    };
    options.onColumnMove = [&config, arrange, columns](std::size_t column, std::size_t position) {
        if (column >= columns->size()) {
            config.touch();
            return;
        }
        arrange(MoveColumnTo{.id = columns->at(column).id, .position = position});
    };
    // A table whose widths its user cannot set offers no edge to drag.
    if (allows(config.allowed, Configure::Resize)) {
        options.onColumnResize = [&config, arrange, columns](std::size_t column, std::optional<int> units) {
            if (column >= columns->size()) {
                config.touch();
                return;
            }
            arrange(ResizeColumn{.id = columns->at(column).id, .units = units});
        };
    }
    // Filters from the header, on every table: a column filters unless it says `filterable: false`, whatever
    // `configurable` lets the user change about the columns. A user's filters go to the rows, and are held and
    // saved, unless the document binds them; then `onFiltersChange` runs with `event` the new filters.
    FilterMenu& menu = *config.filterMenu;
    auto const refilter = [this, ctx, &config, keep,
                           commands = node.event("onFiltersChange")](table::FilterSpec const& spec) {
        static_cast<void>(keep);
        bool applied = true;
        Value const event = filtersValueOf(spec);
        runtime().batch([&] {
            if (!config.filtersBound) {
                session.asUser([&] {
                    session.noteWrite();
                    applied = config.setFilters && config.setFilters(spec);
                });
                if (applied && config.save) {
                    table::StateDocument changed;
                    changed.filters = spec;
                    config.save(changed, StateParts::Filters);
                }
            }
            if (applied) {
                runEvent(commands, ctx, event);
            }
        });
        return applied;
    };
    auto const filtersNow = [&config] { return config.filters ? config.filters() : table::FilterSpec{}; };
    // Every group the table has: those it declares, then those its user formed, each with whether it was formed.
    auto const groupsNow = [&config, layoutHeld] {
        std::vector<std::pair<table::ColumnGroupInfo, bool>> out;
        if (config.declaredGroups) {
            for (table::ColumnGroupInfo& group : config.declaredGroups()) {
                out.emplace_back(std::move(group), false);
            }
        }
        for (table::ColumnGroupInfo const& group : layoutHeld().groups()) {
            out.emplace_back(group, true);
        }
        return out;
    };
    // A menu names a formed group by its id after U+001F, so it never meets a declared group's id.
    auto const groupArgument = [](table::ColumnGroupInfo const& group, bool formed) {
        return formed ? "\x1F" + group.id : group.id;
    };
    auto const columnTarget = [&config](std::string const& columnId) -> std::optional<FilterTarget> {
        return config.filterTarget ? config.filterTarget(columnId) : std::nullopt;
    };
    auto const groupTarget = [columnTarget](table::ColumnGroupInfo const& group,
                                            bool formed) -> std::optional<FilterTarget> {
        if (group.columns.empty()) {
            return std::nullopt;
        }
        std::optional<FilterTarget> target = columnTarget(group.columns.front());
        if (!target) {
            return std::nullopt;
        }
        target->column.clear();
        target->group = formed ? std::string{} : group.id;
        target->columns = group.columns;
        target->formed = formed;
        target->label = group.label;
        return target;
    };
    // The columns a list reads are put in use before the menu reads the column's kind: a table over `rows` infers
    // it from the cells.
    auto const openColumn = [this, &config, &menu, columnTarget](std::string const& columnId) {
        runtime().untracked([&] {
            if (config.listing) {
                config.listing({columnId});
            }
            if (std::optional<FilterTarget> target = columnTarget(columnId)) {
                menu.open(std::move(*target));
            } else if (config.listing) {
                config.listing({});
            }
        });
    };
    auto const openGroup = [this, &config, &menu, groupsNow, groupArgument, groupTarget](std::string const& argument) {
        runtime().untracked([&] {
            for (auto const& [group, formed] : groupsNow()) {
                if (groupArgument(group, formed) != argument) {
                    continue;
                }
                if (config.listing) {
                    config.listing(group.columns);
                }
                if (std::optional<FilterTarget> target = groupTarget(group, formed)) {
                    menu.open(std::move(*target));
                } else if (config.listing) {
                    config.listing({});
                }
                return;
            }
        });
    };
    // The menu outlives a mount, and `refilter` holds the mount's scope: the menu reaches it weakly, through what the
    // mount keeps, so the element does not keep its own scope alive.
    auto const applying = std::make_shared<std::function<bool(table::FilterSpec const&)>>(refilter);
    menu.setHost(FilterMenuHost{
        .rows = config.rows,
        .filters = [this, filtersNow] { return runtime().untracked(filtersNow); },
        .apply =
            [weak = std::weak_ptr{applying}](table::FilterSpec const& spec) {
                auto const live = weak.lock();
                return live != nullptr && (*live)(spec);
            },
        .touch = config.touch,
        .listing = config.listing,
        .scheduler = session.options().scheduler,
        .debounce = session.options().tableFilterDebounce,
        .format = ctx.scope->formatting(),
    });
    // Which columns this mount offers a filter on, read once with its columns.
    auto const filterable = std::make_shared<std::vector<bool>>();
    runtime().untracked([&] {
        for (LayoutColumn const& column : *columns) {
            filterable->push_back(columnTarget(column.id).has_value());
        }
    });
    options.onFilter = [&config, columns, filterable, openColumn, applying](std::size_t index) {
        static_cast<void>(applying);
        config.touch();
        if (index < columns->size() && filterable->at(index)) {
            openColumn(columns->at(index).id);
        }
    };
    // The columns with a filter, whose headers say so, hidden ones included: the filter keeps applying while its
    // column is hidden, and the header says so again once it is shown.
    options.filtered = slotProp<std::vector<std::size_t>>([columns, filtersNow] {
        table::FilterSpec const filters = filtersNow();
        std::vector<std::size_t> out;
        for (std::size_t at = 0; at < columns->size(); ++at) {
            if (filters.columns.contains(columns->at(at).id)) {
                out.push_back(at);
            }
        }
        return out;
    });
    // A group's header row stands over its columns while they stand side by side in the layout, in the group's order.
    auto const sideBySide = [layoutHeld, groupsNow, filtersNow, groupArgument] {
        std::vector<std::pair<TableColumnGroup, std::string>> out;
        table::ColumnLayout const layout = layoutHeld();
        std::vector<std::string> const visible = layout.visible();
        table::FilterSpec const filters = filtersNow();
        for (auto const& [group, formed] : groupsNow()) {
            auto const first =
                std::ranges::find(visible, group.columns.empty() ? std::string{} : group.columns.front());
            auto const index = static_cast<std::size_t>(first - visible.begin());
            bool together = first != visible.end() && index + group.columns.size() <= visible.size();
            for (std::size_t offset = 0; together && offset < group.columns.size(); ++offset) {
                together = visible.at(index + offset) == group.columns.at(offset);
            }
            if (!together) {
                continue;
            }
            FilterTarget probe;
            probe.group = formed ? std::string{} : group.id;
            probe.columns = group.columns;
            probe.formed = formed;
            out.emplace_back(TableColumnGroup{.label = group.label,
                                              .first = index,
                                              .count = group.columns.size(),
                                              .filtered = groupFilterIndex(filters, probe).has_value()},
                             groupArgument(group, formed));
        }
        return out;
    };
    options.columnGroups = slotProp<std::vector<TableColumnGroup>>([sideBySide] {
        std::vector<TableColumnGroup> out;
        for (auto& [group, argument] : sideBySide()) {
            static_cast<void>(argument);
            out.push_back(std::move(group));
        }
        return out;
    });
    options.onGroupFilter = [this, &config, sideBySide, openGroup](std::size_t index) {
        config.touch();
        auto const shown = runtime().untracked(sideBySide);
        if (index < shown.size()) {
            openGroup(shown.at(index).second);
        }
    };
    // While `headerEnabled` is false a sort, filter or arrangement gesture changes nothing: it is refused in the
    // status line. The condition is read at each gesture, so it follows the document's state.
    ExprPtr const headerEnabledExpr = node.prop("headerEnabled");
    options.headerEnabled = bindProp<bool>(ctx, headerEnabledExpr, true, boolOf);
    auto const headerOn = [this, enabled = options.headerEnabled] {
        return runtime().untracked([&] { return enabled.evaluate(); });
    };
    if (headerEnabledExpr) {
        auto const refused = [&config, headerOn] {
            if (headerOn()) {
                return false;
            }
            config.touch();
            config.message(std::string{tableText::kHeaderBusy});
            return true;
        };
        // NOLINTNEXTLINE(bugprone-exception-escape): copying a `Prop` throws `bad_variant_access` only from a valueless source, which `copyOf` documents and nothing here builds
        options.onSort = [refused, sort = std::move(options.onSort)](std::size_t index, HeaderClick click) {
            if (!refused()) {
                sort(index, click);
            }
        };
        // NOLINTNEXTLINE(bugprone-exception-escape): copying a `Prop` throws `bad_variant_access` only from a valueless source, which `copyOf` documents and nothing here builds
        options.onColumnMove = [refused, move = std::move(options.onColumnMove)](std::size_t column,
                                                                                 std::size_t position) {
            if (!refused()) {
                move(column, position);
            }
        };
        if (options.onColumnResize) {
            // NOLINTNEXTLINE(bugprone-exception-escape): copying a `Prop` throws `bad_variant_access` only from a valueless source, which `copyOf` documents and nothing here builds
            options.onColumnResize = [refused, resize = std::move(options.onColumnResize)](std::size_t column,
                                                                                           std::optional<int> units) {
                if (!refused()) {
                    resize(column, units);
                }
            };
        }
        // NOLINTNEXTLINE(bugprone-exception-escape): copying a `Prop` throws `bad_variant_access` only from a valueless source, which `copyOf` documents and nothing here builds
        options.onFilter = [refused, filter = std::move(options.onFilter)](std::size_t column) {
            if (!refused()) {
                filter(column);
            }
        };
        // NOLINTNEXTLINE(bugprone-exception-escape): copying a `Prop` throws `bad_variant_access` only from a valueless source, which `copyOf` documents and nothing here builds
        options.onGroupFilter = [refused, filter = std::move(options.onGroupFilter)](std::size_t group) {
            if (!refused()) {
                filter(group);
            }
        };
    }
    // A cell's menu: "Copy", "Copy with headers", "Filter by this value", "Exclude this value" and, on a column of a
    // declared group, "Filter by this value in any <group>"; then the document's `cellMenu` entries, lowered in the
    // row's scope with `row`, `column` (the column's id) and `value` (the cell) bound. The filter entries take the
    // cell's operand, or `isEmpty` on an empty cell, and are disabled on a cell not loaded or not readable, and while
    // the header is disabled, when one chosen anyway is refused as the header's gestures are.
    if (config.heldCell) {
        // NOLINTNEXTLINE(bugprone-exception-escape, readability-function-cognitive-complexity): builds the cell menu: copy, filter and group entries each gated by the column's kind and the header's state, in menu order; copying a `Prop` throws `bad_variant_access` only from a valueless source, which `copyOf` documents and nothing here builds
        options.cellMenu = [this, ctx, &config, &node, columns, filterable, copy, refilter, filtersNow, keep,
                            headerOn](Key const& key, std::size_t column) {
            static_cast<void>(keep);
            std::vector<MenuItem> items;
            std::string const columnId = column < columns->size() ? columns->at(column).id : std::string{};
            std::optional<HeldCell> const held =
                config.heldCell && !columnId.empty() ? config.heldCell(key, columnId) : std::optional<HeldCell>{};
            CellLook const look = held ? cellLook(held->cell, held->shown, held->units) : CellLook::Unloaded;
            std::optional<std::string> const operand =
                held ? operandOf(held->cell, held->shown, held->units) : std::nullopt;
            bool const usable = (look == CellLook::Empty || (look == CellLook::Text && operand.has_value())) &&
                                column < filterable->size() && filterable->at(column) && headerOn();
            table::FilterEntry const entry = look == CellLook::Empty
                                                 ? table::FilterEntry{.isEmpty = true}
                                                 : table::FilterEntry{.oneOf = std::vector{operand.value_or("")}};
            auto const command = [&config](std::string label, std::function<void()> run, bool enabled = true) {
                MenuItem item;
                item.label = std::move(label);
                item.enabled = enabled;
                item.onSelect = [&config, run = std::move(run)] {
                    config.touch();
                    run();
                };
                return item;
            };
            auto const filtering = [&config, headerOn](std::function<void()> run) {
                // NOLINTNEXTLINE(bugprone-exception-escape): copying a `Prop` throws `bad_variant_access` only from a valueless source, which `copyOf` documents and nothing here builds
                return [&config, headerOn, run = std::move(run)] {
                    if (!headerOn()) {
                        config.message(std::string{tableText::kHeaderBusy});
                        return;
                    }
                    run();
                };
            };
            items.push_back(command("Copy", [copy] { copy(false); }));
            items.push_back(command("Copy with headers", [copy] { copy(true); }));
            items.push_back(command("Filter by this value", filtering([this, columnId, entry, refilter, filtersNow] {
                                        table::FilterSpec spec = runtime().untracked(filtersNow);
                                        spec.columns.insert_or_assign(columnId,
                                                                      table::ColumnFilter{.include = {entry}});
                                        static_cast<void>(refilter(spec));
                                    }),
                                    usable));
            items.push_back(command("Exclude this value", filtering([this, columnId, entry, refilter, filtersNow] {
                                        table::FilterSpec spec = runtime().untracked(filtersNow);
                                        std::vector<table::FilterEntry>& exclude = spec.columns[columnId].exclude;
                                        auto const listed =
                                            std::ranges::find_if(exclude, [](table::FilterEntry const& excluded) {
                                                return excluded.oneOf.has_value();
                                            });
                                        if (entry.oneOf && listed != exclude.end()) {
                                            if (!std::ranges::contains(*listed->oneOf, entry.oneOf->front())) {
                                                listed->oneOf->push_back(entry.oneOf->front());
                                            }
                                        } else if (!std::ranges::contains(exclude, entry)) {
                                            exclude.push_back(entry);
                                        }
                                        static_cast<void>(refilter(spec));
                                    }),
                                    usable));
            if (config.declaredGroups) {
                for (table::ColumnGroupInfo const& group :
                     runtime().untracked([&] { return config.declaredGroups(); })) {
                    if (!std::ranges::contains(group.columns, columnId)) {
                        continue;
                    }
                    items.push_back(
                        command("Filter by this value in any " + group.label,
                                filtering([this, groupId = group.id, entry, refilter, filtersNow] {
                                    table::FilterSpec spec = runtime().untracked(filtersNow);
                                    std::erase_if(spec.groups, [&groupId](table::GroupFilter const& filter) {
                                        return filter.group == groupId;
                                    });
                                    spec.groups.push_back(table::GroupFilter{.group = groupId, .include = {entry}});
                                    static_cast<void>(refilter(spec));
                                }),
                                usable));
                }
            }
            std::span<DocNodePtr const> const parts = node.partList("cellMenu");
            if (parts.empty()) {
                return items;
            }
            MenuItem separator;
            separator.separator = true;
            items.push_back(std::move(separator));
            // The entries read the row as it was when the menu opened; the scope owns the signal that holds it. The
            // scope is the menu's own: state the table's row scope declares starts at its initial value in it.
            Value const record = held ? held->record : Value{};
            auto const scope = std::make_shared<RtScope>(*this, ctx.scope, nullptr, ctx.scope->nodes.depth() + 2);
            scope->row = &scope->nodes.make<reactive::Signal<Value>>(runtime(), record);
            scope->rowKey = canonicalJson(valueOfKey(key));
            declare(*scope, node.scope ? &*node.scope : nullptr, {}, {});
            auto bindings = std::make_shared<Bindings>(*ctx.bindings);
            Value const value = [&] {
                Value const* const field = record.field(columnId);
                return field == nullptr ? Value{} : *field;
            }();
            // NOLINTNEXTLINE(performance-no-automatic-move): the binding's read runs on every evaluation and each returns a copy of the captured record, so the capture stays const
            bindings->push_back(Binding{.name = "row", .read = [record] { return record; }});
            bindings->push_back(Binding{.name = "column", .read = [columnId] { return Value{columnId}; }});
            // NOLINTNEXTLINE(performance-no-automatic-move): the binding's read runs on every evaluation and each returns a copy of the captured value, so the capture stays const
            bindings->push_back(Binding{.name = "value", .read = [value] { return value; }});
            std::vector<MenuItem> own =
                menuItems(parts, LowerCtx{.scope = scope, .bindings = std::move(bindings), .boundary = ctx.boundary});
            clearsMessageFirst(config, own);
            std::ranges::move(own, std::back_inserter(items));
            return items;
        };
    }
    // The chords the document's cell entries declare: a key that is none of them never asks for a cell's entries.
    if (options.cellMenu) {
        options.cellMenuKeys = chordsOf(node.partList("cellMenu"));
    }
    // A table with `configurable: false` has no menus, so neither the column chooser nor the sort dialog; with no
    // column that filters, no dialog can open either, and the table is mounted alone.
    if (!config.menus) {
        return std::ranges::contains(*filterable, true) ? filterMenuDialog(menu) : nullptr;
    }
    // A header's menu ends with the document's `headerMenu` entries, read in a scope with `column`, the column's id.
    if (std::span<DocNodePtr const> const parts = node.partList("headerMenu"); !parts.empty()) {
        options.headerEntries = [this, ctx, &config, parts, columns](std::size_t column) {
            auto bindings = std::make_shared<Bindings>(*ctx.bindings);
            std::string const columnId = column < columns->size() ? columns->at(column).id : std::string{};
            bindings->push_back(Binding{.name = "column", .read = [columnId] { return Value{columnId}; }});
            std::vector<MenuItem> own = menuItems(
                parts, LowerCtx{.scope = ctx.scope, .bindings = std::move(bindings), .boundary = ctx.boundary});
            clearsMessageFirst(config, own);
            return own;
        };
    }
    // The saved views. A view is fitted to the table as a stored state is, and the arrangement is the view's while its
    // layout, laid over the table's columns, its sort and its filters are those fitted ones.
    auto const fittedView = [&config](table::SavedView const& view) {
        table::StateDocument document;
        document.layout = view.layout;
        document.sort = view.sort;
        document.filters = view.filters;
        if (std::optional<table::TableDescription> const description =
                config.description ? config.description() : std::nullopt) {
            table::FitReport ignored;
            document = table::fitState(std::move(document), *description, ignored);
        }
        return document;
    };
    auto const arrangementNow = [this, &element, layoutHeld, filtersNow] {
        return runtime().untracked([&] {
            return table::SavedView{.name = {},
                                    .layout = layoutHeld().value(),
                                    .sort = sortChainOf(tableSort(element)),
                                    .filters = filtersNow()};
        });
    };
    auto const viewsInput = [&config, columns, hidden, fittedView, arrangementNow]() -> std::optional<ViewsMenuInput> {
        TableViews const* const views = config.views;
        if (views == nullptr) {
            return std::nullopt;
        }
        ViewsMenuInput input;
        for (table::SavedView const& view : views->own().views) {
            input.own.push_back(view.name);
        }
        for (table::SavedView const& view : views->shared().views) {
            input.shared.push_back(view.name);
        }
        input.current = config.savedView;
        input.currentShared = config.viewShared;
        input.defaultOnMount = views->own().defaultOnMount;
        input.shares = views->shares();
        table::SavedView const* const current =
            !config.savedView ? nullptr
            // NOLINTNEXTLINE(readability-avoid-nested-conditional-operator): a three-way pick: none, the shared view or the user's own view
            : config.viewShared ? views->sharedView(*config.savedView)
                                : views->ownView(*config.savedView);
        if (current != nullptr) {
            table::StateDocument const view = fittedView(*current);
            table::SavedView const now = arrangementNow();
            input.modified = layoutOver(*columns, *hidden, view.layout).value() != now.layout ||
                             view.sort.value_or(SortChain{}) != now.sort ||
                             view.filters.value_or(table::FilterSpec{}) != now.filters;
        }
        return input;
    };
    // Records the view the arrangement was taken from in the table's state, and shows it at the table's points.
    auto const recordView = [this, &config, &element](std::optional<std::string> name, bool shared) {
        config.savedView = std::move(name);
        config.viewShared = shared && config.savedView.has_value();
        if (config.save) {
            table::StateDocument changed;
            changed.view = config.savedView;
            config.save(changed, StateParts::View);
        }
        tablePoints(element);
    };
    // Applying a view sets its layout, sort and filters as the gestures do, through their handlers where they are
    // bound, and records its name.
    auto const applyView = [this, &config, columns, hidden, fittedView, arrangementNow, relayout, resort, refilter,
                            recordView](table::SavedView const& saved, bool shared) {
        auto const apply = [this, columns, hidden, fittedView, arrangementNow, relayout, resort, refilter, recordView,
                            saved, shared] {
            table::StateDocument const view = fittedView(saved);
            table::SavedView const now = arrangementNow();
            runtime().batch([&] {
                if (view.layout && layoutOver(*columns, *hidden, view.layout).value() != now.layout) {
                    relayout(*view.layout);
                }
                SortChain const chain = view.sort.value_or(SortChain{});
                resort([&chain](SortChain const&) { return SortChange{.chain = chain, .refusal = {}}; });
                if (table::FilterSpec const filters = view.filters.value_or(table::FilterSpec{});
                    filters != now.filters) {
                    static_cast<void>(refilter(filters));
                }
                recordView(saved.name, shared);
            });
        };
        if (!config.prepare) {
            apply();
            return;
        }
        // The rows take a filter on a column only once they have taken in its kind, which putting it in use gives
        // them when they next flush their changes: the view is applied in a task after that.
        config.prepare(table::StateDocument{.version = table::kStateVersion,
                                            .layout = {},
                                            .sort = saved.sort,
                                            .filters = saved.filters,
                                            .view = {},
                                            .carried = {}});
        runtime().owner().post(config.viewsScope.token().guard(apply));
    };
    // A views change answers on the owner: its refusal goes to the status line, and what it did once done.
    auto const changeViews = [&config](async::Completion<TableViews::Result> completion, std::function<void()> done) {
        completion.then(config.viewsScope, [&config, done = std::move(done)](TableViews::Result const& result) {
            if (!result) {
                config.message(result.error());
                return;
            }
            done();
        });
        // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks): the analyzer loses the owner of the callable `std::function` heap-allocates for this capture; the function object owns it and frees it
    };
    auto const nameView = [&config, arrangementNow, recordView, changeViews] {
        ViewDialog const dialog = config.viewDialog.peek();
        std::string name = dialog.name;
        name.erase(0, name.find_first_not_of(" \t"));
        name.erase(name.find_last_not_of(" \t") + 1);
        TableViews* const views = config.views;
        if (name.empty() || views == nullptr) {
            return;
        }
        config.viewDialog.set(ViewDialog{});
        config.touch();
        table::SavedView view = arrangementNow();
        view.name = name;
        switch (dialog.purpose) {
            case ViewNaming::Save: {
                // A name the user has, in the views stored when the save is applied, replaces that view.
                changeViews(views->saveView(std::move(view)), [&config, recordView, name] {
                    recordView(name, false);
                    config.message("Saved view " + name);
                });
                break;
            }
            case ViewNaming::Rename: {
                if (!config.savedView || config.viewShared) {
                    return;
                }
                changeViews(views->change(table::RenameView{.from = *config.savedView, .to = name}),
                            [recordView, name] { recordView(name, false); });
                break;
            }
            case ViewNaming::Publish: {
                // A name the shared views have, as stored when the publish is applied, replaces that view.
                changeViews(views->publishView(std::move(view)),
                            [&config, name] { config.message("Published view " + name); });
                break;
            }
            default:
                break;
        }
    };
    auto const viewCommand = [&config, applyView, recordView, changeViews](TableMenuEntry const& entry) {
        TableViews* const views = config.views;
        if (views == nullptr) {
            return;
        }
        std::optional<std::string> const current = config.viewShared ? std::nullopt : config.savedView;
        auto const naming = [&config](ViewNaming purpose, std::string name) {
            config.viewDialog.set(ViewDialog{.open = true, .purpose = purpose, .name = std::move(name)});
        };
        switch (entry.command) {
            case MenuCommand::ApplyView:
                if (table::SavedView const* const view = views->ownView(entry.argument)) {
                    applyView(table::SavedView{*view}, false);
                }
                break;
            case MenuCommand::ApplySharedView:
                if (table::SavedView const* const view = views->sharedView(entry.argument)) {
                    applyView(table::SavedView{*view}, true);
                }
                break;
            case MenuCommand::SaveView:
                naming(ViewNaming::Save, current.value_or(std::string{}));
                break;
            case MenuCommand::RenameView:
                if (current) {
                    naming(ViewNaming::Rename, *current);
                }
                break;
            case MenuCommand::PublishView:
                naming(ViewNaming::Publish, config.savedView.value_or(std::string{}));
                break;
            case MenuCommand::RemoveView:
                if (current) {
                    changeViews(views->change(table::RemoveView{*current}),
                                [recordView] { recordView(std::nullopt, false); });
                }
                break;
            case MenuCommand::SetDefaultView:
                if (current) {
                    changeViews(views->change(table::SetDefaultView{current}), [] {});
                }
                break;
            case MenuCommand::DefaultOnMount:
                changeViews(views->change(table::SetDefaultOnMount{!views->own().defaultOnMount}), [] {});
                break;
            case MenuCommand::SaveAsMyView:
                if (table::SavedView const* const view = views->sharedView(entry.argument)) {
                    std::string const name = view->name;
                    changeViews(views->change(table::AddView{*view}), [&config, recordView, name] {
                        recordView(name, false);
                        config.message("Saved view " + name);
                    });
                }
                break;
            case MenuCommand::SortAscending:
            case MenuCommand::SortDescending:
            case MenuCommand::AddToSort:
            case MenuCommand::RemoveFromSort:
            case MenuCommand::Filter:
            case MenuCommand::FilterGroup:
            case MenuCommand::MoveLeft:
            case MenuCommand::MoveRight:
            case MenuCommand::PinColumn:
            case MenuCommand::UnpinColumn:
            case MenuCommand::HideColumn:
            case MenuCommand::DisplayUnit:
            case MenuCommand::Columns:
            case MenuCommand::SortDialog:
            case MenuCommand::ClearAllFilters:
            case MenuCommand::ResetColumns:
            case MenuCommand::Copy:
            case MenuCommand::CopyWithHeaders:
            case MenuCommand::None:
            default:
                break;
        }
    };
    // A menu is built when it opens, and again when an entry is chosen, from the chain and the layout of the moment,
    // so the path the backend reports names the entry it showed.
    auto const entriesOf = [this, &config, &element, columns, sortable, filterable, layoutHeld, groupsNow,
                            groupArgument, filtersNow,
                            viewsInput](std::optional<std::size_t> column) -> std::vector<TableMenuEntry> {
        return runtime().untracked([&]() -> std::vector<TableMenuEntry> {
            table::ColumnLayout const layout = layoutHeld();
            // The groups, each named in its members' menus and in the table's.
            auto const groupsOf = [&](std::string_view columnId) {
                std::vector<MenuGroup> out;
                for (auto const& [group, formed] : groupsNow()) {
                    if (columnId.empty() || std::ranges::contains(group.columns, columnId)) {
                        out.push_back(MenuGroup{.id = groupArgument(group, formed),
                                                .label = group.label,
                                                .columns = group.columns.size()});
                    }
                }
                return out;
            };
            if (!column) {
                return offeredEntries(tableMenu(TableMenuInput{.anyFilter = !filtersNow().empty(),
                                                               .groups = groupsOf({}),
                                                               .allowed = config.allowed,
                                                               .views = viewsInput()}));
            }
            if (*column >= columns->size()) {
                return {};
            }
            std::vector<std::string> const visible = layout.visible();
            LayoutColumn const& index = columns->at(*column);
            SortChain const chain = sortChainOf(tableSort(element));
            auto const held = std::ranges::find(chain, index.id, &table::SortKey::column);
            std::vector<MenuDimension> dimensions =
                config.dimensions ? config.dimensions(index.id) : std::vector<MenuDimension>{};
            table::ColumnLayoutValue const value = layout.value();
            if (auto const units = value.units.find(index.id); units != value.units.end()) {
                for (MenuDimension& dimension : dimensions) {
                    if (auto const chosen = units->second.find(dimension.dimension); chosen != units->second.end()) {
                        dimension.chosen = chosen->second;
                    }
                }
            }
            return offeredEntries(headerMenu(HeaderMenuInput{
                .column = index,
                .sortable = sortable->at(*column),
                .filterable = filterable->at(*column),
                .sortPlace =
                    held == chain.end() ? std::nullopt : std::optional{static_cast<std::size_t>(held - chain.begin())},
                .chainSize = chain.size(),
                .maxSortKeys = config.maxSortKeys(),
                .visibleIndex = static_cast<std::size_t>(std::ranges::find(visible, index.id) - visible.begin()),
                .visibleCount = visible.size(),
                .pinned = layout.pinned(),
                .dimensions = std::move(dimensions),
                .groups = groupsOf(index.id),
                .allowed = config.allowed,
            }));
        });
    };
    // While the header is disabled the table's menu offers copy alone; the mount greys a column's menu itself.
    // NOLINTNEXTLINE(bugprone-exception-escape): copying a `Prop` throws `bad_variant_access` only from a valueless source, which `copyOf` documents and nothing here builds
    options.menu = [entriesOf, headerOn](std::optional<std::size_t> column) {
        std::vector<TableMenuEntry> entries = entriesOf(column);
        if (!column && !headerOn()) {
            for (TableMenuEntry& entry : entries) {
                if (entry.command != MenuCommand::Copy && entry.command != MenuCommand::CopyWithHeaders) {
                    entry.entry.enabled = false;
                }
            }
        }
        return menuEntries(entries);
    };
    // NOLINTNEXTLINE(bugprone-exception-escape): copying a `Prop` throws `bad_variant_access` only from a valueless source, which `copyOf` documents and nothing here builds
    options.onMenu = [&config, &menu, entriesOf, sortBy, arrange, columns, openColumn, openGroup, refilter, copy,
                      headerOn, viewCommand](std::optional<std::size_t> column, std::vector<std::size_t> const& path) {
        config.touch();
        std::vector<TableMenuEntry> const entries = entriesOf(column);
        TableMenuEntry const* const entry = chosen(entries, path);
        if (entry == nullptr) {
            return;
        }
        // A menu opened while the header was enabled cannot sort, filter or arrange once it is not.
        if (entry->command != MenuCommand::Copy && entry->command != MenuCommand::CopyWithHeaders && !headerOn()) {
            config.message(std::string{tableText::kHeaderBusy});
            return;
        }
        std::size_t const index = column.value_or(columns->size());
        std::string const columnId = index < columns->size() ? columns->at(index).id : std::string{};
        switch (entry->command) {
            case MenuCommand::SortAscending:
                sortBy(index, SortGesture::Ascending);
                break;
            case MenuCommand::SortDescending:
                sortBy(index, SortGesture::Descending);
                break;
            case MenuCommand::AddToSort:
                sortBy(index, SortGesture::Add);
                break;
            case MenuCommand::RemoveFromSort:
                sortBy(index, SortGesture::Remove);
                break;
            case MenuCommand::MoveLeft:
                arrange(MoveColumn{.id = columnId, .by = -1});
                break;
            case MenuCommand::MoveRight:
                arrange(MoveColumn{.id = columnId, .by = 1});
                break;
            case MenuCommand::PinColumn:
                arrange(PinColumn{.id = columnId});
                break;
            case MenuCommand::UnpinColumn:
                arrange(UnpinColumn{.id = columnId});
                break;
            case MenuCommand::HideColumn:
                arrange(HideColumn{.id = columnId});
                break;
            case MenuCommand::ResetColumns:
                arrange(ResetColumns{});
                break;
            case MenuCommand::Columns:
                config.chooserOpen.set(true);
                break;
            case MenuCommand::SortDialog:
                config.sortOpen.set(true);
                break;
            case MenuCommand::Filter:
                if (!columnId.empty()) {
                    openColumn(columnId);
                }
                break;
            case MenuCommand::FilterGroup:
                openGroup(entry->argument);
                break;
            case MenuCommand::ClearAllFilters:
                menu.close();
                static_cast<void>(refilter(table::FilterSpec{}));
                break;
            case MenuCommand::DisplayUnit: {
                // Choosing the checked unit clears it: the cells show their own units again.
                std::size_t const cut = entry->argument.find('\x1F');
                if (cut != std::string::npos && !columnId.empty()) {
                    std::string const unit = entry->argument.substr(cut + 1);
                    arrange(SetDisplayUnit{.id = columnId,
                                           .dimension = entry->argument.substr(0, cut),
                                           .unit = entry->entry.checked == true ? std::nullopt : std::optional{unit}});
                }
                break;
            }
            case MenuCommand::Copy:
                copy(false);
                break;
            case MenuCommand::CopyWithHeaders:
                copy(true);
                break;
            case MenuCommand::ApplyView:
            case MenuCommand::SaveView:
            case MenuCommand::RenameView:
            case MenuCommand::RemoveView:
            case MenuCommand::SetDefaultView:
            case MenuCommand::DefaultOnMount:
            case MenuCommand::ApplySharedView:
            case MenuCommand::PublishView:
            case MenuCommand::SaveAsMyView:
                viewCommand(*entry);
                break;
            case MenuCommand::None:
            default:
                // `offeredEntries` offers no other entry.
                break;
        }
    };
    // The column chooser and the sort dialog apply each change as it is made, through the gestures the header and
    // the menus use; each change clears the status line's message first, as every gesture on the table does.
    Node chooser = columnChooser(ColumnChooserView{
        .open = [&config] { return config.chooserOpen.get(); },
        .state =
            [&config, columns, layoutHeld] {
                static_cast<void>(config.version.get());
                return chooserState(layoutHeld(), *columns, config.allowed);
            },
        .draft = [&config] { return config.groupDraft.get(); },
        .setDraft = [&config](GroupDraft draft) { config.groupDraft.set(std::move(draft)); },
        .onShow =
            [arrange](std::string const& columnId, bool shown) {
                if (shown) {
                    arrange(ShowColumn{.id = columnId});
                } else {
                    arrange(HideColumn{.id = columnId});
                }
            },
        .onMove = [arrange](std::string const& columnId,
                            int delta) { arrange(MoveColumn{.id = columnId, .by = delta}); },
        .onReset = [arrange] { arrange(ResetColumns{}); },
        .onFormGroup =
            [this, arrange, layoutHeld](std::string label, std::vector<std::string> members) {
                std::size_t const before = runtime().untracked([&] { return layoutHeld().groups().size(); });
                arrange(FormGroup{.label = std::move(label), .columns = std::move(members)});
                return runtime().untracked([&] { return layoutHeld().groups().size(); }) > before;
            },
        // A removed group takes its filter with it: the group filter that names the group's columns.
        .onRemoveGroup =
            [this, &config, &menu, arrange, layoutHeld, filtersNow, refilter](std::string const& columnId) {
                std::vector<std::string> members;
                std::size_t const before = runtime().untracked([&] {
                    table::ColumnLayout const layout = layoutHeld();
                    auto const group = std::ranges::find(layout.groups(), columnId, &table::ColumnGroupInfo::id);
                    if (group != layout.groups().end()) {
                        members = group->columns;
                    }
                    return layout.groups().size();
                });
                arrange(RemoveGroup{.id = columnId});
                if (runtime().untracked([&] { return layoutHeld().groups().size(); }) >= before) {
                    return;
                }
                FilterMenuState const& open =
                    runtime().untracked([&]() -> FilterMenuState const& { return menu.state(); });
                if (open.open && open.target.formed && open.target.columns == members) {
                    menu.close();
                }
                table::FilterSpec spec = runtime().untracked(filtersNow);
                auto const dropped = std::erase_if(spec.groups, [&](table::GroupFilter const& group) {
                    return group.group.empty() && group.columns == members;
                });
                if (dropped > 0 && config.setFilters) {
                    static_cast<void>(refilter(spec));
                }
            },
        .onClose =
            [&config] {
                config.chooserOpen.set(false);
                config.groupDraft.set(GroupDraft{});
            },
    });
    Node sorter = sortDialog(SortDialogView{
        .open = [&config] { return config.sortOpen.get(); },
        .chain = [&element] { return sortChainOf(tableSort(element)); },
        .sortable =
            [columns, sortable] {
                std::vector<SortColumnOption> out;
                for (std::size_t at = 0; at < columns->size(); ++at) {
                    if (sortable->at(at)) {
                        out.push_back(SortColumnOption{.id = columns->at(at).id, .label = columns->at(at).label});
                    }
                }
                return out;
            },
        .onEdit =
            [&config, columns, sortable, resort](SortEdit const& edit) {
                config.touch();
                std::vector<std::string> ids;
                for (std::size_t at = 0; at < columns->size(); ++at) {
                    if (sortable->at(at)) {
                        ids.push_back(columns->at(at).id);
                    }
                }
                resort(
                    [&](SortChain const& current) { return applySortEdit(current, ids, edit, config.maxSortKeys()); });
            },
        .onClose = [&config] { config.sortOpen.set(false); },
    });
    std::vector<Node> dialogs{std::move(chooser), std::move(sorter), filterMenuDialog(menu)};
    if (config.views != nullptr) {
        dialogs.push_back(viewNameDialog(ViewNameDialogView{
            .open = [&config] { return config.viewDialog.get().open; },
            .title =
                [&config] {
                    switch (config.viewDialog.get().purpose) {
                        case ViewNaming::Rename:
                            return std::string{"Rename view"};
                        case ViewNaming::Publish:
                            return std::string{"Publish view"};
                        case ViewNaming::Save:
                        default:
                            return std::string{"Save view"};
                    }
                },
            .name = [&config] { return config.viewDialog.get().name; },
            .onName =
                [&config](std::string name) {
                    ViewDialog dialog = config.viewDialog.peek();
                    dialog.name = std::move(name);
                    config.viewDialog.set(std::move(dialog));
                },
            .onSave = nameView,
            .onClose = [&config] { config.viewDialog.set(ViewDialog{}); },
        }));
    }
    return ui::column(Column{.children = std::move(dialogs)});
}

Node ScreenCore::lowerModelTable(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    if (element == nullptr || element->modelTable == nullptr || element->modelTable->window == nullptr) {
        return ui::text(Text{.text = std::string{"table"}, .role = TextRole::Muted});
    }
    ModelTable& model = *element->modelTable;
    // One row, keyed by the description's revision: a new revision mounts the table again, with the columns, levels
    // and cells of that description, while the rows, their sort and filters, the selection and the message, which
    // the element holds, stay.
    Common placed;
    commonOf(node, ctx, element, placed);
    Common listCommon;
    listCommon.layout = placed.layout;
    Node list = ui::forEach<Value>([&model] { return std::vector<Value>{Value{model.revision.get()}}; },
                                   [](Value const& revision) -> Key { return Key{*revision.asString()}; },
                                   [this, ctx, &node, element](reactive::Signal<Value> const& revision) {
                                       static_cast<void>(revision);
                                       return modelTableOf(node, ctx, *element);
                                   },
                                   Axis::Vertical, 0, listCommon);
    std::span<DocNodePtr const> const empty = node.nodes("empty");
    if (empty.empty()) {
        return list;
    }
    // `empty` shows once a reply said the view has no row, exactly, and no row is undecided.
    Column wrapper;
    wrapper.common.layout = placed.layout;
    wrapper.children.push_back(std::move(list));
    Column content{.children = {lowerAll(empty, ctx)}};
    content.common.visible = slotProp<bool>([&model] {
        static_cast<void>(model.version.get());
        TableCountsView const counts = model.rows->counts();
        return counts.view.known && counts.view.rows == 0 && counts.view.kind == table::TotalKind::Exact &&
               counts.undecided == 0;
    });
    wrapper.children.push_back(ui::column(std::move(content)));
    return ui::column(std::move(wrapper));
}

}  // namespace morph::ui::detail
