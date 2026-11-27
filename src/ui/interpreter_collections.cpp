// SPDX-License-Identifier: Apache-2.0

// Tables over a document's collections and rows.

#include "interpreter_grid_detail.hpp"

namespace morph::ui::detail {

[[nodiscard]] Node besideDialogs(Common& placed, Node dialogs, std::function<Node()> const& table) {
    if (dialogs == nullptr) {
        return table();
    }
    std::vector<Node> beside;
    beside.push_back(std::move(dialogs));
    return besideTable(placed, std::move(beside), table);
}

void markFilterable(TableConfig const& config, std::vector<TableColumn>& columns, TableOptions const& options) {
    if (!options.onFilter || !config.filterTarget) {
        return;
    }
    for (std::size_t at = 0; at < columns.size() && at < config.columns.size(); ++at) {
        columns.at(at).filterable = config.filterTarget(config.columns.at(at).id).has_value();
    }
}

namespace {

/// The rows a collection's table shows its mounts: its engine's, every change of the view told to the mount and
/// then to the table, whose status line and release read the engine after it.
class HeardRows final : public TableRows {
public:
    HeardRows(std::shared_ptr<EngineRows> engine, std::function<void()> heard)
        : _engine{std::move(engine)}, _heard{std::move(heard)} {
        _engine->onChange([this](table::ViewChange const& change) {
            if (_handler) {
                _handler(change);
            }
            _heard();
        });
    }

    HeardRows(HeardRows const&) = delete;
    HeardRows& operator=(HeardRows const&) = delete;
    HeardRows(HeardRows&&) = delete;
    HeardRows& operator=(HeardRows&&) = delete;
    ~HeardRows() override { _engine->onChange({}); }

