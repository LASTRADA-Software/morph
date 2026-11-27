// SPDX-License-Identifier: Apache-2.0

// What the interpreter's table lowering shares: the table structs and the helpers over them.

#pragma once

#include <morph/table/data_source.hpp>
#include <morph/table/description.hpp>
#include <morph/table/edits.hpp>
#include <morph/table/filter.hpp>
#include <morph/table/selection.hpp>
#include <morph/table/state_store.hpp>
#include <morph/table/view_change.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/table_config.hpp>
#include <morph/ui/table_dialogs.hpp>
#include <morph/ui/table_filter_menu.hpp>
#include <morph/ui/table_rows.hpp>
#include <morph/ui/table_state.hpp>
#include <morph/ui/table_views.hpp>

#include "interpreter_detail.hpp"

namespace morph::ui {

class SummaryRows;
struct SummarySpec;

namespace detail {

/// The rows a model-backed table's mount reads: its `WindowRows`, with what the table hears of them. A change of the
/// rows, or of their counts and error, bumps the table's version, which its status line, its `empty` content and its
/// element values read. A `visible` call other than a new mount's first is the user's scroll. The fields the table's
/// row bindings read and the rows lacked are named to every `visible` call after the mount's own columns.
class ObservedRows final : public TableRows {
public:
    ObservedRows(std::shared_ptr<WindowRows> inner, std::function<void()> changed, std::function<void()> scrolled,
                 std::function<void()> reset, std::function<void()> filtered)
        : _inner{std::move(inner)},
          _changed{std::move(changed)},
          _scrolled{std::move(scrolled)},
          _reset{std::move(reset)},
          _filtered{std::move(filtered)} {
        _inner->onChange([this](table::ViewChange const& change) {
            if (_handler) {
                _handler(change);
            }
            _changed();
            // A new query moves every row: what is found by key is found again.
            if (change.isReset() && _reset) {
                _reset();
            }
        });
        _inner->onStatus([this] { _changed(); });
    }

    ObservedRows(ObservedRows const&) = delete;
    ObservedRows& operator=(ObservedRows const&) = delete;
    ObservedRows(ObservedRows&&) = delete;
    ObservedRows& operator=(ObservedRows&&) = delete;
    ~ObservedRows() override {
        _inner->onChange({});
        _inner->onStatus({});
    }

    [[nodiscard]] TableTotal total() const override { return _inner->total(); }
    [[nodiscard]] std::optional<TableRowRef> row(std::int64_t index) const override { return _inner->row(index); }
    [[nodiscard]] table::UnloadedLevels unloadedLevels(std::int64_t index) const override {
        return _inner->unloadedLevels(index);
    }
    void visible(std::int64_t first, std::int64_t count, std::span<std::string const> columns) override {
        _first = first;
        _count = count;
        _columns.assign(columns.begin(), columns.end());
        if (!std::exchange(_mounting, false)) {
            _scrolled();
        }
        send();
    }
    async::Completion<std::expected<std::vector<table::RowId>, table::TableError>> keysOf(
        std::int64_t first, std::int64_t count) override {
        return _inner->keysOf(first, count);
    }
    void locateRow(table::RowId key, ::core::async::StopToken stop,
                   std::function<void(RowLocation const&)> done) override {
        _inner->locateRow(std::move(key), std::move(stop), std::move(done));
    }
    async::Completion<std::expected<std::vector<FetchedRow>, table::TableError>> fetchRows(
        std::vector<table::RowId> keys, std::vector<std::string> columns) override {
        return _inner->fetchRows(std::move(keys), std::move(columns));
    }
    std::expected<void, table::TableError> setSort(SortChain sort) override {
        return _inner->setSort(std::move(sort));
    }
    /// A filter set here is the user's, from the table's own gestures.
    std::expected<void, table::TableError> setFilter(table::FilterSpec filter) override {
        auto set = _inner->setFilter(std::move(filter));
        if (set) {
            _filtered();
        }
        return set;
    }
    async::Completion<std::expected<table::DistinctValues, table::TableError>> distinctValues(
        table::DistinctRequest request) override {
        return _inner->distinctValues(std::move(request));
    }
    [[nodiscard]] TableCountsView counts() const override { return _inner->counts(); }
    [[nodiscard]] std::expected<ViewSelection, SelectAllRefusal> selectView() const override {
        return _inner->selectView();
    }
    [[nodiscard]] SelectedCount selectedCount(TableSelection const& selection) const override {
        return _inner->selectedCount(selection);
    }
    [[nodiscard]] bool leftView(table::RowId const& key) const override { return _inner->leftView(key); }
    [[nodiscard]] RetainResult retain(std::span<table::RowId const> keys) override { return _inner->retain(keys); }
    void clearRetained() override { _inner->clearRetained(); }
    [[nodiscard]] bool unmatched(table::RowId const& key) const override { return _inner->unmatched(key); }
    void holdRefetches(std::uint64_t run) override { _inner->holdRefetches(run); }
    void releaseRefetches(std::uint64_t run) override { _inner->releaseRefetches(run); }
    [[nodiscard]] RowStates& states() override { return _inner->states(); }
    [[nodiscard]] RowStates const& states() const override { return _inner->states(); }
    void moveKey(table::RowId const& from, table::RowId const& target) override { _inner->moveKey(from, target); }
    [[nodiscard]] std::vector<std::pair<std::int64_t, std::int64_t>> heldRanges() const override {
        return _inner->heldRanges();
    }
    async::Completion<std::expected<std::vector<FetchedRow>, table::TableError>> rowsOf(
        std::int64_t first, std::int64_t count, std::span<std::string const> columns) override {
        return _inner->rowsOf(first, count, columns);
    }
    void onChange(std::function<void(table::ViewChange const&)> handler) override { _handler = std::move(handler); }

