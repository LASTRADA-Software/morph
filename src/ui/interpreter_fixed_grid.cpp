// SPDX-License-Identifier: Apache-2.0

// Lowering fixed grids.

#include "interpreter_grid_detail.hpp"

namespace morph::ui::detail {

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one test per way a table can be configured, in the order the document lists them.
[[nodiscard]] std::pair<Configure, bool> configurableOf(DocNode const& node) {
    ExprPtr const expr = node.prop("configurable");
    if (!expr || expr->kind != ExprKind::Literal) {
        return {Configure::All, true};
    }
    if (bool const* const flag = expr->literal.asBool()) {
        return {*flag ? Configure::All : Configure::None, *flag};
    }
    Configure allowed = Configure::None;
    if (ValueList const* const names = expr->literal.asList()) {
        for (Value const& item : *names) {
            std::string const* const name = item.asString();
            std::string_view const named = name == nullptr ? std::string_view{} : std::string_view{*name};
            allowed =
                allowed |
                (named == "reorder" ? Configure::Reorder
                 // NOLINTBEGIN(readability-avoid-nested-conditional-operator): a flat pick over the configurable names, one per line
                 : named == "hide"   ? Configure::Hide
                 : named == "pin"    ? Configure::Pin
                 : named == "resize" ? Configure::Resize
                                     : Configure::None);
            // NOLINTEND(readability-avoid-nested-conditional-operator)
        }
    }
    return {allowed, true};
}

[[nodiscard]] table::ColumnLayout layoutOver(std::span<LayoutColumn const> columns,
                                             std::vector<std::string> const& declaredHidden,
                                             std::optional<table::ColumnLayoutValue> const& value) {
    std::vector<std::string> ids;
    ids.reserve(columns.size());
    for (LayoutColumn const& column : columns) {
        ids.push_back(column.id);
    }
    table::ColumnLayout layout{std::move(ids), declaredHidden};
    if (value) {
        layout.apply(*value);
    }
    return layout;
}

namespace {

/// A layout read from a document's value; none for `null`, or for a value that is not a layout.
[[nodiscard]] std::optional<table::ColumnLayoutValue> layoutFromValue(Value const& value) {
    if (value.isNull()) {
        return std::nullopt;
    }
    table::ColumnLayoutValue out;
    if (glz::read<glz::opts{.error_on_unknown_keys = false}>(out, canonicalJson(value))) {
        return std::nullopt;
    }
    return out;
}

}  // namespace

[[nodiscard]] std::optional<table::ColumnLayoutValue> layoutNow(TableConfig const& config) {
    static_cast<void>(config.version.get());
    if (config.bound != nullptr) {
        return layoutFromValue(config.bound->get());
    }
    return config.held;
}

// NOLINTNEXTLINE(misc-no-recursion): a band nests bands, and the walk follows them.
[[nodiscard]] std::vector<table::BandInfo> bandsOf(std::span<DocNodePtr const> parts) {
    std::vector<table::BandInfo> bands;
    for (DocNodePtr const& part : parts) {
        table::BandInfo band{.label = std::string{part->name("label")},
                             .unit = std::string{part->name("unit")},
                             .columns = {},
                             .bands = bandsOf(part->partList("bands"))};
        if (ExprPtr const members = part->prop("columns"); members != nullptr && members->kind == ExprKind::Literal) {
            if (ValueList const* const list = members->literal.asList()) {
                for (Value const& member : *list) {
                    if (std::string const* const columnId = member.asString()) {
                        band.columns.push_back(*columnId);
                    }
                }
            }
        }
        bands.push_back(std::move(band));
    }
    return bands;
}

namespace {

/// A grid row's record, as its columns' expressions read it: each member's encoded value, `null` when blank or not
/// encoding.
[[nodiscard]] Value gridRowRecord(forms::RowState const& row) {
    ValueRecord record;
    for (auto const& member : row.fields()) {
        forms::Encoding const& encoding = member->encoded().get();
        record.insert_or_assign(member->spec().name,
                                encoding.has_value() && encoding->has_value() ? (*encoding)->value : Value{});
    }
    return Value{std::move(record)};
}

}  // namespace

[[nodiscard]] std::vector<SummarySpec> summarySpecsOf(std::span<DocNodePtr const> parts) {
    std::vector<SummarySpec> specs;
    for (DocNodePtr const& part : parts) {
        specs.push_back(SummarySpec{.key = std::string{part->name("key")}, .top = part->name("position") == "top"});
    }
    return specs;
}

namespace {

/// A fixed grid's copy and cell menu, set before each mount begins, as a mount's own slots are: a copy writes each
/// cell's text as the grid shows it under its column's label, and a cell menu opens on the row's record, with no
/// filter.
void fixedGridSlots(reactive::Runtime& runtime, GridTable& held) {
    GridTable* const grid = &held;
    held.config.gather = [&runtime, grid](std::vector<Key> keys, std::vector<std::string> const& ids,
                                          CopyDone const& done) {
        CopyGathered gathered;
        std::vector<std::string> kept;
        for (std::string const& columnId : ids) {
            auto const found = std::ranges::find(grid->config.columns, columnId, &LayoutColumn::id);
            if (found != grid->config.columns.end()) {
                kept.push_back(columnId);
                gathered.headers.push_back(found->label);
            }
        }
        FixedGridSource const& source = *grid->fixed;
        runtime.untracked([&] {
            for (Key const& key : keys) {
                std::optional<std::size_t> const position = source.positionOf(key);
                if (!position) {
                    continue;
                }
                CopyRow row{.viewRow = static_cast<std::int64_t>(*position), .cells = {}};
                for (std::string const& columnId : kept) {
                    row.cells.emplace_back(source.text(key, columnId));
                }
                gathered.rows.push_back(std::move(row));
            }
        });
        done(std::move(gathered));
    };
    held.config.heldCell = [grid](Key const& key, std::string const& columnId) -> std::optional<HeldCell> {
        static_cast<void>(columnId);
        std::optional<std::size_t> const position = grid->fixed->positionOf(key);
        std::optional<TableRowRef> const ref =
            position ? grid->fixed->view().row(static_cast<std::int64_t>(*position)) : std::nullopt;
        if (!ref) {
            return std::nullopt;
        }
        return HeldCell{.record = ref->row, .cell = table::Cell{}, .shown = {}, .units = {}, .keep = nullptr};
    };
}

}  // namespace

// A fixed grid, made once per element as a grid of a form is: its rows the table's `layoutRows`, each block's repeated
// for each row of its collection, and each cell a field of the form `form` names, an expression read in the table's
// scope (in a block, with `row` its collection row's record), or a text, with a grid column's overlays read the same
// way. A row's `label` is read once. Its header, its menus and its arrangement are
// a table's, with no column that sorts or filters. What its cells' expressions keep holds the scope weakly.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): one section per part of a fixed grid.
Node ScreenCore::lowerFixedGrid(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    auto const muted = [] { return ui::text(Text{.text = std::string{}, .role = TextRole::Muted}); };
    if (element == nullptr) {
        return muted();
    }
    if (element->gridTable != nullptr) {
        return gridTableNode(node, ctx, *element);
    }
    forms::FormSession* form = nullptr;
    if (!node.name("form").empty()) {
        NameRef const* const named = ctx.scope->find(node.name("form"));
        form = named != nullptr && named->element != nullptr ? named->element->form.get() : nullptr;
        if (form == nullptr) {
            return muted();
        }
    }
    DetachedLower const detached = detachedLower(ctx);
    // An expression read in the table's scope, `row` the block row's record when there is one; nothing once the scope
    // is gone. Tracked.
    auto const reader = [detached](ExprPtr const& expr) {
        return [detached, expr](forms::RowState const* row) -> std::optional<std::pair<Value, FormatContext>> {
            LowerCtx const live = attached(detached);
            if (live.scope == nullptr) {
                return std::nullopt;
            }
            Bindings bindings = live.bindings != nullptr ? *live.bindings : Bindings{};
            if (row != nullptr) {
                Value const record = gridRowRecord(*row);
                // NOLINTNEXTLINE(performance-no-automatic-move): the binding's read runs on every evaluation and each returns a copy of the captured record, so the capture stays const
                bindings.push_back(Binding{.name = "row", .read = [record] { return record; }});
            }
            return std::pair{evalIn(*live.scope, &bindings, expr), live.scope->formatting()};
        };
    };
    auto const count = [](ExprPtr const& expr) {
        std::int64_t const* const literal =
            expr != nullptr && expr->kind == ExprKind::Literal ? expr->literal.asInt() : nullptr;
        return literal != nullptr && *literal > 0 ? static_cast<std::size_t>(*literal) : std::size_t{1};
    };
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): lowers one row part of a grid: its cells and options in document order, each reading the row part before
    auto const rowOf = [&](DocNode const& part) {
        ExprPtr const label = part.prop("label");
        std::string const key{part.name("key")};
        FixedRow made{
            .key = key, .label = label ? textOf(once(ctx, label), ctx.scope->formatting()) : key, .cells = {}};
        for (DocNodePtr const& record : part.partList("cells")) {
            for (DocNodePtr const& given : record->partList("cells")) {
                FixedCell cell;
                cell.field = std::string{given->name("field")};
                cell.text = std::string{given->name("text")};
                cell.rows = count(given->prop("rows"));
                cell.columns = count(given->prop("columns"));
                if (ExprPtr const value = given->prop("value")) {
                    cell.value = [read = reader(value)](forms::RowState const* row) {
                        auto const readRow = read(row);
                        return readRow ? textOf(readRow->first, readRow->second) : std::string{};
                    };
                }
                if (ExprPtr const readonly = given->prop("readonly")) {
                    cell.readonly = [read = reader(readonly)](forms::RowState const* row) {
                        auto const readRow = read(row);
                        bool const* const flag = readRow ? readRow->first.asBool() : nullptr;
                        return flag != nullptr && *flag;
                    };
                }
                if (ExprPtr const decimals = given->prop("decimals")) {
                    cell.decimals =
                        [read = reader(decimals)](forms::RowState const* row) -> std::optional<std::uint32_t> {
                        auto const readRow = read(row);
                        int const places = readRow ? placesOf(readRow->first, readRow->second) : -1;
                        return places >= 0 ? std::optional{static_cast<std::uint32_t>(places)} : std::nullopt;
                    };
                }
                if (ExprPtr const unit = given->prop("unit")) {
                    cell.unit = [read = reader(unit)](forms::RowState const* row) -> std::optional<std::string> {
                        auto const readRow = read(row);
                        std::string const* const columnId = readRow ? readRow->first.asString() : nullptr;
                        return columnId != nullptr && !columnId->empty() ? std::optional{*columnId} : std::nullopt;
                    };
                }
                if (ExprPtr const offered = given->prop("options")) {
                    cell.options = [read = reader(offered)](
                                       forms::RowState const* row) -> std::optional<std::vector<std::string>> {
                        auto const readRow = read(row);
                        ValueList const* const items = readRow ? readRow->first.asList() : nullptr;
                        if (items == nullptr) {
                            return std::nullopt;
                        }
                        std::vector<std::string> kept;
                        for (Value const& item : *items) {
                            kept.push_back(glz::write_json(valueToJson(item)).value_or(std::string{}));
                        }
                        return kept;
                    };
                }
                if (ExprPtr const placeholder = given->prop("placeholder")) {
                    cell.placeholder = [read = reader(placeholder)](forms::RowState const* row) {
                        auto const readRow = read(row);
                        return readRow ? textOf(readRow->first, readRow->second) : std::string{};
                    };
                }
                made.cells.insert_or_assign(given->id, std::move(cell));
            }
        }
        return made;
    };
    std::vector<FixedGridSource::Part> parts;
    for (DocNodePtr const& part : node.partList("layoutRows")) {
        if (std::string_view const forEach = part->name("forEach"); !forEach.empty()) {
            FixedBlock block{.forEach = std::string{forEach}, .rows = {}};
            for (DocNodePtr const& row : part->partList("rows")) {
                block.rows.push_back(rowOf(*row));
            }
            parts.emplace_back(std::move(block));
        } else {
            parts.emplace_back(rowOf(*part));
        }
    }
    std::vector<GridColumn> columns;
    for (DocNodePtr const& column : node.partList("columns")) {
        ExprPtr const label = column->prop("label");
        columns.push_back(GridColumn{.id = column->id,
                                     .label = label ? textOf(once(ctx, label), ctx.scope->formatting()) : column->id,
                                     .field = {},
                                     .value = {}});
    }
    GridOptions options;
    gridOptions(node, ctx, options);
    auto grid = std::make_unique<GridTable>(runtime());
    GridTable* const held = grid.get();
    TableConfig& config = held->config;
    config.element = element;
    std::tie(config.allowed, config.menus) = configurableOf(node);
    for (GridColumn const& column : columns) {
        config.columns.push_back(
            LayoutColumn{.id = column.id, .label = column.label, .locked = false, .kind = table::ColumnKind::Text});
        config.sortable.push_back(false);
        held->shown.push_back(TableColumn{.id = column.id, .label = column.label});
    }
    config.maxSortKeys = [] { return std::size_t{8}; };
    config.message = [this, element](std::string text) { tableMessage(*element, std::move(text)); };
    config.touch = [this, element] { tableMessage(*element, std::string{}); };
    config.rows = [held]() -> TableRows* { return held->fixed ? &held->fixed->view() : nullptr; };
    if (ExprPtr const layout = node.prop("layout");
        layout && (layout->kind != ExprKind::Literal || !layout->literal.isNull())) {
        RtScope& scope = *element->scope;
        config.bound = &scope.nodes.computed(
            [&scope, bindings = ctx.bindings, layout] { return evalIn(scope, bindings.get(), layout); });
    }
    try {
        held->fixed = std::make_shared<FixedGridSource>(runtime(), form, std::move(parts), std::move(columns),
                                                        ctx.scope->formatting());
    } catch (std::invalid_argument const& failure) {
        report(node.path, failure.what());
        return muted();
    }
    held->grid.grid = std::make_shared<GridController>(runtime(), *held->fixed, std::move(options));
    held->grid.grid->keepAlive(held->fixed);
    held->fixed->onBeforeSubmit([weak = std::weak_ptr<GridController>{held->grid.grid}] {
        if (auto const live = weak.lock()) {
            live->leave();
        }
    });
    element->gridTable = std::move(grid);
    held->grid.grid->useClipboard([this] { return pasteboard ? pasteboard() : std::nullopt; });
    // The cell selection a document binds is shown; each change of it runs `onSelectionChange` with the new range.
    if (ExprPtr const bound = node.prop("selection");
        bound && (bound->kind != ExprKind::Literal || !bound->literal.isNull())) {
        element->scope->nodes.effect([this, held, scope = element->scope, bindings = ctx.bindings, bound] {
            std::optional<CellRange> const range = cellRangeOf(evalIn(*scope, bindings.get(), bound));
            runtime().untracked([&] {
                if (held->grid.grid->selection() != range) {
                    held->grid.grid->select(range);
                }
            });
        });
    }
    if (Commands const* const commands = node.event("onSelectionChange"); commands != nullptr && !commands->empty()) {
        held->grid.grid->onSelectionChange([this, detached, commands](std::optional<CellRange> const& range) {
            LowerCtx const live = attached(detached);
            if (live.scope != nullptr) {
                runEvent(commands, live, cellRangeValue(range));
            }
        });
    }
    // The walk follows the visible columns in layout order, and the cells the scope's formatting.
    RtScope* const scope = element->scope;
    scope->nodes.effect([this, held, scope] {
        TableConfig const& arranged = held->config;
        std::vector<std::string> visible =
            layoutOver(arranged.columns, arranged.declaredHidden, layoutNow(arranged)).visible();
        FormatContext format = scope->formatting();
        std::optional<CellRef> focused = held->grid.grid->focus();
        runtime().untracked([&] {
            held->fixed->setLayout(std::move(visible));
            held->fixed->setFormat(std::move(format));
            held->config.focused = std::move(focused);
        });
    });
    scope->nodes.effect([this, element, held] {
        std::string const text = held->grid.grid->message();
        static_cast<void>(held->grid.grid->notices());
        std::vector<StatusEntry> const offered = held->grid.grid->messageEntries();
        runtime().untracked([&] {
            std::vector<std::pair<StatusEntry, std::function<void()>>> entries;
            entries.reserve(offered.size());
            for (StatusEntry const& entry : offered) {
                entries.emplace_back(entry, [held, entryId = entry.id] { held->grid.grid->chooseEntry(entryId); });
            }
            tableMessage(*element, text, std::move(entries));
        });
    });
    held->setMountSlots = [this, held] { fixedGridSlots(runtime(), *held); };
    return gridTableNode(node, ctx, *element);
}