    [[nodiscard]] TableTotal total() const override { return _engine->total(); }
    [[nodiscard]] std::optional<TableRowRef> row(std::int64_t index) const override { return _engine->row(index); }
    [[nodiscard]] table::UnloadedLevels unloadedLevels(std::int64_t index) const override {
        return _engine->unloadedLevels(index);
    }
    void visible(std::int64_t first, std::int64_t count, std::span<std::string const> columns) override {
        _engine->visible(first, count, columns);
    }
    async::Completion<std::expected<std::vector<table::RowId>, table::TableError>> keysOf(
        std::int64_t first, std::int64_t count) override {
        return _engine->keysOf(first, count);
    }
    void locateRow(table::RowId key, ::core::async::StopToken stop,
                   std::function<void(RowLocation const&)> done) override {
        _engine->locateRow(std::move(key), std::move(stop), std::move(done));
    }
    async::Completion<std::expected<std::vector<FetchedRow>, table::TableError>> fetchRows(
        std::vector<table::RowId> keys, std::vector<std::string> columns) override {
        return _engine->fetchRows(std::move(keys), std::move(columns));
    }
    std::expected<void, table::TableError> setSort(SortChain sort) override {
        return _engine->setSort(std::move(sort));
    }
    std::expected<void, table::TableError> setFilter(table::FilterSpec filter) override {
        return _engine->setFilter(std::move(filter));
    }
    async::Completion<std::expected<table::DistinctValues, table::TableError>> distinctValues(
        table::DistinctRequest request) override {
        return _engine->distinctValues(std::move(request));
    }
    [[nodiscard]] TableCountsView counts() const override { return _engine->counts(); }
    void onChange(std::function<void(table::ViewChange const&)> handler) override { _handler = std::move(handler); }
    [[nodiscard]] RowStates& states() override { return _engine->states(); }
    [[nodiscard]] RowStates const& states() const override { return _engine->states(); }
    void moveKey(table::RowId const& from, table::RowId const& target) override { _engine->moveKey(from, target); }

private:
    std::shared_ptr<EngineRows> _engine;
    std::function<void()> _heard;
    std::function<void(table::ViewChange const&)> _handler;
};

}  // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity): builds a rows table: the engine, the columns, the sort, the filter and the state wiring in order, each taking the engine and config before
std::unique_ptr<RowsTable> ScreenCore::makeRowsTable(ElementState& element, std::shared_ptr<Bindings const> bindings,
                                                     bool persisted) {
    DocNode const& node = *element.node;
    RtScope& scope = *element.scope;
    auto made = std::make_unique<RowsTable>(runtime());
    RowsTable& table = *made;
    table.core = this;
    table.element = &element;
    table.node = &node;
    table.where = &scope;
    table.bindings = bindings;
    if (node.flags.contains("status")) {
        table.status = node.flag("status");
    }
    for (DocNodePtr const& part : node.partList("columns")) {
        ValueColumn column{.id = part->id, .kind = columnKindNamed(part->name("kind")), .sortBy = {}};
        if (ExprPtr const sortBy = part->prop("sortBy")) {
            column.sortBy = [&scope, bindings, sortBy](Value const& row) {
                return evaluate(*sortBy, ScopeEnv{scope, bindings.get(), &row});
            };
        }
        table.columns.push_back(std::move(column));
    }
    table.keyOfRow = [&scope, bindings, keyExpr = node.prop("key")](Value const& row) -> Key {
        if (!keyExpr) {
            Value const rowKey = rowId(row);
            return keyOf(rowKey.isNull() ? row : rowKey);
        }
        return keyOf(evaluate(*keyExpr, ScopeEnv{scope, bindings.get(), &row}));
    };
    auto const groups = node.partList("group");
    ExprPtr const delta = groups.empty() ? nullptr : groups.front()->prop("by");
    auto const byOf = [&scope, bindings, delta](Value const& row) {
        return evaluate(*delta, ScopeEnv{scope, bindings.get(), &row});
    };
    if (delta) {
        table.grouped = true;
        table.byOf = byOf;
    }
    table.units = scope.formatting().units;
    table.elementOwn = persisted;
    // The columns its layout arranges, in the order the lowering gives them: the derived `columns`, then the parts.
    // A column with no id cannot be named by a stored layout: it is named by its place, and locked there.
    TableConfig& config = table.config;
    config.element = &element;
    auto const [allowed, menus] = configurableOf(node);
    config.allowed = allowed;
    config.menus = menus;
    runtime().untracked([&] {
        FormatContext const& format = scope.formatting();
        auto const add = [&config](std::string columnId, std::string label, bool locked, bool sortable,
                                   table::ColumnKind kind) {
            if (columnId.empty()) {
                columnId = "#" + std::to_string(config.columns.size());
                locked = true;
            }
            config.columns.push_back(
                LayoutColumn{.id = std::move(columnId), .label = std::move(label), .locked = locked, .kind = kind});
            config.sortable.push_back(sortable);
        };
        if (ExprPtr const derived = node.prop("columns")) {
            Value const list = evalIn(scope, bindings.get(), derived);
            if (ValueList const* const items = list.asList()) {
                for (Value const& item : *items) {
                    Value const* const field = item.field("id");
                    Value const* const label = item.field("label");
                    std::string columnId = field == nullptr ? std::string{} : textOf(*field, format);
                    std::string text = label != nullptr ? textOf(*label, format) : columnId;
                    add(std::move(columnId), std::move(text), false, false, table::ColumnKind::Text);
                }
            }
        }
        for (DocNodePtr const& part : node.partList("columns")) {
            ExprPtr const label = part->prop("label");
            add(part->id, label ? textOf(evalIn(scope, bindings.get(), label), format) : part->id,
                part->flag("locked"), part->flag("sortable"),
                columnKindNamed(part->name("kind")).value_or(table::ColumnKind::Text));
        }
    });
    std::size_t sortBound = 8;
    if (ExprPtr const bound = node.prop("maxSortKeys"); bound && bound->kind == ExprKind::Literal) {
        if (std::int64_t const* const keys = bound->literal.asInt(); keys != nullptr && *keys >= 1) {
            sortBound = static_cast<std::size_t>(*keys);
        }
    }
    config.maxSortKeys = [sortBound] { return sortBound; };
    // The groups the node declares, read once: their ids, labels and columns are literals the loader checked.
    for (DocNodePtr const& part : node.partList("groups")) {
        table::ColumnGroupInfo group{.id = part->id, .label = part->id, .columns = {}};
        if (ExprPtr const label = part->prop("label")) {
            group.label =
                runtime().untracked([&] { return textOf(evalIn(scope, bindings.get(), label), scope.formatting()); });
        }
        if (ExprPtr const members = part->prop("columns"); members && members->kind == ExprKind::Literal) {
            if (ValueList const* const ids = members->literal.asList()) {
                for (Value const& columnId : *ids) {
                    if (std::string const* const name = columnId.asString()) {
                        group.columns.push_back(*name);
                    }
                }
            }
        }
        table.groups.push_back(std::move(group));
    }
    // The filters the user set from the header: the engine's, held here, and the columns they read kept in use.
    config.filters = [&table] {
        static_cast<void>(table.version.get());
        return table.filter;
    };
    config.setFilters = [this, &table](table::FilterSpec const& filter) {
        useColumns(table, usedColumns(table, table.sort, filter));
        if (auto const set = table.engine->setFilter(filter); !set) {
            useColumns(table, usedColumns(table, table.sort, table.filter));
            rowsTableMessage(table, "The filter is refused: " + set.error().message);
            return false;
        }
        table.filter = filter;
        table.bump();
        return true;
    };
    config.rows = [&table]() -> TableRows* { return table.engine.get(); };
    config.declaredGroups = [&table] { return table.groups; };
    config.listing = [&table](std::vector<std::string> const& columns) {
        table.listedColumns = columns;
        useColumns(table, usedColumns(table, table.sort, table.filter));
    };
    config.message = [this, &table](std::string text) { rowsTableMessage(table, std::move(text)); };
    config.touch = [this, &table] {
        table.touched = true;
        rowsTableMessage(table, {});
    };
    config.save = [&table](table::StateDocument const& document, StateParts parts) {
        if (table.stateKey) {
            table.states->changed(*table.stateKey, document, parts);
        }
    };
    // What a stored state or a saved view is fitted to: the node's column parts, of the kinds their cells infer.
    config.description = [&table]() -> std::optional<table::TableDescription> {
        DocNode const& declared = *table.node;
        table::TableDescription description;
        description.id = declared.id;
        std::span<table::ColumnInfo const> const infos = table.source->columns();
        auto const parts = declared.partList("columns");
        for (std::size_t at = 0; at < parts.size() && at < infos.size(); ++at) {
            table::TableColumn column;
            // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access): `at` runs below `parts.size()`, and `infos` was built in step with `parts`
            column.info = infos[at];
            column.label = parts[at]->id;
            column.sortable = parts[at]->flag("sortable");
            // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            column.filterable = true;
            description.columns.push_back(std::move(column));
        }
        description.groups = table.groups;
        return description;
    };
    // The columns a state or a view reads are put in use beside those in use now; the gestures that apply it put
    // exactly the columns they read in use after.
    config.prepare = [&table](table::StateDocument const& document) {
        std::vector<std::string> next = table.used;
        for (std::string& columnId :
             usedColumns(table, document.sort.value_or(SortChain{}), document.filters.value_or(table::FilterSpec{}))) {
            if (!std::ranges::contains(next, columnId)) {
                next.push_back(std::move(columnId));
            }
        }
        useColumns(table, std::move(next));
    };
    // A bound layout is controlled, as a bound sort is: any expression but a literal `null` binds it.
    if (ExprPtr const layout = node.prop("layout");
        layout && (layout->kind != ExprKind::Literal || !layout->literal.isNull())) {
        config.bound =
            &scope.nodes.computed([&scope, bindings, layout] { return evalIn(scope, bindings.get(), layout); });
    }
    // Shows the page of @p view: each group's heading entry before the first of its rows the page holds, and the
    // engine's own operations when the page is the whole ungrouped view and they made it, a reset otherwise.
    auto const show = [&table](TableView const& view, table::ViewChange const* change) {
        std::vector<PageRows::Entry> entries;
        entries.reserve(view.last - view.first);
        table::Engine const& engine = table.engine->engine();
        for (std::size_t index = view.first; index < view.last; ++index) {
            if (!view.runOf.empty() && (index == view.first || view.runOf.at(index) != view.runOf.at(index - 1))) {
                Value const& heading = view.runs.at(view.runOf.at(index));
                Value const* const key = heading.field(kGroupHeadingField);
                entries.push_back(PageRows::Entry{
                    .key =
                        table::RowId{key != nullptr && key->asString() != nullptr ? *key->asString() : std::string{}},
                    .row = heading});
            }
            entries.push_back(PageRows::Entry{.key = engine.rowIdAt(index), .row = view.rows.at(index)});
        }
        bool const whole = view.runOf.empty() && view.first == 0 && view.last == view.rows.size();
        table::ViewChange reset;
        reset.ops.push_back(table::ViewOp{.kind = table::ViewOp::Kind::Reset, .first = 0, .count = 0, .to = 0});
        bool const passed = whole && change != nullptr && !change->ops.empty();
        table.shown =
            RowsTable::Shown{.version = table.version.peek(), .first = view.first, .last = view.last, .any = true};
        table.rows->show(std::move(entries), passed ? *change : reset);
    };
    table.showPage = show;
    table.heard = [this, &table](table::ViewChange const& change) {
        table.bump();
        if (table.engine->restoring()) {
            // The page stays empty while the rows are held back; the mount draws the engine's rows as placeholders.
            // NOLINTNEXTLINE(readability-ambiguous-smartptr-reset-call): `reset()` here is the TableRows interface's own, which empties the page; it is not the smart pointer's
            table.rows->reset();
        } else {
            // Shown now, from the view the change made, so the change reaches the mount as it is.
            runtime().untracked([&] { table.showPage(table.view->get(), &change); });
        }
        rowsTableChanged(table);
    };
    startRowsEngine(table, runtime());
    if (persisted) {
        std::string qualifier = _doc->id;
        bool persists = true;
        if (ExprPtr const persist = node.prop("persist"); persist && persist->kind == ExprKind::Literal) {
            if (bool const* const flag = persist->literal.asBool()) {
                persists = *flag;
            } else if (std::string const* const name = persist->literal.asString()) {
                qualifier = *name;
            }
        }
        table.states = session.tableStates();
        // A table with no `id` has no key to keep its arrangement under; the loader refuses a `persist` it writes.
        if (persists && table.states != nullptr && !node.id.empty()) {
            table.stateKey = table::TableStateKey{.application = session.options().application,
                                                  .model = {},
                                                  .table = node.id,
                                                  .qualifier = std::move(qualifier),
                                                  .part = "state"};
        }
    }
    table.view =
        std::make_unique<reactive::Computed<TableView>>(runtime(), [this, &table] { return tableView(table); });
    // The page the mount shows again when the page moves, or the view changed under it other than through the
    // engine (the rows held back, `rows` with no change of the engine's view); a page the engine's change showed is
    // not shown twice.
    table.effects.push_back(std::make_unique<reactive::Effect>(runtime(), [this, &table] {
        TableView const& shown = table.view->get();
        runtime().untracked([&] {
            if (table.shown.any && table.shown.version == table.version.peek() && table.shown.first == shown.first &&
                table.shown.last == shown.last) {
                return;
            }
            table.showPage(shown, nullptr);
        });
    }));
    if (table.stateKey) {
        table.waiting = true;
        table.listed = true;
        modelTables.push_back(&element);
        table.engine->setRestoring(true);
    }
    // The rows, matched by key: a changed row is repaired, not sorted again. A list over the client's bound is
    // said once, for as long as the table lives.
    std::size_t const parts = node.partList("columns").size();
    table.effects.push_back(std::make_unique<reactive::Effect>(
        runtime(), [this, &table, &scope, bindings, rowsExpr = node.prop("rows"), parts] {
            Value const value = evalIn(scope, bindings.get(), rowsExpr);
            runtime().untracked([&] {
                ValueList rows;
                if (ValueList const* const items = value.asList()) {
                    rows = *items;
                }
                // A quantity in a unit the screen's table does not list is its own canonical unit: the source and
                // the engine are made again over a registry that has it.
                bool brought = false;
                for (Value const& row : rows) {
                    ValueRecord const* const fields = row.asRecord();
                    if (fields == nullptr) {
                        continue;
                    }
                    for (auto const& [name, field] : *fields) {
                        static_cast<void>(name);
                        if (auto const measure = detail::measureOf(field);
                            measure && !table.units.contains(*measure->unit) &&
                            table.unitsSeen.insert(*measure->unit).second) {
                            brought = true;
                        }
                    }
                }
                if (brought) {
                    startRowsEngine(table, runtime());
                }
                if (!table.boundReported && (rows.size() > kClientRows || rows.size() * parts > kClientCells)) {
                    table.boundReported = true;
                    std::string const& columnId = table.node->id;
                    report(table.node->path, "table " + (columnId.empty() ? table.node->path : columnId) + " holds " +
                                                 groupedCount(rows.size()) +
                                                 " rows; a table over rows is ordered on the client up to " +
                                                 groupedCount(kClientRows) + " rows or " + groupedCount(kClientCells) +
                                                 " cells — serve it as a model's table");
                }
                table.source->assign(std::move(rows));
            });
        }));
    // The sort the table shows: the bound one, or the table's own, which a header click and a stored state set.
    table.effects.push_back(std::make_unique<reactive::Effect>(runtime(), [this, &table, &element] {
        Value const sort = tableSort(element);
        runtime().untracked([&] { sortRowsTable(table, sort); });
    }));
    // The source reads `by` untracked; what `by` reads besides the row regroups the rows when it changes.
    if (delta) {
        table.effects.push_back(std::make_unique<reactive::Effect>(
            runtime(), [this, &table, &scope, bindings, byOf, rowsExpr = node.prop("rows"),
                        first = std::make_shared<bool>(true)] {
                Value const list = runtime().untracked([&] { return evalIn(scope, bindings.get(), rowsExpr); });
                if (ValueList const* const items = list.asList()) {
                    for (Value const& row : *items) {
                        static_cast<void>(byOf(row));
                    }
                }
                if (std::exchange(*first, false)) {
                    return;
                }
                runtime().untracked([&] { table.source->setGroup(byOf, scope.formatting().units); });
            }));
    }
    if (!table.stateKey) {
        return made;
    }
    // The rows wait for the stored state, at most `tableStateTimeout`; a state that comes later is applied unless
    // the user changed the table meanwhile.
    if (reactive::Scheduler* const scheduler = session.options().scheduler) {
        table.loadTimer = scheduler->after(session.options().tableStateTimeout,
                                           table.scope.token().guard([&table] { table.release(); }));
    }
    table.views = std::make_unique<TableViews>(*table.states, *table.stateKey, runtime().owner());
    table.config.views = table.views.get();
    loadStateAndViews(*table.states, *table.stateKey, *table.views, table.scope,
                      [this, &table](std::optional<std::string> const& json, std::string const& failure) {
                          storedRowsState(table, json, failure);
                      });
    return made;
}

void ScreenCore::sortRowsTable(RowsTable& table, Value const& sort) {
    DocNode const& node = *table.node;
    std::optional<std::vector<SortKey>> const keys = sortKeysOf(node, sort);
    SortChain chain;
    if (!keys) {
        report(
            pointerAppend(node.path, "sort"),
            "sort is " + canonicalJson(sort) +
                R"(; a table's sort is a list of {"column": <a sortable column's id>, "direction": "asc" or "desc"})");
    } else {
        for (SortKey const& key : *keys) {
            chain.push_back(table::SortKey{
                .column = key.column->id,
                .dir = key.descending ? table::SortDirection::Descending : table::SortDirection::Ascending});
        }
    }
    // The columns the new keys read are in use before the old ones leave, so a column both read keeps its cells.
    useColumns(table, usedColumns(table, chain, table.filter));
    SortChain ordered;
    if (table.grouped) {
        ordered.push_back(
            table::SortKey{.column = std::string{kGroupRunColumn}, .dir = table::SortDirection::Ascending});
    }
    ordered.insert(ordered.end(), chain.begin(), chain.end());
    if (auto const set = table.engine->setSort(std::move(ordered)); !set) {
        report(pointerAppend(node.path, "sort"), "the sort is refused: " + set.error().message);
        return;
    }
    table.sort = std::move(chain);
    rowsTableChanged(table);
}

void ScreenCore::rowsTableChanged(RowsTable& table) {
    if (!table.invalidReported) {
        for (table::SortKey const& key : table.sort) {
            if (table.source->invalidCells(key.column) == 0) {
                continue;
            }
            table.invalidReported = true;
            report(pointerAppend(table.node->path, "sort"),
                   "a value of the column `" + key.column +
                       "` is not of the column's kind; it sorts last, in either direction");
            break;
        }
    }
    if (table.releasing && !table.engine->engine().pending()) {
        table.releasing = false;
        table.release();
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): applies a stored state: layout, sort, filters and groups each fit to the description and report their own refusal, in that order
void ScreenCore::storedRowsState(RowsTable& table, std::optional<std::string> const& json,
                                 std::string const& failure) {
    DocNode const& node = *table.node;
    ElementState& element = *table.element;
    bool const late = !table.waiting;
    if (!failure.empty()) {
        report(node.path, "the stored state of table `" + node.id + "` could not be loaded: " + failure);
        table.release();
        return;
    }
    std::optional<table::StateDocument> stored;
    if (json) {
        auto read = table::readState(*json);
        if (!read) {
            report(node.path, "the stored state of table `" + node.id + "` is not used: " + read.error());
            table.release();
            return;
        }
        stored = std::move(*read);
    }
    // The default view stands in for a state never stored, and its sort and filters for the stored ones when the
    // user starts from it.
    std::optional<table::StateDocument> start =
        table.views != nullptr ? stateAtMount(std::move(stored), table.views->own()) : std::move(stored);
    if (!start) {
        table.release();
        return;
    }
    if (late && table.touched) {
        return;
    }
    // Fitted to what the node declares: its column parts, of the kinds their cells infer once they are in use.
    table.config.prepare(*start);
    table::FitReport fitted;
    table::StateDocument const document =
        table::fitState(std::move(*start), table.config.description().value_or(table::TableDescription{}), fitted);
    // A table started from the default view, with nothing stored, holds it as its state: the next change saves on
    // top of it rather than over nothing.
    if (!json && table.states != nullptr && table.stateKey) {
        table.states->start(*table.stateKey, document);
    }
    if (element.sortBound == nullptr && document.sort) {
        element.sort.set(sortValueOf(*document.sort));
    }
    // The filters go to the engine in a task of their own: it takes in the kinds of the columns just put in use when
    // it flushes the change that put them there, and compiles a filter against the kinds it has taken in.
    if (document.filters && !document.filters->empty()) {
        runtime().owner().post(table.scope.token().guard([&table, filters = *document.filters] {
            useColumns(table, usedColumns(table, table.sort, filters));
            if (table.engine->setFilter(filters)) {
                table.filter = filters;
                table.bump();
            } else {
                useColumns(table, usedColumns(table, table.sort, table.filter));
            }
        }));
    }
    // A bound layout is the document's: the stored one is neither applied nor saved over it.
    if (table.config.bound == nullptr && document.layout) {
        table.config.held = *document.layout;
        table.config.version.set(table.config.version.peek() + 1);
    }
    if (document.view) {
        table.config.savedView = document.view;
        table.config.viewShared = false;
    }
    tablePoints(element);
    if (!late) {
        // The rows come through once the engine's view is in the stored order: after the work the new sort posted.
        bool const timed = session.options().scheduler != nullptr;
        runtime().owner().post(table.scope.token().guard([&table, timed] {
            if (!table.waiting) {
                return;
            }
            if (table.engine->engine().pending() && timed) {
                table.releasing = true;
                return;
            }
            table.release();
        }));
    }
}

namespace {

/// @brief The text a collection addresses a row by: an integer key's digits, a text key as it is.
/// @param key The row's key, as the engine keys it.
/// @return The text.
[[nodiscard]] std::string collectionKeyText(table::RowId const& key) {
    if (std::int64_t const* const whole = std::get_if<std::int64_t>(&key)) {
        return std::to_string(*whole);
    }
    return std::get<std::string>(key);
}

/// @brief The member of a collection row's record that holds its key text; a view document's field never begins
///        with U+001F.
inline constexpr std::string_view kCollectionKeyField = "\x1Fkey";

/// @brief Where a collection's rows are, by key, for the records its engine builds: made again from the session's
///        rows whenever the source's current snapshot changes. That snapshot is held, so no later one can take its
///        address while the index is kept for it.
struct CollectionRecords {
    std::shared_ptr<table::RowSnapshot const> snapshot;
    std::unordered_map<table::RowId, std::size_t> at;
};

}  // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity): wires a collection's table to its model: rows, state, runs and actions in the order each needs the one before
void ScreenCore::wireCollectionTable(ElementState& element, std::string modelType) {
    forms::CollectionSession& collection = *element.collection;
    element.rowsTable = std::make_unique<RowsTable>(runtime());
    RowsTable& table = *element.rowsTable;
    table.core = this;
    table.element = &element;
    table.node = element.node;
    table.where = &*element.scope;
    // Every column of the view document is laid out, those it hides hidden, and every one sorts and filters.
    TableConfig& config = table.config;
    config.element = &element;
    for (forms::CollectionColumn const& column : collection.model().columns()) {
        config.columns.push_back(
            LayoutColumn{.id = column.field, .label = column.label, .locked = false, .kind = column.kind});
        config.sortable.push_back(true);
        if (column.hidden) {
            config.declaredHidden.push_back(column.field);
        }
    }
    config.maxSortKeys = [] { return std::size_t{8}; };
    config.filters = [&table] {
        static_cast<void>(table.version.get());
        return table.filter;
    };
    config.setFilters = [this, &table](table::FilterSpec const& filter) {
        if (auto const set = table.engine->setFilter(filter); !set) {
            rowsTableMessage(table, "The filter is refused: " + set.error().message);
            return false;
        }
        table.filter = filter;
        table.bump();
        return true;
    };
    config.rows = [&table]() -> TableRows* { return table.engine.get(); };
    // The session's source holds every cell of every column: a values list puts none in use.
    config.listing = [](std::vector<std::string> const& columns) { static_cast<void>(columns); };
    config.message = [this, &table](std::string text) { rowsTableMessage(table, std::move(text)); };
    config.touch = [this, &table] {
        table.touched = true;
        rowsTableMessage(table, {});
    };
    config.save = [&table](table::StateDocument const& document, StateParts parts) {
        if (table.stateKey) {
            table.states->changed(*table.stateKey, document, parts);
        }
    };
    // What a stored state or a saved view is fitted to: the view document's columns, each of which sorts and filters.
    config.description = [&collection]() -> std::optional<table::TableDescription> {
        table::TableDescription description;
        description.id = std::string{collection.model().query()};
        for (table::ColumnInfo const& info : collection.source()->columns()) {
            table::TableColumn column;
            column.info = info;
            column.label = info.id;
            column.sortable = true;
            column.filterable = true;
            description.columns.push_back(std::move(column));
        }
        return description;
    };
    // A row's record is its key text and every column's cell as the session formats it, read from the row of that
    // key: the engine may read a snapshot older than the session's rows, and a row it no longer has reads empty.
    auto const records = std::make_shared<CollectionRecords>();
    RecordOf recordOf = [reactiveRuntime = &runtime(), session = &collection, records](
                            table::RowSnapshot const& snapshot, std::size_t sourceRow) {
        table::RowId key = snapshot.rowId(sourceRow);
        ValueRecord record;
        record.insert_or_assign(std::string{kCollectionKeyField}, Value{collectionKeyText(key)});
        reactiveRuntime->untracked([&] {
            std::vector<forms::CollectionRow> const& rows = session->rows();
            // The index is of the session's rows, whichever snapshot the engine reads: it is made again when the
            // source's current snapshot changes, which the session's rows changing makes it do.
            if (std::shared_ptr<table::RowSnapshot const> current = session->source()->snapshot();
                records->snapshot != current) {
                records->snapshot = std::move(current);
                records->at.clear();
                for (std::size_t at = 0; at < rows.size(); ++at) {
                    records->at.emplace(rows.at(at).id, at);
                }
            }
            auto const found = records->at.find(key);
            if (found == records->at.end() || found->second >= rows.size() || rows.at(found->second).id != key) {
                return;
            }
            auto const dom = forms::detail::engine::parseJson(rows.at(found->second).json);
            if (!dom) {
                return;
            }
            for (forms::CollectionColumn const& column : session->model().columns()) {
                record.insert_or_assign(column.field, Value{forms::detail::engine::cellText(*dom, column)});
            }
        });
        return RowRecord{.key = std::move(key), .row = Value{std::move(record)}};
    };
    // Text orders as it does in every other table of the screen.
    table::EngineOptions options;
    options.owner = &runtime().owner();
    options.worker = nullptr;
    options.frameBudget = std::chrono::milliseconds{4};
    options.services.collator = std::make_shared<ValueRowsCollator const>();
    table.engine = std::make_shared<EngineRows>(collection.source(), std::move(recordOf), std::move(options));
    element.tableRows = std::make_shared<HeardRows>(table.engine, [&table] {
        table.bump();
        if (table.releasing && !table.engine->engine().pending()) {
            table.releasing = false;
            table.release();
        }
    });
    // The sort the table shows: its own, which a header click and a stored state set.
    table.effects.push_back(std::make_unique<reactive::Effect>(runtime(), [this, &table, &element] {
        Value const sort = tableSort(element);
        runtime().untracked([&] {
            SortChain chain;
            for (table::SortKey& key : sortChainOf(sort)) {
                if (std::ranges::contains(table.config.columns, key.column, &LayoutColumn::id)) {
                    chain.push_back(std::move(key));
                }
            }
            if (table.engine->setSort(chain)) {
                table.sort = std::move(chain);
            }
        });
    }));
    // Kept under the collection's action, on this screen, for the model it lists.
    table.states = session.tableStates();
    if (table.states == nullptr) {
        return;
    }
    table.stateKey = table::TableStateKey{.application = session.options().application,
                                          .model = std::move(modelType),
                                          .table = std::string{collection.model().query()},
                                          .qualifier = _doc->id,
                                          .part = "state"};
    table.waiting = true;
    table.listed = true;
    modelTables.push_back(&element);
    table.engine->setRestoring(true);
    // The rows wait for the stored state, at most `tableStateTimeout`; a state that comes later is applied unless
    // the user changed the table meanwhile.
    if (reactive::Scheduler* const scheduler = session.options().scheduler) {
        table.loadTimer = scheduler->after(session.options().tableStateTimeout,
                                           table.scope.token().guard([&table] { table.release(); }));
    }
    table.views = std::make_unique<TableViews>(*table.states, *table.stateKey, runtime().owner());
    table.config.views = table.views.get();
    loadStateAndViews(*table.states, *table.stateKey, *table.views, table.scope,
                      [this, &table](std::optional<std::string> const& json, std::string const& failure) {
                          storedCollectionState(table, json, failure);
                      });
}

void ScreenCore::storedCollectionState(RowsTable& table, std::optional<std::string> const& json,
                                       std::string const& failure) {
    DocNode const& node = *table.node;
    ElementState& element = *table.element;
    std::string const action{node.name("action")};
    bool const late = !table.waiting;
    if (!failure.empty()) {
        report(node.path, "the stored state of collection `" + action + "` could not be loaded: " + failure);
        table.release();
        return;
    }
    std::optional<table::StateDocument> stored;
    if (json) {
        auto read = table::readState(*json);
        if (!read) {
            report(node.path, "the stored state of collection `" + action + "` is not used: " + read.error());
            table.release();
            return;
        }
        stored = std::move(*read);
    }
    // The default view stands in for a state never stored, and its sort and filters for the stored ones when the
    // user starts from it.
    std::optional<table::StateDocument> start =
        table.views != nullptr ? stateAtMount(std::move(stored), table.views->own()) : std::move(stored);
    if (!start) {
        table.release();
        return;
    }
    if (late && table.touched) {
        return;
    }
    // Fitted to the view document's columns, each of which sorts and filters.
    table::FitReport fitted;
    table::StateDocument const document =
        table::fitState(std::move(*start), table.config.description().value_or(table::TableDescription{}), fitted);
    // A table started from the default view, with nothing stored, holds it as its state: the next change saves on
    // top of it rather than over nothing.
    if (!json && table.states != nullptr && table.stateKey) {
        table.states->start(*table.stateKey, document);
    }
    if (element.sortBound == nullptr && document.sort) {
        element.sort.set(sortValueOf(*document.sort));
    }
    if (document.filters && !document.filters->empty() && table.engine->setFilter(*document.filters)) {
        table.filter = *document.filters;
        table.bump();
    }
    if (document.layout) {
        table.config.held = *document.layout;
        table.config.version.set(table.config.version.peek() + 1);
    }
    if (document.view) {
        table.config.savedView = document.view;
        table.config.viewShared = false;
    }
    if (late) {
        return;
    }
    // The rows come through once the engine's view is in the stored order: after the work the new sort posted.
    bool const timed = session.options().scheduler != nullptr;
    runtime().owner().post(table.scope.token().guard([&table, timed] {
        if (!table.waiting) {
            return;
        }
        if (table.engine->engine().pending() && timed) {
            table.releasing = true;
            return;
        }
        table.release();
    }));
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): lowers a collection's rows table: columns, engine, selection, filters and options in document order
Node ScreenCore::lowerCollectionRows(ElementState& element, LowerCtx const& ctx,
                                     std::function<Node(std::string const&)> const& buttons) {
    RowsTable& table = *element.rowsTable;
    forms::CollectionSession& collection = *element.collection;
    TableConfig& config = table.config;
    // Every column of the layout, then the row's buttons, which are no column of it.
    std::vector<std::string> fields;
    std::vector<TableColumn> columns;
    for (LayoutColumn const& column : config.columns) {
        fields.push_back(column.id);
        columns.push_back(TableColumn{.id = column.id, .label = column.label, .sortable = true});
    }
    std::size_t const laidOut = columns.size();
    // The buttons' column is sized to its widest cell like any content column: a button's frame and padding are
    // pixels, not character cells, so no count of units can say how wide a proportional font draws them.
    if (buttons) {
        columns.push_back(TableColumn{.label = std::string{}, .width = Sizing::content()});
    }
    config.beginMount();
    config.filterTarget = [&table, &collection](std::string const& columnId) -> std::optional<FilterTarget> {
        std::vector<LayoutColumn> const& arranged = table.config.columns;
        auto const found = std::ranges::find(arranged, columnId, &LayoutColumn::id);
        std::span<table::ColumnInfo const> const infos = collection.source()->columns();
        auto const info = std::ranges::find(infos, columnId, &table::ColumnInfo::id);
        if (found == arranged.end() || info == infos.end()) {
            return std::nullopt;
        }
        FilterTarget target;
        target.column = columnId;
        target.label = found->label;
        target.shown = table::TableColumn{.info = *info, .display = table::ColumnDisplay{}};
        target.listed = true;
        return target;
    };
    // A copy reads the engine's view, which holds every row, each cell as the session formats it.
    config.gather = [&table](std::vector<Key> keys, std::vector<std::string> const& ids, CopyDone const& done) {
        CopyGathered gathered;
        std::vector<std::string> read;
        for (std::string const& columnId : ids) {
            auto const found = std::ranges::find(table.config.columns, columnId, &LayoutColumn::id);
            if (found != table.config.columns.end()) {
                gathered.headers.push_back(found->label);
                read.push_back(columnId);
            }
        }
        std::unordered_set<Key> wanted{keys.begin(), keys.end()};
        EngineRows const& rows = *table.engine;
        std::int64_t const total = rows.total().rows;
        for (std::int64_t at = 0; at < total && !wanted.empty(); ++at) {
            std::optional<TableRowRef> const ref = rows.row(at);
            if (!ref || wanted.erase(ref->key) == 0) {
                continue;
            }
            CopyRow copied{.viewRow = at, .cells = {}};
            for (std::string const& columnId : read) {
                Value const* const text = ref->row.field(columnId);
                copied.cells.emplace_back(text != nullptr && text->asString() != nullptr ? *text->asString()
                                                                                         : std::string{});
            }
            gathered.rows.push_back(std::move(copied));
        }
        done(std::move(gathered));
    };
    // Each cell shows one row, so its test id names that row's key; the trailing cell holds the row's buttons.
    TableSource source;
    source.rows = element.tableRows;
    source.alwaysWindowed = false;
    source.rowScopedCells.assign(columns.size(), true);
    source.cellView = [this, buttons, fields](std::size_t column, reactive::Signal<Value> const& row) -> Node {
        Value const* const keyed = row.peek().field(kCollectionKeyField);
        std::string const key = keyed != nullptr && keyed->asString() != nullptr ? *keyed->asString() : std::string{};
        if (column >= fields.size()) {
            return buttons ? buttons(key) : ui::text(Text{});
        }
        Text cell{.text = slotProp<std::string>([&row, field = fields.at(column)] {
                      Value const* const text = row.get().field(field);
                      return text != nullptr && text->asString() != nullptr ? *text->asString() : std::string{};
                  })};
        cell.common.testId = "cell_" + fields.at(column) + "_" + key;
        return ui::text(std::move(cell));
    };
    // The engine keys a row by its integer key's value, the session by the key's text: the selection and the
    // activation the session's options make are translated between the two.
    TableOptions options = forms::detail::engine::rowsTableOptions(collection);
    forms::CollectionSession const* const view = &collection;
    if (options.onActivate) {
        options.onActivate = [inner = std::move(options.onActivate)](Key const& key) {
            inner(Key{collectionKeyText(key)});
        };
    }
    if (options.onSelectionChange) {
        options.onSelectionChange = [inner = std::move(options.onSelectionChange)](std::vector<Key> const& keys,
                                                                                   SelectionChange change) {
            std::vector<Key> texts;
            texts.reserve(keys.size());
            for (Key const& key : keys) {
                texts.emplace_back(collectionKeyText(key));
            }
            inner(texts, change);
        };
    }
    if (options.selection.isBound()) {
        options.selection = slotProp<std::vector<Key>>([read = options.selection.binding(), view] {
            std::vector<Key> keys;
            std::vector<forms::CollectionRow> const& rows = view->rows();
            for (Key const& key : read()) {
                std::string const* const text = std::get_if<std::string>(&key);
                auto const found =
                    text == nullptr ? rows.end() : std::ranges::find(rows, *text, &forms::CollectionRow::key);
                if (found != rows.end()) {
                    keys.push_back(found->id);
                }
            }
            return keys;
        });
    }
    // The status line takes space while it has something to say: a refusal, or what a copy did.
    options.status = slotProp<TableStatus>([&table, view, scope = ctx.scope] {
        static_cast<void>(table.version.get());
        TableStatus status;
        bool const selected =
            view->model().kind() == forms::CollectionKind::MasterDetail && view->openRow().has_value();
        status.counts = countsText(table.engine->counts(), selected ? 1 : 0, scope->formatting());
        status.message = table.status == false ? std::string{} : table.message.get();
        status.shown = table.status.value_or(!status.state.empty() || !status.message.empty());
        return status;
    });
    Node dialogs = configureTable(config, ctx, options, nullptr);
    markFilterable(config, columns, options);
    // An arrangement that lists the columns it shows lists the buttons last.
    if (buttons) {
        // Held by pointer: the slot's closure is moved into the property, and moving a property that holds a
        // `std::function` makes GCC 15 at -O3 report that function as uninitialized.
        auto const inner = std::make_shared<Prop<TableArrangement> const>(std::move(options.arrangement));
        options.arrangement = slotProp<TableArrangement>([inner, laidOut] {
            TableArrangement arranged = inner->evaluate();
            if (!arranged.visible.empty()) {
                arranged.visible.push_back(laidOut);
            }
            return arranged;
        });
    }
    return besideDialogs(options.common, std::move(dialogs),
                         [&] { return ui::table(std::move(columns), std::move(source), std::move(options)); });
}

}  // namespace morph::ui::detail