    /// The next `visible` call is a new mount's first, not a scroll.
    void mounting() { _mounting = true; }

    /// The view rows the mount last named in or near view: the rows a backend holds.
    [[nodiscard]] std::pair<std::int64_t, std::int64_t> shownRange() const { return {_first, _count}; }

    /// Sets what widens the columns the mount names into those fetched: each with its companions.
    void companions(std::function<std::vector<std::string>(std::span<std::string const>)> widen) {
        _companions = std::move(widen);
    }

    /// Names @p id to every later `visible` call, and fetches it now for the rows in view; an id named already is
    /// not added again.
    void fetchAlso(std::span<std::string const> ids) {
        bool added = false;
        for (std::string const& columnId : ids) {
            if (!std::ranges::contains(_extra, columnId)) {
                _extra.push_back(columnId);
                added = true;
            }
        }
        if (added) {
            send();
        }
    }

private:
    void send() {
        std::vector<std::string> named = _companions ? _companions(_columns) : _columns;
        for (std::string const& columnId : _extra) {
            if (!std::ranges::contains(named, columnId)) {
                named.push_back(columnId);
            }
        }
        _inner->visible(_first, _count, named);
    }

    std::shared_ptr<WindowRows> _inner;
    std::function<void()> _changed;
    std::function<void()> _scrolled;
    std::function<void()> _reset;
    std::function<void()> _filtered;
    std::function<void(table::ViewChange const&)> _handler;
    std::int64_t _first = 0;
    std::int64_t _count = 0;
    std::vector<std::string> _columns;
    std::vector<std::string> _extra;
    std::function<std::vector<std::string>(std::span<std::string const>)> _companions;
    bool _mounting = false;
};

/// What a table keeps of its configuring across every mount of it: the columns its layout arranges, in the order of
/// `Table::columns`, what the user may change of them, the layout it holds, and what a change does beyond that.
///
/// The slots a mount sets (`filterTarget`, `dimensions`, `gather`, `heldCell`) belong to that mount: `beginMount`
/// starts one, and the token `mountSlots` gives clears them when the mount's handlers go, unless a later mount has set
/// them since. A slot may then hold the mount's scope: the element keeps the configuring, so a slot that outlived its
/// mount would keep the scope that keeps the element.
struct TableConfig {
    // Defined out of line, in one translation unit: inlined into every table's constructor, `FilterMenu`'s constructor
    // makes GCC at -O3 report its temporary state's strings as maybe-uninitialized.
    explicit TableConfig(reactive::Runtime& runtime);

