// SPDX-License-Identifier: Apache-2.0

// A model-backed table's lowering.

#include "interpreter_grid_detail.hpp"

namespace morph::ui::detail {

// NOLINTNEXTLINE(readability-function-cognitive-complexity, readability-function-size): the columns, the cells, the row's parts and the handlers; each stage hands the next the same element, options and config
Node ScreenCore::modelTableOf(DocNode const& node, LowerCtx const& ctx, ElementState& element) {
    ModelTable& model = *element.modelTable;
    TableOptions options;
    commonOf(node, ctx, &element, options.common);
    options.status = slotProp<TableStatus>([&element, &model, scope = ctx.scope] {
        static_cast<void>(model.version.get());
        TableCountsView const counts = model.rows->counts();
        TableStatus status;
        status.shown = model.statusShown;
        SelectedCount const selected = model.rows->selectedCount(selectionNow(element));
        status.counts = countsText(counts, selected.rows, scope->formatting());
        if (selected.kind == CountKind::Approximate && selected.rows > 0 && counts.view.known &&
            counts.undecided == 0) {
            auto const grouped = [&scope](std::int64_t number) {
                return render::formatCanonicalNumber(std::to_string(number), scope->formatting().numeric);
            };
            status.counts = "about " + grouped(selected.rows) + " of " + grouped(counts.view.rows) +
                            (counts.view.kind == table::TotalKind::AtLeast ? " or more" : "") + " selected";
        }
        if (!counts.failed.empty()) {
            status.state = levelWords(counts.failed.front()) + " data could not be loaded.";
            status.retry = true;
        } else if (auto const& error = model.window->error()) {
            status.state = "The rows could not be loaded: " + error->message;
            status.retry = true;
        } else if (Value const failed = model.describe != nullptr ? model.describe->error() : Value{};
                   failed.asString() != nullptr) {
            status.state = "The table's description could not be loaded: " + *failed.asString();
        }
        if (model.statusShown) {
            status.message = model.message.get();
            status.entries = model.entries.get();
        }
        // The standing states, in the order saving, failed, unmatched, joined by ". ".
        static_cast<void>(model.marksRevision.get());
        auto const grouped = [&scope](std::int64_t number) {
            return render::formatCanonicalNumber(std::to_string(number), scope->formatting().numeric);
        };
        auto const stand = [&status](std::string text) {
            status.standing += (status.standing.empty() ? "" : ". ") + std::move(text);
        };
        if (std::int64_t const busy = model.rows->states().busyCount(); busy > 0) {
            stand("Saving " + grouped(busy) + (busy == 1 ? " row" : " rows"));
        }
        if (auto const failed = static_cast<std::int64_t>(model.rows->states().failedKeys().size()); failed > 0) {
            stand(grouped(failed) + (failed == 1 ? " row could not be saved" : " rows could not be saved"));
            status.standingEntries.push_back(StatusEntry{.id = std::string{kSelectFailed}, .label = "Select them"});
            status.standingEntries.push_back(StatusEntry{.id = std::string{kClearMarks}, .label = "Clear marks"});
        }
        if (counts.retained > 0) {
            stand(grouped(counts.retained) +
                  (counts.retained == 1 ? " row shown that no longer matches" : " rows shown that no longer match"));
            status.standingEntries.push_back(
                StatusEntry{.id = std::string{kShowMatching}, .label = "Show only matching rows"});
        }
        return status;
    });
    options.onStatusEntry = [this, &element, &model](std::string const& entryId) {
        // "Select them" makes the failed rows the selection; "Clear marks" drops their marks.
        if (entryId == kSelectFailed || entryId == kClearMarks) {
            tableGesture(element);
            runtime().batch([&] {
                if (entryId == kSelectFailed) {
                    std::vector<Key> const failed = model.rows->states().failedKeys();
                    model.viewSelection.set(std::nullopt);
                    if (model.config.selectKeys) {
                        model.config.selectKeys(failed);
                    }
                } else {
                    model.rows->states().clearFailed();
                }
                model.marksRevision.set(model.marksRevision.peek() + 1);
            });
            return;
        }
        // "Show only matching rows": the retained rows go, and with them a selection of the view, which named them.
        if (entryId == kShowMatching) {
            tableGesture(element);
            runtime().batch([&] {
                model.rows->clearRetained();
                model.marksRevision.set(model.marksRevision.peek() + 1);
                if (model.viewSelection.peek()) {
                    model.viewSelection.set(std::nullopt);
                    if (element.bound == nullptr) {
                        session.asUser([&] {
                            session.noteWrite();
                            element.selection.set(Value{ValueList{}});
                        });
                    }
                }
            });
            return;
        }
        auto const found = model.entryHandlers.find(entryId);
        if (found == model.entryHandlers.end()) {
            return;
        }
        // Taken before the message is cleared, which drops the handlers.
        std::function<void()> const chosen = found->second;
        tableMessage(element, {});
        chosen();
    };
    options.onRetry = [this, &element, &model] {
        tableGesture(element);
        model.window->retry();
    };
    table::TableDescription const* const description = model.window->description();
    if (description == nullptr) {
        // Before the description, and after one that does not read: the status line alone.
        Table spec;
        spec.status = std::move(options.status);
        spec.onRetry = std::move(options.onRetry);
        spec.common = std::move(options.common);
        return makeNode(std::move(spec));
    }
    FormatContext const& format = ctx.scope->formatting();
    Align const tableValign = cellPlaceNamed(node.name("valign")).value_or(Align::Center);
    auto const describedColumn = [description](std::string_view columnId) -> table::TableColumn const* {
        auto const found =
            std::ranges::find(description->columns, columnId,
                              [](table::TableColumn const& column) -> auto const& { return column.info.id; });
        return found == description->columns.end() ? nullptr : &*found;
    };
    std::vector<TableColumn> columns;
    std::vector<DocNode const*> parts;
    std::vector<TextRole> roles;
    TableSource source;
    // Each shown column as it shows its cells: the description's display under the part's display parts.
    auto const shown = std::make_shared<std::vector<table::TableColumn>>();
    auto const displayUnits = std::make_shared<std::vector<DisplayUnits>>();
    auto const cellExprs = std::make_shared<std::vector<CellExprs>>();
    std::shared_ptr<table::UnitRegistry const> registry;
    if (auto made = table::registryOf(*description)) {
        registry = std::move(*made);
    } else {
        report(node.path, "the units of `" + model.id + "` do not read: " + made.error().message);
    }
    std::vector<std::string> const companions = table::companionColumns(*description);
    table::CompanionOverrides overrides{.tone = {}, .tooltip = {}, .rowTone = node.prop("rowTone") != nullptr};
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): adds one described column: its kind, layout, display, filter and edit parts each read the same part and description
    auto const add = [&](table::TableColumn const& described, DocNode const* part) {
        TableColumn column{.id = described.info.id,
                           .label = described.label.empty() ? described.info.id : described.label,
                           .valign = tableValign,
                           .sortable = described.sortable};
        table::TableColumn display = described;
        CellExprs exprs{.tone = nullptr, .tooltip = nullptr, .path = part != nullptr ? part->path : node.path};
        if (part != nullptr) {
            display.display =
                displayOf(*part, described.display, [&](ExprPtr const& expr) { return once(ctx, expr); });
            if (described.info.kind == table::ColumnKind::Measure &&
                (display.display.decimals || !display.display.unit.empty())) {
                report(part->path, "`" + described.info.id +
                                       "` is a measure column, which shows each cell in its own unit; `decimals` "
                                       "and `unit` do not apply");
                display.display.decimals.reset();
                display.display.unit.clear();
            }
            exprs.tone = part->prop("tone");
            exprs.tooltip = part->prop("tooltip");
            if (exprs.tone) {
                overrides.tone.insert(described.info.id);
            }
            if (exprs.tooltip) {
                overrides.tooltip.insert(described.info.id);
            }
        }
        column.unit = display.display.unit;
        DisplayUnits units{.units = registry.get(), .byDimension = {}};
        if (model.config.held) {
            if (auto const chosen = model.config.held->units.find(described.info.id);
                chosen != model.config.held->units.end()) {
                units.byDimension.insert(chosen->second.begin(), chosen->second.end());
            }
        }
        shown->push_back(std::move(display));
        displayUnits->push_back(std::move(units));
        cellExprs->push_back(std::move(exprs));
        TextRole role = TextRole::Normal;
        if (part != nullptr) {
            if (ExprPtr const label = part->prop("label")) {
                column.label = textOf(once(ctx, label), format);
            }
            if (ExprPtr const width = part->prop("width")) {
                column.width = sizingOf(once(ctx, width));
            }
            if (ExprPtr const named = part->prop("role")) {
                Value const value = once(ctx, named);
                std::optional<TextRole> const found = roleNamed(textOf(value, format));
                if (!found) {
                    report(named->path, "role is " + canonicalJson(value) + "; " + std::string{kRoleNames});
                }
                role = found.value_or(TextRole::Normal);
            }
            column.role = role;
            column.align = cellPlaceNamed(part->name("align")).value_or(Align::Start);
            column.valign = cellPlaceNamed(part->name("valign")).value_or(tableValign);
            column.wrap = part->flag("wrap", true);
            column.shrink = part->flag("shrink") && column.width.kind == Sizing::Kind::Stretch;
        }
        // A measure's amounts line up on their digits, as an `end` column's do.
        if (described.info.kind == table::ColumnKind::Measure && (part == nullptr || part->name("align").empty())) {
            column.align = Align::End;
        }
        columns.push_back(std::move(column));
        parts.push_back(part);
        roles.push_back(role);
        source.levels.push_back(described.info.level);
    };
    // A companion column (a tone, a tooltip, a row's tone or lock) is never shown: it travels with its column.
    if (auto const laidOut = node.partList("columns"); !laidOut.empty()) {
        for (DocNodePtr const& part : laidOut) {
            if (std::ranges::binary_search(companions, part->id)) {
                report(part->path, "`" + part->id + "` is a companion column of `" + model.id + "`; it is not shown");
            } else if (table::TableColumn const* const described = describedColumn(part->id)) {
                add(*described, part.get());
            } else {
                report(part->path, "the table `" + model.id + "` has no column `" + part->id + "`");
            }
        }
    } else {
        // A column the description does not show is a column too, hidden by the layout until its user shows it.
        for (table::TableColumn const& described : description->columns) {
            if (!std::ranges::binary_search(companions, described.info.id)) {
                add(described, nullptr);
            }
        }
    }
    // What the row's own parts and the cells read of the row is fetched with every page: the key, each field named,
    // and every column when one of them passes the row on whole.
    RowReads reads;
    reads.ids.insert(description->key);
    for (std::string_view const key : {"rowTone", "rowLabel"}) {
        if (ExprPtr const expr = node.prop(key)) {
            rowReadsOf(*expr, reads);
        }
    }
    if (Commands const* const activate = node.event("onActivate")) {
        rowReadsOf(*activate, reads);
    }
    // A cell menu's entries act on any row, so what they read is fetched with every row, as the row's tone is.
    for (DocNodePtr const& entry : node.partList("cellMenu")) {
        rowReadsOf(*entry, reads);
    }
    // A column's own reads are fetched while the column is in the window; one that passes the row on whole makes
    // every column always fetched.
    std::vector<RowReads> partReads(parts.size());
    for (std::size_t at = 0; at < parts.size(); ++at) {
        if (parts.at(at) != nullptr) {
            rowReadsOf(*parts.at(at), partReads.at(at));
            reads.whole = reads.whole || partReads.at(at).whole;
        }
    }
    if (reads.whole) {
        report(node.path, "the table reads its row whole; every column of `" + model.id + "` is fetched");
        for (table::TableColumn const& column : description->columns) {
            reads.ids.insert(column.info.id);
        }
    }
    for (std::string const& columnId : reads.ids) {
        if (model.described.contains(columnId)) {
            source.alwaysFetched.push_back(columnId);
        }
    }
    std::vector<std::string> ids;
    ids.reserve(columns.size());
    for (TableColumn const& column : columns) {
        ids.push_back(column.id);
    }
    source.cellReads.resize(parts.size());
    source.ownCell.resize(parts.size(), false);
    for (std::size_t at = 0; at < parts.size(); ++at) {
        for (std::string const& columnId : partReads.at(at).ids) {
            if (columnId != ids.at(at) && model.described.contains(columnId) &&
                !std::ranges::contains(source.alwaysFetched, columnId)) {
                source.cellReads.at(at).push_back(columnId);
            }
        }
        source.ownCell.at(at) = parts.at(at) != nullptr && !parts.at(at)->nodes("cell").empty();
    }
    // The companions travel with their column: the query names them beside the columns the mount names.
    model.rows->companions([described = std::make_shared<table::TableDescription const>(*description),
                            overrides](std::span<std::string const> named) {
        return table::columnsWithCompanions(*described, named, overrides);
    });
    // A cell's text, by its column's display. Each column's `DisplayUnits` points into `registry`, so the formatters
    // hold it: it lives as long as the cells do.
    auto const textOfCell = [shown, displayUnits, registry](std::size_t column, Value const& record,
                                                            FormatContext const& formatting) {
        table::TableColumn const& display = shown->at(column);
        DisplayUnits const& units = displayUnits->at(column);
        return formatCell(cellOfRecord(record.field(display.info.id), display, units.units), display, formatting,
                          units);
    };
    // A cell's accent: the document's tone and tooltip, else its companions'; an unreadable value is invalid. An
    // unknown tone is reported once per column.
    auto const cellToneReported = std::make_shared<std::set<std::size_t>>();
    auto const accentOf = [this, shown, displayUnits, registry, cellExprs, cellToneReported, bindings = ctx.bindings](
                              std::size_t column, Value const& record, RtScope const* scope) {
        table::TableColumn const& display = shown->at(column);
        CellExprs const& exprs = cellExprs->at(column);
        auto const toneOf = [&](Value const& named) -> std::optional<Tone> {
            if (named.isNull() || (named.asString() != nullptr && named.asString()->empty())) {
                return std::nullopt;
            }
            if (std::optional<Tone> const tone = toneNamed(named)) {
                return tone;
            }
            if (cellToneReported->insert(column).second) {
                report(exprs.path, "column `" + display.info.id + "`'s tone is " + canonicalJson(named) +
                                       "; a tone is neutral, info, ok, warn or err");
            }
            return std::nullopt;
        };
        CellAccent accent;
        if (exprs.tone && scope != nullptr) {
            accent.tone = toneOf(evalIn(*scope, bindings.get(), exprs.tone));
        } else if (Value const* const named = display.tone.empty() ? nullptr : record.field(display.tone)) {
            accent.tone = toneOf(*named);
        }
        if (exprs.tooltip && scope != nullptr) {
            Value const tip = evalIn(*scope, bindings.get(), exprs.tooltip);
            accent.tooltip = tip.isNull() ? std::string{} : textOf(tip, scope->formatting());
        } else if (Value const* const tip = display.tooltip.empty() ? nullptr : record.field(display.tooltip)) {
            accent.tooltip = tip->asString() != nullptr ? *tip->asString() : std::string{};
        }
        DisplayUnits const& units = displayUnits->at(column);
        if (cellLook(cellOfRecord(record.field(display.info.id), display, units.units), display, units) ==
            CellLook::Invalid) {
            accent.invalid = true;
            accent.tooltip = std::string{kUnreadable};
        }
        return accent;
    };
    bool const cellExprsGiven = std::ranges::any_of(
        *cellExprs, [](CellExprs const& exprs) { return exprs.tone != nullptr || exprs.tooltip != nullptr; });
    // The row scope a cell's own tone and tooltip expressions read; none when no column gives one.
    auto const exprScope = [this, ctx, &node, &model](reactive::Signal<Value> const& row) {
        std::string key = runtime().untracked([&] {
            Value const* const rowKey = row.peek().field(model.keyColumn);
            return rowKey == nullptr ? std::string{} : canonicalJson(*rowKey);
        });
        auto const scope = rowScope(node, ctx, row, {}, std::move(key));
        scope->onMissingRowField = &model.onMissing;
        scope->rowKeyNow = [&row, &model] {
            Value const* const rowKey = row.get().field(model.keyColumn);
            return rowKey == nullptr ? std::string{} : canonicalJson(*rowKey);
        };
        return scope;
    };
    // The default cell follows the column the mount gives it, so one cell serves every column of its role.
    source.defaultCellView = [this, scope = ctx.scope, roles, textOfCell, accentOf, cellExprsGiven, exprScope](
                                 reactive::Signal<std::size_t> const& column,
                                 reactive::Signal<Value> const& row) -> Node {
        std::shared_ptr<RtScope const> const cellScope = cellExprsGiven ? exprScope(row) : nullptr;
        Node text = ui::text(Text{
            .text = slotProp<std::string>([scope, &row, &column, textOfCell] {
                return textOfCell(column.get(), row.get(), scope->formatting());
            }),
            // Read once: the mount pools default cells by role, so a cell only ever moves to a column of its own role.
            .role = roles.at(column.peek()),
        });
        return accentedCell(std::move(text), slotProp<CellAccent>([&row, &column, accentOf, cellScope] {
                                return accentOf(column.get(), row.get(), cellScope.get());
                            }));
    };
    source.rows = model.rows;
    // The cells' drafts, once an editable table's grid is made below: a plain cell shows its draft while one is held.
    auto const drafts = std::make_shared<std::weak_ptr<ModelGridSource>>();
    source.cellView = [this, ctx, &node, &model, parts, roles, textOfCell, accentOf, cellExprs, exprScope, drafts,
                       shown](std::size_t column, reactive::Signal<Value> const& row) -> Node {
        DocNode const* const part = column < parts.size() ? parts.at(column) : nullptr;
        std::span<DocNodePtr const> const cell = part != nullptr ? part->nodes("cell") : std::span<DocNodePtr const>{};
        if (!cell.empty()) {
            std::vector<DocNode const*> content;
            for (DocNodePtr const& child : cell) {
                content.push_back(child.get());
            }
            std::string key = runtime().untracked([&] {
                Value const* const rowKey = row.peek().field(model.keyColumn);
                return rowKey == nullptr ? std::string{} : canonicalJson(*rowKey);
            });
            auto const scope = rowScope(node, ctx, row, content, std::move(key));
            scope->onMissingRowField = &model.onMissing;
            // The mount rebinds a pooled cell to other rows by its signal: the row its elements are keyed by is the
            // one the signal holds now.
            scope->rowKeyNow = [&row, &model] {
                Value const* const rowKey = row.get().field(model.keyColumn);
                return rowKey == nullptr ? std::string{} : canonicalJson(*rowKey);
            };
            LowerCtx const rowCtx{.scope = scope, .bindings = ctx.bindings, .boundary = ctx.boundary};
            // The cell owns its scope for as long as its node lives.
            auto const owner =
                std::make_shared<std::pair<Node, std::shared_ptr<RtScope>> const>(lowerAll(cell, rowCtx), scope);
            return accentedCell(Node{owner, owner->first.get()}, slotProp<CellAccent>([&row, column, accentOf, scope] {
                                    return accentOf(column, row.get(), scope.get());
                                }));
        }
        CellExprs const& exprs = cellExprs->at(column);
        std::shared_ptr<RtScope const> const cellScope =
            exprs.tone != nullptr || exprs.tooltip != nullptr ? exprScope(row) : nullptr;
        Node text = ui::text(Text{
            .text = slotProp<std::string>([scope = ctx.scope, &row, &model, column, textOfCell, drafts, shown] {
                if (std::shared_ptr<ModelGridSource> const grid = drafts->lock()) {
                    if (Value const* const rowKey = row.get().field(model.keyColumn); rowKey != nullptr) {
                        if (std::optional<std::string> held =
                                grid->heldText(keyOf(*rowKey), shown->at(column).info.id)) {
                            return std::move(*held);
                        }
                    }
                }
                return textOfCell(column, row.get(), scope->formatting());
            }),
            .role = roles.at(column),
        });
        return accentedCell(std::move(text), slotProp<CellAccent>([&row, column, accentOf, cellScope] {
                                return accentOf(column, row.get(), cellScope.get());
                            }));
    };
    ExprPtr const toneExpr = node.prop("rowTone");
    ExprPtr const labelExpr = node.prop("rowLabel");
    Commands const* const activateCommands = node.event("onActivate");
    // Every row of a model's table has a look: its marks and its tone say whether an action works on it, failed on it
    // or kept it in view, and whether it is locked.
    {
        auto const toneReported = std::make_shared<bool>(false);
        source.rowView = [this, ctx, &node, &model, toneExpr, labelExpr,
                          toneReported](reactive::Signal<Value> const& row) {
            std::string key = runtime().untracked([&] {
                Value const* const rowKey = row.peek().field(model.keyColumn);
                return rowKey == nullptr ? std::string{} : canonicalJson(*rowKey);
            });
            auto const scope = rowScope(node, ctx, row, {}, key);
            scope->onMissingRowField = &model.onMissing;
            // A row that left the realized window released its scope; its entry goes when the next row comes, so the
            // index holds the realized rows and those that left since.
            std::erase_if(model.rowScopes, [](auto const& entry) { return entry.second.expired(); });
            model.rowScopes[std::move(key)] = scope;
            LowerCtx const rowCtx{.scope = scope, .bindings = ctx.bindings, .boundary = ctx.boundary};
            TableRowLook look;
            look.keep = scope;
            if (labelExpr) {
                look.label = bindProp<std::string>(rowCtx, labelExpr, std::string{}, stringOf);
            }
            std::optional<Key> const rowKey = runtime().untracked([&]() -> std::optional<Key> {
                Value const* const keyValue = row.peek().field(model.keyColumn);
                return keyValue == nullptr ? std::nullopt : std::optional{keyOf(*keyValue)};
            });
            // A locked row's tooltip is why it is locked; else a failed row's is its message, else an unmatched row
            // says so.
            look.marks = slotProp<RowMarks>([&model, &row, rowKey] {
                static_cast<void>(model.marksRevision.get());
                RowMarks marks;
                if (!rowKey) {
                    return marks;
                }
                Value const& record = row.get();
                table::TableDescription const* const rowDescription = model.window->description();
                std::optional<std::string> const locked =
                    rowDescription == nullptr ? std::nullopt
                                              : lockText(TableRowRef{.key = *rowKey, .row = record}, *rowDescription);
                RowStates const& states = model.rows->states();
                marks.busy = states.isBusy(*rowKey);
                marks.locked = locked.has_value();
                marks.unmatched = model.rows->unmatched(*rowKey);
                if (locked) {
                    marks.tooltip = *locked;
                } else if (std::optional<std::string> failure = states.failure(*rowKey)) {
                    marks.tooltip = std::move(*failure);
                } else if (marks.unmatched) {
                    marks.tooltip = "This row no longer matches the filter";
                }
                return marks;
            });
            // A failed row draws `err`; else the row's own tone.
            look.tone = slotProp<std::optional<Tone>>(
                [this, &model, rowKey, scope, bindings = ctx.bindings, toneExpr, toneReported] -> std::optional<Tone> {
                    static_cast<void>(model.marksRevision.get());
                    if (rowKey && model.rows->states().failure(*rowKey)) {
                        return Tone::Err;
                    }
                    if (!toneExpr) {
                        return std::nullopt;
                    }
                    return rowToneOf(evalIn(*scope, bindings.get(), toneExpr), toneExpr->path, *toneReported);
                });
            return look;
        };
    }
    options.showHeader = node.flag("showHeader", true);
    // The document's bands, else the description's; bands that cannot be drawn over the description's columns are
    // reported and none is drawn.
    options.bands = node.partList("bands").empty() ? description->bands : bandsOf(node.partList("bands"));
    if (!options.bands.empty()) {
        std::vector<std::string> described;
        described.reserve(description->columns.size());
        for (table::TableColumn const& column : description->columns) {
            described.push_back(column.info.id);
        }
        if (std::optional<std::string> const refused = table::bandRefusal(options.bands, described)) {
            report(pointerAppend(node.path, "bands"), *refused);
            options.bands.clear();
        }
    }
    options.selectColumn = node.flag("selectColumn");
    if (ExprPtr const selectAll = node.prop("selectAllLabel")) {
        options.selectAllLabel = bindProp<std::string>(ctx, selectAll, std::string{}, stringOf);
    }
    if (ExprPtr const overscan = node.prop("virtualizeOverscan")) {
        options.virtualize.overscan =
            static_cast<std::size_t>(std::max<std::int64_t>(0, intOf(once(ctx, overscan), 10)));
    }
    std::string const mode =
        node.prop("selectionMode") ? textOf(once(ctx, node.prop("selectionMode")), format) : std::string{};
    options.selectionMode =
        mode == "single" ? SelectionMode::Single
        // NOLINTNEXTLINE(readability-avoid-nested-conditional-operator): a three-way map of the mode text to the enum
        : mode == "multiple" ? SelectionMode::Multiple
                             : SelectionMode::None;
    options.selection = slotProp<std::vector<Key>>([&element] {
        std::vector<Key> keys;
        if (Value const itemsValue = tableSelection(element); ValueList const* const items = itemsValue.asList()) {
            for (Value const& item : *items) {
                keys.push_back(keyOf(item));
            }
        }
        return keys;
    });
    // The selection the user made: held by the element unless the document binds it, then the command list.
    auto const selectRows = [this, ctx, &element,
                             commands = node.event("onSelectionChange")](std::vector<Key> const& keys) {
        ValueList items;
        for (Key const& key : keys) {
            items.push_back(valueOfKey(key));
        }
        Value const selected{std::move(items)};
        runtime().batch([&] {
            if (element.bound == nullptr) {
                session.asUser([&] {
                    session.noteWrite();
                    element.selection.set(selected);
                });
            }
            runEvent(commands, ctx, selected);
        });
    };
    // Held by the table's handlers; a range's reply, which may come after the table is gone, holds it weakly, so it
    // keeps no scope of the screen alive.
    auto const select = std::make_shared<std::function<void(std::vector<Key> const&)>>(selectRows);
    // While the selection is of the view, the backend marks every row but `except`. A `Ctrl`+click reports the keys
    // now selected among the rows it holds: each held key absent from the report is left out, each present one is
    // selected again. A plain click ends the selection of the view.
    options.keyMoves = slotProp<KeyMoves>([&model] { return model.keyMoves.get(); });
    options.selectionView = slotProp<std::optional<std::vector<Key>>>([&model]() -> std::optional<std::vector<Key>> {
        auto const& view = model.viewSelection.get();
        if (!view) {
            return std::nullopt;
        }
        return std::vector<Key>(view->except.begin(), view->except.end());
    });
    options.onSelectionChange = [this, &element, &model, select](std::vector<Key> const& keys,
                                                                 SelectionChange change) {
        tableGesture(element);
        std::optional<TableSelection> const viewing = model.viewSelection.peek();
        if (viewing && change == SelectionChange::Extend) {
            std::unordered_set<Key> const inReport(keys.begin(), keys.end());
            TableSelection next = *viewing;
            // The rows the backend holds are those the mount named in or near view; a held row outside them is not
            // in the report, and is not read as left out.
            auto const [first, count] = model.rows->shownRange();
            for (std::int64_t at = first; at < first + count; ++at) {
                std::optional<TableRowRef> const row = model.rows->row(at);
                if (!row) {
                    continue;
                }
                if (inReport.contains(row->key)) {
                    std::erase(next.except, row->key);
                } else if (!std::ranges::contains(next.except, row->key)) {
                    next.except.push_back(row->key);
                }
            }
            model.viewSelection.set(std::move(next));
            return;
        }
        runtime().batch([&] {
            if (viewing) {
                model.viewSelection.set(std::nullopt);
            }
            (*select)(keys);
        });
    };
    // Select-all on a table that may select many rows selects the view: no row is fetched, and the server resolves
    // it when an action runs. Refused while the view's membership is open.
    if (options.selectionMode == SelectionMode::Multiple) {
        options.onSelectAll = [this, ctx, &element, &model] {
            tableGesture(element);
            auto view = model.rows->selectView();
            if (!view) {
                tableMessage(element, selectAllRefusalText(view.error(), ctx.scope->formatting()));
                return;
            }
            runtime().batch([&] {
                model.viewSelection.set(TableSelection{.keys = {}, .view = std::move(*view), .except = {}});
                if (element.bound == nullptr) {
                    session.asUser([&] {
                        session.noteWrite();
                        element.selection.set(Value{ValueList{}});
                    });
                }
            });
        };
    }
    // A range the backend reports as view rows: its keys come from the rows, fetched as pages of keys alone where the
    // window does not hold them, and select as the gesture says. Past `maxRangeRows` it is refused and selects
    // nothing.
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): resolves a reported range to keys, fetching pages of keys where the window lacks them, and applies the gesture; the paths share the range state
    options.onSelectRange = [this, ctx, &element, &model, select](std::size_t anchor, std::size_t focus,
                                                                  SelectionChange change) {
        tableGesture(element);
        std::size_t const first = std::min(anchor, focus);
        std::size_t const count = std::max(anchor, focus) - first + 1;
        std::size_t const bound = session.options().maxRangeRows;
        if (count > bound) {
            tableMessage(element,
                         "A range selects at most " +
                             render::formatCanonicalNumber(std::to_string(bound), ctx.scope->formatting().numeric) +
                             " rows");
            return;
        }
        // In a selection of the view a range changes `except`: from an anchor left out it leaves the range out,
        // from a selected anchor it selects the range again.
        if (std::optional<TableSelection> const viewing = model.viewSelection.peek()) {
            std::optional<TableRowRef> const anchorRow = model.rows->row(static_cast<std::int64_t>(anchor));
            bool const leaving = anchorRow && std::ranges::contains(viewing->except, anchorRow->key);
            model.rows->keysOf(static_cast<std::int64_t>(first), static_cast<std::int64_t>(count))
                .then(model.scope, [this, &element, &model, leaving](
                                       std::expected<std::vector<table::RowId>, table::TableError> const& found) {
                    if (!found) {
                        tableMessage(element, "The range could not be selected: " + found.error().message);
                        return;
                    }
                    std::optional<TableSelection> next = model.viewSelection.peek();
                    if (!next) {
                        return;
                    }
                    for (table::RowId const& key : *found) {
                        if (!leaving) {
                            std::erase(next->except, key);
                        } else if (!std::ranges::contains(next->except, key)) {
                            next->except.push_back(key);
                        }
                    }
                    model.viewSelection.set(std::move(next));
                });
            return;
        }
        model.rows->keysOf(static_cast<std::int64_t>(first), static_cast<std::int64_t>(count))
            .then(model.scope, [this, &element, select = std::weak_ptr{select},
                                change](std::expected<std::vector<table::RowId>, table::TableError> const& found) {
                if (!found) {
                    tableMessage(element, "The range could not be selected: " + found.error().message);
                    return;
                }
                std::vector<Key> keys;
                if (change == SelectionChange::Extend) {
                    if (Value const heldValue = tableSelection(element);
                        ValueList const* const held = heldValue.asList()) {
                        for (Value const& item : *held) {
                            keys.push_back(keyOf(item));
                        }
                    }
                }
                for (table::RowId const& rowId : *found) {
                    Key const key = std::visit([](auto const& value) { return Key{value}; }, rowId);
                    if (!std::ranges::contains(keys, key)) {
                        keys.push_back(key);
                    }
                }
                if (auto const live = select.lock()) {
                    (*live)(keys);
                }
            });
        // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks): the analyzer loses the owner of the callable `std::function` heap-allocates for this capture; the function object owns it and frees it
    };
    options.highlight = slotProp<std::optional<TableHighlight>>([&model] { return model.highlight.get(); });
    // The header's sort chain and menus, and the columns' arrangement; the column chooser and the sort dialog.
    // A column's filter menu shows its values as the column shows its cells, and reads a measure's operand with the
    // description's units.
    {
        auto const described = std::make_shared<table::TableDescription const>(*description);
        auto const labels = std::make_shared<std::vector<std::string>>();
        auto const refused = std::make_shared<std::vector<bool>>();
        for (std::size_t at = 0; at < columns.size(); ++at) {
            labels->push_back(columns.at(at).label);
            refused->push_back(parts.at(at) != nullptr && !parts.at(at)->flag("filterable", true));
        }
        auto const unitsOf = [described](table::TableColumn const& column) {
            std::vector<std::string> dimensions = column.info.dimensions;
            if (dimensions.empty()) {
                for (table::UnitInfo const& unit : described->units) {
                    if (!std::ranges::contains(dimensions, unit.dimension)) {
                        dimensions.push_back(unit.dimension);
                    }
                }
            }
            std::vector<MenuDimension> out;
            for (std::string const& dimension : dimensions) {
                MenuDimension entry{.dimension = dimension, .units = {}, .chosen = {}};
                for (table::UnitInfo const& unit : described->units) {
                    if (unit.dimension == dimension) {
                        entry.units.push_back(unit.id);
                    }
                }
                if (!entry.units.empty()) {
                    out.push_back(std::move(entry));
                }
            }
            return out;
        };
        bool const listed = described->capabilities.distinct && node.name("action").empty();
        model.config.beginMount();
        model.config.filterTarget = [shown, displayUnits, registry, described, labels, refused, unitsOf,
                                     listed](std::string const& columnId) -> std::optional<FilterTarget> {
            auto const found = std::ranges::find_if(
                *shown, [&columnId](table::TableColumn const& column) { return column.info.id == columnId; });
            if (found == shown->end() || !found->filterable) {
                return std::nullopt;
            }
            auto const index = static_cast<std::size_t>(found - shown->begin());
            if (refused->at(index)) {
                return std::nullopt;
            }
            FilterTarget target;
            target.column = columnId;
            target.label = labels->at(index);
            target.shown = *found;
            target.units = displayUnits->at(index);
            target.unitList = described->units;
            if (found->info.kind == table::ColumnKind::Measure) {
                for (MenuDimension const& dimension : unitsOf(*found)) {
                    target.unitChoices.insert(target.unitChoices.end(), dimension.units.begin(),
                                              dimension.units.end());
                }
            }
            target.listed = listed;
            target.keep = registry;
            return target;
        };
        // A cell a menu opens on: unloaded while its row holds no field for it or its level has not loaded.
        model.config.heldCell = [&model, shown, displayUnits, registry](
                                    Key const& key, std::string const& columnId) -> std::optional<HeldCell> {
            auto const found = std::ranges::find_if(
                *shown, [&columnId](table::TableColumn const& column) { return column.info.id == columnId; });
            if (found == shown->end()) {
                return std::nullopt;
            }
            auto const place = static_cast<std::size_t>(found - shown->begin());
            for (auto const& [first, count] : model.window->heldRanges()) {
                for (std::int64_t at = first; at < first + count; ++at) {
                    std::optional<TableRowRef> const ref = model.rows->row(at);
                    if (!ref || ref->key != key) {
                        continue;
                    }
                    Value const* const field = ref->row.field(columnId);
                    table::UnloadedLevels const levels = model.rows->unloadedLevels(at);
                    bool const unloaded =
                        field == nullptr ||
                        (!found->info.level.empty() && std::ranges::contains(levels.levels, found->info.level));
                    return HeldCell{.record = ref->row,
                                    .cell = unloaded ? table::Cell{table::Unloaded{}}
                                                     : cellOfRecord(field, *found, registry.get()),
                                    .shown = *found,
                                    .units = displayUnits->at(place),
                                    .keep = registry};
                }
            }
            return std::nullopt;
        };
        model.config.dimensions = [shown, unitsOf](std::string const& unitId) {
            auto const found = std::ranges::find_if(
                *shown, [&unitId](table::TableColumn const& column) { return column.info.id == unitId; });
            return found == shown->end() || found->info.kind != table::ColumnKind::Measure
                       ? std::vector<MenuDimension>{}
                       : unitsOf(*found);
        };
    }
    // A copy reads the rows the window holds and fetches the others by key, naming the columns copied; each cell
    // is written as the column shows it, in its display unit, and each header with its unit. A column's own `cell`
    // is not copied: its value's text is.
    auto const headers = std::make_shared<std::vector<std::string>>();
    for (TableColumn const& column : columns) {
        headers->push_back(headerText(column.label, column.unit));
    }
    model.config.selectKeys = [select](std::vector<Key> const& keys) { (*select)(keys); };
    model.config.viewSelected = [&model]() -> std::optional<std::int64_t> {
        auto const& view = model.viewSelection.peek();
        if (!view) {
            return std::nullopt;
        }
        return model.rows->selectedCount(*view).rows;
    };
    // A selection of the view is copied from page queries of the view's rows naming the columns copied, in view
    // order, its `except` left out.
    model.config.gatherView = [&model, shown, displayUnits, registry, headers, scope = ctx.scope](
                                  std::vector<std::string> const& columnIds, CopyDone const& done) {
        std::vector<std::size_t> places;
        std::vector<std::string> named;
        CopyGathered gathered;
        for (std::string const& columnId : columnIds) {
            auto const found = std::ranges::find_if(
                *shown, [&columnId](table::TableColumn const& column) { return column.info.id == columnId; });
            if (found != shown->end()) {
                places.push_back(static_cast<std::size_t>(found - shown->begin()));
                named.push_back(columnId);
                gathered.headers.push_back(headers->at(places.back()));
            }
        }
        std::optional<TableSelection> const view = model.viewSelection.peek();
        if (!view) {
            done(std::move(gathered));
            return;
        }
        std::unordered_set<Key> const except(view->except.begin(), view->except.end());
        FormatContext const formatting = scope->formatting();
        model.rows->rowsOf(0, model.rows->total().rows, named)
            .then(model.scope,
                  [shown, displayUnits, registry, places, formatting, except, done, gathered = std::move(gathered)](
                      std::expected<std::vector<FetchedRow>, table::TableError> const& found) mutable {
                      if (!found) {
                          done(std::unexpected("The rows could not be copied: " + found.error().message));
                          return;
                      }
                      for (FetchedRow const& fetchedRow : *found) {
                          if (except.contains(fetchedRow.key)) {
                              continue;
                          }
                          CopyRow row{.viewRow = fetchedRow.viewRow, .cells = {}};
                          for (std::size_t column = 0; column < places.size() && column < fetchedRow.cells.size();
                               ++column) {
                              table::Cell const& cell = fetchedRow.cells.at(column);
                              std::size_t const place = places.at(column);
                              row.cells.push_back(std::holds_alternative<table::Unloaded>(cell)
                                                      ? std::nullopt
                                                      : std::optional{formatCell(cell, shown->at(place), formatting,
                                                                                 displayUnits->at(place))});
                          }
                          gathered.rows.push_back(std::move(row));
                      }
                      done(std::move(gathered));
                  });
    };
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): the gather of a model table: it formats each row's cells by the visible columns with the units, registry and places; the steps share those
    model.config.gather = [&model, shown, displayUnits, registry, headers, textOfCell, scope = ctx.scope](
                              std::vector<Key> keys, std::vector<std::string> const& columnIds, CopyDone const& done) {
        std::vector<std::size_t> places;
        std::vector<std::string> named;
        CopyGathered gathered;
        for (std::string const& columnId : columnIds) {
            auto const found = std::ranges::find_if(
                *shown, [&columnId](table::TableColumn const& column) { return column.info.id == columnId; });
            if (found != shown->end()) {
                places.push_back(static_cast<std::size_t>(found - shown->begin()));
                named.push_back(columnId);
                gathered.headers.push_back(headers->at(places.back()));
            }
        }
        FormatContext const formatting = scope->formatting();
        std::unordered_set<Key> wanted{keys.begin(), keys.end()};
        // The rows held with every column copied; a row held without one of them is fetched with the rest.
        TableRows const& rows = *model.rows;
        std::vector<std::int64_t> held;
        for (auto const& [first, count] : model.window->heldRanges()) {
            for (std::int64_t at = first; at < first + count; ++at) {
                held.push_back(at);
            }
        }
        for (std::int64_t const index : held) {
            if (wanted.empty()) {
                break;
            }
            std::optional<TableRowRef> const ref = rows.row(index);
            if (!ref || !wanted.contains(ref->key) || !std::ranges::all_of(named, [&](std::string const& columnId) {
                    return ref->row.field(columnId) != nullptr;
                })) {
                continue;
            }
            std::span<std::string const> const unloaded = rows.unloadedLevels(index).levels;
            CopyRow row{.viewRow = index, .cells = {}};
            for (std::size_t const place : places) {
                std::string const& level = shown->at(place).info.level;
                bool const missing = !level.empty() && std::ranges::contains(unloaded, level);
                row.cells.push_back(missing ? std::nullopt : std::optional{textOfCell(place, ref->row, formatting)});
            }
            gathered.rows.push_back(std::move(row));
            wanted.erase(ref->key);
        }
        if (wanted.empty()) {
            done(std::move(gathered));
            return;
        }
        std::vector<table::RowId> fetched;
        for (Key const& key : keys) {
            if (wanted.erase(key) != 0) {
                fetched.push_back(key);
            }
        }
        model.rows->fetchRows(std::move(fetched), std::move(named))
            .then(model.scope,
                  [shown, displayUnits, registry, places, formatting, done, gathered = std::move(gathered)](
                      std::expected<std::vector<FetchedRow>, table::TableError> const& found) mutable {
                      if (!found) {
                          done(std::unexpected("The rows could not be copied: " + found.error().message));
                          return;
                      }
                      for (FetchedRow const& fetchedRow : *found) {
                          CopyRow row{.viewRow = fetchedRow.viewRow, .cells = {}};
                          for (std::size_t column = 0; column < places.size() && column < fetchedRow.cells.size();
                               ++column) {
                              table::Cell const& cell = fetchedRow.cells.at(column);
                              std::size_t const place = places.at(column);
                              row.cells.push_back(std::holds_alternative<table::Unloaded>(cell)
                                                      ? std::nullopt
                                                      : std::optional{formatCell(cell, shown->at(place), formatting,
                                                                                 displayUnits->at(place))});
                          }
                          gathered.rows.push_back(std::move(row));
                      }
                      done(std::move(gathered));
                  });
    };
    Node dialogs = configureTable(model.config, ctx, options, nullptr);
    markFilterable(model.config, columns, options);
    options.onActivate = [this, ctx, &element, &model, commands = activateCommands](Key const& key) {
        Value const activated = valueOfKey(key);
        session.asUser([&] {
            session.noteWrite();
            element.activeKey.set(activated);
        });
        auto const found = model.rowScopes.find(canonicalJson(activated));
        std::shared_ptr<RtScope> const scope = found == model.rowScopes.end() ? nullptr : found->second.lock();
        LowerCtx const rowCtx{.scope = scope ? scope : ctx.scope, .bindings = ctx.bindings, .boundary = ctx.boundary};
        runEvent(commands, rowCtx, activated);
    };
    model.rows->mounting();
    if (std::shared_ptr<GridController> const grid =
            editableModelTable(node, ctx, element, *description, *shown, source, options, drafts)) {
        return besideDialogs(options.common, std::move(dialogs),
                             [&] { return grid->node(std::move(columns), std::move(options), std::move(source)); });
    }
    return besideDialogs(options.common, std::move(dialogs),
                         [&] { return ui::table(std::move(columns), std::move(source), std::move(options)); });
}