// A grid of a form, made once per element: every mount of the table shows the same grid, so its focus, its open
// editor, its messages, its layout, its sort and its filters outlive a remount. Its columns and walk are `gridParts`';
// its header, its menus, its cell menu and its arrangement are a table's (`configureTable`): an edit column whose part
// says `sortable` sorts, every edit column filters unless its part says `filterable: false`, and a display column does
// neither. The grid's own cell entries come after the built-ins and before the separator and the document's `cellMenu`
// entries. Sorting and filtering change the view, never the record's order.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): one section per part of a table's configuring.
Node ScreenCore::formGridTable(DocNode const& part, LowerCtx const& ctx, ElementState& element,
                               forms::FormSession& form, forms::FieldState& array) {
    if (element.gridTable != nullptr) {
        return gridTableNode(part, ctx, element);
    }
    auto const muted = [&array] { return ui::text(Text{.text = array.path(), .role = TextRole::Muted}); };
    if (array.spec().kind != forms::FieldKind::ObjectArray || array.spec().truncated) {
        report(part.path, "`" + array.path() + "` is not a list of records");
        return muted();
    }
    std::vector<GridColumn> columns;
    GridOptions options;
    gridParts(part, ctx, array, columns, options);
    auto grid = std::make_unique<GridTable>(runtime());
    GridTable* const held = grid.get();
    TableConfig& config = held->config;
    config.element = &element;
    std::tie(config.allowed, config.menus) = configurableOf(part);
    // A column part filters unless it says `filterable: false`; a display column never does.
    std::vector<bool> filterable;
    std::span<DocNodePtr const> const parts = part.partList("columns");
    for (std::size_t at = 0; at < columns.size(); ++at) {
        GridColumn const& column = columns.at(at);
        auto const member = std::ranges::find(array.spec().children, column.field, &forms::FieldSpec::name);
        // A member per row sorts and filters its drafts as text.
        bool const perRow = static_cast<bool>(column.fieldOf);
        bool const edits = perRow || (!column.field.empty() && member != array.spec().children.end());
        config.columns.push_back(
            LayoutColumn{.id = column.id,
                         .label = column.label,
                         .locked = false,
                         .kind = edits && !perRow ? detail::columnKindOf(member->kind) : table::ColumnKind::Text});
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): guarded by `at < parts.size()` in the same expression
        config.sortable.push_back(edits && at < parts.size() && parts[at]->flag("sortable"));
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): `at >= parts.size()` short-circuits before the index in the same expression
        filterable.push_back(edits && (at >= parts.size() || parts[at]->flag("filterable", true)));
        held->shown.push_back(TableColumn{.id = column.id, .label = column.label, .sortable = config.sortable.back()});
    }
    config.maxSortKeys = [] { return std::size_t{8}; };
    config.message = [this, &element](std::string text) { tableMessage(element, std::move(text)); };
    config.touch = [this, &element] { tableMessage(element, std::string{}); };
    config.filters = [held] {
        static_cast<void>(held->filterVersion.get());
        return held->filter;
    };
    config.setFilters = [this, &element, held](table::FilterSpec const& filter) {
        if (auto const set = held->grid.source->view().setFilter(filter); !set) {
            tableMessage(element, "The filter is refused: " + set.error().message);
            return false;
        }
        held->filter = filter;
        held->filterVersion.set(held->filterVersion.peek() + 1);
        return true;
    };
    config.rows = [held]() -> TableRows* { return held->grid.source ? &held->grid.source->view() : nullptr; };
    if (ExprPtr const layout = part.prop("layout");
        layout && (layout->kind != ExprKind::Literal || !layout->literal.isNull())) {
        RtScope& scope = *element.scope;
        config.bound = &scope.nodes.computed(
            [&scope, bindings = ctx.bindings, layout] { return evalIn(scope, bindings.get(), layout); });
    }
    // The slots a mount owns, set again before each mount begins: a mount that ends while it is the latest clears
    // them, and a later lowering of the same grid must find them set.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): sets a mount's slots in one pass; each slot is set from the same held grid and checked against the later mount
    held->setMountSlots = [this, held, filterable] {
        TableConfig& slots = held->config;
        slots.filterTarget = [held, filterable](std::string const& columnId) -> std::optional<FilterTarget> {
            auto const found = std::ranges::find(held->config.columns, columnId, &LayoutColumn::id);
            auto const index = static_cast<std::size_t>(found - held->config.columns.begin());
            if (held->grid.source == nullptr || found == held->config.columns.end() || !filterable.at(index)) {
                return std::nullopt;
            }
            std::span<table::ColumnInfo const> const infos = held->grid.source->view().source().columns();
            auto const info = std::ranges::find(infos, columnId, &table::ColumnInfo::id);
            if (info == infos.end()) {
                return std::nullopt;
            }
            FilterTarget target;
            target.column = columnId;
            target.label = found->label;
            target.shown = table::TableColumn{.info = *info};
            target.listed = true;
            return target;
        };
        // A cell a menu opens on: the member as its column's filter reads it, and the row's record.
        slots.heldCell = [held](Key const& key, std::string const& columnId) -> std::optional<HeldCell> {
            FormGridSource const* const source = held->grid.source.get();
            if (source == nullptr) {
                return std::nullopt;
            }
            std::optional<std::size_t> const position = source->positionOf(key);
            std::optional<TableRowRef> const ref =
                position ? held->grid.source->view().row(static_cast<std::int64_t>(*position)) : std::nullopt;
            if (!ref) {
                return std::nullopt;
            }
            std::optional<std::pair<table::Cell, table::TableColumn>> cell = source->filterCell(key, columnId);
            if (!cell) {
                return HeldCell{.record = ref->row, .cell = table::Cell{}, .shown = {}, .units = {}, .keep = nullptr};
            }
            return HeldCell{.record = ref->row,
                            .cell = std::move(cell->first),
                            .shown = std::move(cell->second),
                            .units = {},
                            .keep = nullptr};
        };
        // A copy: each cell's text as the grid shows it, under its column's label.
        slots.gather = [this, held](std::vector<Key> keys, std::vector<std::string> const& ids, CopyDone const& done) {
            CopyGathered gathered;
            std::vector<std::string> kept;
            for (std::string const& columnId : ids) {
                auto const found = std::ranges::find(held->config.columns, columnId, &LayoutColumn::id);
                if (found != held->config.columns.end()) {
                    kept.push_back(columnId);
                    gathered.headers.push_back(found->label);
                }
            }
            FormGridSource const& source = *held->grid.source;
            runtime().untracked([&] {
                for (Key const& key : keys) {
                    std::optional<std::size_t> const position = source.positionOf(key);
                    if (!position) {
                        continue;
                    }
                    CopyRow row{.viewRow = static_cast<std::int64_t>(*position), .cells = {}};
                    for (std::string const& columnId : kept) {
                        row.cells.emplace_back(source.text(key, columnId));
                    }
                    gathered.rows.push_back(std::move(row));
                }
            });
            done(std::move(gathered));
        };
    };
    for (GridColumn const& column : columns) {
        auto const member = std::ranges::find(array.spec().children, column.field, &forms::FieldSpec::name);
        if (member == array.spec().children.end()) {
            continue;
        }
        std::optional<std::uint32_t> places = member->decimalPlaces ? member->decimalPlaces : member->displayDecimals;
        if (!places && member->kind == forms::FieldKind::Integer) {
            places = 0;
        }
        held->places.insert_or_assign(column.id, places);
    }
    try {
        held->grid = makeFormGrid(form, array, std::move(columns), std::move(options), ctx.scope->formatting());
    } catch (std::invalid_argument const& failure) {
        report(part.path, failure.what());
        return muted();
    }
    // Each lowering makes the table node anew, over the grid kept here: the node's handlers hold the lowering's
    // scope, which holds this element, so the element never holds a node.
    held->grid.node = nullptr;
    element.gridTable = std::move(grid);
    held->grid.grid->useClipboard([this] { return pasteboard ? pasteboard() : std::nullopt; });
    // A `group` part groups the rows by its `by`, read with `row` the row's record, over the array's order; the
    // summaries of the table and of each run, and each run's heading, are rows of a decorator the mount is handed in
    // place of the grid's rows. What the decorator and `by` keep holds the scope weakly, as the columns do.
    std::span<DocNodePtr const> const groups = part.partList("group");
    DocNode const* const group = groups.empty() ? nullptr : groups.front().get();
    if (group != nullptr) {
        held->grouped = true;
        held->grid.source->view().setGroup(
            [detached = detachedLower(ctx), groupBy = group->prop("by")](forms::RowState const& row) -> Value {
                LowerCtx const live = attached(detached);
                if (live.scope == nullptr) {
                    return Value{};
                }
                Value const record = gridRowRecord(row);
                Bindings bindings = live.bindings != nullptr ? *live.bindings : Bindings{};
                // NOLINTNEXTLINE(performance-no-automatic-move): the binding's read runs on every evaluation and each returns a copy of the captured record, so the capture stays const
                bindings.push_back(Binding{.name = "row", .read = [record] { return record; }});
                return evalIn(*live.scope, &bindings, groupBy);
            });
    }
    std::vector<SummarySpec> tableSummaries = summarySpecsOf(part.partList("summaries"));
    std::vector<SummarySpec> runSummaries =
        group != nullptr ? summarySpecsOf(group->partList("summaries")) : std::vector<SummarySpec>{};
    if (group != nullptr || !tableSummaries.empty()) {
        SummaryRows::Runs runs;
        if (group != nullptr) {
            runs = [held] { return gridRuns(*held); };
        }
        held->summaries = std::make_shared<SummaryRows>(
            std::shared_ptr<TableRows>(std::shared_ptr<void>{}, &held->grid.source->view()), std::move(tableSummaries),
            std::move(runSummaries), std::move(runs));
        if (group != nullptr) {
            held->summaries->setHeadings([held](std::size_t run) { return gridHeading(*held, run); });
        }
        // A copy that spans a summary row writes its cells as they are drawn; the scope is held weakly.
        held->grid.grid->copySummaries(held->summaries, [this, &part, held, detached = detachedLower(ctx)](
                                                            Value const& record, std::string const& column) {
            LowerCtx const live = attached(detached);
            return live.scope == nullptr ? std::string{} : gridSummaryText(part, live, *held, record, column);
        });
    }
    // The cell selection a document binds is shown; each change of it runs `onSelectionChange` with the new range.
    if (ExprPtr const bound = part.prop("selection");
        bound && (bound->kind != ExprKind::Literal || !bound->literal.isNull())) {
        element.scope->nodes.effect([this, held, scope = element.scope, bindings = ctx.bindings, bound] {
            std::optional<CellRange> const range = cellRangeOf(evalIn(*scope, bindings.get(), bound));
            runtime().untracked([&] {
                if (held->grid.grid->selection() != range) {
                    held->grid.grid->select(range);
                }
            });
        });
    }
    if (Commands const* const commands = part.event("onSelectionChange"); commands != nullptr && !commands->empty()) {
        held->grid.grid->onSelectionChange(
            [this, detached = detachedLower(ctx), commands](std::optional<CellRange> const& range) {
                LowerCtx const live = attached(detached);
                if (live.scope != nullptr) {
                    runEvent(commands, live, cellRangeValue(range));
                }
            });
    }
    // The walk follows the visible columns in layout order, and the cells the scope's formatting; a copy with no row
    // selected copies the focused cell.
    RtScope* const scope = element.scope;
    scope->nodes.effect([this, held, scope] {
        TableConfig const& arranged = held->config;
        std::vector<std::string> visible =
            layoutOver(arranged.columns, arranged.declaredHidden, layoutNow(arranged)).visible();
        FormatContext format = scope->formatting();
        std::optional<CellRef> focused = held->grid.grid->focus();
        runtime().untracked([&] {
            held->grid.source->setLayout(std::move(visible));
            held->grid.source->setFormat(std::move(format));
            held->config.focused = std::move(focused);
        });
    });
    // The sort the element holds, or the document binds, orders the view; one naming no edit column is refused.
    scope->nodes.effect([this, &element, held, path = part.path] {
        Value const sort = tableSort(element);
        runtime().untracked([&] {
            SortChain chain;
            if (ValueList const* const keys = sort.asList()) {
                for (Value const& key : *keys) {
                    Value const* const column = key.field("column");
                    Value const* const direction = key.field("direction");
                    if (column != nullptr && column->asString() != nullptr) {
                        bool const descending = direction != nullptr && direction->asString() != nullptr &&
                                                *direction->asString() == "desc";
                        chain.push_back(table::SortKey{
                            .column = *column->asString(),
                            .dir = descending ? table::SortDirection::Descending : table::SortDirection::Ascending});
                    }
                }
            }
            // A grouped grid sorts within its runs: the run column leads every sort. The array's order already keeps
            // each run together, so an empty sort stays empty.
            if (held->grouped && !chain.empty()) {
                chain.insert(chain.begin(), table::SortKey{.column = std::string{kGroupRunColumn},
                                                           .dir = table::SortDirection::Ascending});
            }
            if (auto const set = held->grid.source->view().setSort(std::move(chain)); !set) {
                report(pointerAppend(path, "sort"), "the sort is refused: " + set.error().message);
            }
        });
    });
    scope->nodes.effect([this, &element, held] {
        std::string const text = held->grid.grid->message();
        static_cast<void>(held->grid.grid->notices());
        std::vector<StatusEntry> const offered = held->grid.grid->messageEntries();
        runtime().untracked([&] {
            std::vector<std::pair<StatusEntry, std::function<void()>>> entries;
            entries.reserve(offered.size());
            for (StatusEntry const& entry : offered) {
                entries.emplace_back(entry, [held, entryId = entry.id] { held->grid.grid->chooseEntry(entryId); });
            }
            tableMessage(element, text, std::move(entries));
        });
    });
    return gridTableNode(part, ctx, element);
}