    /// The table's element, whose sort, status line and commands the gestures reach.
    // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes): TableConfig is the table's configuring state: the mount, the menus and the document's wiring read and assign these slots directly, each slot set by whichever mount owns it
    ElementState* element = nullptr;
    /// The columns, each with its id, label, lock and kind; those declared hidden; and which of them sort.
    std::vector<LayoutColumn> columns;
    std::vector<std::string> declaredHidden;
    std::vector<bool> sortable;
    /// What `configurable` lets the user change, and whether the table has menus (`configurable: false` has none).
    Configure allowed = Configure::All;
    bool menus = true;
    /// The table's own layout, as the user or the stored state left it; none: as declared.
    std::optional<table::ColumnLayoutValue> held;
    /// The document's `layout`; null while the table holds its own.
    reactive::Computed<Value>* bound = nullptr;
    /// Bumped on every change of the columns or of the held layout.
    reactive::Signal<std::uint64_t> version;
    /// The bound on the sort chain, read at each gesture: a model's table has it from the description it holds.
    std::function<std::size_t()> maxSortKeys;
    /// Shows a message in the table's status line, or clears it with an empty one.
    std::function<void(std::string)> message;
    /// Marks the table as changed by its user, so a stored state that arrives later is not applied over it, and
    /// clears its status line's message.
    std::function<void()> touch;
    /// Saves parts of the arrangement; empty for a table that persists nothing.
    std::function<void(table::StateDocument const&, StateParts)> save;
    /// Whether the column chooser and the sort dialog are open, and the group "Group columns…" is forming: kept
    /// here, so a new mount of the table (a model's new revision) keeps them.
    reactive::Signal<bool> chooserOpen;
    reactive::Signal<bool> sortOpen;
    reactive::Signal<GroupDraft> groupDraft;
    /// The filters the table holds, tracked; empty for a table that filters nothing.
    std::function<table::FilterSpec()> filters;
    /// Gives the rows the user's filters and holds them; returns whether the rows took them, a refusal said in the
    /// status line. Not called while the document binds the filters.
    std::function<bool(table::FilterSpec const&)> setFilters;
    /// Whether the document binds the filters: a user's change runs `onFiltersChange` and is neither held nor saved.
    bool filtersBound = false;
    /// The rows a filter menu's values are asked of.
    std::function<TableRows*()> rows;
    /// A column as a filter menu edits it, by id: its kind, its display and its units; none for a column that is not
    /// filterable. Set by each mount, with what it shows.
    std::function<std::optional<FilterTarget>(std::string const&)> filterTarget;
    /// The groups the table declares: a `rows` table's `groups`, a model's table's description's.
    std::function<std::vector<table::ColumnGroupInfo>()> declaredGroups;
    /// A `Measure` column's dimensions and the units each can show, by id; none for another column.
    std::function<std::vector<MenuDimension>(std::string const&)> dimensions;
    /// The columns an open values list reads, which a table over `rows` keeps in use; empty when it closes.
    std::function<void(std::vector<std::string> const&)> listing;
    /// The filter menu, kept here so a new mount of the table keeps it open.
    std::unique_ptr<FilterMenu> filterMenu;
    /// The cell keyboard focus is on, as the mount last told it; none before any.
    std::optional<CellRef> focused;
    /// Gathers the rows of these keys, by the column ids given, each cell as the table shows it; a key the rows do
    /// not have is left out. Set by each mount, with what it shows.
    std::function<void(std::vector<Key>, std::vector<std::string> const&, CopyDone)> gather;
    /// A held row's cell, by the row's key and the column's id; none while the rows do not hold the row, and empty for
    /// a table whose cells have no menu. Set by each mount, with what it shows.
    std::function<std::optional<HeldCell>(Key const&, std::string const&)> heldCell;
    /// The latest copy: a copy whose rows arrive after a later one started writes nothing.
    std::uint64_t copies = 0;
    /// The rows a selection of the view holds, while the selection is one; none while it is keys. Set by each mount
    /// of a model's table.
    std::function<std::optional<std::int64_t>()> viewSelected;
    /// Gathers the rows of the selection of the view, in view order, by the column ids given. Set by each mount of a
    /// model's table.
    std::function<void(std::vector<std::string> const&, CopyDone)> gatherView;
    /// Selects these keys as a user's selection does: held by the element, or given to `onSelectionChange` where the
    /// document binds the selection. Set by each mount of a model's table.
    std::function<void(std::vector<Key> const&)> selectKeys;
    /// The saved view the arrangement was taken from, the state's `view`, and whether it is a shared one applied from
    /// "Shared views"; a stored `view` names one of the user's own.
    std::optional<std::string> savedView;
    bool viewShared = false;
    /// The table's saved views, which the table's element holds beside this; null for a table that keeps no state.
    TableViews* views = nullptr;
    /// The description a saved view is fitted to, as a stored state is; none while the table has none.
    std::function<std::optional<table::TableDescription>()> description;
    /// Readies the columns a state or a view sorts and filters by before it is fitted: a table over `rows` knows a
    /// column's kind only while it is in use, so a filter on another would be fitted to the wrong kind and dropped.
    /// Empty for a table whose description holds its kinds.
    std::function<void(table::StateDocument const&)> prepare;
    /// The view name dialog, kept here so a new mount of the table keeps it open.
    reactive::Signal<ViewDialog> viewDialog;
    /// What a views change answers into: nothing once the configuring is gone.
    async::CallbackScope viewsScope;
    // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)

    /// Starts a mount: the slots it sets are its own until a later mount starts.
    void beginMount() { ++_mount; }