// A model's table with a column its description edits is mounted as a grid over its cells: each editable column's
// cells the field of its edit action, read from the catalog's schema, read-only where the principal may not run it.
// The grid's controller owns its source and what follows the table for it: the status line's notices, the rows'
// busy and failed states, and the layout the walk follows. Null for a table with no editable column.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): builds an editable model table: each editable column's field, its read-only state and the controller's ownership of source and status line
std::shared_ptr<GridController> ScreenCore::editableModelTable(
    DocNode const& node, LowerCtx const& ctx, ElementState& element, table::TableDescription const& description,
    std::vector<table::TableColumn> const& shown, TableSource& source, TableOptions const& options,
    std::shared_ptr<std::weak_ptr<ModelGridSource>> const& drafts) {
    ModelTable& model = *element.modelTable;
    if (model.edits == nullptr) {
        return nullptr;
    }
    Catalog const& catalog = session.options().catalog;
    std::vector<ModelColumn> columns;
    bool editable = false;
    for (table::TableColumn const& described : shown) {
        ModelColumn column{.id = described.info.id, .edit = described.edit, .model = nullptr, .path = {}};
        if (described.edit) {
            std::optional<std::string> const schema =
                catalog.schema ? catalog.schema(described.edit->action) : std::nullopt;
            if (schema) {
                if (auto made = forms::FormModel::fromSchema(described.edit->action, *schema)) {
                    column.model = std::make_shared<forms::FormModel const>(std::move(*made));
                    column.path =
                        described.edit->member.empty() ? std::string{"value"} : "values." + described.edit->member;
                    editable = true;
                } else {
                    report(node.path, "`" + described.info.id + "` is edited by `" + described.edit->action +
                                          "`, whose input schema does not read: " + made.error().message);
                }
            }
        }
        columns.push_back(std::move(column));
    }
    if (!editable) {
        return nullptr;
    }
    EditMode const mode = node.name("editMode") == "row" ? EditMode::Row : EditMode::Cell;
    // The edit sessions' number locale holds views into the formatting it was given, which the grid keeps.
    std::shared_ptr<FormatContext const> const formatting = session.formattingHandle();
    auto const grid = std::make_shared<ModelGridSource>(
        model.rows, *model.edits, std::move(columns), mode,
        ModelGridContext{.runtime = &runtime(),
                         .choices = model.calls,
                         .session = engineOptions(),
                         .format = ctx.scope->formatting(),
                         .may = [this](std::string_view action) { return session.can(action); }});
    grid->setDescription(description);
    *drafts = grid;
    model.editGrid = grid;
    GridOptions walk{};
    gridOptions(node, ctx, walk);
    walk.appendRows = false;
    walk.rowsFixed = true;
    walk.onActivate = options.onActivate;
    auto controller = std::make_shared<GridController>(runtime(), *grid, std::move(walk));
    // What the mounted grid holds while it lives: its source, the formatting its sessions read, and its effects.
    struct Held {
        std::shared_ptr<ModelGridSource> grid;
        std::shared_ptr<FormatContext const> formatting;
        std::vector<std::unique_ptr<reactive::Effect>> effects;
    };
    auto const held = std::make_shared<Held>(Held{.grid = grid, .formatting = formatting, .effects = {}});
    std::weak_ptr<GridController> const weak = controller;
    ModelGridSource* const cells = grid.get();
    // The grid's refusals and notices go to the status line as the table's messages, each once.
    auto const posted = std::make_shared<std::uint64_t>(0);
    held->effects.push_back(std::make_unique<reactive::Effect>(runtime(), [this, &element, weak, posted] {
        std::shared_ptr<GridController> const live = weak.lock();
        if (live == nullptr) {
            return;
        }
        std::string text = live->message();
        std::uint64_t const notices = live->notices();
        runtime().untracked([&] {
            if (notices != *posted) {
                *posted = notices;
                tableMessage(element, std::move(text));
            }
        });
    }));
    // The rows' busy and failed states are not reactive: their revision tells the cells.
    held->effects.push_back(std::make_unique<reactive::Effect>(runtime(), [this, &model, cells] {
        static_cast<void>(model.marksRevision.get());
        runtime().untracked([cells] { cells->touch(); });
    }));
    // The walk follows the visible columns in layout order, and the cells the scope's formatting.
    held->effects.push_back(std::make_unique<reactive::Effect>(runtime(), [this, &model, cells, scope = ctx.scope] {
        TableConfig const& arranged = model.config;
        std::vector<std::string> visible =
            layoutOver(arranged.columns, arranged.declaredHidden, layoutNow(arranged)).visible();
        FormatContext format = scope->formatting();
        runtime().untracked([&] {
            if (!visible.empty()) {
                cells->setLayout(std::move(visible));
            }
            cells->setFormat(std::move(format));
        });
    }));
    controller->keepAlive(held);
    source.rows = std::shared_ptr<TableRows>(grid, &grid->view());
    // Every cell is the column's own, so each draws its marks: no default cell is shared across columns.
    source.defaultCellView = {};
    return controller;
}