// The table node of a grid kept in @p element: its status line, its configuring and its columns' filter buttons.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): builds the grid's table node: its status line, its configuring and each column's filter button in order
Node ScreenCore::gridTableNode(DocNode const& part, LowerCtx const& ctx, ElementState& element) {
    GridTable* const held = element.gridTable.get();
    TableOptions table;
    commonOf(part, ctx, &element, table.common);
    // The status line: the grid's notices, posted through `tableMessage` so they are held as a table's are, and the
    // state that lasts while its cause holds.
    table.status = slotProp<TableStatus>([held] {
        TableStatus status{.message = held->message.get(), .entries = held->entries.get()};
        status.standing = held->grid.grid->standing();
        status.standingEntries = held->grid.grid->standingEntries();
        status.shown = !status.message.empty() || !status.entries.empty() || !status.standing.empty() ||
                       !status.standingEntries.empty();
        return status;
    });
    table.onStatusEntry = [held](std::string const& entryId) {
        if (auto const handler = held->entryHandlers.find(entryId); handler != held->entryHandlers.end()) {
            std::function<void()> const run = handler->second;
            run();
            return;
        }
        held->grid.grid->chooseEntry(entryId);
    };
    if (held->setMountSlots) {
        held->setMountSlots();
    }
    held->config.beginMount();
    Node dialogs = configureTable(held->config, ctx, table, nullptr);
    // A copy of a cell selection writes its rectangle; with none the table's copy runs, the focused cell alone.
    table.onCopy = [this, held, &element, scope = ctx.scope, rows = std::move(table.onCopy)] {
        GridController& grid = *held->grid.grid;
        if (!runtime().untracked([&] { return grid.selection().has_value(); })) {
            if (rows) {
                rows();
            }
            return;
        }
        std::string const separator{scope->formatting().numeric.groupSeparator};
        if (std::optional<std::string> refused = grid.copyRefusal(separator)) {
            tableMessage(element, std::move(*refused));
            return;
        }
        std::string const text = grid.copy();
        if (text.empty()) {
            return;
        }
        std::size_t const lines = static_cast<std::size_t>(std::ranges::count(text, '\n')) + 1;
        if (!clipboard || !clipboard(text)) {
            tableMessage(element, "The rows could not be copied: there is no clipboard");
            return;
        }
        tableMessage(element, tableText::copied(lines, 0, separator));
    };
    // The table's own menu offers the focused row's entries after its own, past a separator.
    if (table.menu && table.onMenu) {
        auto const rowEntries = [this, held] {
            return runtime().untracked([&] {
                std::optional<CellRef> const focused = held->grid.grid->focus();
                return focused ? held->grid.grid->rowEntries(focused->key) : std::vector<MenuItem>{};
            });
        };
        auto const own = table.menu;
        table.menu = [this, own, rowEntries](std::optional<std::size_t> column) {
            std::vector<MenuEntry> entries = own(column);
            if (column) {
                return entries;
            }
            std::vector<MenuItem> const rows = rowEntries();
            if (!rows.empty() && !entries.empty()) {
                entries.push_back(MenuEntry{.separator = true});
            }
            runtime().untracked([&] {
                for (MenuItem const& item : rows) {
                    entries.push_back(MenuEntry{.label = item.label.evaluate(),
                                                .enabled = item.enabled.evaluate(),
                                                .description = item.description.evaluate()});
                }
            });
            return entries;
        };
        table.onMenu = [this, own, rowEntries, chosen = std::move(table.onMenu)](
                           std::optional<std::size_t> column, std::vector<std::size_t> const& path) {
            if (!column && !path.empty()) {
                std::size_t const shown = own(column).size();
                std::size_t const first = shown + (shown > 0 ? 1 : 0);
                if (path.front() >= first) {
                    std::vector<MenuItem> const rows = rowEntries();
                    std::size_t const index = path.front() - first;
                    if (index < rows.size() &&
                        runtime().untracked([&] { return rows.at(index).enabled.evaluate(); }) &&
                        rows.at(index).onSelect) {
                        rows.at(index).onSelect();
                    }
                    return;
                }
            }
            chosen(column, path);
        };
    }
    std::vector<TableColumn> columns = held->shown;
    markFilterable(held->config, columns, table);
    table.bands = bandsOf(part.partList("bands"));
    std::optional<TableSource> base;
    if (held->summaries != nullptr) {
        base = summarySource(part, ctx, *held, columns);
    }
    // A fixed grid's row is named by its label.
    if (held->fixed != nullptr) {
        TableSource named;
        named.rows = std::shared_ptr<TableRows>(held->grid.grid, &held->fixed->view());
        named.rowView = [fixed = held->fixed](reactive::Signal<Value> const& row) {
            TableRowLook look;
            reactive::Signal<Value> const* const record = &row;
            look.label = Prop<std::string>{[fixed, record] {
                std::optional<Key> const key = fixed->keyOf(record->get());
                return key ? fixed->label(*key) : std::string{};
            }};
            return look;
        };
        base = std::move(named);
    }
    return besideDialogs(table.common, std::move(dialogs),
                         [&] { return held->grid.grid->node(std::move(columns), std::move(table), std::move(base)); });
}

}  // namespace morph::ui::detail