    /// A token for the mount begun last: when the last copy of it goes, the slots that mount set are cleared, unless a
    /// later mount has begun or the configuring is gone.
    /// @return The token, for the mount's handlers to hold.
    [[nodiscard]] std::shared_ptr<void const> mountSlots() {
        struct Slots {
            TableConfig* config;
            std::weak_ptr<void const> alive;
            std::uint64_t mount;
            Slots(TableConfig* owner, std::weak_ptr<void const> token, std::uint64_t serial)
                : config{owner}, alive{std::move(token)}, mount{serial} {}
            Slots(Slots const&) = delete;
            Slots& operator=(Slots const&) = delete;
            Slots(Slots&&) = delete;
            Slots& operator=(Slots&&) = delete;
            ~Slots() {
                if (alive.lock() != nullptr && config->_mount == mount) {
                    config->filterTarget = {};
                    config->dimensions = {};
                    config->gather = {};
                    config->heldCell = {};
                    config->viewSelected = {};
                    config->gatherView = {};
                    config->selectKeys = {};
                }
            }
        };
        return std::make_shared<Slots const>(this, _alive, _mount);
    }

private:
    std::uint64_t _mount = 0;
    std::shared_ptr<void const> _alive = std::make_shared<int const>(0);
};

/// What a table's literal `configurable` lets its user change, and whether it has menus: `true` and an absent key
/// allow everything, `false` nothing and offers no menu, a list what it names. The loader refused any other value.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): reads the `configurable` literal: true, false or a list of names, each name adding a bit to the allowed set
[[nodiscard]] std::pair<Configure, bool> configurableOf(DocNode const& node);

/// The layout of a table's columns: as declared, fitted to @p value when one is given.
[[nodiscard]] table::ColumnLayout layoutOver(std::span<LayoutColumn const> columns,
                                             std::vector<std::string> const& declaredHidden,
                                             std::optional<table::ColumnLayoutValue> const& value);

/// A layout as the `layout` element value and `onLayoutChange`'s event hold it: `order`, `hidden`, `pinned` and
/// `widths`, and `groups` and `units` while they hold anything.
[[nodiscard]] Value layoutValueOf(table::ColumnLayoutValue const& layout);

/// Filters as the `filters` element value and `onFiltersChange`'s event hold them: the filter's JSON, each group
/// filter naming its declared group or its columns, never an empty one of the two.
[[nodiscard]] Value filtersValueOf(table::FilterSpec const& filters);

/// The layout a table holds now: the document's, when it binds one, else the table's own. Tracked.
[[nodiscard]] std::optional<table::ColumnLayoutValue> layoutNow(TableConfig const& config);

/// Draws a filter button on each column a filter menu edits.
/// @param config  The table's configuring, whose `filterTarget` says which columns filter.
/// @param columns The mount's columns, in the order of `config.columns`.
/// @param options The table's options; without `onFilter` no column draws a button.
void markFilterable(TableConfig const& config, std::vector<TableColumn>& columns, TableOptions const& options);

/// What a model-backed table keeps in its element, across every mount of it: its rows and the query of its
/// description, the arrangement it gave the rows, its persisted state's key and load, and its status line's message.
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding): the fields are grouped and commented by role, and the callback scope is declared last so it is destroyed first; a size-ordered layout would break both
struct ModelTable {
    explicit ModelTable(reactive::Runtime& runtime)
        : version{runtime, 0},
          resets{runtime, 0},
          revision{runtime, std::string{}},
          highlight{runtime, std::nullopt},
          message{runtime, std::string{}},
          entries{runtime, std::vector<StatusEntry>{}},
          viewSelection{runtime, std::nullopt},
          marksRevision{runtime, 0},
          keyMoves{runtime, KeyMoves{}},
          config{runtime} {}

    ModelTable(ModelTable const&) = delete;
    ModelTable& operator=(ModelTable const&) = delete;
    ModelTable(ModelTable&&) = delete;
    ModelTable& operator=(ModelTable&&) = delete;

    /// A change not yet saved is saved at once: the table is unmounting.
    // NOLINTNEXTLINE(bugprone-exception-escape): the table's teardown has no recovery if unlinking the runs observer or flushing the state throws, so terminating is the outcome
    ~ModelTable() {
        if (core != nullptr) {
            std::erase(core->modelTables, element);
            if (runs != nullptr) {
                core->session.removeRunObserver(runs.get());
            }
        }
        if (states != nullptr && stateKey) {
            states->flush(*stateKey);
        }
    }

    /// Lets the rows send their first query, with the sort and filters set meanwhile; once.
    void release() {
        if (std::exchange(waiting, false)) {
            loadTimer.cancel();
            window->release();
        }
    }

    /// Tells every reader of the rows' counts and error that they may have changed.
    void bump() { version.set(version.peek() + 1); }