TableRowLook ScreenCore::headingRow(DocNode const& node, LowerCtx const& ctx, reactive::Signal<Value> const& row,
                                    std::string key) {
    auto const heading = node.partList("group").front()->nodes("heading");
    std::vector<DocNode const*> content;
    content.reserve(heading.size());
    for (DocNodePtr const& child : heading) {
        content.push_back(child.get());
    }
    auto const scope = rowScope(node, ctx, row, content, std::move(key));
    LowerCtx const rowCtx{.scope = scope, .bindings = ctx.bindings, .boundary = ctx.boundary};
    // The row keeps the heading's scope for as long as it is realized.
    TableRowLook look;
    look.heading = lowerAll(heading, rowCtx);
    look.keep = scope;
    return look;
}

Node ScreenCore::lowerChart(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    Chart spec;
    spec.data = slotProp<ChartData>(
        [this, &node, scope = ctx.scope, bindings = ctx.bindings] { return chartData(node, *scope, bindings.get()); });
    commonOf(node, ctx, element, spec.common);
    return ui::chart(std::move(spec));
}

std::optional<math::Rational> ScreenCore::chartMagnitude(Value const& value, std::string const& path,
                                                         std::optional<std::string>& unit) {
    std::optional<Number> const number = numeric(value);
    if (!number) {
        report(path, "a chart value is a number or a quantity; this one is left out");
        return std::nullopt;
    }
    std::string const own = number->unit != nullptr ? *number->unit : std::string{};
    if (!unit) {
        unit = own;
        return number->value;
    }
    if (*unit == own) {
        return number->value;
    }
    std::optional<math::Rational> converted;
    if (!own.empty() && !unit->empty()) {
        converted = convertUnit(number->value, own, *unit, session.formatting().units);
    }
    if (!converted) {
        report(path, "a chart value does not convert to the unit of the chart's first value; it is left out");
    }
    return converted;
}

