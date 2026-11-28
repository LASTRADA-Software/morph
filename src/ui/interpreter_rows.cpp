// SPDX-License-Identifier: Apache-2.0

// Lowering tables over rows and collections.

#include "interpreter_grid_detail.hpp"

namespace morph::ui {

std::shared_ptr<TableStates> detail::makeTableStates(Session& session, reactive::Runtime const& runtime,
                                                     SessionOptions const& options) {
    if (options.tableState == nullptr) {
        return nullptr;
    }
    // A save the server refuses for its open uploads is sent again after the session's `tableStateTimeout`, on the
    // session's timers; a store with none would send it again on the owner's next turn, a burst of retries.
    if (auto* const preferences = dynamic_cast<PreferencesTableStateStore*>(options.tableState.get());
        preferences != nullptr && options.scheduler != nullptr) {
        preferences->retryOn(*options.scheduler, options.tableStateTimeout);
    }
    auto states = std::make_shared<TableStates>(options.tableState, runtime.owner(), options.scheduler,
                                                options.tableStateDebounce);
    states->onSaveFailed([&session](table::TableStateKey const& key, table::SaveResult const& result) {
        session.tableSaveFailed(key, result);
    });
    return states;
}

void Session::tableSaveFailed(table::TableStateKey const& key, table::SaveResult const& result) {
    // A screen a message unmounts is passed over; one it does not stays attached.
    std::vector<detail::ScreenCore*> const screens = _screens;
    for (detail::ScreenCore* const screen : screens) {
        if (std::ranges::find(_screens, screen) != _screens.end()) {
            screen->tableSaveFailed(key, result);
        }
    }
}

namespace detail {

TableConfig::TableConfig(reactive::Runtime& runtime)
    : version{runtime, 0},
      chooserOpen{runtime, false},
      sortOpen{runtime, false},
      groupDraft{runtime, GroupDraft{}},
      filterMenu{std::make_unique<FilterMenu>(runtime)},
      viewDialog{runtime, ViewDialog{}} {}

ElementState::ElementState(reactive::Runtime& runtime)
    : draft{runtime},
      committed{runtime},
      edited{runtime, false},
      touched{runtime, false},
      focused{runtime, false},
      open{runtime, false},
      submitted{runtime, false},
      selection{runtime, Value{ValueList{}}},
      sort{runtime, Value{ValueList{}}},
      activeKey{runtime},
      checksVersion{runtime, 0},
      inner{runtime, nullptr} {}

ElementState::~ElementState() = default;

ScreenCore::ScreenCore(std::shared_ptr<Document const> doc, Session& owner, Dispatcher& calls,
                       InterpreterOptions const& options, MountOptions const& mount)
    : session{owner},
      dispatcher{calls},
      host{options.host},
      onDiagnostic{options.diagnostics},
      borrowed{mount.borrowed},
      depth{mount.depth},
      prefs{session.runtime()},
      submitted{session.runtime(), false},
      pendingWork{session.runtime(), 0},
      recording{owner.options().developmentTools == DevelopmentTools::On},
      _doc{std::move(doc)} {
    ValueRecord defaults;
    for (auto const& [name, point] : _doc->customize) {
        defaults.insert_or_assign(name, point.defaultValue);
    }
    prefDefaults = defaults;
    prefs.set(std::move(defaults));
    session.attachScreen(*this);
}

RtScope::~RtScope() {
    // A grid of a form calls back into its form's session, an element declared before it.
    for (auto const& element : elements) {
        element->fieldGrids.clear();
        element->gridTable.reset();
    }
    if (core.recording) {
        std::erase(core.scopes, this);
    }
    if (owner != nullptr) {
        core.addPending(-1);
        if (owner->inner.peek() == this) {
            owner->inner.set(nullptr);
        }
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): lowers one `rows` table: its columns, groups, summaries, selection and options in document order, each reading the element and config the previous set up
Node ScreenCore::lowerRows(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    ExprPtr const rowsExpr = node.prop("rows");
    if (!rowsExpr) {
        report(node.path, "a model-backed `table` needs the table engine, which this interpreter does not lower yet");
        return ui::text(Text{.text = std::string{"table"}, .role = TextRole::Muted});
    }
    std::function<std::vector<Value>()> rows = [scope = ctx.scope, bindings = ctx.bindings, rowsExpr] {
        Value const value = evalIn(*scope, bindings.get(), rowsExpr);
        ValueList const* const items = value.asList();
        return items == nullptr ? std::vector<Value>{} : std::vector<Value>{items->begin(), items->end()};
    };
    auto keyFn = [scope = ctx.scope, bindings = ctx.bindings, keyExpr = node.prop("key")](Value const& row) -> Key {
        if (Value const* const heading = row.field(kGroupHeadingField); heading != nullptr && heading->asString()) {
            return Key{*heading->asString()};
        }
        if (!keyExpr) {
            Value const rowKey = rowId(row);
            return keyOf(rowKey.isNull() ? row : rowKey);
        }
        ScopeEnv const env{*scope, bindings.get(), &row};
        return keyOf(evaluate(*keyExpr, env));
    };
    if (node.kind == "forEach") {
        std::vector<DocNode const*> content;
        for (DocNodePtr const& child : node.nodes("children")) {
            content.push_back(child.get());
        }
        auto rowView = [this, ctx, &node, content, keyFn](reactive::Signal<Value> const& row) -> Node {
            std::string key = runtime().untracked([&] { return canonicalJson(valueOfKey(keyFn(row.peek()))); });
            auto const scope = rowScope(node, ctx, row, content, std::move(key));
            LowerCtx const rowCtx{.scope = scope, .bindings = ctx.bindings, .boundary = ctx.boundary};
            std::vector<Node> children;
            children.reserve(content.size());
            for (DocNode const* const child : content) {
                children.push_back(lower(*child, rowCtx));
            }
            return holding(std::move(children), scope);
        };
        ExprPtr const axis = node.prop("axis");
        ExprPtr const gap = node.prop("gap");
        Common common;
        commonOf(node, ctx, element, common);
        Axis const along = axis ? axisOf(once(ctx, axis)) : Axis::Vertical;
        bool wrap = node.flag("wrap");
        if (wrap && along != Axis::Horizontal) {
            // Only a computed axis gets here: the loader refuses a literal one that is not horizontal.
            report(node.path + "/wrap",
                   "`wrap` fills lines along a row; this `forEach`'s `axis` is not \"horizontal\"");
            wrap = false;
        }
        bool divided = node.flag("divided");
        if (divided && along != Axis::Vertical) {
            report(node.path + "/divided",
                   "`divided` draws rules between rows of a column; this `forEach`'s `axis` is \"horizontal\"");
            divided = false;
        }
        return ui::forEach<Value>(std::move(rows), std::move(keyFn), std::move(rowView), along,
                                  gap ? static_cast<int>(intOf(once(ctx, gap), 0)) : 0, common, wrap, divided);
    }
    // The table's rows on the engine: the element's own, which its element values read too, unless names bound
    // where the table is lowered (a component's `props`) reach its rows, which the element cannot see; then one of
    // the lowering's own, kept by the mount.
    RowsTable* table = element == nullptr ? nullptr : element->rowsTable.get();
    std::shared_ptr<RowsTable> own;
    if (element != nullptr && ctx.bindings != nullptr && !ctx.bindings->empty()) {
        own = makeRowsTable(*element, ctx.bindings, false);
        table = own.get();
    }
    if (table == nullptr) {
        report(node.path, "a table over `rows` keeps its rows in its element, and this one has none");
        return ui::text(Text{.text = std::string{"table"}, .role = TextRole::Muted});
    }
    reactive::Computed<TableView> const* const view = table->view.get();
    std::vector<TableColumn> columns;
    std::vector<DocNode const*> cells;
    std::vector<std::string> fields;
    // One entry per column, in the same order as `cells`/`fields`, each column's role for its default cell.
    std::vector<TextRole> roles;
    FormatContext const& format = ctx.scope->formatting();
    // The table's `valign` is the default of every column that gives none; the loader checked both names.
    Align const tableValign = cellPlaceNamed(node.name("valign")).value_or(Align::Center);
    if (ExprPtr const derived = node.prop("columns")) {
        Value const list = once(ctx, derived);
        // A computed place that names none is reported once for the table, whichever column and key it was.
        bool placeReported = false;
        auto const placeOf = [&](Value const& item, std::string_view key, Align fallback) {
            Value const* const given = item.field(key);
            if (given == nullptr || given->isNull()) {
                return fallback;
            }
            std::string const* const name = given->asString();
            std::optional<Align> const placed = name == nullptr ? std::nullopt : cellPlaceNamed(*name);
            if (!placed && !placeReported) {
                placeReported = true;
                report(derived->path, "a column's `" + std::string{key} + "` is " + canonicalJson(*given) +
                                          "; a cell's place is start, center or end");
            }
            return placed.value_or(fallback);
        };
        if (ValueList const* const items = list.asList()) {
            for (Value const& item : *items) {
                Value const* const field = item.field("id");
                Value const* const label = item.field("label");
                fields.push_back(field == nullptr ? std::string{} : textOf(*field, format));
                columns.push_back(TableColumn{
                    .label = textOf(label != nullptr ? *label : Value{fields.back()}, format),
                    .align = placeOf(item, "align", Align::Start),
                    .valign = placeOf(item, "valign", tableValign),
                });
                cells.push_back(nullptr);
                roles.push_back(TextRole::Normal);
            }
        }
    }
    for (DocNodePtr const& part : node.partList("columns")) {
        ExprPtr const label = part->prop("label");
        ExprPtr const width = part->prop("width");
        ExprPtr const role = part->prop("role");
        TextRole columnRole = TextRole::Normal;
        if (role) {
            // Read once, with the rest of the column; a value that names no role is reported, as a text's is.
            Value const value = once(ctx, role);
            std::optional<TextRole> const named = roleNamed(textOf(value, format));
            if (!named) {
                report(role->path, "role is " + canonicalJson(value) + "; " + std::string{kRoleNames});
            }
            columnRole = named.value_or(TextRole::Normal);
        }
        bool const shrink = part->flag("shrink");
        Sizing const sized = width ? sizingOf(once(ctx, width)) : Sizing::content();
        if (shrink && sized.kind != Sizing::Kind::Stretch) {
            report(part->path + "/shrink",
                   "`shrink` lets a stretch column go narrower than its header; this column's width does not stretch");
        }
        columns.push_back(TableColumn{
            .label = label ? textOf(once(ctx, label), format) : std::string{},
            .width = sized,
            .role = columnRole,
            .align = cellPlaceNamed(part->name("align")).value_or(Align::Start),
            .valign = cellPlaceNamed(part->name("valign")).value_or(tableValign),
            .sortable = part->flag("sortable"),
            .unit = std::string{part->name("unit")},
            .wrap = part->flag("wrap", true),
            .shrink = shrink && sized.kind == Sizing::Kind::Stretch,
        });
        // A cell's accent comes from a model's table: a rows table's cells draw no tone or tooltip.
        for (std::string_view const key : {"tone", "tooltip"}) {
            if (part->prop(key) != nullptr) {
                report(pointerAppend(part->path, key),
                       "`" + std::string{key} +
                           "` is drawn on a model-backed table's cells; a table over `rows` "
                           "draws none");
            }
        }
        cells.push_back(part.get());
        fields.emplace_back(part->id);
        roles.push_back(columnRole);
    }
    // A column's display parts, read once; a rows table's columns have no other source of them.
    std::vector<std::optional<table::ColumnDisplay>> displays;
    displays.reserve(cells.size());
    for (DocNode const* const column : cells) {
        displays.push_back(
            column != nullptr && givesDisplay(*column)
                ? std::optional{displayOf(*column, {}, [&](ExprPtr const& expr) { return once(ctx, expr); })}
                : std::nullopt);
    }
    // A column's filter menu: a column part's values, of the kind its cells infer, shown by its display parts. A
    // derived column and one whose part says `filterable: false` have none.
    table->config.beginMount();
    table->config.filterTarget = [table, cells, displays](std::string const& columnId) -> std::optional<FilterTarget> {
        std::vector<LayoutColumn> const& arranged = table->config.columns;
        auto const found = std::ranges::find(arranged, columnId, &LayoutColumn::id);
        if (found == arranged.end() || table->source == nullptr) {
            return std::nullopt;
        }
        auto const index = static_cast<std::size_t>(found - arranged.begin());
        DocNode const* const part = index < cells.size() ? cells.at(index) : nullptr;
        if (part == nullptr || part->id.empty() || !part->flag("filterable", true)) {
            return std::nullopt;
        }
        std::span<table::ColumnInfo const> const infos = table->source->columns();
        auto const info = std::ranges::find(infos, columnId, &table::ColumnInfo::id);
        if (info == infos.end()) {
            return std::nullopt;
        }
        FilterTarget target;
        target.column = columnId;
        target.label = found->label;
        target.shown =
            table::TableColumn{.info = *info, .display = displays.at(index).value_or(table::ColumnDisplay{})};
        target.listed = true;
        return target;
    };
    // The row scopes `onActivate` runs in, by the row's canonical key, and the count kept at the last prune.
    struct Registry {
        std::map<std::string, std::weak_ptr<RtScope>, std::less<>> scopes;
        std::size_t kept = 64;
    };
    auto const registry = std::make_shared<Registry>();
    // A value that names no tone is reported once for the table, whichever row and value it was.
    auto const toneReported = std::make_shared<bool>(false);
    // Likewise for a `rowSpans` value that is no list of spans.
    auto const spansReported = std::make_shared<bool>(false);
    // Every cell's content is declared in its row's one scope, as a row's own parts are; the mount builds a row's
    // parts before its cells, which find that scope in the registry by the row's key.
    std::vector<DocNode const*> cellContent;
    for (DocNode const* const column : cells) {
        if (column != nullptr) {
            for (DocNodePtr const& cell : column->nodes("cell")) {
                cellContent.push_back(cell.get());
            }
        }
    }
    auto const keyText = [this, keyFn](Value const& row) {
        return runtime().untracked([&] { return canonicalJson(valueOfKey(keyFn(row))); });
    };
    TableSource source;
    source.rows = table->rows;
    source.alwaysWindowed = false;
    // A column with its own `cell` content is lowered in its row's scope; a plain column reads the record alone.
    source.rowScopedCells.reserve(cells.size());
    for (DocNode const* const column : cells) {
        source.rowScopedCells.push_back(column != nullptr && !column->nodes("cell").empty());
    }
    source.rowView = [this, ctx, &node, cellContent, registry, keyText, toneReported, spansReported, fields, own,
                      table](reactive::Signal<Value> const& row) -> TableRowLook {
        std::string key = keyText(row.peek());
        if (row.peek().field(kGroupHeadingField) != nullptr) {
            return headingRow(node, ctx, row, std::move(key));
        }
        auto const scope = rowScope(node, ctx, row, cellContent, key);
        registry->scopes[std::move(key)] = scope;
        // The scopes that went are dropped once the registry doubles, so a pass that realizes n rows costs O(n).
        if (registry->scopes.size() >= 2 * registry->kept) {
            std::erase_if(registry->scopes, [](auto const& entry) { return entry.second.expired(); });
            registry->kept = std::max<std::size_t>(registry->scopes.size(), 64);
        }
        LowerCtx const rowCtx{.scope = scope, .bindings = ctx.bindings, .boundary = ctx.boundary};
        TableRowLook look;
        look.keep = scope;
        // The row's name, in its own scope: what its select box and assistive technology read.
        if (ExprPtr const labelExpr = node.prop("rowLabel")) {
            look.label = bindProp<std::string>(rowCtx, labelExpr, std::string{}, stringOf);
        }
        if (ExprPtr const toneExpr = node.prop("rowTone")) {
            look.tone = slotProp<std::optional<Tone>>(
                [this, scope, bindings = ctx.bindings, toneExpr, toneReported] -> std::optional<Tone> {
                    return rowToneOf(evalIn(*scope, bindings.get(), toneExpr), toneExpr->path, *toneReported);
                });
        }
        if (ExprPtr const spansExpr = node.prop("rowSpans")) {
            look.spans = slotProp<std::vector<RowSpan>>(
                [this, scope, bindings = ctx.bindings, spansExpr, spansReported, fields, table] {
                    std::vector<RowSpan> spans =
                        rowSpansOf(evalIn(*scope, bindings.get(), spansExpr), fields, spansExpr->path, *spansReported);
                    // A span across a column the user moved away or hid is cut at that column.
                    std::vector<std::string> visible = fields;
                    if (table != nullptr) {
                        TableConfig const& config = table->config;
                        visible = layoutOver(config.columns, config.declaredHidden, layoutNow(config)).visible();
                    }
                    std::erase_if(spans, [&](RowSpan& span) {
                        span.columns = cutColumns(
                            CellSpan{.column = fields.at(span.column), .rows = span.rows, .columns = span.columns},
                            fields, visible);
                        return span.columns == 0;
                    });
                    return spans;
                });
        }
        return look;
    };
    // A field's text as a column's default cell shows it: by the column's display parts, of the kind the value has,
    // or the value's own text. A copy writes it for every column, one with its own `cell` too.
    auto const plainText = [](Value const* value, std::optional<table::ColumnDisplay> const& display,
                              FormatContext const& formatting) {
        if (display) {
            Value const shown = value == nullptr ? Value{} : *value;
            table::TableColumn const kind = columnFor(shown, *display);
            table::Cell const cell = cellOfRecord(value, kind, nullptr);
            std::string text = formatCell(cell, kind, formatting);
            // A quantity keeps its unit after its formatted amount, as its own text writes it; a label's text stands
            // for the value whole and takes none.
            std::optional<std::string> const operand = operandOf(cell, kind);
            bool const labelled = operand && std::ranges::any_of(display->labels, [&](table::ValueLabel const& label) {
                                      return detail::cells::sameOperand(label.operand, *operand, kind.info.kind);
                                  });
            if (Quantity const* const quantity = shown.asQuantity();
                quantity != nullptr && !text.empty() && !labelled) {
                auto const unit = formatting.units.find(quantity->unit);
                bool const named = unit != formatting.units.end() && !unit->second.display.empty();
                text += " " + (named ? unit->second.display : quantity->unit);
            }
            return text;
        }
        return value == nullptr ? std::string{} : textOf(*value, formatting);
    };
    source.cellView = [this, ctx, &node, cells, fields, roles, keyText, registry, displays, plainText](
                          std::size_t column, reactive::Signal<Value> const& row) -> Node {
        DocNode const* const part = column < cells.size() ? cells.at(column) : nullptr;
        auto const nodes = part == nullptr ? std::span<DocNodePtr const>{} : part->nodes("cell");
        if (nodes.empty()) {
            return ui::text(Text{
                .text = slotProp<std::string>(
                    [scope = ctx.scope, &row, field = fields.at(column), display = displays.at(column), plainText] {
                        return plainText(row.get().field(field), display, scope->formatting());
                    }),
                .role = roles.at(column),
            });
        }
        std::string key = keyText(row.peek());
        auto const found = registry->scopes.find(key);
        std::shared_ptr<RtScope> scope = found == registry->scopes.end() ? nullptr : found->second.lock();
        if (scope == nullptr) {
            std::vector<DocNode const*> cellNodes;
            for (DocNodePtr const& cell : nodes) {
                cellNodes.push_back(cell.get());
            }
            scope = rowScope(node, ctx, row, cellNodes, std::move(key));
        }
        LowerCtx const rowCtx{.scope = scope, .bindings = ctx.bindings, .boundary = ctx.boundary};
        // The cell owns its row's scope as long as its node lives.
        auto const owner =
            std::make_shared<std::pair<Node, std::shared_ptr<RtScope>> const>(lowerAll(nodes, rowCtx), scope);
        return Node{owner, owner->first.get()};
    };
    // Summary rows: the table's `summaries` before or after its page's rows and each run's after the run, rows of a
    // decorator the table keeps over the page's entries. A summary reads every row of the view, or its run's, not
    // the page's alone. Every column's cells are built per row, so a data cell is never rebound to a summary row.
    std::vector<SummarySpec> tableSummaries = summarySpecsOf(node.partList("summaries"));
    std::span<DocNodePtr const> const groupParts = node.partList("group");
    std::vector<SummarySpec> runSummaries =
        groupParts.empty() ? std::vector<SummarySpec>{} : summarySpecsOf(groupParts.front()->partList("summaries"));
    if (!tableSummaries.empty() || !runSummaries.empty()) {
        if (table->summaries == nullptr) {
            SummaryRows::Runs runs;
            if (!groupParts.empty()) {
                runs = [table] { return rowsRuns(*table); };
            }
            table->summaries = std::make_shared<SummaryRows>(table->rows, std::move(tableSummaries),
                                                             std::move(runSummaries), std::move(runs));
        }
        source.rows = table->summaries;
        source.rowScopedCells.assign(fields.size(), true);
        source.cellView = [this, ctx, &node, table, fields, displays, plainText, inner = source.cellView](
                              std::size_t column, reactive::Signal<Value> const& row) -> Node {
            Value const& record = row.peek();
            if (record.field(kSummaryField) == nullptr) {
                return inner(column, row);
            }
            std::string const columnId = column < fields.size() ? fields.at(column) : std::string{};
            Text text;
            text.common.testId = summaryTestId(record, columnId);
            text.text = slotProp<std::string>(
                [this, &node, ctx, table, record = &row, columnId, display = displays.at(column), plainText] {
                    Value const& now = record->get();
                    std::optional<Value> const value =
                        summaryValue(node, now, columnId, ctx, rowsSummaryScope(*table, now));
                    return value ? plainText(&*value, display, ctx.scope->formatting()) : std::string{};
                });
            return ui::text(std::move(text));
        };
        source.rowView = [this, ctx, &node,
                          inner = source.rowView](reactive::Signal<Value> const& row) -> TableRowLook {
            if (row.peek().field(kSummaryField) != nullptr) {
                return summaryLook(node, ctx, row.peek());
            }
            return inner(row);
        };
    }
    TableOptions options;
    options.showHeader = node.flag("showHeader", true);
    options.bands = bandsOf(node.partList("bands"));
    options.selectColumn = node.flag("selectColumn");
    if (ExprPtr const selectAll = node.prop("selectAllLabel")) {
        options.selectAllLabel = bindProp<std::string>(ctx, selectAll, std::string{}, stringOf);
    }
    if (ExprPtr const threshold = node.prop("virtualizeThreshold")) {
        options.virtualize.threshold =
            static_cast<std::size_t>(std::max<std::int64_t>(0, intOf(once(ctx, threshold), 2000)));
    }
    if (ExprPtr const overscan = node.prop("virtualizeOverscan")) {
        options.virtualize.overscan =
            static_cast<std::size_t>(std::max<std::int64_t>(0, intOf(once(ctx, overscan), 10)));
    }
    std::string const mode =
        node.prop("selectionMode") ? textOf(once(ctx, node.prop("selectionMode")), format) : std::string{};
    options.selectionMode = SelectionMode::None;
    if (mode == "single") {
        options.selectionMode = SelectionMode::Single;
    } else if (mode == "multiple") {
        options.selectionMode = SelectionMode::Multiple;
    }
    // The status line: its counts, and the latest message; it takes space while it has something to say, or always
    // or never as a literal `status` says.
    options.status = slotProp<TableStatus>([element, table, scope = ctx.scope] {
        static_cast<void>(table->version.get());
        TableStatus status;
        Value const selectedValue = tableSelection(*element);
        ValueList const* const selected = selectedValue.asList();
        status.counts =
            countsText(table->rows->counts(), selected == nullptr ? 0 : static_cast<std::int64_t>(selected->size()),
                       scope->formatting());
        status.message = table->status == false ? std::string{} : table->message.get();
        status.shown = table->status.value_or(!status.state.empty() || !status.message.empty());
        return status;
    });
    // The selection the table shows: the bound `selection`, or the table's own. A slot even for a literal list, so
    // a bound selection is fixed until the document writes it, and a user's change the document leaves unwritten
    // snaps back. A bound value that is neither a list nor `null` is reported once.
    auto const selectionReported = std::make_shared<bool>(false);
    options.selection = slotProp<std::vector<Key>>([this, element, selectionReported, path = node.path] {
        std::vector<Key> keys;
        if (element->bound != nullptr) {
            Value const bound = element->bound->get();
            if (!bound.isNull() && bound.asList() == nullptr && !*selectionReported) {
                *selectionReported = true;
                report(pointerAppend(path, "selection"),
                       "selection is " + canonicalJson(bound) + "; a table's selection is a list of keys");
            }
        }
        Value const selection = tableSelection(*element);
        if (ValueList const* const items = selection.asList()) {
            for (Value const& item : *items) {
                keys.push_back(keyOf(item));
            }
        }
        return keys;
    });
    // A user's change: the table's own selection takes it when no key binds one, then `onSelectionChange` runs with
    // `event` the new selection, in the table's scope, in the same batch. The backend holds one page: a `Replace`
    // is the whole new selection, and an `Extend` keeps the selected keys of the rows on other pages, before the
    // reported ones. A key whose row `rows` no longer has drops on any user change.
    options.onSelectionChange = [this, ctx, element, view, keyFn, own, commands = node.event("onSelectionChange")](
                                    std::vector<Key> const& keys, SelectionChange change) {
        ValueList items;
        if (change == SelectionChange::Extend) {
            items = runtime().untracked([&] { return selectedOffPage(view->get(), tableSelection(*element), keyFn); });
        }
        // A heading is never selected. A virtualized backend knows the keys of rows it has not realized but not
        // which of them are headings, so its Ctrl+A or range can name one.
        for (Key const& key : keys) {
            if (std::string const* const text = std::get_if<std::string>(&key);
                text != nullptr && text->starts_with("\x1Fgroup:")) {
                continue;
            }
            items.push_back(valueOfKey(key));
        }
        Value const selected{std::move(items)};
        runtime().batch([&] {
            if (element->bound == nullptr) {
                session.asUser([&] {
                    session.noteWrite();
                    element->selection.set(selected);
                });
            }
            runEvent(commands, ctx, selected);
        });
    };
    // Select-all selects the engine's view, every page, through the table's selection; when every row of the view
    // is selected already it clears them, as a short table's Ctrl+A does.
    if (options.selectionMode == SelectionMode::Multiple) {
        options.onSelectAll = [this, ctx, element, table, own, commands = node.event("onSelectionChange")] {
            auto selection = table::Selection::create(table::SelectionOptions{.mode = table::SelectionMode::Multiple,
                                                                              .pruneOnFilter = false,
                                                                              .tableMode = table::TableMode::Client});
            if (!selection || !selection->selectAll(table->engine->engine())) {
                return;
            }
            std::unordered_set<Key> held;
            runtime().untracked([&] {
                if (Value const itemsValue = tableSelection(*element);
                    ValueList const* const items = itemsValue.asList()) {
                    for (Value const& item : *items) {
                        held.insert(keyOf(item));
                    }
                }
            });
            bool const every = std::ranges::all_of(selection->keys(),
                                                   [&held](table::RowId const& key) { return held.contains(key); });
            ValueList items;
            if (!every) {
                for (table::RowId const& key : selection->keys()) {
                    items.push_back(valueOfKey(key));
                }
            }
            Value const selected{std::move(items)};
            runtime().batch([&] {
                if (element->bound == nullptr) {
                    session.asUser([&] {
                        session.noteWrite();
                        element->selection.set(selected);
                    });
                }
                runEvent(commands, ctx, selected);
            });
        };
    }
    // A copy reads the view, which holds every row; the focus names a column by its layout id.
    for (std::size_t at = 0; at < columns.size() && at < table->config.columns.size(); ++at) {
        columns.at(at).id = table->config.columns.at(at).id;
    }
    auto const headers = std::make_shared<std::vector<std::string>>();
    for (TableColumn const& column : columns) {
        headers->push_back(headerText(column.label, column.unit));
    }
    // A cell a menu opens on, read as its column's filter reads it: by the kind the column's cells infer and the
    // column's display parts.
    table->config.heldCell = [table, fields, displays](Key const& key,
                                                       std::string const& columnId) -> std::optional<HeldCell> {
        std::vector<LayoutColumn> const& arranged = table->config.columns;
        auto const found = std::ranges::find(arranged, columnId, &LayoutColumn::id);
        auto const index = static_cast<std::size_t>(found - arranged.begin());
        if (found == arranged.end() || index >= fields.size() || table->source == nullptr ||
            table->engine == nullptr) {
            return std::nullopt;
        }
        std::span<table::ColumnInfo const> const infos = table->source->columns();
        auto const info = std::ranges::find(infos, columnId, &table::ColumnInfo::id);
        if (info == infos.end()) {
            return std::nullopt;
        }
        table::TableColumn const shown{.info = *info, .display = displays.at(index).value_or(table::ColumnDisplay{})};
        EngineRows const& engineRows = *table->engine;
        std::int64_t const total = engineRows.total().rows;
        for (std::int64_t row = 0; row < total; ++row) {
            std::optional<TableRowRef> const ref = engineRows.row(row);
            if (ref && ref->key == key) {
                return HeldCell{.record = ref->row,
                                .cell = cellOfRecord(ref->row.field(fields.at(index)), shown, nullptr),
                                .shown = shown,
                                .units = {},
                                .keep = nullptr};
            }
        }
        return std::nullopt;
    };
    // The engine's view holds every row, its record the row as `rows` gave it.
    table->config.gather = [table, fields, displays, plainText, headers, scope = ctx.scope](
                               std::vector<Key> keys, std::vector<std::string> const& ids, CopyDone const& done) {
        std::vector<std::size_t> places;
        CopyGathered gathered;
        std::vector<LayoutColumn> const& arranged = table->config.columns;
        for (std::string const& columnId : ids) {
            auto const found = std::ranges::find(arranged, columnId, &LayoutColumn::id);
            auto const place = static_cast<std::size_t>(found - arranged.begin());
            if (found != arranged.end() && place < fields.size() && place < headers->size()) {
                places.push_back(place);
                gathered.headers.push_back(headers->at(place));
            }
        }
        FormatContext const& formatting = scope->formatting();
        std::unordered_set<Key> wanted{keys.begin(), keys.end()};
        EngineRows const& engineRows = *table->engine;
        std::int64_t const total = engineRows.total().rows;
        for (std::int64_t at = 0; at < total && !wanted.empty(); ++at) {
            std::optional<TableRowRef> const ref = engineRows.row(at);
            if (!ref || wanted.erase(ref->key) == 0) {
                continue;
            }
            CopyRow copied{.viewRow = at, .cells = {}};
            for (std::size_t const place : places) {
                copied.cells.emplace_back(plainText(ref->row.field(fields.at(place)), displays.at(place), formatting));
            }
            gathered.rows.push_back(std::move(copied));
        }
        done(std::move(gathered));
    };
    // The header's sort chain and menus, and the columns' arrangement; the column chooser and the sort dialog.
    Node dialogs = configureTable(table->config, ctx, options, own);
    markFilterable(table->config, columns, options);
    options.onActivate = [this, ctx, element, registry, commands = node.event("onActivate")](Key const& key) {
        Value const activated = valueOfKey(key);
        session.asUser([&] {
            session.noteWrite();
            element->activeKey.set(activated);
        });
        auto const found = registry->scopes.find(canonicalJson(activated));
        std::shared_ptr<RtScope> const scope = found == registry->scopes.end() ? nullptr : found->second.lock();
        LowerCtx const rowCtx{.scope = scope ? scope : ctx.scope, .bindings = ctx.bindings, .boundary = ctx.boundary};
        runEvent(commands, rowCtx, activated);
    };
    commonOf(node, ctx, element, options.common);
    std::span<DocNodePtr const> const empty = node.nodes("empty");
    if (empty.empty()) {
        return besideDialogs(options.common, std::move(dialogs),
                             [&] { return ui::table(std::move(columns), std::move(source), std::move(options)); });
    }
    // With `empty`, a column holds the table and the content shown in its place while `rows` is an empty list; a
    // `null` (a query not answered yet) shows the table. The column is placed as the table would be: it takes the
    // node's visibility and layout, and every other common property stays on the table, so its test id and the
    // element id `focus` finds name the table.
    auto const listed = [scope = ctx.scope, bindings = ctx.bindings, rowsExpr] {
        Value const itemsValue = evalIn(*scope, bindings.get(), rowsExpr);
        ValueList const* const items = itemsValue.asList();
        return items == nullptr || !items->empty();
    };
    Column wrapper;
    wrapper.common.visible = std::move(options.common.visible);
    wrapper.common.layout = options.common.layout;
    options.common.layout = wrappedTableLayout(options.common.layout);
    options.common.visible = slotProp<bool>(listed);
    Column content{.children = {lowerAll(empty, ctx)}};
    content.common.visible = slotProp<bool>([listed] { return !listed(); });
    wrapper.children.push_back(ui::table(std::move(columns), std::move(source), std::move(options)));
    wrapper.children.push_back(ui::column(std::move(content)));
    wrapper.children.push_back(std::move(dialogs));
    return ui::column(std::move(wrapper));
}

[[nodiscard]] SortChain sortChainOf(Value const& value) {
    SortChain chain;
    ValueList const* const keys = value.asList();
    if (keys == nullptr) {
        return chain;
    }
    for (Value const& key : *keys) {
        Value const* const column = key.field("column");
        if (column == nullptr || column->asString() == nullptr) {
            continue;
        }
        Value const* const direction = key.field("direction");
        bool const descending =
            direction != nullptr && direction->asString() != nullptr && *direction->asString() == "desc";
        chain.push_back(
            table::SortKey{.column = *column->asString(),
                           .dir = descending ? table::SortDirection::Descending : table::SortDirection::Ascending});
    }
    return chain;
}

[[nodiscard]] Value sortValueOf(SortChain const& chain) {
    ValueList keys;
    for (table::SortKey const& key : chain) {
        ValueRecord entry;
        entry.insert_or_assign("column", Value{key.column});
        entry.insert_or_assign("direction",
                               Value{std::string{key.dir == table::SortDirection::Descending ? "desc" : "asc"}});
        keys.emplace_back(std::move(entry));
    }
    return Value{std::move(keys)};
}

// NOLINTBEGIN(misc-no-recursion): an expression, a command list and a document nest, and each walk follows its own
// nesting.
void rowReadsOf(Expr const& expr, RowReads& out, bool readFrom) {
    // A cell menu's entries also name their row `row`.
    if (expr.kind == ExprKind::Ref && expr.name == "row") {
        if (!expr.fieldPath.empty()) {
            out.ids.insert(expr.fieldPath.front());
        } else if (!readFrom) {
            out.whole = true;
        }
        return;
    }
    if (expr.kind == ExprKind::Row) {
        if (!expr.fieldPath.empty()) {
            out.ids.insert(expr.fieldPath.front());
        } else if (!readFrom) {
            out.whole = true;
        }
        return;
    }
    if (expr.kind == ExprKind::Get) {
        rowReadsOf(*expr.args.at(0), out, true);
        return;
    }
    for (ExprPtr const& arg : expr.args) {
        if (arg) {
            rowReadsOf(*arg, out);
        }
    }
    for (auto const& [name, field] : expr.fields) {
        static_cast<void>(name);
        if (field) {
            rowReadsOf(*field, out);
        }
    }
}

void rowReadsOf(Commands const& commands, RowReads& out) {
    for (Command const& command : commands) {
        for (ExprPtr const* const expr : {&command.value, &command.extra, &command.mediaType}) {
            if (*expr) {
                rowReadsOf(**expr, out);
            }
        }
        rowReadsOf(command.commands, out);
        rowReadsOf(command.alternative, out);
    }
}

void rowReadsOf(DocNode const& node, RowReads& out) {
    for (auto const& [key, expr] : node.props) {
        static_cast<void>(key);
        if (expr) {
            rowReadsOf(*expr, out);
        }
    }
    for (auto const& [key, commands] : node.events) {
        static_cast<void>(key);
        rowReadsOf(commands, out);
    }
    for (auto const& [key, nodes] : node.children) {
        static_cast<void>(key);
        for (DocNodePtr const& child : nodes) {
            rowReadsOf(*child, out);
        }
    }
    for (auto const& [key, parts] : node.parts) {
        static_cast<void>(key);
        for (DocNodePtr const& part : parts) {
            rowReadsOf(*part, out);
        }
    }
}
// NOLINTEND(misc-no-recursion)

[[nodiscard]] std::string levelWords(std::string level) {
    if (!level.empty() && level.front() >= 'a' && level.front() <= 'z') {
        level.front() = static_cast<char>(level.front() - 'a' + 'A');
    }
    return level;
}

[[nodiscard]] std::optional<table::ColumnKind> columnKindNamed(std::string_view name) {
    static constexpr std::array<std::pair<std::string_view, table::ColumnKind>, 7> kKinds = {{
        {"text", table::ColumnKind::Text},
        {"integer", table::ColumnKind::Integer},
        {"decimal", table::ColumnKind::Decimal},
        {"number", table::ColumnKind::Number},
        {"date", table::ColumnKind::Date},
        {"dateTime", table::ColumnKind::DateTime},
        {"bool", table::ColumnKind::Bool},
    }};
    // NOLINTNEXTLINE(readability-qualified-auto): an array iterator is a pointer on some standard libraries only.
    auto const found = std::ranges::find(kKinds, name, &std::pair<std::string_view, table::ColumnKind>::first);
    return found == kKinds.end() ? std::nullopt : std::optional{found->second};
}

namespace {

/// @brief The registry a table over `rows` reads its quantities' units from: each unit of the screen's table in the
///        dimension of its canonical unit, each canonical unit the table names but does not list, and each unit the
///        rows brought that the table does not know, which is its own canonical unit, as an expression reads it.
/// @param units The screen's units.
/// @param seen  The units the rows brought.
/// @return The registry.
[[nodiscard]] std::shared_ptr<table::UnitRegistry const> registryOfUnits(
    UnitTable const& units, std::set<std::string, std::less<>> const& seen) {
    table::UnitRegistryBuilder builder;
    auto const add = [&builder](std::string const& columnId, std::string const& canonical, math::Rational factor) {
        std::ignore = (builder.add(table::UnitInfo{
            .id = columnId, .symbol = columnId, .dimension = canonical, .toBase = factor, .decimals = 0}));
    };
    for (auto const& [unitId, info] : units) {
        add(unitId, info.canonical.empty() ? unitId : info.canonical, info.toCanonical);
    }
    for (auto const& [unitId, info] : units) {
        if (!info.canonical.empty() && !units.contains(info.canonical)) {
            add(info.canonical, info.canonical, math::Rational{1, math::DecimalPlaces{0}});
        }
    }
    // A unit already added (a canonical one) is refused by the builder and keeps its first entry.
    for (std::string const& columnId : seen) {
        if (!units.contains(columnId)) {
            add(columnId, columnId, math::Rational{1, math::DecimalPlaces{0}});
        }
    }
    return std::move(builder).build();
}

}  // namespace

void startRowsEngine(RowsTable& table, reactive::Runtime& runtime) {
    auto const registry = registryOfUnits(table.units, table.unitsSeen);
    auto source = std::make_shared<ValueRowsSource>(runtime, table.columns, table.keyOfRow, registry);
    source->setGroups(table.groups);
    // The runs are the leading key of every sort the engine is given, a column the engine knows from its start.
    if (table.byOf) {
        source->setGroup(table.byOf, table.units);
    }
    table::EngineOptions options;
    options.owner = &runtime.owner();
    options.worker = nullptr;
    options.frameBudget = std::chrono::milliseconds{4};
    options.services.collator = std::make_shared<ValueRowsCollator const>();
    options.services.units = registry;
    auto const engine =
        std::make_shared<EngineRows>(source, morph::ui::ValueRowsSource::recordOf(), std::move(options));
    bool const restoring = table.engine != nullptr && table.engine->restoring();
    table::FilterSpec const filter = table.engine != nullptr ? table.engine->engine().filter() : table::FilterSpec{};
    if (table.engine != nullptr) {
        table.engine->onChange({});
        for (std::string const& column : table.used) {
            table.source->unuse(column);
        }
        if (table.byOf) {
            table.source->setGroup({}, {});
        }
    }
    for (std::string const& column : table.used) {
        source->use(column);
    }
    engine->setRestoring(restoring);
    table.source = std::move(source);
    table.engine = engine;
    if (table.rows != nullptr) {
        table.rows->setEngine(engine);
    } else {
        table.rows = std::make_shared<PageRows>(engine, runtime.owner());
    }
    if (table.elementOwn) {
        table.element->tableRows = engine;
    }
    engine->onChange(table.heard);
    SortChain ordered;
    if (table.byOf) {
        ordered.push_back(
            table::SortKey{.column = std::string{kGroupRunColumn}, .dir = table::SortDirection::Ascending});
    }
    ordered.insert(ordered.end(), table.sort.begin(), table.sort.end());
    std::ignore = (engine->setSort(std::move(ordered)));
    if (!filter.empty()) {
        std::ignore = (engine->setFilter(filter));
    }
}

[[nodiscard]] std::vector<std::string> usedColumns(RowsTable const& table, SortChain const& sort,
                                                   table::FilterSpec const& filter) {
    std::vector<std::string> out;
    auto const add = [&out](std::string const& columnId) {
        if (!std::ranges::contains(out, columnId)) {
            out.push_back(columnId);
        }
    };
    for (table::SortKey const& key : sort) {
        add(key.column);
    }
    for (auto const& [columnId, column] : filter.columns) {
        static_cast<void>(column);
        add(columnId);
    }
    for (table::GroupFilter const& group : filter.groups) {
        auto const declared = std::ranges::find(table.groups, group.group, &table::ColumnGroupInfo::id);
        std::vector<std::string> const members =
            !group.group.empty() && declared != table.groups.end() ? declared->columns : group.columns;
        for (std::string const& columnId : members) {
            add(columnId);
        }
    }
    for (std::string const& columnId : table.listedColumns) {
        add(columnId);
    }
    return out;
}

void useColumns(RowsTable& table, std::vector<std::string> next) {
    for (std::string const& column : next) {
        if (!std::ranges::contains(table.used, column)) {
            table.source->use(column);
        }
    }
    for (std::string const& column : table.used) {
        if (!std::ranges::contains(next, column)) {
            table.source->unuse(column);
        }
    }
    table.used = std::move(next);
}

[[nodiscard]] std::string groupedCount(std::size_t count) {
    std::string digits = std::to_string(count);
    for (auto at = static_cast<std::ptrdiff_t>(digits.size()) - 3; at > 0; at -= 3) {
        digits.insert(static_cast<std::size_t>(at), ",");
    }
    return digits;
}

void loadStateAndViews(TableStates& states, table::TableStateKey const& key, TableViews& views,
                       async::CallbackScope const& scope,
                       std::function<void(std::optional<std::string> const&, std::string const&)> done) {
    struct Joined {
        int waiting = 2;
        std::optional<std::string> json;
        std::string failure;
        std::function<void(std::optional<std::string> const&, std::string const&)> done;
    };
    auto const joined =
        std::make_shared<Joined>(Joined{.waiting = 2, .json = {}, .failure = {}, .done = std::move(done)});
    auto const arrive = [joined] {
        if (--joined->waiting == 0) {
            joined->done(joined->json, joined->failure);
        }
    };
    views.load()
        .then(scope, [arrive](TableViews::Result const&) { arrive(); })
        .onError(scope, [arrive](std::exception_ptr const&) { arrive(); });
    states.load(key)
        .then(scope,
              [joined, arrive](std::optional<table::StoredTableState> const& stored) {
                  if (stored) {
                      joined->json = stored->json;
                  }
                  arrive();
              })
        .onError(scope, [joined, arrive](std::exception_ptr const& error) {
            joined->failure = failureText(error);
            arrive();
        });
}

}  // namespace detail

}  // namespace morph::ui