    /// The screen, which lists the table while it lives, and the table's element.
    ScreenCore* core = nullptr;
    ElementState* element = nullptr;
    /// The table's id among its model's tables.
    std::string id;
    /// The model's type.
    std::string modelType;
    /// The description's key column.
    std::string keyColumn;
    /// The query of `Table.<id>.describe`, and its declaration.
    QueryDecl describeDecl;
    std::unique_ptr<QueryRuntime> describe;
    std::shared_ptr<WindowRows> window;
    std::shared_ptr<ObservedRows> rows;
    /// Bumped on every change of the rows, their counts or their error.
    reactive::Signal<std::uint64_t> version;
    /// Bumped on every reset of the rows (a new query): a row found by key is located again.
    reactive::Signal<std::uint64_t> resets;
    /// The held description's revision; empty while none is held.
    reactive::Signal<std::string> revision;
    /// The highlighted row: the key `highlightKey` names and the view row a keyed fetch found it at.
    reactive::Signal<std::optional<TableHighlight>> highlight;
    /// The status line's message, the choices it offers, and what each choice runs, by its id.
    reactive::Signal<std::string> message;
    reactive::Signal<std::vector<StatusEntry>> entries;
    std::map<std::string, std::function<void()>, std::less<>> entryHandlers;
    reactive::TimerHandle messageTimer;
    /// The selection while it is of the view (`view` engaged): the element's `selection` is then empty.
    reactive::Signal<std::optional<TableSelection>> viewSelection;
    /// Read first by every row's marks: the rows' retained and unmatched state is not reactive, so this is bumped
    /// after every change of it, and a row that becomes unmatched is drawn again.
    reactive::Signal<std::uint64_t> marksRevision;
    /// The rows an action of this session gave new keys, for the mount's focus and highlight to follow.
    reactive::Signal<KeyMoves> keyMoves;
    /// The node's `retainChanged`: the rows an action of this session changed stay in view.
    bool retainChanged = false;
    /// Hears the session's runs while the table lives, to offer a refused selection of the view again.
    std::unique_ptr<RunObserver> runs;
    /// Whether the status line takes space; `status: false` sends messages to the host instead.
    bool statusShown = true;
    /// The persisted state's key, and the states it is loaded from and saved to; none without persistence.
    std::optional<table::TableStateKey> stateKey;
    TableStates* states = nullptr;
    /// Whether the rows wait for the stored state before their first query.
    bool waiting = false;
    reactive::TimerHandle loadTimer;
    /// Whether the user scrolled, selected or changed the arrangement: a stored state that arrives later is not
    /// applied over it.
    bool touched = false;
    /// The sort and filters the rows were given, so a new description's fitting can name what it drops.
    SortChain sort;
    table::FilterSpec filter;
    /// The column layout, which the stored state, a `columns` point and the user's gestures set, and the saved
    /// view's name, as the stored state, a `saved` point or the "Views" menu gave it.
    TableConfig config;
    /// Whether the document binds the filters: they are neither restored nor saved.
    bool filtersBound = false;
    /// A filter set while the kinds of a typed list action's columns wait for its first reply, applied once they
    /// are fixed; and whether it is the stored one, fitted and reported as stored state is.
    std::optional<table::FilterSpec> heldFilter;
    bool heldStored = false;
    /// Whether the user set the filter through the table's own gestures: a held literal or stored filter does not
    /// replace it.
    bool userFiltered = false;
    /// The described columns' ids.
    std::set<std::string, std::less<>> described;
    /// The fields the row bindings missed this turn; one task fetches them all.
    std::set<std::string, std::less<>> missing;
    std::function<void(std::string_view)> onMissing;
    /// The row scopes `onActivate` runs in, by the row's canonical key.
    std::map<std::string, std::weak_ptr<RtScope>, std::less<>> rowScopes;
    /// The table's saved views; none without persistence.
    std::unique_ptr<TableViews> views;
    /// The calls of the table's model alias: what the table's own edits are sent through, and a choice's options
    /// fetched with.
    forms::Submitter calls;
    /// The drafts, errors, marks and commits of the table's editable cells, held by row key for as long as the table:
    /// a new description's mount finds them where they were.
    std::unique_ptr<table::CellEdits> edits;
    /// The mounted table's cells as a grid, while an editable table is mounted.
    std::weak_ptr<ModelGridSource> editGrid;
    /// Last, so it is the first to go: no load, timer or post reaches a half-destroyed table.
    async::CallbackScope scope;
};

/// The rows a table over `rows` shows its mount: the page of its engine's view, each group's heading entry before
/// the first of its rows the page holds, so a group split across pages gets its heading on both. The table sets the
/// entries after each change of the view or the page; while the engine's rows are held back, so are these.
class PageRows final : public TableRows {
public:
    /// One row the page shows: a data row, or a group's heading whose record is its run's.
    struct Entry {
        table::RowId key;
        Value row;
    };

    PageRows(std::shared_ptr<EngineRows> engine, exec::IExecutor& owner)
        : _engine{std::move(engine)}, _owner{&owner} {}

    PageRows(PageRows const&) = delete;
    PageRows& operator=(PageRows const&) = delete;
    PageRows(PageRows&&) = delete;
    PageRows& operator=(PageRows&&) = delete;
    ~PageRows() override = default;

    /// Reads a new engine's rows from now on: the table made one for units its rows brought.
    void setEngine(std::shared_ptr<EngineRows> engine) { _engine = std::move(engine); }

    /// Takes the page's new entries and tells the mount how the view changed.
    void show(std::vector<Entry> entries, table::ViewChange const& change) {
        _entries = std::move(entries);
        if (_handler) {
            _handler(change);
        }
    }

    /// Tells the mount the whole view changed, the entries kept: the engine's rows changed under a page that did
    /// not, such as while they are held back.
    void reset() {
        if (_handler) {
            table::ViewChange change;
            change.ops.push_back(table::ViewOp{.kind = table::ViewOp::Kind::Reset, .first = 0, .count = 0, .to = 0});
            _handler(change);
        }
    }