std::optional<Tone> ScreenCore::chartToneOf(Value const& value, std::string const& path) {
    static constexpr std::array<std::pair<std::string_view, Tone>, 5> kTones = {
        {
            {"neutral", Tone::Neutral},
            {"info", Tone::Info},
            {"ok", Tone::Ok},
            {"warn", Tone::Warn},
            {"err", Tone::Err},
        },
    };
    if (value.isNull()) {
        return std::nullopt;
    }
    if (std::string const* const text = value.asString()) {
        for (auto const& [name, tone] : kTones) {
            if (name == *text) {
                return tone;
            }
        }
    }
    report(path, "a chart tone is neutral, info, ok, warn or err; this one marks nothing");
    return std::nullopt;
}

std::optional<math::Rational> ScreenCore::chartPosition(Value const& value, std::string const& path,
                                                        detail::ChartAxisKinds& kinds) {
    if (value.isNull()) {
        return std::nullopt;
    }
    if (std::optional<Number> const number = numeric(value)) {
        kinds.numbers = true;
        return number->value;
    }
    if (std::optional<time::DateTime> const instant =
            dateInstantOf(value, session.formatting().utcOffsetMinutes).instant) {
        kinds.instants = true;
        return math::Rational{instant->value.time_since_epoch().count(), math::DecimalPlaces{0}};
    }
    report(path, "a chart's `x` is a number, a date or an instant; this point is left out");
    return std::nullopt;
}