    [[nodiscard]] TableTotal total() const override {
        if (_engine->restoring()) {
            return _engine->total();
        }
        return TableTotal{
            .rows = static_cast<std::int64_t>(_entries.size()), .kind = table::TotalKind::Exact, .known = true};
    }
    [[nodiscard]] std::optional<TableRowRef> row(std::int64_t index) const override {
        if (_engine->restoring() || index < 0 || static_cast<std::size_t>(index) >= _entries.size()) {
            return std::nullopt;
        }
        Entry const& entry = _entries.at(static_cast<std::size_t>(index));
        return TableRowRef{.key = entry.key, .row = entry.row};
    }
    [[nodiscard]] table::UnloadedLevels unloadedLevels(std::int64_t index) const override {
        static_cast<void>(index);
        return {};
    }
    void visible(std::int64_t first, std::int64_t count, std::span<std::string const> columns) override {
        static_cast<void>(first);
        static_cast<void>(count);
        static_cast<void>(columns);
    }
    /// The keys of a range of the page's entries, headings included, at once.
    async::Completion<std::expected<std::vector<table::RowId>, table::TableError>> keysOf(
        std::int64_t first, std::int64_t count) override {
        using Result = std::expected<std::vector<table::RowId>, table::TableError>;
        auto [completion, promise] = async::Completion<Result>::makeSettleable(_owner);
        auto const size = static_cast<std::int64_t>(_entries.size());
        if (first < 0 || count < 0 || count > size - first) {
            promise.resolve(Result{std::unexpected(table::TableError{
                .code = table::TableErrorCode::InvalidSpec,
                .column = {},
                .message = std::format("{} rows from row {} are not in a page of {}", count, first, size)})});
            return std::move(completion);
        }
        std::vector<table::RowId> keys;
        keys.reserve(static_cast<std::size_t>(count));
        for (std::int64_t at = first; at < first + count; ++at) {
            keys.push_back(_entries.at(static_cast<std::size_t>(at)).key);
        }
        promise.resolve(Result{std::move(keys)});
        return std::move(completion);
    }
    /// Where the key is among the page's entries, answered on the owner: a row of the engine's view the page does
    /// not hold is not in the view the mount sees.
    void locateRow(table::RowId key, ::core::async::StopToken stop,
                   std::function<void(RowLocation const&)> done) override {
        auto const hit = std::ranges::find(_entries, key, &Entry::key);
        // Assigned, not cast: the iterator difference is `std::ptrdiff_t`, which is `std::int64_t` only on some
        // targets.
        std::int64_t const index = hit - _entries.begin();
        RowLocation const found{hit == _entries.end() ? std::nullopt : std::optional{index}};
        _owner->post([stop = std::move(stop), done = std::move(done), found] {
            if (!stop.stop_requested()) {
                done(found);
            }
        });
    }
    /// The engine's rows: a key's view index is its place in the engine's view, not among the page's entries.
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
    /// The engine's counts: the view's rows, not the page's entries, and nothing known while held back.
    [[nodiscard]] TableCountsView counts() const override { return _engine->counts(); }
    void onChange(std::function<void(table::ViewChange const&)> handler) override { _handler = std::move(handler); }
    [[nodiscard]] RowStates& states() override { return _engine->states(); }
    [[nodiscard]] RowStates const& states() const override { return _engine->states(); }
    void moveKey(table::RowId const& from, table::RowId const& target) override { _engine->moveKey(from, target); }

private:
    std::shared_ptr<EngineRows> _engine;
    exec::IExecutor* _owner;
    std::vector<Entry> _entries;
    std::function<void(table::ViewChange const&)> _handler;
};

/// What a table over `rows` keeps in its element, across every mount of it: its rows on the engine and the page of
/// them it shows, the arrangement it gave them, its persisted state's key and load, and its status line's message.
/// A table lowered where names its element cannot see are bound has one of its own, kept by its mount, which
/// persists nothing. A collection's table is one too, without a source or a page: its engine is over the session's
/// source, and its mounts read the engine's rows through the element's `tableRows`.
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding): the fields are grouped and commented by role, and the callback scope is declared last so it is destroyed first; a size-ordered layout would break both
struct RowsTable {
    explicit RowsTable(reactive::Runtime& runtime)
        : version{runtime, 0}, message{runtime, std::string{}}, config{runtime} {}

    RowsTable(RowsTable const&) = delete;
    RowsTable& operator=(RowsTable const&) = delete;
    RowsTable(RowsTable&&) = delete;
    RowsTable& operator=(RowsTable&&) = delete;

    /// Its computations stop first; then the columns' tracked `sortBy`s and the `group`'s `by`, which read the
    /// table's scope, leave the source, which the mount may keep longer; a change not yet saved is saved at once.
    // NOLINTNEXTLINE(bugprone-exception-escape): the table's teardown has no recovery if unlinking the engine, the source or the state store throws, so terminating is the outcome
    ~RowsTable() {
        effects.clear();
        if (engine != nullptr) {
            engine->onChange({});
        }
        if (source != nullptr) {
            for (std::string const& column : used) {
                source->unuse(column);
            }
            if (grouped) {
                source->setGroup({}, {});
            }
        }
        if (core != nullptr && listed) {
            std::erase(core->modelTables, element);
        }
        if (states != nullptr && stateKey) {
            states->flush(*stateKey);
        }
    }

    /// Tells every reader of the view, its counts and its page that they may have changed.
    void bump() { version.set(version.peek() + 1); }

    /// Lets the rows through, once: the stored arrangement is in place, or it will not come in time.
    void release() {
        loadTimer.cancel();
        waiting = false;
        engine->setRestoring(false);
    }

    /// The screen, and the table's element and node.
    ScreenCore* core = nullptr;
    ElementState* element = nullptr;
    DocNode const* node = nullptr;
    RtScope* where = nullptr;
    /// The names bound where the table is lowered; null for the element's own.
    std::shared_ptr<Bindings const> bindings;
    /// Whether the screen lists the table among those that keep a state.
    bool listed = false;
    /// Whether the table is its element's own, whose `tableRows` reads its engine.
    bool elementOwn = false;
    std::shared_ptr<ValueRowsSource> source;
    std::shared_ptr<EngineRows> engine;
    std::shared_ptr<PageRows> rows;
    /// The summary rows over the page's entries, which the mount is handed in their place; null with no summaries.
    std::shared_ptr<SummaryRows> summaries;
    /// Each run of the page by ordinal: its heading's key, by which its rows are found in the view.
    std::vector<std::string> runHeadings;
    /// Bumped on every change of the engine's view.
    reactive::Signal<std::uint64_t> version;
    /// The rows in view order and the page shown.
    std::unique_ptr<reactive::Computed<TableView>> view;
    /// The status line's message, and the literal `status`: shown always, never, or while it has something to say.
    reactive::Signal<std::string> message;
    reactive::TimerHandle messageTimer;
    std::optional<bool> status;
    /// The column layout and what its user may change.
    TableConfig config;
    /// The persisted state's key, and the states it is loaded from and saved to; none without persistence.
    std::optional<table::TableStateKey> stateKey;
    TableStates* states = nullptr;
    /// Whether the rows are held back for the stored state; whether they are let through once the engine settles.
    bool waiting = false;
    bool releasing = false;
    reactive::TimerHandle loadTimer;
    /// Whether the user changed the arrangement or the selection: a stored state that arrives later is not applied.
    bool touched = false;
    /// The sort the engine was given, without the group's key; the filters it was given; the columns an open values
    /// list reads; and the columns in use for all three.
    SortChain sort;
    table::FilterSpec filter;
    std::vector<std::string> listedColumns;
    std::vector<std::string> used;
    bool grouped = false;
    /// The column groups the node declares (`groups`), which the source declares to the engine.
    std::vector<table::ColumnGroupInfo> groups;
    /// What has been reported once: the list over the client's bound, and an invalid cell of a sort column.
    bool boundReported = false;
    bool invalidReported = false;
    /// The page last shown: the `version` of the view it was cut from, and its place in it.
    struct Shown {
        std::uint64_t version = 0;
        std::size_t first = 0;
        std::size_t last = 0;
        bool any = false;
    } shown;
    /// What a new source and engine are made from: the columns, a row's key, the `group`'s `by`, the screen's unit
    /// table and the units the rows brought that it does not list.
    std::vector<ValueColumn> columns;
    std::function<Key(Value const&)> keyOfRow;
    std::function<Value(Value const&)> byOf;
    UnitTable units;
    std::set<std::string, std::less<>> unitsSeen;
    /// What the table does on each change of its engine's view.
    std::function<void(table::ViewChange const&)> heard;
    /// Shows a page of the view to the mount, passing on the engine's change when it may.
    std::function<void(TableView const&, table::ViewChange const*)> showPage;
    std::vector<std::unique_ptr<reactive::Effect>> effects;
    /// The table's saved views; none without persistence.
    std::unique_ptr<TableViews> views;
    /// Last, so it is the first to go: no load, timer or post reaches a half-destroyed table.
    async::CallbackScope scope;
};

/// A table's `bands` parts as bands: each part's label, unit, literal list of column ids and bands one level down. The
/// loader refused any other shape.
// NOLINTNEXTLINE(misc-no-recursion): a band's bands are bands one level down.
[[nodiscard]] std::vector<table::BandInfo> bandsOf(std::span<DocNodePtr const> parts);

/// A table's or a group's `summaries` parts as the decorator takes them: each key, and whether it is drawn on top.
[[nodiscard]] std::vector<SummarySpec> summarySpecsOf(std::span<DocNodePtr const> parts);