std::vector<detail::ChartRawPoint> ScreenCore::chartPoints(DocNode const& series, RtScope const& scope,
                                                           Bindings const* bindings, std::optional<std::string>& unit,
                                                           detail::ChartAxisKinds& kinds) {
    // A chart is a picture of a few hundred values; a larger set is a table's.
    static constexpr std::size_t kMaxPoints = 1000;
    FormatContext const& format = scope.formatting();
    ExprPtr const pointsExpr = series.prop("points");
    Value const listed = evalIn(scope, bindings, pointsExpr);
    ValueList const* const list = listed.asList();
    if (list == nullptr) {
        // `null` is a query still loading, and draws as empty without a word.
        if (!listed.isNull()) {
            report(pointsExpr->path, "`points` is a list; the series draws as empty");
        }
        return {};
    }
    std::size_t const first = list->size() > kMaxPoints ? list->size() - kMaxPoints : 0;
    if (first > 0) {
        report(pointsExpr->path, "a series draws its last 1000 points");
    }
    ExprPtr const yExpr = series.prop("y");
    ExprPtr const xExpr = series.prop("x");
    ExprPtr const labelExpr = series.prop("xLabel");
    ExprPtr const toneExpr = series.prop("tone");
    ExprPtr const noteExpr = series.prop("note");
    std::vector<detail::ChartRawPoint> points;
    for (std::size_t index = first; index < list->size(); ++index) {
        ScopeEnv const env{scope, bindings, &list->at(index)};
        detail::ChartRawPoint point{.index = index};
        if (xExpr) {
            point.x = chartPosition(evaluate(*xExpr, env), xExpr->path, kinds);
            if (!point.x) {
                continue;
            }
        }
        Value const value = evaluate(*yExpr, env);
        if (!value.isNull()) {
            point.y = chartMagnitude(value, yExpr->path, unit);
            if (!point.y) {
                continue;
            }
        }
        point.xLabel = labelExpr ? textOf(evaluate(*labelExpr, env), format) : std::string{};
        point.note = noteExpr ? textOf(evaluate(*noteExpr, env), format) : std::string{};
        point.tone = toneExpr ? chartToneOf(evaluate(*toneExpr, env), toneExpr->path) : std::nullopt;
        points.push_back(std::move(point));
    }
    return points;
}

}  // namespace morph::ui::detail