/// @brief One key of a table's sort: the column part that orders the rows, and whether it is descending.
struct SortKey {
    /// @brief The sortable column.
    DocNode const* column = nullptr;
    /// @brief Whether the key sorts descending.
    bool descending = false;
};

/// @brief A table's sort read as its keys: a list of `{"column", "direction"}` records naming sortable columns.
/// @param table The table.
/// @param sort  The sort; `null` is the source order, as `[]` is.
/// @return The keys, most significant first; `nullopt` when @p sort is not such a list.
[[nodiscard]] std::optional<std::vector<SortKey>> sortKeysOf(DocNode const& table, Value const& sort);

/// @brief A table's selection as an action takes it: the selection of the view while a model's table holds one,
///        else the selected keys. Tracked.
/// @param element The table's element.
/// @return The selection.
[[nodiscard]] TableSelection selectionNow(ElementState const& element);

/// A column's display with a column part's display parts over it, member by member.
/// @param part    The column part.
/// @param display The display the part overrides.
/// @param read    Reads one of the part's expressions once.
[[nodiscard]] table::ColumnDisplay displayOf(DocNode const& part, table::ColumnDisplay display,
                                             std::function<Value(ExprPtr const&)> const& read);

/// A record's field as a cell of its column: what `formatCell` reads. The record holds a `Date` as `YYYY-MM-DD`, a
/// `DateTime` as a timestamp and a measure as a `Quantity` named by its unit's id, and holds `null` for an empty
/// cell and for an unloaded one alike: the placeholder of an unloaded cell is the mount's (`setCellLoad`).
/// @param value  The field; null when the record lacks it.
/// @param column The column.
/// @param units  The registry a measure's unit is found in; null for none.
[[nodiscard]] table::Cell cellOfRecord(Value const* value, table::TableColumn const& column,
                                       table::UnitRegistry const* units);

/// What a rows table's column shows for a value, by the kind the value has: a rows table declares no kinds.
[[nodiscard]] table::TableColumn columnFor(Value const& value, table::ColumnDisplay const& display);

/// @brief A sort as a table's element value holds it, a list of `{column, direction}`, as a sort chain; an entry
///        that names no column is left out.
/// @param value The list.
/// @return The chain.
[[nodiscard]] SortChain sortChainOf(Value const& value);

/// @brief A sort chain as a table's element value holds it.
/// @param chain The chain.
/// @return The list of `{column, direction}`.
[[nodiscard]] Value sortValueOf(SortChain const& chain);

/// @brief The column kind a column part's literal `kind` names.
/// @param name The name; empty for none.
/// @return The kind; nothing for no name, which infers the kind from the cells.
[[nodiscard]] std::optional<table::ColumnKind> columnKindNamed(std::string_view name);

/// @brief Gives a table over `rows` a new source and engine over the units known now. The columns in use, the
///        groups, the sort and whether the rows are held back carry over from the ones they replace; the caller
///        assigns the rows.
/// @param table   The table.
/// @param runtime The screen's runtime.
void startRowsEngine(RowsTable& table, reactive::Runtime& runtime);

/// @brief The columns a table over `rows` keeps in use: those a sort, filters and an open values list read, each once.
/// @param table  The table, whose declared groups and open list are read.
/// @param sort   The sort.
/// @param filter The filters.
/// @return The columns' ids.
[[nodiscard]] std::vector<std::string> usedColumns(RowsTable const& table, SortChain const& sort,
                                                   table::FilterSpec const& filter);

/// @brief Puts columns in use and takes the others out: the new ones first, so a column both read keeps its cells.
/// @param table The table.
/// @param next  The columns in use from now.
void useColumns(RowsTable& table, std::vector<std::string> next);

/// @brief Loads a table's state and its saved views in the same turn, so a store that batches reads them in one call,
///        and calls @p done once both have answered: the default view decides what a table with no stored state
///        starts from. Views that cannot be read are left unsaid.
/// @param states The session's table states.
/// @param key    The table's state key.
/// @param views  The table's saved views.
/// @param scope  The table's scope: nothing is called once it is gone.
/// @param done   Called with the stored state's text, none when nothing is stored, and the reason its load failed,
///               empty when it did not.
void loadStateAndViews(TableStates& states, table::TableStateKey const& key, TableViews& views,
                       async::CallbackScope const& scope,
                       std::function<void(std::optional<std::string> const&, std::string const&)> done);

// The rows a grid with summaries or a group is mounted over: the decorator's, a data row's cell as the grid draws it,
// a summary row's cell its part's expression for the column read with `rows` the records of the data rows in its
// scope and `column` the column's id, shown at the column's places, and a heading row its group's heading. Every
// column's cells are built per row, so a cell built for a data row is never rebound to a summary row.

std::vector<std::pair<std::int64_t, std::int64_t>> rowsRuns(RowsTable& table);

ValueList rowsSummaryScope(RowsTable const& table, Value const& record);

}  // namespace detail

}  // namespace morph::ui
