// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <expected>
#include <format>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/completion.hpp>
#include <morph/core/wire.hpp>
#include <morph/forms/engine/collection.hpp>
#include <morph/forms/engine/collection_view.hpp>
#include <morph/forms/engine/field_model.hpp>
#include <morph/forms/engine/form_session.hpp>
#include <morph/forms/engine/form_view.hpp>
#include <morph/forms/engine/wizard.hpp>
#include <morph/forms/engine/wizard_view.hpp>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/scheduler.hpp>
#include <morph/reactive/scope.hpp>
#include <morph/reactive/signal.hpp>
#include <morph/core/callback_scope.hpp>
#include <morph/table/data_source.hpp>
#include <morph/table/description.hpp>
#include <morph/table/filter.hpp>
#include <morph/table/selection.hpp>
#include <morph/table/state_store.hpp>
#include <morph/table/view_change.hpp>
#include <morph/ui/backend.hpp>
#include <morph/ui/cell_format.hpp>
#include <morph/ui/grid.hpp>
#include <morph/ui/chart_scale.hpp>
#include <morph/ui/dev_tools.hpp>
#include <morph/ui/document.hpp>
#include <morph/ui/expression.hpp>
#include <morph/ui/interpreter.hpp>
#include <morph/ui/locale_table.hpp>
#include <morph/ui/message.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/table_config.hpp>
#include <morph/ui/table_dialogs.hpp>
#include <morph/ui/table_filter_menu.hpp>
#include <morph/ui/table_rows.hpp>
#include <morph/ui/table_views.hpp>
#include <morph/ui/table_state.hpp>
#include <morph/ui/value.hpp>
#include <morph/ui/view.hpp>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace morph::ui {

static_assert(kMaxInlineFileBytes == (wire::kMaxEnvelopeBytes - (std::size_t{64} * 1024)) * 3 / 4,
              "the inline cap must stay tied to the wire envelope");

class BridgeDispatcher::Channel final : public ModelChannel {
public:
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the type id, then the key, as `bind` takes them.
    Channel(bridge::Bridge& bridge, exec::IExecutor* cbExec, Evaluator evaluator, std::string const& typeId,
            bridge::BindSharing sharing, std::string const& instanceKey)
        : _cbExec{cbExec},
          _evaluator{std::move(evaluator)},
          _typeId{typeId},
          _handler{bridge, cbExec, typeId, sharing, instanceKey} {}

    [[nodiscard]] async::Completion<std::string> execute(std::string const& actionId, std::string const& bodyJson,
                                                         CallOptions options) override;

    [[nodiscard]] ChannelFacts facts() const override;

private:
    exec::IExecutor* _cbExec;
    Evaluator _evaluator;
    std::string _typeId;
    bridge::RawHandler _handler;
};

namespace detail {

/// @brief The watches a session made without entries, shared with each registration so that a holder that outlives
///        its session finds nothing to release.
struct WatchRegistry {
    /// @brief One watch's record and handler.
    struct Watch {
        std::string record;
        std::function<void(effects::RecordChange const&)> handler;
    };
    /// @brief A `watchRecord` whose watch is not answered yet.
    struct Waiter {
        std::string record;
        async::Completion<WatchHolder>::Promise promise;
        WatchHolder holder;
    };

    /// @brief The session; null once it is being destroyed.
    Session* session = nullptr;
    /// @brief The last watch id given.
    std::uint64_t next = 0;
    /// @brief The watches, by id.
    std::map<std::uint64_t, Watch> watches;
    /// @brief The records whose `watch` is sent and not answered.
    std::set<std::string, std::less<>> inFlight;
    /// @brief The `watchRecord` calls waiting for an answer.
    std::vector<Waiter> waiters;

    /// @brief A `watch` naming @p records was answered: each waiting `watchRecord` for one of them resolves.
    /// @param records The records.
    void answered(std::vector<std::string> const& records) {
        for (std::string const& record : records) {
            inFlight.erase(record);
        }
        // Moved out first: resolving may release a registration, which reaches back into this registry.
        std::vector<Waiter> ready;
        std::erase_if(waiters, [&](Waiter& waiter) {
            if (std::ranges::find(records, waiter.record) == records.end()) {
                return false;
            }
            ready.push_back(std::move(waiter));
            return true;
        });
        for (Waiter& waiter : ready) {
            waiter.promise.resolve(waiter.holder);
        }
    }
};

/// @brief One watch of `Session::watchRecord`, released when the last `WatchHolder` copy dies.
struct WatchRegistration {
    WatchRegistration() = default;
    WatchRegistration(WatchRegistration const&) = delete;
    WatchRegistration& operator=(WatchRegistration const&) = delete;
    WatchRegistration(WatchRegistration&&) = delete;
    WatchRegistration& operator=(WatchRegistration&&) = delete;

    ~WatchRegistration() {
        auto const held = registry.lock();
        if (!held) {
            return;
        }
        held->watches.erase(id);
        if (held->session == nullptr) {
            return;
        }
        try {
            held->session->countRecord(record, -1);
            held->session->flushWatches();
        } catch (...) {  // NOLINT(bugprone-empty-catch): a destructor has no one to report a failed unwatch to.
        }
    }

    /// @brief The session's registry.
    std::weak_ptr<WatchRegistry> registry;
    /// @brief The watch's id there.
    std::uint64_t id = 0;
    /// @brief The record.
    std::string record;
};

}  // namespace detail

namespace detail {

namespace {

// The shortest text that reads back as @p number, in fixed notation: `0.0001`, never `1e-04`, so that every value
// a decimal can hold is read as one. The buffer holds the longest fixed form of a double (about 330 characters).
Value decimalOrText(double number) {
    std::array<char, 512> buffer{};
    auto const [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), number,
                                            std::chars_format::fixed);
    if (error == std::errc{}) {
        if (auto const decimal = ::morph::table::parseDecimal(std::string_view{buffer.data(), end})) {
            return Value{Decimal{*decimal}};
        }
    }
    return Value{std::format("{}", number)};
}

}  // namespace

std::expected<Value, std::string> tableValueFromJson(Json const& json) {
    return structuralFromJson(json, &decimalOrText);
}

}  // namespace detail

std::shared_ptr<TableStates> detail::makeTableStates(Session& session, reactive::Runtime& runtime,
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

Session::~Session() {
    if (_watchRegistry) {
        _watchRegistry->session = nullptr;
        _watchRegistry->waiters.clear();
    }
}

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
    async::Completion<std::expected<std::vector<table::RowId>, table::TableError>> keysOf(std::int64_t first,
                                                                                       std::int64_t count) override {
        return _inner->keysOf(first, count);
    }
    void locateRow(table::RowId key, std::stop_token stop, std::function<void(RowLocation const&)> done) override {
        _inner->locateRow(std::move(key), std::move(stop), std::move(done));
    }
    async::Completion<std::expected<std::vector<FetchedRow>, table::TableError>> fetchRows(
        std::vector<table::RowId> keys, std::vector<std::string> columns) override {
        return _inner->fetchRows(std::move(keys), std::move(columns));
    }
    std::expected<void, table::TableError> setSort(SortChain sort) override { return _inner->setSort(std::move(sort)); }
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
    void moveKey(table::RowId const& from, table::RowId const& to) override { _inner->moveKey(from, to); }
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
        for (std::string const& id : ids) {
            if (!std::ranges::contains(_extra, id)) {
                _extra.push_back(id);
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
        for (std::string const& id : _extra) {
            if (!std::ranges::contains(named, id)) {
                named.push_back(id);
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

/// One row a copy writes: its view index, and its cells' texts by the columns copied, none for an unloaded cell.
struct CopyRow {
    std::int64_t viewRow = 0;
    std::vector<std::optional<std::string>> cells;
};

/// What a copy writes: each column's header text, and the rows found, in any order.
struct CopyGathered {
    std::vector<std::string> headers;
    std::vector<CopyRow> rows;
};

/// Called once with what a copy gathered, or with the message that says why it could not.
using CopyDone = std::function<void(std::expected<CopyGathered, std::string>)>;

/// What the view name dialog names a view for.
enum class ViewNaming : std::uint8_t {
    Save,     ///< "Save view…": a name the user already has replaces that view.
    Rename,   ///< "Rename view…": the view the arrangement was taken from.
    Publish,  ///< "Publish view…": a name the shared views already have replaces that one.
};

/// The view name dialog: whether it is open, what for, and the name typed.
struct ViewDialog {
    bool open = false;
    ViewNaming purpose = ViewNaming::Save;
    std::string name;

    bool operator==(ViewDialog const&) const = default;
};

/// A cell of a row the table holds, as a cell menu reads it.
struct HeldCell {
    /// The row's record.
    Value record;
    /// The cell, as its column reads it.
    table::Cell cell;
    /// The column, as it shows its cells.
    table::TableColumn shown;
    /// What a `Measure` cell is shown through.
    DisplayUnits units;
    /// What `units` points into.
    std::shared_ptr<void const> keep;
};

/// What a table keeps of its configuring across every mount of it: the columns its layout arranges, in the order of
/// `Table::columns`, what the user may change of them, the layout it holds, and what a change does beyond that.
///
/// The slots a mount sets (`filterTarget`, `dimensions`, `gather`, `heldCell`) belong to that mount: `beginMount` starts one,
/// and the token `mountSlots` gives clears them when the mount's handlers go, unless a later mount has set them
/// since. A slot may then hold the mount's scope: the element keeps the configuring, so a slot that outlived its
/// mount would keep the scope that keeps the element.
struct TableConfig {
    explicit TableConfig(reactive::Runtime& runtime)
        : version{runtime, 0},
          chooserOpen{runtime, false},
          sortOpen{runtime, false},
          groupDraft{runtime, GroupDraft{}},
          filterMenu{std::make_unique<FilterMenu>(runtime)},
          viewDialog{runtime, ViewDialog{}} {}

    /// The table's element, whose sort, status line and commands the gestures reach.
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
            Slots(TableConfig* config, std::weak_ptr<void const> alive, std::uint64_t mount)
                : config{config}, alive{std::move(alive)}, mount{mount} {}
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

namespace {

/// What a table's literal `configurable` lets its user change, and whether it has menus: `true` and an absent key
/// allow everything, `false` nothing and offers no menu, a list what it names. The loader refused any other value.
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
            allowed = allowed | (named == "reorder"  ? Configure::Reorder
                                 : named == "hide"   ? Configure::Hide
                                 : named == "pin"    ? Configure::Pin
                                 : named == "resize" ? Configure::Resize
                                                     : Configure::None);
        }
    }
    return {allowed, true};
}

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

/// The layout of a table's columns: as declared, fitted to @p value when one is given.
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

/// A layout as the `layout` element value and `onLayoutChange`'s event hold it: `order`, `hidden`, `pinned` and
/// `widths`, and `groups` and `units` while they hold anything.
[[nodiscard]] Value layoutValueOf(table::ColumnLayoutValue const& layout) {
    auto const ids = [](std::span<std::string const> list) {
        ValueList out;
        for (std::string const& id : list) {
            out.emplace_back(id);
        }
        return Value{std::move(out)};
    };
    ValueRecord out;
    out.insert_or_assign("order", ids(layout.order));
    out.insert_or_assign("hidden", ids(layout.hidden));
    out.insert_or_assign("pinned", Value{static_cast<std::int64_t>(layout.pinned)});
    ValueRecord widths;
    for (auto const& [id, width] : layout.widths) {
        widths.insert_or_assign(id, Value{static_cast<std::int64_t>(width)});
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

/// Filters as the `filters` element value and `onFiltersChange`'s event hold them: the filter's JSON, each group
/// filter naming its declared group or its columns, never an empty one of the two.
[[nodiscard]] Value filtersValueOf(table::FilterSpec const& filters) {
    auto const json = parseJson(glz::write_json(filters).value_or("null"));
    auto read = json ? valueFromJson(*json) : std::expected<Value, std::string>{Value{}};
    ValueRecord const* const whole = read ? read->asRecord() : nullptr;
    if (whole == nullptr) {
        return Value{};
    }
    ValueRecord out = *whole;
    ValueList groups;
    if (auto const found = whole->find("groups"); found != whole->end() && found->second.asList() != nullptr) {
        for (Value const& group : *found->second.asList()) {
            ValueRecord const* const fields = group.asRecord();
            if (fields == nullptr) {
                continue;
            }
            ValueRecord kept = *fields;
            if (auto const id = kept.find("group"); id != kept.end() && id->second == Value{std::string{}}) {
                kept.erase(id);
            }
            if (auto const columns = kept.find("columns");
                columns != kept.end() && columns->second.asList() != nullptr && columns->second.asList()->empty()) {
                kept.erase(columns);
            }
            groups.emplace_back(std::move(kept));
        }
    }
    out.insert_or_assign("groups", Value{std::move(groups)});
    return Value{std::move(out)};
}

/// The layout a table holds now: the document's, when it binds one, else the table's own. Tracked.
[[nodiscard]] std::optional<table::ColumnLayoutValue> layoutNow(TableConfig const& config) {
    static_cast<void>(config.version.get());
    if (config.bound != nullptr) {
        return layoutFromValue(config.bound->get());
    }
    return config.held;
}

/// The arrangement a mount takes from a layout, by index into @p columns; the default one while the layout is the
/// declared order with every column shown, none pinned and no width set, so such a table is laid out as one with
/// no layout is.
[[nodiscard]] TableArrangement arrangementOf(std::span<LayoutColumn const> columns, table::ColumnLayout const& layout) {
    std::vector<std::string> const visible = layout.visible();
    bool plain = visible.size() == columns.size() && layout.pinned() == 0 && layout.widths().empty();
    for (std::size_t at = 0; plain && at < visible.size(); ++at) {
        plain = visible[at] == columns[at].id;
    }
    TableArrangement out;
    if (plain) {
        return out;
    }
    auto const indexOf = [columns](std::string const& id) -> std::optional<std::size_t> {
        auto const found = std::ranges::find(columns, id, &LayoutColumn::id);
        return found == columns.end() ? std::nullopt
                                      : std::optional{static_cast<std::size_t>(found - columns.begin())};
    };
    for (std::string const& id : visible) {
        if (std::optional<std::size_t> const index = indexOf(id)) {
            out.visible.push_back(*index);
        }
    }
    out.pinned = layout.pinned();
    for (auto const& [id, width] : layout.widths()) {
        if (std::optional<std::size_t> const index = indexOf(id)) {
            out.widths.insert_or_assign(*index, width);
        }
    }
    return out;
}

/// A table's `layout`, `visibleColumns` or `hiddenCount` element value. Tracked.
[[nodiscard]] Value layoutElementValue(TableConfig const& config, std::string_view name) {
    table::ColumnLayout const layout = layoutOver(config.columns, config.declaredHidden, layoutNow(config));
    if (name == "layout") {
        return layoutValueOf(layout.value());
    }
    std::vector<std::string> const visible = layout.visible();
    if (name == "hiddenCount") {
        return static_cast<std::int64_t>(layout.order().size() - visible.size());
    }
    ValueList ids;
    for (std::string const& id : visible) {
        ids.emplace_back(id);
    }
    return Value{std::move(ids)};
}

/// A table with the dialogs its menu opens mounted beside it (`ui::besideTable`); without dialogs, the table alone.
/// @param placed  The table's common properties; its layout becomes the one inside the column.
/// @param dialogs The dialogs; null for none.
/// @param table   Makes the table, after @p placed is changed.
[[nodiscard]] Node besideDialogs(Common& placed, Node dialogs, std::function<Node()> const& table) {
    if (dialogs == nullptr) {
        return table();
    }
    std::vector<Node> beside;
    beside.push_back(std::move(dialogs));
    return besideTable(placed, std::move(beside), table);
}

/// Draws a filter button on each column a filter menu edits.
/// @param config  The table's configuring, whose `filterTarget` says which columns filter.
/// @param columns The mount's columns, in the order of `config.columns`.
/// @param options The table's options; without `onFilter` no column draws a button.
void markFilterable(TableConfig const& config, std::vector<TableColumn>& columns, TableOptions const& options) {
    if (!options.onFilter || !config.filterTarget) {
        return;
    }
    for (std::size_t at = 0; at < columns.size() && at < config.columns.size(); ++at) {
        columns.at(at).filterable = config.filterTarget(config.columns.at(at).id).has_value();
    }
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

/// What a model-backed table keeps in its element, across every mount of it: its rows and the query of its
/// description, the arrangement it gave the rows, its persisted state's key and load, and its status line's message.
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
        return TableTotal{.rows = static_cast<std::int64_t>(_entries.size()), .kind = table::TotalKind::Exact,
                          .known = true};
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
    async::Completion<std::expected<std::vector<table::RowId>, table::TableError>> keysOf(std::int64_t first,
                                                                                       std::int64_t count) override {
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
    void locateRow(table::RowId key, std::stop_token stop, std::function<void(RowLocation const&)> done) override {
        auto const at = std::ranges::find(_entries, key, &Entry::key);
        RowLocation const found{at == _entries.end() ? std::nullopt
                                                     : std::optional{static_cast<std::int64_t>(at - _entries.begin())}};
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
    void moveKey(table::RowId const& from, table::RowId const& to) override { _engine->moveKey(from, to); }

private:
    std::shared_ptr<EngineRows> _engine;
    exec::IExecutor* _owner;
    std::vector<Entry> _entries;
    std::function<void(table::ViewChange const&)> _handler;
};

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
    async::Completion<std::expected<std::vector<table::RowId>, table::TableError>> keysOf(std::int64_t first,
                                                                                       std::int64_t count) override {
        return _engine->keysOf(first, count);
    }
    void locateRow(table::RowId key, std::stop_token stop, std::function<void(RowLocation const&)> done) override {
        _engine->locateRow(std::move(key), std::move(stop), std::move(done));
    }
    async::Completion<std::expected<std::vector<FetchedRow>, table::TableError>> fetchRows(
        std::vector<table::RowId> keys, std::vector<std::string> columns) override {
        return _engine->fetchRows(std::move(keys), std::move(columns));
    }
    std::expected<void, table::TableError> setSort(SortChain sort) override { return _engine->setSort(std::move(sort)); }
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
    void moveKey(table::RowId const& from, table::RowId const& to) override { _engine->moveKey(from, to); }

private:
    std::shared_ptr<EngineRows> _engine;
    std::function<void()> _heard;
    std::function<void(table::ViewChange const&)> _handler;
};

/// What a table over `rows` keeps in its element, across every mount of it: its rows on the engine and the page of
/// them it shows, the arrangement it gave them, its persisted state's key and load, and its status line's message.
/// A table lowered where names its element cannot see are bound has one of its own, kept by its mount, which
/// persists nothing. A collection's table is one too, without a source or a page: its engine is over the session's
/// source, and its mounts read the engine's rows through the element's `tableRows`.
struct RowsTable {
    explicit RowsTable(reactive::Runtime& runtime)
        : version{runtime, 0}, message{runtime, std::string{}}, config{runtime} {}

    RowsTable(RowsTable const&) = delete;
    RowsTable& operator=(RowsTable const&) = delete;
    RowsTable(RowsTable&&) = delete;
    RowsTable& operator=(RowsTable&&) = delete;

    /// Its computations stop first; then the columns' tracked `sortBy`s and the `group`'s `by`, which read the
    /// table's scope, leave the source, which the mount may keep longer; a change not yet saved is saved at once.
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

    /// The grid: its source, its controller and its table node.
    FormGrid grid;
    /// The table's columns, one per grid column.
    std::vector<TableColumn> shown;
    /// The table's configuring: its columns, layout, menus and filters.
    TableConfig config;
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

[[nodiscard]] Value evalIn(RtScope const& scope, Bindings const* bindings, ExprPtr const& expr) {
    if (!expr) {
        return Value{};
    }
    ScopeEnv const env{scope, bindings};
    return evaluate(*expr, env);
}

[[nodiscard]] bool engaged(Value const& value) {
    if (value.isNull()) {
        return false;
    }
    if (std::string const* const text = value.asString()) {
        return !text->empty();
    }
    if (ValueList const* const items = value.asList()) {
        return !items->empty();
    }
    return true;
}

[[nodiscard]] std::string textOf(Value const& value, FormatContext const& format) {
    if (value.isNull()) {
        return {};
    }
    if (std::string const* const text = value.asString()) {
        return *text;
    }
    if (bool const* const flag = value.asBool()) {
        return *flag ? "true" : "false";
    }
    // A number that cannot be rounded at its own places shows as its exact JSON rather than as a clamped value.
    if (Quantity const* const quantity = value.asQuantity()) {
        auto number = formatNumber(value, withoutGrouping(format.numeric));
        return number ? *number + " " + unitDisplay(quantity->unit, format.units) : canonicalJson(value);
    }
    if (auto number = formatNumber(value, withoutGrouping(format.numeric))) {
        return *std::move(number);
    }
    if (time::DateTime const* const instant = value.asTimestamp()) {
        return instant->toIso8601();
    }
    return canonicalJson(value);
}

namespace {

/// @brief A value as a view key: an `int` or a string as itself, anything else as its JSON text.
/// @param value The value.
/// @return The key.
[[nodiscard]] Key keyOf(Value const& value) {
    if (std::int64_t const* const number = value.asInt()) {
        return Key{*number};
    }
    if (std::string const* const text = value.asString()) {
        return Key{*text};
    }
    return Key{canonicalJson(value)};
}

}  // namespace

[[nodiscard]] Value valueOfKey(Key const& key) {
    if (std::int64_t const* const number = std::get_if<std::int64_t>(&key)) {
        return Value{*number};
    }
    return Value{std::get<std::string>(key)};
}

namespace {

// A cell range as a document reads it: `{"anchor": {"key", "column"}, "focus": {"key", "column"}}`, `null` for none.
[[nodiscard]] Value cellRangeValue(std::optional<CellRange> const& range) {
    if (!range) {
        return Value{};
    }
    auto const corner = [](CellRef const& cell) {
        return Value{ValueRecord{{"key", valueOfKey(cell.key)}, {"column", Value{cell.column}}}};
    };
    return Value{ValueRecord{{"anchor", corner(range->anchor)}, {"focus", corner(range->focus)}}};
}

// A document's cell range: two corners, each a key and a column id; anything else is none.
[[nodiscard]] std::optional<CellRange> cellRangeOf(Value const& value) {
    auto const corner = [&value](std::string_view name) -> std::optional<CellRef> {
        Value const* const part = value.field(name);
        Value const* const key = part == nullptr ? nullptr : part->field("key");
        Value const* const column = part == nullptr ? nullptr : part->field("column");
        if (key == nullptr || key->isNull() || column == nullptr || column->asString() == nullptr) {
            return std::nullopt;
        }
        return CellRef{.key = keyOf(*key), .column = *column->asString()};
    };
    std::optional<CellRef> anchor = corner("anchor");
    std::optional<CellRef> focus = corner("focus");
    if (!anchor || !focus) {
        return std::nullopt;
    }
    return CellRange{.anchor = std::move(*anchor), .focus = std::move(*focus)};
}

/// @brief A list of strings out of a value: a string, a list of strings, or nothing.
/// @param value The value.
/// @return The strings.
[[nodiscard]] std::vector<std::string> stringsOf(Value const& value) {
    std::vector<std::string> out;
    if (std::string const* const text = value.asString()) {
        out.push_back(*text);
    } else if (ValueList const* const items = value.asList()) {
        for (Value const& item : *items) {
            if (std::string const* const element = item.asString()) {
                out.push_back(*element);
            }
        }
    }
    return out;
}

/// @brief A list of strings as a value.
/// @param texts The strings.
/// @return The list.
[[nodiscard]] Value listOfStrings(std::vector<std::string> const& texts) {
    ValueList items;
    items.reserve(texts.size());
    for (std::string const& text : texts) {
        items.emplace_back(text);
    }
    return Value{std::move(items)};
}

/// @brief A command context without its scope: what a callback the host keeps may hold, so that the host never
///        keeps a scope alive past its screen. The callback holds the scope weakly and puts it back.
/// @param ctx The context.
/// @return The context, its scope dropped.
[[nodiscard]] CommandCtx detached(CommandCtx const& ctx) {
    CommandCtx kept = ctx;
    kept.scope = nullptr;
    return kept;
}

/// @brief Collects the `onClick` lists of menu entries, submenus included.
/// @param entries The entries.
/// @param lists Where they go, in document order.
// NOLINTNEXTLINE(misc-no-recursion): a submenu is a menu one level down.
void menuCommandLists(std::span<DocNodePtr const> entries, std::vector<Commands const*>& lists) {
    for (DocNodePtr const& entry : entries) {
        if (Commands const* const commands = entry->event("onClick")) {
            lists.push_back(commands);
        }
        menuCommandLists(entry->partList("items"), lists);
    }
}

/// @brief Collects the command lists of a screen's head: its actions', its menu entries' and its tools'.
/// @param doc The screen's document.
/// @param lists Where they go, in document order.
void headCommandLists(Document const& doc, std::vector<Commands const*>& lists) {
    for (DocNodePtr const& action : doc.actions) {
        if (Commands const* const commands = action->event("onClick")) {
            lists.push_back(commands);
        }
    }
    menuCommandLists(doc.menu, lists);
    for (DocNodePtr const& tool : doc.tools) {
        for (std::string_view const key : {"onChange", "onSubmit", "onChoose"}) {
            if (Commands const* const commands = tool->event(key)) {
                lists.push_back(commands);
            }
        }
    }
}

/// @brief A kind's key spec, from the kind table.
/// @param kind The kind spec.
/// @param key  The key.
/// @return The spec, or `nullptr` when the kind has no such key of its own.
[[nodiscard]] KeySpec const* keySpec(KindSpec const& kind, std::string_view key) {
    for (KeySpec const& spec : kind.keys) {
        if (spec.key == key) {
            return &spec;
        }
    }
    return nullptr;
}

/// @brief Whether a node kind keeps per-element state even without an id: the inputs, dialogs, tables and the forms
///        engine's screens.
/// @param kind The kind.
/// @return It.
[[nodiscard]] bool statefulKind(std::string_view kind) {
    static constexpr std::array<std::string_view, 12> kStateful = {
        "textInput", "checkbox", "select", "dateTimeInput", "slider",     "filePicker",
        "dialog",    "drawer",   "table",  "form",          "collection", "wizard",
    };
    return listed(kStateful, kind);
}

}  // namespace

// NOLINTNEXTLINE(modernize-use-string-view): callers keep the key as an owned string, in the log and in tests.
[[nodiscard]] std::string commandKey(CommandKind kind) {
    switch (kind) {
        case CommandKind::Set:
            return "set";
        case CommandKind::Reset:
            return "reset";
        case CommandKind::Run:
            return "run";
        case CommandKind::Refetch:
            return "refetch";
        case CommandKind::Open:
            return "open";
        case CommandKind::Close:
            return "close";
        case CommandKind::Navigate:
            return "navigate";
        case CommandKind::Return:
            return "return";
        case CommandKind::Focus:
            return "focus";
        case CommandKind::Host:
            return "host";
        case CommandKind::SwitchBackend:
            return "switchBackend";
        case CommandKind::ResetPrefs:
            return "resetPrefs";
        case CommandKind::SetLocale:
            return "setLocale";
        case CommandKind::SetContext:
            return "setContext";
        case CommandKind::Notify:
            return "notify";
        case CommandKind::Seq:
            return "seq";
        case CommandKind::All:
            return "all";
        case CommandKind::If:
            return "if";
        case CommandKind::CancelClose:
            return "cancelClose";
        case CommandKind::Retry:
            return "retry";
        case CommandKind::SignIn:
            return "signIn";
        case CommandKind::SignOut:
            return "signOut";
        case CommandKind::Upload:
            return "upload";
        case CommandKind::Download:
            return "download";
        default:
            return "unknown";
    }
}

}  // namespace detail

namespace detail {

[[nodiscard]] std::string failureText(std::exception_ptr const& error) {
    try {
        std::rethrow_exception(error);
    } catch (std::exception const& failure) {
        return failure.what();
    } catch (...) {
        return "unknown error";
    }
}

namespace {

/// @brief A failure's action refusal, as a document reads it.
/// @param error The failure.
/// @return `{"code": c, "args": [{"name": n, "value": v}, ...]}` when it is an `ActionRefusal`, else `null`.
[[nodiscard]] Value refusalValue(std::exception_ptr const& error) {
    try {
        std::rethrow_exception(error);
    } catch (model::ActionRefusal const& refusal) {
        ValueList args;
        for (model::RefusalArg const& arg : refusal.args()) {
            args.emplace_back(ValueRecord{{"name", Value{arg.name}}, {"value", Value{arg.value}}});
        }
        return Value{ValueRecord{{"code", Value{refusal.code()}}, {"args", Value{std::move(args)}}}};
    } catch (...) {
        return Value{};
    }
}

}  // namespace

// NOLINTNEXTLINE(modernize-use-string-view): callers keep the kind as an owned string, in a reply's `kind`.
[[nodiscard]] std::string defaultErrorKind(std::exception_ptr const& error) {
    try {
        std::rethrow_exception(error);
    } catch (backend::DisconnectedError const&) {
        return "offline";
    } catch (backend::TimeoutError const&) {
        return "timeout";
    } catch (backend::ClientTimeoutError const&) {
        return "timeout";
    } catch (backend::BackendChangedError const&) {
        return "backendChanged";
    } catch (model::ActionRefusal const&) {
        // An action's own refusal is an answer about its data, whatever its message says, never the authoriser's.
        return "error";
    } catch (wire::UnauthorizedError const&) {
        // A refused control request (register, attach): its text carries the request's name before the refusal.
        return "denied";
    } catch (std::runtime_error const& refused) {
        // The authoriser says no to an execute with this one text; no exception type tells it from another failure.
        return std::string_view{refused.what()} == wire::kUnauthorizedMessage ? "denied" : "error";
    } catch (...) {
        return "error";
    }
}

[[nodiscard]] std::string canonicalJson(Value const& value) {
    return glz::write_json(valueToJson(value)).value_or(std::string{});
}

namespace {

/// @brief A value with the field at @p path replaced, creating records along the way.
/// @param base  The value written into.
/// @param path  The field path.
/// @param value The new value at the path.
/// @return The new value.
// NOLINTNEXTLINE(misc-no-recursion): one level per path segment
[[nodiscard]] Value withPath(Value const& base, std::span<std::string const> path, Value value) {
    if (path.empty()) {
        return value;
    }
    ValueRecord fields;
    if (ValueRecord const* const record = base.asRecord()) {
        fields = *record;
    }
    Value const inner = base.field(path.front()) != nullptr ? *base.field(path.front()) : Value{};
    fields.insert_or_assign(path.front(), withPath(inner, path.subspan(1), std::move(value)));
    return Value{std::move(fields)};
}

/// @brief A row's identity for `patch` and `optimistic`: its `id` field.
/// @param row The row.
/// @return The `id` field, or `null`.
[[nodiscard]] Value rowId(Value const& row) {
    Value const* const found = row.field("id");
    return found == nullptr ? Value{} : *found;
}

}  // namespace

[[nodiscard]] Value mergeRow(Value const& row, Value const& with) {
    ValueRecord const* const fields = row.asRecord();
    ValueRecord const* const over = with.asRecord();
    if (fields == nullptr || over == nullptr) {
        return with;
    }
    ValueRecord merged = *fields;
    for (auto const& [name, field] : *over) {
        merged.insert_or_assign(name, field);
    }
    return Value{std::move(merged)};
}

[[nodiscard]] std::expected<Value, PatchMiss> patchRows(
    Value const& value, std::string_view listMember,
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a list, a key and a row are distinct roles of one type.
    Value const& key, Value const& with) {
    ValueRecord const* const record = listMember.empty() ? nullptr : value.asRecord();
    if (!listMember.empty() && (record == nullptr || !record->contains(listMember))) {
        return std::unexpected(PatchMiss::NoList);
    }
    Value const* const rows = record == nullptr ? &value : &record->find(listMember)->second;
    ValueList const* const items = rows->asList();
    if (items == nullptr) {
        return std::unexpected(PatchMiss::NoList);
    }
    if (key.isNull()) {
        return std::unexpected(PatchMiss::NoRow);
    }
    ValueList out = *items;
    bool found = false;
    for (Value& row : out) {
        if (rowId(row) == key) {
            row = mergeRow(row, with);
            found = true;
        }
    }
    if (!found) {
        return std::unexpected(PatchMiss::NoRow);
    }
    if (record == nullptr) {
        return Value{std::move(out)};
    }
    ValueRecord patched = *record;
    patched.insert_or_assign(std::string{listMember}, Value{std::move(out)});
    return Value{std::move(patched)};
}

namespace {

/// @brief What a patch that found no list reports.
/// @param query      The query it patches.
/// @param listMember The record member it names; empty when none.
/// @return The diagnostic's reason.
[[nodiscard]] std::string patchMissReason(std::string_view query, std::string_view listMember) {
    if (listMember.empty()) {
        return "the value of `" + std::string{query} +
               "` is not a list, so the patch changes nothing; `at` names the record member that holds the list";
    }
    return "the value of `" + std::string{query} + "` has no list at `" + std::string{listMember} +
           "`, so the patch changes nothing";
}

}  // namespace

// NOLINTNEXTLINE(misc-no-recursion): one level per nested list or record of the value.
[[nodiscard]] Value withoutFields(Value const& value, std::vector<std::string> const& fields) {
    if (fields.empty()) {
        return value;
    }
    if (ValueRecord const* const record = value.asRecord()) {
        ValueRecord kept = *record;
        for (std::string const& field : fields) {
            kept.erase(field);
        }
        return Value{std::move(kept)};
    }
    if (ValueList const* const items = value.asList()) {
        ValueList kept;
        kept.reserve(items->size());
        for (Value const& item : *items) {
            kept.push_back(withoutFields(item, fields));
        }
        return Value{std::move(kept)};
    }
    return value;
}

namespace {

/// @brief The members of a catalog record path: `"header.rows"` is `header`, then `rows`; `""` is none.
/// @param path The path.
/// @return Its members in order.
[[nodiscard]] std::vector<std::string> pathMembers(std::string_view path) {
    std::vector<std::string> members;
    for (auto const part : std::views::split(path, '.')) {
        if (!part.empty()) {
            members.emplace_back(part.begin(), part.end());
        }
    }
    return members;
}

/// @brief The rows of a keyed record that a value holds at a path.
///
/// A list on the way is looked through, as the result walker looks through a `std::vector`. At the path's end a
/// record with the key field is a row, and a record without it is a map whose values are the rows.
/// @param value    The value.
/// @param path     The members still to follow.
/// @param keyField The record's key field.
/// @param out      Where the rows go.
// NOLINTNEXTLINE(misc-no-recursion): one level per path member or nested list.
void keyedRowsAt(Value const& value, std::span<std::string const> path, std::string_view keyField,
                 std::vector<Value const*>& out) {
    if (ValueList const* const items = value.asList()) {
        for (Value const& item : *items) {
            keyedRowsAt(item, path, keyField, out);
        }
        return;
    }
    ValueRecord const* const record = value.asRecord();
    if (record == nullptr) {
        return;
    }
    if (!path.empty()) {
        if (auto const found = record->find(path.front()); found != record->end()) {
            keyedRowsAt(found->second, path.subspan(1), keyField, out);
        }
        return;
    }
    if (record->contains(keyField)) {
        out.push_back(&value);
        return;
    }
    for (auto const& [name, item] : *record) {
        static_cast<void>(name);
        if (ValueRecord const* const row = item.asRecord(); row != nullptr && row->contains(keyField)) {
            out.push_back(&item);
        }
    }
}

/// @brief Merges new rows over the rows with the same key that a value holds at a path.
/// @param value    The value.
/// @param path     The members still to follow.
/// @param keyField The record's key field.
/// @param rows     The new rows.
/// @return The new value, or nothing when no row matched.
// NOLINTNEXTLINE(misc-no-recursion): one level per path member or nested list.
[[nodiscard]] std::optional<Value> patchKeyedRows(Value const& value, std::span<std::string const> path,
                                                  std::string_view keyField, RowsByKey const& rows) {
    if (ValueList const* const items = value.asList()) {
        ValueList out = *items;
        bool changed = false;
        for (Value& item : out) {
            if (auto patched = patchKeyedRows(item, path, keyField, rows)) {
                item = std::move(*patched);
                changed = true;
            }
        }
        return changed ? std::optional<Value>{Value{std::move(out)}} : std::nullopt;
    }
    ValueRecord const* const record = value.asRecord();
    if (record == nullptr) {
        return std::nullopt;
    }
    if (!path.empty()) {
        auto const found = record->find(path.front());
        if (found == record->end()) {
            return std::nullopt;
        }
        auto patched = patchKeyedRows(found->second, path.subspan(1), keyField, rows);
        if (!patched) {
            return std::nullopt;
        }
        ValueRecord out = *record;
        out.insert_or_assign(path.front(), std::move(*patched));
        return Value{std::move(out)};
    }
    if (auto const key = record->find(keyField); key != record->end()) {
        auto const row = rows.find(canonicalJson(key->second));
        return row == rows.end() ? std::nullopt : std::optional<Value>{mergeRow(value, *row->second)};
    }
    ValueRecord out = *record;
    bool changed = false;
    for (auto& [name, item] : out) {
        static_cast<void>(name);
        if (ValueRecord const* const row = item.asRecord(); row != nullptr && row->contains(keyField)) {
            if (auto patched = patchKeyedRows(item, {}, keyField, rows)) {
                item = std::move(*patched);
                changed = true;
            }
        }
    }
    return changed ? std::optional<Value>{Value{std::move(out)}} : std::nullopt;
}

/// @brief Whether a member at a path is set: a non-empty list, a record, `true`, or a number other than zero.
///
/// A list on the way is looked through: the member is set when it is set in any element.
/// @param value The value.
/// @param path  The members still to follow.
/// @return `true` when set.
// NOLINTNEXTLINE(misc-no-recursion): one level per path member or nested list.
[[nodiscard]] bool memberSetAt(Value const& value, std::span<std::string const> path) {
    if (!path.empty()) {
        if (ValueList const* const items = value.asList()) {
            return std::ranges::any_of(*items, [&](Value const& item) { return memberSetAt(item, path); });
        }
        ValueRecord const* const record = value.asRecord();
        if (record == nullptr) {
            return false;
        }
        auto const found = record->find(path.front());
        return found != record->end() && memberSetAt(found->second, path.subspan(1));
    }
    if (ValueList const* const items = value.asList()) {
        return !items->empty();
    }
    if (value.asRecord() != nullptr) {
        return true;
    }
    if (bool const* const flag = value.asBool()) {
        return *flag;
    }
    if (std::int64_t const* const number = value.asInt()) {
        return *number != 0;
    }
    if (Decimal const* const decimal = value.asDecimal()) {
        return !decimal->value.isZero();
    }
    return false;
}

/// @brief Whether any member at @p paths is set in @p value.
/// @param value The value.
/// @param paths The members' paths.
/// @return `true` when one is.
[[nodiscard]] bool anyMemberSet(Value const& value, std::vector<std::string> const& paths) {
    return std::ranges::any_of(
        paths, [&](std::string const& path) { return memberSetAt(value, pathMembers(path)); });
}

}  // namespace

[[nodiscard]] std::vector<Value const*> keyedRowsOf(Value const& value, std::vector<std::string> const& paths,
                                                    std::string_view keyField) {
    std::vector<Value const*> rows;
    for (std::string const& path : paths) {
        std::vector<std::string> const members = pathMembers(path);
        keyedRowsAt(value, members, keyField, rows);
    }
    return rows;
}

[[nodiscard]] std::optional<Value> patchHeldRows(Value const& value, std::vector<std::string> const& paths,
                                                 std::string_view keyField, RowsByKey const& rows) {
    std::optional<Value> patched;
    for (std::string const& path : paths) {
        std::vector<std::string> const members = pathMembers(path);
        auto next = patched ? patchKeyedRows(*patched, members, keyField, rows)
                            : patchKeyedRows(value, members, keyField, rows);
        if (next) {
            patched = std::move(next);
        }
    }
    return patched;
}

}  // namespace detail

std::shared_ptr<ModelChannel> BridgeDispatcher::bind(std::string const& typeId, bridge::BindSharing sharing,
                                                     std::string const& instanceKey) {
    return std::make_shared<Channel>(*_bridge, _cbExec, _evaluator, typeId, sharing, instanceKey);
}

async::Completion<std::string> BridgeDispatcher::Channel::execute(std::string const& actionId,
                                                                  std::string const& bodyJson, CallOptions options) {
    if (options.evaluate) {
        if (_evaluator) {
            return _evaluator(_typeId, actionId, bodyJson);
        }
        auto [refused, promise] = async::Completion<std::string>::makeSettleable(_cbExec);
        promise.reject(std::make_exception_ptr(std::runtime_error{"evaluate needs the UI service"}));
        return std::move(refused);
    }
    if (options.stop.stop_possible()) {
        return _handler.execute(actionId, bodyJson, std::move(options.stop));
    }
    return _handler.execute(actionId, bodyJson);
}

ChannelFacts BridgeDispatcher::Channel::facts() const {
    return ChannelFacts{.instanceId = _handler.instanceId(), .primary = _handler.primary()};
}
// NOLINTBEGIN(performance-unnecessary-value-param,bugprone-easily-swappable-parameters): every default below ignores
// its arguments, so each keeps the parameter types of the interface it implements.
bool InterpreterHost::navigate(NavigateRequest const& request, std::function<void(Value)> onResult,
                               std::function<void()> onCancel) {
    static_cast<void>(request);
    static_cast<void>(onResult);
    static_cast<void>(onCancel);
    return false;
}

bool InterpreterHost::closeSelf(std::string const& screen, std::optional<Value> result) {
    static_cast<void>(screen);
    static_cast<void>(result);
    return false;
}

bool InterpreterHost::hostCommand(std::string const& name, Value const& args) {
    static_cast<void>(name);
    static_cast<void>(args);
    return false;
}

bool InterpreterHost::switchBackend(std::string const& name,
                                    std::function<void(std::expected<void, std::string>)> done) {
    static_cast<void>(name);
    static_cast<void>(done);
    return false;
}

void InterpreterHost::resetPrefs(std::string const& screen, std::string const& point) {
    static_cast<void>(screen);
    static_cast<void>(point);
}

bool InterpreterHost::notify(std::string const& text, std::string const& role) {
    static_cast<void>(text);
    static_cast<void>(role);
    return false;
}

bool InterpreterHost::signIn(Value const& reply) {
    static_cast<void>(reply);
    return false;
}

bool InterpreterHost::setAppValue(std::string const& name, Value const& value) {
    static_cast<void>(name);
    static_cast<void>(value);
    return false;
}

bool InterpreterHost::upload(Value const& file, std::function<void(Value)> progress,
                             std::function<void(std::expected<Value, std::string>)> done) {
    static_cast<void>(file);
    static_cast<void>(progress);
    static_cast<void>(done);
    return false;
}

bool InterpreterHost::uploadFor(UploadRequest const& request, std::function<void(Value)> progress,
                                std::function<void(std::expected<Value, std::string>)> done) {
    return upload(request.file, std::move(progress), std::move(done));
}

bool InterpreterHost::download(Value const& file, std::string const& option, Value const& name,
                               std::function<void(std::expected<Value, std::string>)> done) {
    static_cast<void>(file);
    static_cast<void>(option);
    static_cast<void>(name);
    static_cast<void>(done);
    return false;
}

bool InterpreterHost::readFile(Value const& file, std::size_t maxBytes,
                               std::function<void(std::expected<InlineFile, std::string>)> done) {
    static_cast<void>(file);
    static_cast<void>(maxBytes);
    static_cast<void>(done);
    return false;
}

bool InterpreterHost::saveFile(InlineFile const& file, std::string const& option,
                               std::function<void(std::expected<Value, std::string>)> done) {
    static_cast<void>(file);
    static_cast<void>(option);
    static_cast<void>(done);
    return false;
}

void InterpreterHost::preferenceChanged(std::string const& screen, std::string const& point, Value const& value) {
    static_cast<void>(screen);
    static_cast<void>(point);
    static_cast<void>(value);
}
// NOLINTEND(performance-unnecessary-value-param,bugprone-easily-swappable-parameters)

bool Session::can(std::string_view action) const {
    static constexpr std::string_view kHostPrefix = "host:";
    if (action.starts_with(kHostPrefix)) {
        return _hostCommandNames.get().contains(action.substr(kHostPrefix.size()));
    }
    if (action == "context") {
        return _contextOffered.get();
    }
    return _permitted.get().contains(action);
}

void Session::setFormatLocale(std::string tag) {
    _formatLocale = std::move(tag);
    applyFacts(_format.peek()->locale);
}

std::shared_ptr<FormatContext const> Session::scopedFormatting(std::string_view tag) const {
    FormatContext next = *_format.get();
    LocaleFacts const& facts = localeFacts(tag);
    next.locale = std::string{tag};
    next.numeric = facts.numeric;
    next.currency = facts.currency;
    next.datePattern = _declaredDate.empty() ? std::string{facts.datePattern} : _declaredDate;
    next.dateTimePattern = _declaredDateTime.empty() ? std::string{facts.dateTimePattern} : _declaredDateTime;
    return std::make_shared<FormatContext const>(std::move(next));
}

bool Session::firstUnlistedLocale(std::string const& tag) {
    return findLocaleFacts(tag) == nullptr && _unlistedLocales.insert(tag).second;
}

void Session::setFormatting(FormatContext format) {
    // One batch, so an effect never sees the new `env.locale` beside the old facts, and runs once.
    _runtime->batch([this, &format] {
        setEnvironmentValue("locale", Value{format.locale});
        _format.set(std::make_shared<FormatContext const>(std::move(format)));
    });
}

void Session::setDatePatterns(std::string date, std::string dateTime) {
    _declaredDate = std::move(date);
    _declaredDateTime = std::move(dateTime);
    FormatContext next = *_format.peek();
    next.datePattern = _declaredDate.empty() ? _localeDate : _declaredDate;
    next.dateTimePattern = _declaredDateTime.empty() ? _localeDateTime : _declaredDateTime;
    setFormatting(std::move(next));
}

void Session::setTranslations(render::TranslationProvider translations) {
    FormatContext next = *_format.peek();
    next.translations = std::move(translations);
    setFormatting(std::move(next));
}

void Session::setEnvironment(ValueRecord values) {
    values.insert_or_assign("locale", Value{_format.peek()->locale});
    _env.set(std::move(values));
}

void Session::setEnvironmentValue(std::string const& name, Value value) {
    ValueRecord env = _env.peek();
    env.insert_or_assign(name, std::move(value));
    _env.set(std::move(env));
}

void Session::setHostCommands(HostCommands commands) {
    _hostCommandRunner = std::move(commands.run);
    _hostCommandNames.set(std::move(commands.available));
}

std::function<HostCommandResult(std::string const&, Value const&)> const& Session::hostCommandRunner() const noexcept {
    return _hostCommandRunner;
}

Value Session::client(ExprKind form, std::string_view name) const {
    ValueRecord const& values = [&] -> ValueRecord const& {
        if (form == ExprKind::Env) {
            return _env.get();
        }
        return form == ExprKind::Session ? _sessionValues.get() : _hostValues.get();
    }();
    auto const found = values.find(name);
    return found == values.end() ? Value{} : found->second;
}

void Session::clear() {
    _runtime->batch([this] {
        for (auto const& [key, entry] : _entries) {
            static_cast<void>(key);
            ++entry->generation;
            entry->evict.cancel();
            countTopics(*entry, -1);
        }
        _entries.clear();
        for (detail::QueryRuntime* const query : _mounted) {
            detachQuery(*query);
        }
    });
}

void Session::prefetch(std::shared_ptr<ModelChannel> const& channel, std::string const& token,
                       std::string const& typeId, std::string const& action, Value const& body) {
    auto const entry = acquire(token, typeId, action, false, body);
    if (!entry->pending.peek() && (!entry->hasValue.peek() || entry->stale)) {
        fetch(entry, channel);
    }
    release(entry);
}

void Session::asUser(std::function<void()> const& body) {
    ++_userDepth;
    try {
        body();
    } catch (...) {
        --_userDepth;
        throw;
    }
    --_userDepth;
}

std::string Session::cacheKey(std::string const& token, std::string const& typeId, std::string const& action,
                              bool evaluate, std::string const& body) {
    return typeId + '\x1f' + token + '\x1f' + (evaluate ? "evaluate:" : "") + action + '\x1f' + body;
}

std::shared_ptr<detail::CacheEntry> Session::acquire(std::string const& token, std::string const& typeId,
                                                     std::string const& action, bool evaluate, Value const& body) {
    std::string const text = detail::canonicalJson(body);
    std::string key = cacheKey(token, typeId, action, evaluate, text);
    auto found = _entries.find(key);
    if (found == _entries.end()) {
        auto entry = std::make_shared<detail::CacheEntry>(*_runtime);
        entry->key = key;
        entry->typeId = typeId;
        entry->action = action;
        entry->body = text;
        entry->evaluate = evaluate;
        entry->instanceToken = token;
        if (_options.catalog.action) {
            if (auto const info = _options.catalog.action(action)) {
                entry->secretFields = info->secretFields;
                if (!evaluate) {
                    entry->records = info->records;
                }
            }
        }
        countTopics(*entry, +1);
        found = _entries.emplace(std::move(key), std::move(entry)).first;
    }
    ++found->second->users;
    found->second->evict.cancel();
    return found->second;
}

void Session::release(std::shared_ptr<detail::CacheEntry> const& entry) {
    if (--entry->users > 0 || _options.scheduler == nullptr) {
        return;
    }
    std::weak_ptr<detail::CacheEntry> const weak = entry;
    entry->evict = _options.scheduler->after(_options.keepUnused, [this, weak] {
        auto const held = weak.lock();
        if (held && held->users == 0) {
            countTopics(*held, -1);
            _entries.erase(held->key);
            flushWatches();
        }
    });
}

void Session::fetch(std::shared_ptr<detail::CacheEntry> const& entry, std::shared_ptr<ModelChannel> const& channel) {
    // The watch goes out before the read, on the same connection, so a change committed after the read is
    // seen as an event.
    flushWatches();
    std::uint64_t const generation = ++entry->generation;
    if (channel == nullptr) {
        settleFailure(*entry, "the model alias has no instance", "error");
        return;
    }
    entry->pending.set(true);
    std::weak_ptr<detail::CacheEntry> const weak = entry;
    try {
        auto completion = channel->execute(entry->action, entry->body, CallOptions{.evaluate = entry->evaluate});
        completion
            .then([this, weak, generation](std::string const& reply) {
                auto const held = weak.lock();
                if (held && held->generation == generation) {
                    settleReply(*held, reply);
                }
            })
            .onError([this, weak, generation](std::exception_ptr const& error) {
                auto const held = weak.lock();
                if (held && held->generation == generation) {
                    settleFailure(*held, detail::failureText(error), classify(error), detail::refusalValue(error));
                }
            });
    } catch (...) {
        std::exception_ptr const error = std::current_exception();
        settleFailure(*entry, detail::failureText(error), classify(error), detail::refusalValue(error));
    }
}

void Session::confirm(detail::CacheEntry& entry, Value const& confirmed) {
    entry.confirmed = detail::withoutFields(confirmed, entry.secretFields);
    showLayers(entry);
}

void Session::showLayers(detail::CacheEntry& entry) {
    Value shown = entry.confirmed;
    for (detail::OptimisticLayer const& layer : entry.layers) {
        if (auto patched = detail::patchRows(shown, layer.at, layer.key, layer.with)) {
            shown = std::move(*patched);
        }
    }
    noteWrite();
    entry.value.set(std::move(shown));
}

std::string Session::classify(std::exception_ptr const& error) const {
    return _options.classify ? _options.classify(error) : detail::defaultErrorKind(error);
}

std::chrono::milliseconds Session::now() const {
    if (_options.clock) {
        return _options.clock();
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch());
}

void Session::markStale(detail::CacheEntry& entry) {
    entry.stale = true;
    if (entry.pending.peek()) {
        entry.refetchAfter = true;
    }
}

SessionOptions Session::checkedOptions(SessionOptions options) {
    if (options.maxInlineFileBytes == 0 || options.maxInlineFileBytes > kMaxInlineFileBytes) {
        throw std::invalid_argument{"morph::ui::Session: maxInlineFileBytes is " +
                                    std::to_string(options.maxInlineFileBytes) + "; it is 1 to " +
                                    std::to_string(kMaxInlineFileBytes) + " (kMaxInlineFileBytes)"};
    }
    return options;
}

void Session::settleReply(detail::CacheEntry& entry, std::string const& reply) {
    // A table's page holds its record opaquely, and its Number cells are numbers with fractions, which the strict
    // rule refuses; every other reply keeps the strict rule.
    bool const opaque = std::ranges::any_of(entry.records, &ActionRecordInfo::opaque);
    auto json = parseJson(reply);
    auto value = json ? (opaque ? detail::tableValueFromJson(*json) : valueFromJson(*json))
                      : std::unexpected(json.error());
    if (!value) {
        settleFailure(entry, "the reply is not a document value: " + value.error(), "error");
        return;
    }
    _runtime->batch([&] {
        confirm(entry, *value);
        entry.hasValue.set(true);
        entry.error.set({});
        entry.errorKind.set({});
        entry.refusal.set(Value{});
        entry.pending.set(false);
        entry.fetchedAt = now();
        entry.stale = entry.refetchAfter;
        if (entry.refetchAfter) {
            entry.refetchAfter = false;
            refetchMounted(entry);
        }
    });
}

void Session::settleFailure(detail::CacheEntry& entry, std::string text, std::string kind, Value refusal) {
    _runtime->batch([&] {
        noteWrite();
        entry.error.set(std::move(text));
        entry.errorKind.set(std::move(kind));
        entry.refusal.set(std::move(refusal));
        entry.pending.set(false);
    });
}

void Session::applyFacts(std::string locale) {
    LocaleFacts const& facts = localeFacts(_formatLocale.empty() ? locale : _formatLocale);
    _localeDate = std::string{facts.datePattern};
    _localeDateTime = std::string{facts.dateTimePattern};
    FormatContext next = *_format.peek();
    next.locale = std::move(locale);
    next.numeric = facts.numeric;
    next.currency = facts.currency;
    next.datePattern = _declaredDate.empty() ? _localeDate : _declaredDate;
    next.dateTimePattern = _declaredDateTime.empty() ? _localeDateTime : _declaredDateTime;
    setFormatting(std::move(next));
}

Screen::Screen() = default;

Screen::~Screen() {
    try {
        if (Commands const* const commands = lifecycleList("onUnmount")) {
            runCommands(*commands);
        }
    } catch (...) {  // NOLINT(bugprone-empty-catch): a destructor has no one to report to.
    }
    _core->focusElement = {};
    _core->clipboard = {};
    _core->pasteboard = {};
    _mounted.reset();
    _headActions.clear();
    _headStatus.clear();
    _headMenu.clear();
    _headTools.clear();
    _scope.reset();
}

Widget& Screen::root() const {
    if (!_mounted) {
        throw std::logic_error{"the screen is not mounted"};
    }
    return _mounted->root();
}

Value Screen::read(Json const& expression) const {
    auto parsed = parseExpression(expression);
    if (!parsed) {
        return Value{};
    }
    return detail::evalIn(*_scope, nullptr, *parsed);
}

auto Screen::boundAliases() const -> std::vector<BoundAlias> {
    std::vector<BoundAlias> bound;
    for (auto const& alias : _scope->models) {
        if (alias->channel == nullptr) {
            continue;
        }
        ChannelFacts facts = alias->channel->facts();
        if (facts.instanceId != 0) {
            bound.push_back(BoundAlias{.decl = alias->decl, .facts = std::move(facts)});
        }
    }
    return bound;
}

std::vector<HeadBadge> Screen::status() const {
    std::vector<HeadBadge> shown;
    for (detail::BadgeModel const& badge : _headStatus) {
        if (badge.visible.evaluate()) {
            shown.push_back(HeadBadge{
                .text = badge.text.evaluate(),
                .tone = badge.tone.evaluate(),
                .icon = badge.icon.evaluate(),
            });
        }
    }
    return shown;
}

std::vector<HeadAction> Screen::actions() const {
    std::vector<HeadAction> shown;
    for (detail::ActionModel const& action : _headActions) {
        if (action.visible.evaluate()) {
            shown.push_back(HeadAction{
                .id = action.id,
                .label = action.label.evaluate(),
                .icon = action.icon.evaluate(),
                .iconOnly = action.iconOnly.evaluate(),
                .emphasis = action.emphasis,
                .enabled = action.enabled.evaluate(),
            });
        }
    }
    return shown;
}

bool Screen::runAction(std::string_view actionId) {
    auto const found = std::ranges::find(_headActions, actionId, &detail::ActionModel::id);
    if (found == _headActions.end()) {
        _core->report("/actions", "head action " + std::string{actionId} + " is unknown");
        return false;
    }
    auto const [visible, enabled] =
        runtime().untracked([&] { return std::pair{found->visible.evaluate(), found->enabled.evaluate()}; });
    if (!visible || !enabled) {
        _core->report(found->path, "head action " + found->id + (visible ? " is disabled" : " is hidden"));
        return false;
    }
    if (found->run) {
        found->run();
    }
    return true;
}

std::string Screen::menuLabel() const {
    if (!_core->doc().menuLabel) {
        return "More actions";
    }
    return detail::textOf(detail::evalIn(*_scope, nullptr, _core->doc().menuLabel), _scope->formatting());
}

bool Screen::chooseMenuEntry(std::string_view entryId) {
    FoundEntry const found = runtime().untracked([&] { return findHeadEntry(_headMenu, entryId, FoundEntry{}); });
    if (found.model == nullptr) {
        _core->report("/menu", "head menu entry " + std::string{entryId} + " is unknown");
        return false;
    }
    if (!found.visible || !found.enabled) {
        _core->report(found.model->path,
                      "head menu entry " + found.model->id + (found.visible ? " is disabled" : " is hidden"));
        return false;
    }
    if (found.model->item.onSelect) {
        found.model->item.onSelect();
    }
    return true;
}

std::vector<HeadTool> Screen::tools() const {
    std::vector<HeadTool> shown;
    for (detail::HeadToolModel const& tool : _headTools) {
        if (!tool.visible.evaluate()) {
            continue;
        }
        bool const search = tool.kind == HeadToolKind::Search;
        shown.push_back(HeadTool{
            .id = tool.id,
            .kind = tool.kind,
            .label = tool.label.evaluate(),
            .value = search ? tool.value.evaluate() : std::string{},
            .placeholder = search ? tool.placeholder.evaluate() : std::string{},
            .options = search ? std::vector<SelectOption>{} : tool.options.evaluate(),
            .selected = search ? std::nullopt : tool.selected.evaluate(),
            .width = tool.width,
            .enabled = tool.enabled.evaluate(),
        });
    }
    return shown;
}

bool Screen::editTool(std::string_view toolId, Value const& value) {
    detail::HeadToolModel const* const tool = usableTool(toolId);
    if (tool == nullptr) {
        return false;
    }
    if (tool->kind == HeadToolKind::Search && value.asString() == nullptr) {
        _core->report(tool->path, "head tool " + tool->id + " is a search, edited with text");
        return false;
    }
    tool->edit(value);
    return true;
}

bool Screen::submitTool(std::string_view toolId) {
    detail::HeadToolModel const* const tool = usableTool(toolId);
    if (tool == nullptr) {
        return false;
    }
    if (tool->kind != HeadToolKind::Search) {
        _core->report(tool->path, "head tool " + tool->id + " is a select, which is not submitted");
        return false;
    }
    tool->submit(Value{runtime().untracked([&] { return tool->value.evaluate(); })});
    return true;
}

bool Screen::dirty() const {
    Value const value = detail::evalIn(*_scope, nullptr, _core->doc().dirty);
    bool const* const flag = value.asBool();
    return flag != nullptr && *flag;
}

bool Screen::requestClose() {
    std::shared_ptr<bool const> const cancelled = beginClose();
    return cancelled == nullptr || !*cancelled;
}

std::shared_ptr<bool const> Screen::beginClose() {
    Commands const* const commands = lifecycleList("onClose");
    if (commands == nullptr) {
        return nullptr;
    }
    return runCommandsFlag(*commands);
}

void Screen::runLifecycle(std::string_view key) {
    if (Commands const* const commands = lifecycleList(key)) {
        runCommands(*commands);
    }
}

bool Screen::setPreference(std::string const& point, Value value) {
    auto const found = _core->doc().customize.find(point);
    if (found == _core->doc().customize.end() || !fits(found->second, value)) {
        return false;
    }
    // A point that names a model-backed table edits the table's state, which then says what the point reads.
    if (std::optional<bool> const taken = _core->tablePoint(found->second, value)) {
        if (!*taken) {
            return false;
        }
    }
    ValueRecord prefs = _core->prefs.peek();
    prefs.insert_or_assign(point, std::move(value));
    _core->prefs.set(std::move(prefs));
    return true;
}

std::map<std::string, detail::ScreenCore::KeptState, std::less<>> Screen::ownState() const {
    std::map<std::string, detail::ScreenCore::KeptState, std::less<>> states;
    for (auto const& slot : _scope->states) {
        if (slot->value) {
            states.insert_or_assign(slot->decl->name, detail::ScreenCore::KeptState{
                                                          .type = slot->decl->type.name(),
                                                          .value = slot->value->peek(),
                                                      });
        }
    }
    return states;
}

Commands const* Screen::lifecycleList(std::string_view key) const {
    auto const found = _core->doc().lifecycle.find(key);
    return found == _core->doc().lifecycle.end() ? nullptr : &found->second;
}

std::shared_ptr<bool const> Screen::runCommandsFlag(Commands const& commands) {
    detail::CommandCtx const ctx{
        .scope = _scope,
        .bindings = std::make_shared<detail::Bindings>(),
        .cancelClose = std::make_shared<bool>(false),
        .boundary = nullptr,
    };
    _core->runtime().batch([&] { _core->runList(commands, ctx, {}); });
    return ctx.cancelClose;
}

bool Screen::fits(CustomizePoint const& point, Value const& value) {
    if (point.kind == "choice") {
        std::string const* const text = value.asString();
        return text != nullptr && listed(std::span<std::string const>{point.options}, *text);
    }
    if (point.kind == "value") {
        return point.type.admits(value);
    }
    return true;
}

bool Screen::listed(std::span<std::string const> options, std::string const& text) {
    return std::ranges::find(options, text) != options.end();
}

detail::HeadToolModel const* Screen::usableTool(std::string_view toolId) const {
    auto const found = std::ranges::find(_headTools, toolId, &detail::HeadToolModel::id);
    if (found == _headTools.end()) {
        _core->report("/tools", "head tool " + std::string{toolId} + " is unknown");
        return nullptr;
    }
    auto const [visible, enabled] =
        runtime().untracked([&] { return std::pair{found->visible.evaluate(), found->enabled.evaluate()}; });
    if (!visible || !enabled) {
        _core->report(found->path, "head tool " + found->id + (visible ? " is disabled" : " is hidden"));
        return nullptr;
    }
    return &*found;
}
// NOLINTNEXTLINE(misc-no-recursion): a submenu is a menu one level down.
auto Screen::findHeadEntry(std::vector<detail::HeadMenuModel> const& level, std::string_view entryId,
                           FoundEntry const& outer) -> FoundEntry {
    for (detail::HeadMenuModel const& model : level) {
        FoundEntry const here{
            .model = &model,
            .visible = outer.visible && model.item.visible.evaluate(),
            .enabled = outer.enabled && model.item.enabled.evaluate(),
        };
        if (!model.id.empty() && model.id == entryId) {
            return here;
        }
        if (FoundEntry const inner = findHeadEntry(model.items, entryId, here); inner.model != nullptr) {
            return inner;
        }
    }
    return FoundEntry{};
}
// NOLINTNEXTLINE(misc-no-recursion): a submenu is a menu one level down.
std::vector<HeadMenuEntry> Screen::shownHeadMenu(std::vector<detail::HeadMenuModel> const& level) {
    std::vector<HeadMenuEntry> shown;
    for (detail::HeadMenuModel const& model : level) {
        MenuItem const& item = model.item;
        if (item.separator) {
            if (!shown.empty() && !shown.back().separator) {
                shown.push_back(HeadMenuEntry{.separator = true});
            }
            continue;
        }
        if (!item.visible.evaluate()) {
            continue;
        }
        shown.push_back(HeadMenuEntry{
            .id = model.id,
            .label = item.label.evaluate(),
            .icon = item.icon,
            .keys = item.keys,
            .checked = item.checked.evaluate(),
            .enabled = item.enabled.evaluate(),
            .items = shownHeadMenu(model.items),
            .separator = false,
            .description = item.description.evaluate(),
        });
    }
    if (!shown.empty() && shown.back().separator) {
        shown.pop_back();
    }
    return shown;
}

AppRoot::~AppRoot() {
    try {
        if (auto const found = _core->doc().lifecycle.find("onUnmount"); found != _core->doc().lifecycle.end()) {
            runCommands(found->second);
        }
    } catch (...) {  // NOLINT(bugprone-empty-catch): a destructor has no one to report to.
    }
    // The counts read the scope, so they go before it.
    _navigationBadges.reset();
    _navigation.clear();
    _core->session.setAppScope(nullptr);
    _scope.reset();
}

reactive::Computed<std::vector<NavigationBadge>> const& AppRoot::navigationBadges() const {
    return *_navigationBadges;
}

PrincipalPlacement AppRoot::principalPlacement() const noexcept {
    return _core->doc().principalPlacement == "head" ? PrincipalPlacement::Head : PrincipalPlacement::Sidebar;
}

Value AppRoot::read(Json const& expression) const {
    auto parsed = parseExpression(expression);
    if (!parsed) {
        return Value{};
    }
    return detail::evalIn(*_scope, nullptr, *parsed);
}

auto AppRoot::query(std::string_view name) const -> QueryRead {
    detail::NameRef const* const found = _scope->find(name);
    if (found == nullptr || found->category != detail::NameCategory::Query) {
        return QueryRead{};
    }
    detail::QueryRuntime const& runtime = *found->query;
    Value const error = runtime.error();
    Value const kind = runtime.errorKind();
    std::string const* const kindText = kind.asString();
    return QueryRead{
        .value = runtime.value(),
        .pending = runtime.pending(),
        .failed = !error.isNull(),
        .declared = true,
        .denied = kindText != nullptr && *kindText == "denied",
    };
}

bool AppRoot::setState(std::string_view name, Value value) {
    detail::NameRef const* const found = _scope->find(name);
    if (found == nullptr || found->category != detail::NameCategory::State) {
        return false;
    }
    detail::StateSlot& slot = *found->state;
    slot.scope->core.initState(*slot.scope, slot);
    _core->session.noteWrite();
    slot.value->set(std::move(value));
    return true;
}

bool AppRoot::runMutation(std::string_view name, Value body, std::function<void(bool succeeded)> done) {
    detail::NameRef const* const found = _scope->find(name);
    if (found == nullptr || found->category != detail::NameCategory::Mutation) {
        return false;
    }
    detail::MutationRuntime& mutation = *found->mutation;
    _core->runtime().batch([&] {
        mutation.run(std::move(body), std::make_shared<detail::Bindings>(),
                     [done = std::move(done)](bool succeeded, Value const& /*reply*/) {
                         if (done) {
                             done(succeeded);
                         }
                     });
    });
    return true;
}

void AppRoot::runCommands(Commands const& commands) {
    detail::CommandCtx const ctx{
        .scope = _scope,
        .bindings = std::make_shared<detail::Bindings>(),
        .cancelClose = std::make_shared<bool>(false),
        .boundary = nullptr,
    };
    _core->runtime().batch([&] { _core->runList(commands, ctx, {}); });
}

namespace detail {

ActionRecordInfo const* CacheEntry::holding(std::string_view record) const {
    auto const found = std::ranges::find(records, record, &ActionRecordInfo::record);
    return found == records.end() ? nullptr : &*found;
}

void Marks::add(CacheEntry& entry) {
    if (std::ranges::find(entries, &entry) == entries.end()) {
        entries.push_back(&entry);
    }
}

CheckRuntime::~CheckRuntime() {
    for (ElementState* const target : targets) {
        std::erase(target->checks, this);
        target->checksVersion.set(target->checksVersion.peek() + 1);
    }
}

NameRef const* RtScope::find(std::string_view name) const {
    for (RtScope const* scope = this; scope != nullptr; scope = scope->parent.get()) {
        auto const found = scope->names.find(name);
        if (found != scope->names.end()) {
            return &found->second;
        }
    }
    return nullptr;
}

reactive::Signal<Value> const* RtScope::innermostRow() const {
    for (RtScope const* scope = this; scope != nullptr; scope = scope->parent.get()) {
        if (scope->row != nullptr) {
            return scope->row;
        }
    }
    return nullptr;
}

RtScope const& RtScope::policyScope() const {
    RtScope const* scope = this;
    while (scope->submitted == nullptr && scope->parent != nullptr) {
        scope = scope->parent.get();
    }
    return *scope;
}

Value ScopeEnv::row(std::span<std::string const> path) const {
    if (_row != nullptr) {
        return walk(*_row, path);
    }
    RtScope const* owner = _scope;
    while (owner != nullptr && owner->row == nullptr) {
        owner = owner->parent.get();
    }
    if (owner == nullptr) {
        return Value{};
    }
    Value const& current = owner->row->get();
    // A server-mode row holds the columns fetched: a field it lacks is named, to be fetched after this binding.
    if (owner->onMissingRowField != nullptr && !path.empty() && current.asRecord() != nullptr &&
        current.field(path.front()) == nullptr) {
        (*owner->onMissingRowField)(path.front());
    }
    return walk(current, path);
}

void QueryRuntime::invalidate() {
    if (auto const& current = _entry.peek()) {
        current->stale = true;
    }
    refetch();
}

void QueryRuntime::detach() {
    _entry.set(nullptr);
    _carried.set(Value{});
    _target = {};
}

bool QueryRuntime::Target::operator==(Target const& other) const {
    return idle == other.idle && token == other.token && bodyText == other.bodyText;
}

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

void ScreenCore::runLifecycleOf(std::string_view key) {
    auto const found = _doc->lifecycle.find(key);
    auto const scope = rootScope.lock();
    if (found == _doc->lifecycle.end() || !scope) {
        return;
    }
    CommandCtx const ctx{
        .scope = scope,
        .bindings = std::make_shared<Bindings>(),
        .cancelClose = std::make_shared<bool>(false),
        .boundary = nullptr,
    };
    runList(found->second, ctx, {});
}

void ScreenCore::report(std::string_view path, std::string_view reason) {
    if (inspecting) {
        return;
    }
    std::string key = std::string{path} + '\x1f' + std::string{reason};
    if (!reported.insert(std::move(key)).second) {
        return;
    }
    Refusal const refusal{.document = _doc->id, .path = std::string{path}, .reason = std::string{reason}};
    diagnostics.push_back(refusal);
    if (onDiagnostic) {
        onDiagnostic(refusal);
    }
}

void ScreenCore::initState(RtScope const& scope, StateSlot& slot) const {
    if (slot.value || slot.initializing) {
        return;
    }
    if (std::optional<Value> keptValue = keptState(scope, *slot.decl)) {
        slot.value = std::make_unique<reactive::Signal<Value>>(runtime(), std::move(*keptValue));
        return;
    }
    slot.initializing = true;
    Value init = runtime().untracked([&] { return evalIn(scope, nullptr, slot.decl->init); });
    slot.value = std::make_unique<reactive::Signal<Value>>(runtime(), std::move(init));
    slot.initializing = false;
}

void ScreenCore::runEvent(Commands const* commands, LowerCtx const& ctx, Value event) {
    if (commands == nullptr || commands->empty()) {
        return;
    }
    auto bindings = std::make_shared<Bindings>(*ctx.bindings);
    bindings->push_back(Binding{.name = "event", .read = [event = std::move(event)] { return event; }});
    CommandCtx const command{
        .scope = ctx.scope,
        .bindings = std::move(bindings),
        .cancelClose = std::make_shared<bool>(false),
        .boundary = ctx.boundary,
    };
    session.asUser([&] { runtime().batch([&] { runList(*commands, command, {}); }); });
}

std::optional<Value> ScreenCore::keptState(RtScope const& scope, StateDecl const& decl) const {
    if (keptStates.empty() || rootScope.lock().get() != &scope) {
        return std::nullopt;
    }
    auto const found = keptStates.find(decl.name);
    if (found == keptStates.end() || found->second.type != decl.type.name() ||
        !decl.type.admits(found->second.value)) {
        return std::nullopt;
    }
    return found->second.value;
}

bool ScreenCore::unsupported(Command const& command, std::string_view what) {
    report(command.path, std::string{what} + " is not supported by the host");
    return false;
}

Value ScreenCore::once(LowerCtx const& ctx, ExprPtr const& expr) const {
    return runtime().untracked([&] { return evalIn(*ctx.scope, ctx.bindings.get(), expr); });
}

void ScreenCore::logList(Commands const& commands) {
    CommandLogEntry entry{.event = {}, .commands = {}, .batch = runtime().core()->currentBatch()};
    std::string_view const first = commands.front().path;
    entry.event = std::string{first.substr(0, first.rfind('/') == std::string_view::npos ? 0 : first.rfind('/'))};
    for (Command const& command : commands) {
        entry.commands.push_back(
            LoggedCommand{.kind = commandKey(command.kind), .target = command.target, .path = command.path});
    }
    if (commandLog.size() == kCommandLogSize) {
        commandLog.pop_front();
    }
    commandLog.push_back(std::move(entry));
}

}  // namespace detail

void Session::backendChanged(std::string const& backend) {
    _runtime->batch([this, &backend] {
        std::vector<detail::ScreenCore*> const screens = _screens;
        for (detail::ScreenCore* const screen : screens) {
            if (std::ranges::find(_screens, screen) != _screens.end()) {
                screen->runLifecycleOf("onBackendChange");
            }
        }
        ValueRecord env = _env.peek();
        env.insert_or_assign("backend", Value{backend});
        _env.set(std::move(env));
        for (auto const& [key, entry] : _entries) {
            static_cast<void>(key);
            markStale(*entry);
        }
        std::vector<detail::QueryRuntime*> const mounted = _mounted;
        for (detail::QueryRuntime* const query : mounted) {
            query->refetchCurrent();
        }
    });
}

void Session::refetchMounted(detail::CacheEntry const& entry) {
    std::vector<detail::QueryRuntime*> const mounted = _mounted;
    for (detail::QueryRuntime* const query : mounted) {
        if (_runtime->untracked([query] { return query->entry(); }).get() == &entry) {
            query->refetchCurrent();
        }
    }
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a model type and an action id, as every caller names them.
void Session::applyReplyEffects(std::string const& typeId, std::string const& actionId, Value const& reply,
                                ReplyUse use, std::string const& instanceToken) {
    if (!_options.catalog.action) {
        return;
    }
    std::optional<ActionInfo> const info = _options.catalog.action(actionId);
    if (!info || !info->effects) {
        return;
    }
    detail::Marks marks;
    for (ActionEffect const& effect : *info->effects) {
        if (effect.kind == "changesInstance") {
            for (auto const& [key, entry] : _entries) {
                static_cast<void>(key);
                if (!entry->evaluate && entry->typeId == typeId && entry->instanceToken == instanceToken) {
                    marks.add(*entry);
                }
            }
        } else if (effect.record && (effect.kind == "updates" || effect.kind == "changes")) {
            applyRecordEffect(effect, *effect.record, *info, reply, use, marks);
        }
    }
    markAndRefetch(marks);
}

void Session::applyRecordEffect(ActionEffect const& effect, std::string const& record, ActionInfo const& info,
                                Value const& reply, ReplyUse use, detail::Marks& marks) {
    // The reply's rows of the record, by key; none when the reply does not hold it keyed, or must only mark.
    auto const own = std::ranges::find(info.records, record, &ActionRecordInfo::record);
    std::optional<std::string> const ownKey =
        effect.kind == "updates" && use == ReplyUse::Applied && own != info.records.end() ? own->keyField
                                                                                          : std::nullopt;
    detail::RowsByKey rows;
    if (ownKey) {
        for (Value const* const row : detail::keyedRowsOf(reply, own->paths, *ownKey)) {
            rows.insert_or_assign(detail::canonicalJson(*row->field(*ownKey)), row);
        }
    }
    for (auto const& [key, entry] : _entries) {
        static_cast<void>(key);
        ActionRecordInfo const* const held = entry->holding(record);
        if (held == nullptr) {
            continue;
        }
        // A merge replaces rows by key: it cannot find an opaque entry's rows, nor update what an open or partial
        // member says about them by position, so such an entry is marked and fetched again.
        bool const unmergeable = held->opaque || detail::anyMemberSet(entry->confirmed, held->open) ||
                                 detail::anyMemberSet(entry->confirmed, held->partial);
        if (!ownKey || !held->keyField || unmergeable) {
            marks.add(*entry);
            continue;
        }
        if (auto patched = detail::patchHeldRows(entry->confirmed, held->paths, *held->keyField, rows)) {
            confirm(*entry, *patched);
        }
        // A fetch in flight may have read before the write, and would settle over the patch.
        if (entry->pending.peek()) {
            markStale(*entry);
        }
    }
}

void Session::markAndRefetch(detail::Marks const& marks) {
    for (detail::CacheEntry* const entry : marks.entries) {
        markStale(*entry);
    }
    for (detail::CacheEntry const* const entry : marks.entries) {
        refetchMounted(*entry);
    }
}

void Session::countTopics(detail::CacheEntry const& entry, int delta) {
    if (entry.evaluate) {
        return;
    }
    // One topic: its count, and the queues a change of the count from or to zero goes to.
    auto const count = [delta](auto& refs, auto const& topic, auto& watches, auto& unwatches) {
        int& refsOf = refs[topic];
        refsOf += delta;
        if (delta > 0 && refsOf == 1 && unwatches.erase(topic) == 0) {
            watches.insert(topic);
        } else if (refsOf <= 0) {
            refs.erase(topic);
            if (watches.erase(topic) == 0) {
                unwatches.insert(topic);
            }
        }
    };
    if (!entry.records.empty()) {
        for (ActionRecordInfo const& record : entry.records) {
            countRecord(record.record, delta);
        }
        return;
    }
    constexpr std::string_view kShared = "shared:";
    if (entry.instanceToken.starts_with(kShared)) {
        count(_instanceRefs, std::pair{entry.typeId, entry.instanceToken.substr(kShared.size())}, _watchInstances,
              _unwatchInstances);
    }
}

void Session::countRecord(std::string const& record, int delta) {
    int& refs = _recordRefs[record];
    refs += delta;
    if (delta > 0 && refs == 1 && _unwatchRecords.erase(record) == 0) {
        _watchRecords.insert(record);
    } else if (refs <= 0) {
        _recordRefs.erase(record);
        if (_watchRecords.erase(record) == 0) {
            _unwatchRecords.insert(record);
        }
    }
}

void Session::sendWatch(std::string_view kind, wire::WatchBody const& body) const {
    auto completion = _options.watcher(std::string{kind}, glz::write_json(body).value_or(std::string{}));
    if (kind != wire::kWatchKind || body.records.empty()) {
        completion.then([](std::string const&) {}).onError([](std::exception_ptr const&) {});
        return;
    }
    // The records' watch is in flight until its answer: a `watchRecord` for one of them resolves then.
    std::weak_ptr<detail::WatchRegistry> const weak = _watchRegistry;
    if (_watchRegistry) {
        _watchRegistry->inFlight.insert(body.records.begin(), body.records.end());
    }
    auto const answered = [weak, records = body.records] {
        if (auto const registry = weak.lock()) {
            registry->answered(records);
        }
    };
    completion.then([answered](std::string const&) { answered(); })
        .onError([answered](std::exception_ptr const&) { answered(); });
}

void Session::flushWatches() {
    if (!_options.watcher) {
        _watchRecords.clear();
        _unwatchRecords.clear();
        _watchInstances.clear();
        _unwatchInstances.clear();
        return;
    }
    auto const take = [](auto& records, auto& instances) {
        wire::WatchBody body;
        body.records.assign(records.begin(), records.end());
        for (auto const& [modelType, primary] : instances) {
            body.instances.push_back(wire::InstanceTopic{.modelType = modelType, .primary = primary, .instance = 0});
        }
        records.clear();
        instances.clear();
        return body;
    };
    if (!_watchRecords.empty() || !_watchInstances.empty()) {
        static_cast<void>(watchRegistry());
        sendWatch(wire::kWatchKind, take(_watchRecords, _watchInstances));
    }
    if (!_unwatchRecords.empty() || !_unwatchInstances.empty()) {
        sendWatch(wire::kUnwatchKind, take(_unwatchRecords, _unwatchInstances));
    }
}

wire::WatchBody Session::watchSet() const {
    wire::WatchBody body;
    for (auto const& [record, refs] : _recordRefs) {
        static_cast<void>(refs);
        body.records.push_back(record);
    }
    for (auto const& [topic, refs] : _instanceRefs) {
        static_cast<void>(refs);
        body.instances.push_back(
            wire::InstanceTopic{.modelType = topic.first, .primary = topic.second, .instance = 0});
    }
    return body;
}

void Session::markAllStale() {
    _runtime->batch([this] {
        detail::Marks marks;
        for (auto const& [key, entry] : _entries) {
            static_cast<void>(key);
            if (!entry->evaluate) {
                marks.add(*entry);
            }
        }
        markAndRefetch(marks);
    });
}

void Session::reconnected() {
    // The whole set replaces what the server holds, so the queued changes are superseded.
    _watchRecords.clear();
    _unwatchRecords.clear();
    _watchInstances.clear();
    _unwatchInstances.clear();
    if (_options.watcher) {
        wire::WatchBody body = watchSet();
        body.replace = true;
        sendWatch(wire::kWatchKind, body);
    }
    markAllStale();
    if (_watchRegistry) {
        std::set<std::string, std::less<>> records;
        for (auto const& [id, watch] : _watchRegistry->watches) {
            static_cast<void>(id);
            records.insert(watch.record);
        }
        for (std::string const& record : records) {
            notifyWatchers(effects::RecordChange{.record = record, .keys = std::nullopt});
        }
    }
    std::erase_if(_reconnectListeners, [](auto const& listener) { return listener.expired(); });
    std::vector<std::shared_ptr<std::function<void()>>> listeners;
    for (auto const& listener : _reconnectListeners) {
        if (auto held = listener.lock()) {
            listeners.push_back(std::move(held));
        }
    }
    for (auto const& listener : listeners) {
        (*listener)();
    }
}

std::shared_ptr<void> Session::onReconnected(std::function<void()> handler) {
    auto held = std::make_shared<std::function<void()>>(std::move(handler));
    std::erase_if(_reconnectListeners, [](auto const& listener) { return listener.expired(); });
    _reconnectListeners.push_back(held);
    return held;
}

void Session::deliverEvent(wire::EventBody const& event) {
    if (event.topic == wire::kResyncTopic) {
        reconnected();
        return;
    }
    if (event.topic != wire::kChangesTopic) {
        return;
    }
    if (auto changes = wire::readChangesBody(event.payload.str)) {
        applyChanges(*changes);
    }
}

void Session::applyChanges(wire::ChangesBody const& changes) {
    _runtime->batch([&] {
        detail::Marks marks;
        for (effects::RecordChange const& change : changes.records) {
            markRecordChange(change, marks);
        }
        for (wire::InstanceTopic const& instance : changes.instances) {
            markInstanceChange(instance, marks);
        }
        markAndRefetch(marks);
    });
    for (effects::RecordChange const& change : changes.records) {
        notifyWatchers(change);
    }
}

void Session::notifyWatchers(effects::RecordChange const& change) {
    if (!_watchRegistry) {
        return;
    }
    // A handler may drop its own watch, or another, so the handlers are copied out first.
    std::vector<std::function<void(effects::RecordChange const&)>> handlers;
    for (auto const& [id, watch] : _watchRegistry->watches) {
        static_cast<void>(id);
        if (watch.record == change.record) {
            handlers.push_back(watch.handler);
        }
    }
    for (auto const& handler : handlers) {
        handler(change);
    }
}

detail::WatchRegistry& Session::watchRegistry() {
    if (!_watchRegistry) {
        _watchRegistry = std::make_shared<detail::WatchRegistry>();
        _watchRegistry->session = this;
    }
    return *_watchRegistry;
}

async::Completion<WatchHolder> Session::watchRecord(std::string const& record,
                                                    std::function<void(effects::RecordChange const&)> handler) {
    auto& registry = watchRegistry();
    auto const id = ++registry.next;
    registry.watches.emplace(id, detail::WatchRegistry::Watch{.record = record, .handler = std::move(handler)});
    auto registration = std::make_shared<detail::WatchRegistration>();
    registration->registry = _watchRegistry;
    registration->id = id;
    registration->record = record;
    WatchHolder holder{std::move(registration)};
    countRecord(record, +1);
    flushWatches();
    auto [completion, promise] = async::Completion<WatchHolder>::makeSettleable(&_runtime->owner());
    if (registry.inFlight.contains(record)) {
        registry.waiters.push_back(
            detail::WatchRegistry::Waiter{.record = record, .promise = std::move(promise), .holder = std::move(holder)});
    } else {
        promise.resolve(std::move(holder));
    }
    return std::move(completion);
}

async::Completion<std::string> Session::read(std::shared_ptr<ModelChannel> const& channel, std::string const& actionId,
                                             Value const& body, ::core::async::StopToken stop) {
    using Promise = async::Completion<std::string>::Promise;
    auto [completion, promise] = async::Completion<std::string>::makeSettleable(&_runtime->owner());
    auto const settle = std::make_shared<Promise>(std::move(promise));
    if (channel == nullptr) {
        settle->reject(std::make_exception_ptr(std::runtime_error{"the model alias has no instance"}));
        return std::move(completion);
    }
    if (stop.stop_requested()) {
        settle->reject(std::make_exception_ptr(::core::async::OperationCancelled{}));
        return std::move(completion);
    }
    // A stop settles the read at once; the reply that may still come is dropped, since the first settlement wins.
    using OnStop = ::core::async::StopCallback<std::function<void()>>;
    std::shared_ptr<OnStop> onStop;
    if (stop.stop_possible()) {
        onStop = std::make_shared<OnStop>(stop, std::function<void()>{[settle] {
                                              settle->reject(
                                                  std::make_exception_ptr(::core::async::OperationCancelled{}));
                                          }});
    }
    try {
        auto sent = channel->execute(actionId, detail::canonicalJson(body),
                                     CallOptions{.evaluate = false, .queueOffline = false, .stop = stop});
        sent.then([settle, onStop, stop](std::string const& reply) {
                if (!stop.stop_requested()) {
                    settle->resolve(reply);
                }
            })
            .onError([settle, onStop](std::exception_ptr const& error) { settle->reject(error); });
    } catch (...) {
        settle->reject(std::current_exception());
    }
    return std::move(completion);
}

void Session::markRecordChange(effects::RecordChange const& change, detail::Marks& marks) {
    // The keys' canonical text, so a key the server wrote is compared with the rows' own.
    std::set<std::string, std::less<>> keys;
    for (glz::raw_json const& key : change.keys.value_or(std::vector<glz::raw_json>{})) {
        auto json = parseJson(key.str);
        auto value = json ? valueFromJson(*json) : std::unexpected(json.error());
        if (value) {
            keys.insert(detail::canonicalJson(*value));
        }
    }
    for (auto const& [name, entry] : _entries) {
        static_cast<void>(name);
        ActionRecordInfo const* const held = entry->holding(change.record);
        if (held == nullptr) {
            continue;
        }
        // An entry still waiting for its first value may come to hold a key; keyless rows cannot be told apart. An
        // opaque entry's rows cannot be found by key, and while an open member is set a row the entry does not hold
        // may come to belong to it.
        if (!change.keys || !held->keyField || !entry->hasValue.peek() || held->opaque ||
            detail::anyMemberSet(entry->confirmed, held->open)) {
            marks.add(*entry);
            continue;
        }
        std::string const& keyField = *held->keyField;
        bool const holdsKey = std::ranges::any_of(
            detail::keyedRowsOf(entry->confirmed, held->paths, keyField),
            [&](Value const* row) { return keys.contains(detail::canonicalJson(*row->field(keyField))); });
        if (holdsKey) {
            marks.add(*entry);
        }
    }
}

void Session::markInstanceChange(wire::InstanceTopic const& instance, detail::Marks& marks) {
    std::string const shared = "shared:" + instance.primary;
    for (auto const& [name, entry] : _entries) {
        static_cast<void>(name);
        if (entry->evaluate || entry->typeId != instance.modelType) {
            continue;
        }
        // Without a primary key, the instance is one of the connection's own private ones.
        bool const same =
            instance.primary.empty() ? !entry->instanceToken.starts_with("shared") : entry->instanceToken == shared;
        if (same) {
            marks.add(*entry);
        }
    }
}

void Session::actionSucceeded(std::string const& typeId, std::string const& actionId, Value const& reply, ReplyUse use,
                              std::string const& instanceToken) {
    _runtime->batch([&] { applyReplyEffects(typeId, actionId, reply, use, instanceToken); });
}

void Session::detachQuery(detail::QueryRuntime& query) { query.detach(); }

namespace detail {

[[nodiscard]] std::string instanceKeyText(Value const& key) {
    if (key.isNull()) {
        return {};
    }
    if (std::int64_t const* const number = key.asInt()) {
        return std::to_string(*number);
    }
    if (std::string const* const text = key.asString()) {
        return *text;
    }
    return canonicalJson(key);
}

namespace {

/// @brief The channel an alias is bound to for its current token, binding it when the token changed.
/// @param alias The alias.
/// @param core  The screen.
/// @return The channel, or `nullptr` while the alias is unbound.
[[nodiscard]] std::shared_ptr<ModelChannel> channelOf(ModelAlias& alias, ScreenCore& core) {
    std::string const token = core.runtime().untracked([&] { return alias.token->get(); });
    if (token.empty()) {
        return nullptr;
    }
    if (alias.channel && alias.boundToken == token) {
        return alias.channel;
    }
    std::string const& type = alias.decl->type;
    switch (alias.mode) {
        case InstanceMode::Borrowed: {
            auto const found = core.borrowed.find(alias.decl->name);
            alias.channel = found == core.borrowed.end() ? nullptr : found->second.channel;
            break;
        }
        case InstanceMode::Shared:
            // `shared` is for a stateless model, whose instances are interchangeable: its calls go to an instance of
            // the alias's own, and what it shares with other screens is the query cache, through the token.
            alias.channel = core.dispatcher.bind(type, bridge::BindSharing::Private, {});
            break;
        case InstanceMode::Keyed:
            alias.channel = core.dispatcher.bind(type, bridge::BindSharing::Shared, token.substr(7));
            break;
        case InstanceMode::Private:
        case InstanceMode::Default:
        default:
            alias.channel = core.dispatcher.bind(type, bridge::BindSharing::Private, {});
            break;
    }
    alias.boundToken = token;
    return alias.channel;
}
}  // namespace

RtScope::RtScope(ScreenCore& screen, std::shared_ptr<RtScope> enclosing, reactive::Signal<Value> const* current,
                 std::size_t depth)
    : core{screen}, parent{std::move(enclosing)}, row{current}, nodes{screen.runtime(), depth} {
    if (core.recording) {
        core.scopes.push_back(this);
    }
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

// NOLINTNEXTLINE(misc-no-recursion): an element's value can read a nested scope's readiness, one level per nesting.
bool RtScope::ready() const {
    for (auto const& element : elements) {
        Value const shown = core.elementValue(*element, "visible");
        bool const* const visible = shown.asBool();
        ValueList const* const errors = element->errors->get().asList();
        if ((visible == nullptr || *visible) && errors != nullptr && !errors->empty()) {
            return false;
        }
    }
    for (auto const& check : checks) {
        if (check->decl->blocking && check->failing->get()) {
            return false;
        }
    }
    return true;
}

// NOLINTNEXTLINE(misc-no-recursion): an `app.` name is read in the root scope with its head consumed, once per segment.
Value ScopeEnv::ref(std::string_view head, std::span<std::string const> path) const {
    if (head == "app") {
        RtScope const* const app = _scope->core.session.appScope();
        if (app == nullptr || path.empty()) {
            return Value{};
        }
        return ScopeEnv{*app, nullptr}.ref(path.front(), path.subspan(1));
    }
    if (_bindings != nullptr) {
        for (Binding const& bound : std::views::reverse(*_bindings)) {
            if (bound.name == head) {
                return walk(bound.read(), path);
            }
        }
    }
    NameRef const* const name = _scope->find(head);
    if (name == nullptr) {
        return Value{};
    }
    switch (name->category) {
        case NameCategory::Param:
            return walk(name->param, path);
        case NameCategory::State:
            _scope->core.initState(*name->state->scope, *name->state);
            // State inits are acyclic (`loadDocument` checks it), so the value is made by now.
            return walk(name->state->value->get(), path);
        case NameCategory::Let:
            return walk(name->let->get(), path);
        case NameCategory::Query:
            return walk(name->query->value(), path);
        case NameCategory::Element:
            if (path.empty()) {
                return Value{};
            }
            return walk(_scope->core.elementValue(*name->element, path.front()), path.subspan(1));
        case NameCategory::Transfer: {
            if (path.empty()) {
                return Value{};
            }
            TransferState const& transfer = *name->transfer;
            Value value;
            if (path.front() == "progress") {
                value = transfer.progress.get();
            } else if (path.front() == "pending") {
                value = Value{transfer.pending.get()};
            } else if (path.front() == "error") {
                value = transfer.error.get();
            }
            return walk(value, path.subspan(1));
        }
        case NameCategory::Model:
        case NameCategory::Mutation:
        default:
            return Value{};
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one case per control form and kind of name.
Value ScopeEnv::control(ExprKind form, std::string_view name) const {
    Session const& session = _scope->core.session;
    NameRef const* const found = _scope->find(name);
    if (form == ExprKind::Can) {
        if (found != nullptr && found->category == NameCategory::Query) {
            return session.can(found->query->decl().action);
        }
        if (found != nullptr && found->category == NameCategory::Mutation) {
            return session.can(found->mutation->decl().action);
        }
        return session.can(name);
    }
    if (found != nullptr && found->category == NameCategory::Query) {
        QueryRuntime const& query = *found->query;
        if (form == ExprKind::Query) {
            return query.value();
        }
        if (form == ExprKind::Pending) {
            return query.pending();
        }
        if (form == ExprKind::Error) {
            return query.error();
        }
        if (form == ExprKind::Refusal) {
            return query.refusal();
        }
        return form == ExprKind::ErrorKind ? query.errorKind() : Value{};
    }
    if (found != nullptr && found->category == NameCategory::Mutation) {
        MutationRuntime const& mutation = *found->mutation;
        if (form == ExprKind::Result) {
            return mutation.result();
        }
        if (form == ExprKind::Pending) {
            return mutation.pending();
        }
        if (form == ExprKind::Error) {
            return mutation.error();
        }
        if (form == ExprKind::Refusal) {
            return mutation.refusal();
        }
        return form == ExprKind::ErrorKind ? mutation.errorKind() : Value{};
    }
    return Value{};
}

Value ScopeEnv::pref(std::string_view point, std::span<std::string const> path) const {
    ValueRecord const& prefs = _scope->core.prefs.get();
    auto const found = prefs.find(point);
    return found == prefs.end() ? Value{} : walk(found->second, path);
}

Value ScopeEnv::client(ExprKind form, std::string_view name) const { return _scope->core.session.client(form, name); }

FormatContext const& RtScope::formatting() const {
    for (RtScope const* scope = this; scope != nullptr;
         scope = scope->parent != nullptr ? scope->parent.get() : scope->formatSource.get()) {
        if (scope->localeFormat != nullptr) {
            return *scope->localeFormat->get();
        }
    }
    return core.session.formatting();
}

FormatContext const& ScopeEnv::formatting() const { return _scope->formatting(); }

FormatContext const& ScopeEnv::entryFormatting() const { return _scope->core.session.formatting(); }

void ScopeEnv::report(std::string_view path, std::string_view reason) const { _scope->core.report(path, reason); }

// --- Queries ---------------------------------------------------------------------------------------------------

QueryRuntime::QueryRuntime(QueryDecl const& decl, RtScope& scope, ModelAlias* alias)
    : _decl{&decl},
      _typeId{alias->decl->type},
      _scope{&scope},
      _alias{alias},
      _entry{scope.core.runtime()},
      _carried{scope.core.runtime()},
      _waiting{scope.core.runtime(), false} {
    scope.core.session.attachQuery(*this);
}

QueryRuntime::~QueryRuntime() {
    _debounce.cancel();
    _refresh.cancel();
    _scope->core.session.forgetQuery(*this);
    if (auto const& current = _entry.peek()) {
        _scope->core.session.release(current);
    }
}

std::shared_ptr<ModelChannel> QueryRuntime::channel() const { return channelOf(*_alias, _scope->core); }

// NOLINTNEXTLINE(readability-function-cognitive-complexity): the query's key, its fetch and its refresh triggers.
void QueryRuntime::start() {
    RtScope& scope = *_scope;
    auto& key = scope.nodes.computed([this] -> Target {
        Target target;
        if (_decl->when) {
            Value const when = evalIn(*_scope, nullptr, _decl->when);
            bool const* const flag = when.asBool();
            if (flag == nullptr || !*flag) {
                return target;
            }
        }
        std::string token = _alias->token->get();
        if (token.empty()) {
            return target;
        }
        Value body = _decl->body ? evalIn(*_scope, nullptr, _decl->body) : Value{ValueRecord{}};
        if (body.isNull()) {
            return target;
        }
        target.idle = false;
        target.token = std::move(token);
        target.bodyText = canonicalJson(body);
        target.body = std::move(body);
        return target;
    });
    _key = &key;
    reactive::Scheduler* const scheduler = scope.core.session.options().scheduler;
    std::weak_ptr<bool> const alive = _alive;
    scope.nodes.effect([this, &key, scheduler, alive, first = true] mutable {
        Target target = key.get();
        _scope->core.runtime().untracked([&] {
            if (first || target.idle || _decl->debounce <= 0 || scheduler == nullptr) {
                first = false;
                _debounce.cancel();
                _waiting.set(false);
                switchTo(target);
                return;
            }
            _pendingTarget = std::move(target);
            _waiting.set(true);
            _debounce = scheduler->after(std::chrono::milliseconds{_decl->debounce}, [this, alive] {
                if (alive.expired()) {
                    return;
                }
                _scope->core.runtime().batch([this] {
                    _waiting.set(false);
                    switchTo(_pendingTarget);
                });
            });
        });
    });
    if (_decl->refreshEvery > 0 && scheduler != nullptr) {
        _refresh = scheduler->every(std::chrono::milliseconds{_decl->refreshEvery}, [this, alive] {
            if (alive.expired()) {
                return;
            }
            auto const current = _entry.peek();
            if (!current || _waiting.peek() || current->pending.peek()) {
                return;
            }
            _scope->core.runtime().batch([&] { _scope->core.session.fetch(current, channel()); });
        });
    }
}

void QueryRuntime::switchTo(Target const& target) {
    Session& session = _scope->core.session;
    std::shared_ptr<CacheEntry> const old = _entry.peek();
    if (old && target == _target) {
        return;
    }
    Value shown = old && old->hasValue.peek() ? old->value.peek() : _carried.peek();
    _target = target;
    if (target.idle) {
        _carried.set(Value{});
        _entry.set(nullptr);
        if (old) {
            session.release(old);
        }
        return;
    }
    auto const fresh = session.acquire(target.token, _typeId, _decl->action, _decl->evaluate, target.body);
    _carried.set(std::move(shown));
    _entry.set(fresh);
    if (old) {
        session.release(old);
    }
    bool const young = fresh->fetchedAt.has_value() &&
                       session.now() - *fresh->fetchedAt < std::chrono::milliseconds{_decl->staleTime};
    if (!fresh->pending.peek() && (!fresh->hasValue.peek() || fresh->stale || !young)) {
        session.fetch(fresh, channel());
    }
}

void QueryRuntime::refetch() {
    _scope->core.runtime().batch([this] {
        bool switched = false;
        if (_waiting.peek()) {
            _debounce.cancel();
            _waiting.set(false);
            switchTo(_pendingTarget);
            switched = true;
        }
        auto const current = _entry.peek();
        if (current && !(switched && current->pending.peek())) {
            _scope->core.session.fetch(current, channel());
        }
    });
}

void QueryRuntime::refetchCurrent() {
    if (_key == nullptr) {
        refetch();
        return;
    }
    _scope->core.runtime().batch([this] {
        _debounce.cancel();
        _waiting.set(false);
        Target const target = _key->peek();
        if (!(_entry.peek() && target == _target)) {
            switchTo(target);
            return;
        }
        if (auto const current = _entry.peek(); current && !current->pending.peek()) {
            _scope->core.session.fetch(current, channel());
        }
    });
}

Value QueryRuntime::value() const {
    auto const& current = _entry.get();
    if (current) {
        if (current->hasValue.get()) {
            return current->value.get();
        }
        if (!current->error.get().empty()) {
            return Value{};
        }
    }
    return _carried.get();
}

bool QueryRuntime::pending() const {
    if (_waiting.get()) {
        return true;
    }
    auto const& current = _entry.get();
    return current && current->pending.get();
}

Value QueryRuntime::error() const {
    auto const& current = _entry.get();
    if (!current || current->error.get().empty()) {
        return Value{};
    }
    return Value{current->error.get()};
}

Value QueryRuntime::errorKind() const {
    auto const& current = _entry.get();
    if (!current || current->errorKind.get().empty()) {
        return Value{};
    }
    return Value{current->errorKind.get()};
}

Value QueryRuntime::refusal() const {
    auto const& current = _entry.get();
    return current ? current->refusal.get() : Value{};
}

bool QueryRuntime::settled() const {
    auto const& current = _entry.get();
    return !current || current->hasValue.get() || !current->error.get().empty();
}

}  // namespace detail

// --- Rows an action works on ----------------------------------------------------------------------------------

std::vector<TableInput> tableInputsOf(std::string_view schemaJson) {
    std::vector<TableInput> out;
    glz::generic_u64 schema;
    if (glz::read_json(schema, schemaJson) || !schema.is_object()) {
        return out;
    }
    auto const& members = schema.get_object();
    auto const properties = members.find("properties");
    if (properties == members.end() || !properties->second.is_object()) {
        return out;
    }
    for (auto const& [name, property] : properties->second.get_object()) {
        if (!property.is_object()) {
            continue;
        }
        auto const& keywords = property.get_object();
        for (auto const& [keyword, kind] : {std::pair{std::string_view{"x-selectionOf"}, TableInputKind::Selection},
                                            std::pair{std::string_view{"x-rowOf"}, TableInputKind::Row}}) {
            auto const found = keywords.find(std::string{keyword});
            if (found != keywords.end() && found->second.is_string()) {
                out.push_back(TableInput{.member = name, .record = found->second.get_string(), .kind = kind});
            }
        }
    }
    return out;
}

namespace detail {

// --- Mutations -------------------------------------------------------------------------------------------------

MutationRuntime::MutationRuntime(MutationDecl const& decl, RtScope& scope, ModelAlias* alias)
    : _decl{&decl},
      _scope{&scope},
      _alias{alias},
      _typeId{alias->decl->type},
      _core{&scope.core},
      _inFlight{scope.core.runtime(), 0},
      _result{scope.core.runtime()},
      _error{scope.core.runtime()},
      _errorKind{scope.core.runtime()},
      _refusal{scope.core.runtime()} {}

MutationRuntime::~MutationRuntime() {
    *_alive = false;
    // A run whose reply will now never be read is no longer pending.
    if (int const running = _inFlight.peek(); running > 0) {
        _core->addPending(-running);
    }
}

// NOLINTBEGIN(misc-no-recursion): a mutation's commands can run another mutation, and a serial queue sends its next
// run as the last one settles.
void MutationRuntime::run(Value body, std::shared_ptr<Bindings const> bindings, RunDone done) {
    auto const run = std::make_shared<Run>();
    run->body = std::move(body);
    run->bindings = std::move(bindings);
    run->done = std::move(done);
    if (_decl->concurrency == Concurrency::Exclusive && _inFlight.peek() > 0) {
        run->done(false, Value{});
        return;
    }
    _inFlight.set(_inFlight.peek() + 1);
    _core->addPending(1);
    if (_decl->concurrency == Concurrency::Serial) {
        // The key reads the run's body, so a run keys the same whichever command started it.
        Bindings withBody = *run->bindings;
        withBody.push_back(Binding{.name = "body", .read = [body = run->body] { return body; }});
        Value const key = evalIn(*_scope, &withBody, _decl->serialKey);
        run->serialKey = key.isNull() ? std::string{} : canonicalJson(key);
        if (_busy.contains(run->serialKey)) {
            _queues[run->serialKey].push_back(run);
            return;
        }
        _busy.insert(run->serialKey);
    }
    send(run);
}

void MutationRuntime::send(std::shared_ptr<Run> const& run) {
    run->id = ++_runs;
    _latest = run->id;
    run->sessionRun = _core->session.nextRunId();
    for (RunObserver* const observer : _core->session.runObservers()) {
        observer->started(RunStarted{.run = run->sessionRun, .action = _decl->action, .body = run->body});
    }
    applyOptimistic(*run);
    std::shared_ptr<ModelChannel> const channel = channelOf(*_alias, _scope->core);
    if (channel == nullptr) {
        settle(run, std::nullopt, std::make_exception_ptr(std::runtime_error{"the model alias has no instance"}));
        return;
    }
    std::weak_ptr<bool> const alive = _alive;
    try {
        auto completion = channel->execute(_decl->action, canonicalJson(run->body),
                                           CallOptions{.queueOffline = _decl->offlineQueue});
        completion
            .then([this, alive, run](std::string const& reply) {
                if (alive.expired()) {
                    return;
                }
                auto json = parseJson(reply);
                auto value = json ? valueFromJson(*json) : std::unexpected(json.error());
                if (!value) {
                    settle(run, std::nullopt,
                           std::make_exception_ptr(
                               std::runtime_error{"the reply is not a document value: " + value.error()}));
                    return;
                }
                settle(run, std::move(*value), nullptr);
            })
            .onError([this, alive, run](std::exception_ptr const& error) {
                if (!alive.expired()) {
                    settle(run, std::nullopt, error);
                }
            });
    } catch (...) {
        settle(run, std::nullopt, std::current_exception());
    }
}

void MutationRuntime::applyOptimistic(Run& run) {
    if (!_decl->optimistic) {
        return;
    }
    NameRef const* const target = _scope->find(_decl->optimistic->query);
    if (target == nullptr || target->category != NameCategory::Query) {
        return;
    }
    auto entry = _scope->core.runtime().untracked([&] { return target->query->entry(); });
    if (!entry) {
        return;
    }
    Bindings bindings = *run.bindings;
    bindings.push_back(Binding{.name = "body", .read = [body = run.body] { return body; }});
    Value key = _scope->core.runtime().untracked([&] { return evalIn(*_scope, &bindings, _decl->optimistic->key); });
    Value with = _scope->core.runtime().untracked([&] { return evalIn(*_scope, &bindings, _decl->optimistic->with); });
    // Before a value has arrived there is no list to miss.
    if (entry->hasValue.peek() &&
        patchRows(entry->confirmed, _decl->optimistic->at, key, with).error_or(PatchMiss::NoRow) ==
            PatchMiss::NoList) {
        _scope->core.report(pointerAppend(_decl->path, "optimistic"),
                            patchMissReason(_decl->optimistic->query, _decl->optimistic->at));
    }
    entry->layers.push_back(
        OptimisticLayer{.run = run.id, .at = _decl->optimistic->at, .key = std::move(key), .with = std::move(with)});
    _scope->core.session.showLayers(*entry);
    run.layered.push_back(std::move(entry));
}

void MutationRuntime::dropLayers(Run& run) {
    for (auto const& entry : run.layered) {
        std::erase_if(entry->layers, [&](OptimisticLayer const& layer) { return layer.run == run.id; });
        _scope->core.session.showLayers(*entry);
    }
    run.layered.clear();
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one branch per outcome a reply can have.
void MutationRuntime::settle(std::shared_ptr<Run> const& run, std::optional<Value> reply,
                             std::exception_ptr const& error) {
    ScreenCore& core = _scope->core;
    Session& session = core.session;
    bool const applied = _decl->concurrency != Concurrency::Latest || run->id == _latest;
    std::shared_ptr<RtScope> const scope = _scope->shared_from_this();
    auto commandCtx = [&](Bindings extra) {
        return CommandCtx{
            .scope = scope,
            .bindings = std::make_shared<Bindings>(std::move(extra)),
            .cancelClose = std::make_shared<bool>(false),
            .boundary = nullptr,
        };
    };
    Value delivered;
    bool succeeded = false;
    // NOLINTNEXTLINE(readability-function-cognitive-complexity): one branch per outcome a reply can have.
    core.runtime().batch([&] {
        _inFlight.set(_inFlight.peek() - 1);
        _core->addPending(-1);
        if (reply) {
            succeeded = true;
            // The run's optimistic writes are confirmed by the reply: they become part of the confirmed rows.
            for (auto const& entry : run->layered) {
                Value confirmed = entry->confirmed;
                for (OptimisticLayer const& layer : entry->layers) {
                    if (layer.run == run->id) {
                        if (auto patched = patchRows(confirmed, layer.at, layer.key, layer.with)) {
                            confirmed = std::move(*patched);
                        }
                    }
                }
                std::erase_if(entry->layers, [&](OptimisticLayer const& layer) { return layer.run == run->id; });
                session.confirm(*entry, confirmed);
            }
            run->layered.clear();
            std::vector<std::string> secret;
            if (session.options().catalog.action) {
                if (auto const info = session.options().catalog.action(_decl->action)) {
                    secret = info->secretFields;
                }
            }
            delivered = withoutFields(*reply, secret);
            std::string const token =
                _alias == nullptr ? std::string{} : core.runtime().untracked([&] { return _alias->token->get(); });
            if (!applied) {
                session.actionSucceeded(_typeId, _decl->action, *reply, ReplyUse::Discarded, token);
                return;
            }
            _result.set(*reply);
            _error.set(Value{});
            _errorKind.set(Value{});
            _refusal.set(Value{});
            session.actionSucceeded(_typeId, _decl->action, *reply, ReplyUse::Applied, token);
            core.runList(_decl->onSuccess, commandCtx(*run->bindings), {});
            if (!secret.empty()) {
                _result.set(delivered);
            }
            return;
        }
        dropLayers(*run);
        std::string const kind = session.classify(error);
        if (applied) {
            _error.set(Value{failureText(error)});
            _errorKind.set(Value{kind});
            _refusal.set(detail::refusalValue(error));
            core.runList(_decl->onError, commandCtx(*run->bindings), {});
        }
        if (kind == "denied" && core.host != nullptr) {
            core.host->permissionsStale();
        }
    });
    if (_decl->concurrency == Concurrency::Serial) {
        next(run->serialKey);
    }
    if (auto const observers = session.runObservers(); !observers.empty()) {
        std::string code;
        if (!succeeded) {
            Value const refusal = detail::refusalValue(error);
            if (Value const* const named = refusal.field("code"); named != nullptr && named->asString() != nullptr) {
                code = *named->asString();
            }
        }
        // The rerun holds the mutation weakly: it may be chosen after the screen that ran it is gone.
        std::weak_ptr<bool> const alive = _alive;
        auto rerun = [this, alive, bindings = run->bindings](Value body) {
            if (!alive.expired()) {
                this->run(std::move(body), bindings, [](bool, Value const&) {});
            }
        };
        Value const replied = reply ? *reply : Value{};
        for (RunObserver* const observer : observers) {
            observer->settled(RunSettled{.run = run->sessionRun,
                                         .action = _decl->action,
                                         .body = run->body,
                                         .succeeded = succeeded,
                                         .reply = replied,
                                         .refusalCode = code,
                                         .rerun = rerun});
        }
    }
    run->done(succeeded, delivered);
}

void MutationRuntime::next(std::string const& serialKey) {
    auto const found = _queues.find(serialKey);
    if (found == _queues.end() || found->second.empty()) {
        _queues.erase(serialKey);
        _busy.erase(serialKey);
        return;
    }
    std::shared_ptr<Run> const queued = found->second.front();
    found->second.pop_front();
    _scope->core.runtime().batch([&] { send(queued); });
}
// NOLINTEND(misc-no-recursion)

}  // namespace detail

namespace detail {
namespace {

// --- Declaring scopes ------------------------------------------------------------------------------------------

/// @brief The element a node of a scope keeps its state in.
/// @param scope The scope that declares it.
/// @param node  The node.
/// @return The element, or `nullptr` for a node that keeps none.
[[nodiscard]] ElementState* elementFor(RtScope const& scope, DocNode const& node) {
    // A scope that declares nothing (a locale scope and the scope of its mounted content) leaves its children's
    // elements to the enclosing scope, where `collect` registered them, so the lookup continues there.
    for (RtScope const* at = &scope; at != nullptr; at = at->transparent ? at->parent.get() : nullptr) {
        for (auto const& element : at->elements) {
            if (element->node == &node) {
                return element.get();
            }
        }
    }
    return nullptr;
}

/// @brief The transfers of the `upload` commands in a list, nested lists included.
/// @param commands The list.
/// @param out      Where the transfer ids go.
// NOLINTNEXTLINE(misc-no-recursion): one level per nested command list
void transfersOf(Commands const& commands, std::vector<std::string>& out) {
    for (Command const& command : commands) {
        if (command.kind == CommandKind::Upload && !command.transfer.empty()) {
            out.push_back(command.transfer);
        }
        transfersOf(command.commands, out);
        transfersOf(command.alternative, out);
    }
}
}  // namespace

// NOLINTBEGIN(misc-no-recursion): a document is a tree of nodes and parts, and collecting descends it.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): one case per kind of node and part.
void ScreenCore::collect(DocNode const& node, RtScope& scope, std::vector<Commands const*>& commands) {
    KindSpec const* const spec = findKind(node.kind, false);
    if (!node.id.empty() || statefulKind(node.kind)) {
        auto element = std::make_unique<ElementState>(runtime());
        element->node = &node;
        element->scope = &scope;
        if (!node.id.empty()) {
            NameRef name;
            name.category = NameCategory::Element;
            name.element = element.get();
            scope.names.insert_or_assign(node.id, name);
        }
        wireErrors(*element);
        scope.elements.push_back(std::move(element));
    }
    bool const createsScope = spec != nullptr && (spec->flags & kScope) != 0;
    for (auto const& [key, list] : node.events) {
        KeySpec const* const keyed = spec == nullptr ? nullptr : keySpec(*spec, key);
        if (!createsScope || keyed == nullptr || (keyed->flags & kInner) == 0) {
            commands.push_back(&list);
        }
    }
    for (auto const& [chord, list] : node.keys) {
        static_cast<void>(chord);
        commands.push_back(&list);
    }
    for (auto const& [key, nodes] : node.children) {
        KeySpec const* const keyed = spec == nullptr ? nullptr : keySpec(*spec, key);
        if (createsScope && keyed != nullptr && (keyed->flags & kInner) != 0) {
            continue;
        }
        for (DocNodePtr const& child : nodes) {
            collect(*child, scope, commands);
        }
    }
    for (auto const& [key, parts] : node.parts) {
        static_cast<void>(key);
        for (DocNodePtr const& part : parts) {
            KindSpec const* const partSpec = findKind(part->kind, true);
            if (partSpec != nullptr && (partSpec->flags & kScope) != 0) {
                continue;
            }
            for (auto const& [event, list] : part->events) {
                static_cast<void>(event);
                commands.push_back(&list);
            }
            for (auto const& [childKey, nodes] : part->children) {
                KeySpec const* const keyed = partSpec == nullptr ? nullptr : keySpec(*partSpec, childKey);
                if (keyed != nullptr && (keyed->flags & kInner) != 0) {
                    continue;
                }
                for (DocNodePtr const& child : nodes) {
                    collect(*child, scope, commands);
                }
            }
        }
    }
}
// NOLINTEND(misc-no-recursion)

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one section per kind of declaration.
void ScreenCore::declare(RtScope& scope, ScopeDecl const* decl, std::vector<DocNode const*> const& content,
                         std::vector<Commands const*> const& commands) {
    std::vector<Commands const*> lists = commands;
    for (DocNode const* const node : content) {
        collect(*node, scope, lists);
    }
    std::vector<std::string> transferIds;
    for (Commands const* const list : lists) {
        transfersOf(*list, transferIds);
    }
    for (std::string const& transferId : transferIds) {
        if (scope.names.contains(transferId)) {
            continue;
        }
        scope.transfers.push_back(std::make_unique<TransferState>(runtime()));
        NameRef name;
        name.category = NameCategory::Transfer;
        name.transfer = scope.transfers.back().get();
        scope.names.insert_or_assign(transferId, name);
    }
    if (decl != nullptr) {
        for (ModelDecl const& model : decl->models) {
            auto alias = std::make_unique<ModelAlias>();
            alias->decl = &model;
            alias->mode = model.instance;
            if (alias->mode == InstanceMode::Default) {
                std::optional<ModelInfo> const info =
                    session.options().catalog.model ? session.options().catalog.model(model.type) : std::nullopt;
                alias->mode = info && info->stateless ? InstanceMode::Shared : InstanceMode::Private;
            }
            ModelAlias* const raw = alias.get();
            switch (raw->mode) {
                case InstanceMode::Keyed:
                    raw->token = &scope.nodes.computed([&scope, raw] -> std::string {
                        std::string const key = instanceKeyText(evalIn(scope, nullptr, raw->decl->key));
                        return key.empty() ? std::string{} : "shared:" + key;
                    });
                    break;
                case InstanceMode::Shared:
                    raw->token = &scope.nodes.computed([] { return std::string{"shared"}; });
                    break;
                case InstanceMode::Borrowed: {
                    auto const found = borrowed.find(model.name);
                    std::string token = found == borrowed.end() ? std::string{} : found->second.token;
                    raw->token = &scope.nodes.computed([token = std::move(token)] { return token; });
                    break;
                }
                case InstanceMode::Private:
                case InstanceMode::Default:
                default: {
                    std::string token = session.privateToken();
                    raw->token = &scope.nodes.computed([token = std::move(token)] { return token; });
                    break;
                }
            }
            NameRef name;
            name.category = NameCategory::Model;
            name.model = raw;
            scope.names.insert_or_assign(model.name, name);
            scope.models.push_back(std::move(alias));
        }
        for (LetDecl const& let : decl->lets) {
            NameRef name;
            name.category = NameCategory::Let;
            name.letExpr = let.value;
            name.let = &scope.nodes.computed([&scope, expr = let.value] { return evalIn(scope, nullptr, expr); });
            scope.names.insert_or_assign(let.name, name);
        }
        for (StateDecl const& state : decl->state) {
            auto slot = std::make_unique<StateSlot>();
            slot->decl = &state;
            slot->scope = &scope;
            NameRef name;
            name.category = NameCategory::State;
            name.state = slot.get();
            scope.names.insert_or_assign(state.name, name);
            scope.states.push_back(std::move(slot));
        }
        auto const aliasOf = [&scope](std::string const& model) -> ModelAlias* {
            NameRef const* const found = scope.find(model);
            return found != nullptr && found->category == NameCategory::Model ? found->model : nullptr;
        };
        for (QueryDecl const& query : decl->queries) {
            scope.queries.push_back(std::make_unique<QueryRuntime>(query, scope, aliasOf(query.model)));
            NameRef name;
            name.category = NameCategory::Query;
            name.query = scope.queries.back().get();
            scope.names.insert_or_assign(query.name, name);
        }
        for (MutationDecl const& mutation : decl->mutations) {
            scope.mutations.push_back(std::make_unique<MutationRuntime>(mutation, scope, aliasOf(mutation.model)));
            NameRef name;
            name.category = NameCategory::Mutation;
            name.mutation = scope.mutations.back().get();
            scope.names.insert_or_assign(mutation.name, name);
        }
    }
    for (auto const& slot : scope.states) {
        initState(scope, *slot);
    }
    for (auto const& element : scope.elements) {
        wireElement(*element);
    }
    if (decl == nullptr) {
        return;
    }
    for (CheckDecl const& check : decl->checks) {
        auto made = std::make_unique<CheckRuntime>();
        made->decl = &check;
        made->failing = &scope.nodes.computed([&scope, expr = check.assertion] {
            Value const asserted = evalIn(scope, nullptr, expr);
            bool const* const flag = asserted.asBool();
            return flag != nullptr && !*flag;
        });
        made->message = &scope.nodes.computed(
            [&scope, expr = check.message] { return textOf(evalIn(scope, nullptr, expr), scope.formatting()); });
        for (std::string const& target : check.targets) {
            NameRef const* const found = scope.find(target);
            if (found != nullptr && found->category == NameCategory::Element) {
                found->element->checks.push_back(made.get());
                made->targets.push_back(found->element);
                found->element->checksVersion.set(found->element->checksVersion.peek() + 1);
            }
        }
        scope.checks.push_back(std::move(made));
    }
    for (auto const& query : scope.queries) {
        query->start();
    }
    for (WatchDecl const& watch : decl->watches) {
        auto& source = scope.nodes.computed([&scope, expr = watch.on] { return evalIn(scope, nullptr, expr); });
        scope.nodes.effect([this, &scope, &source, watched = &watch, first = true] mutable {
            static_cast<void>(source.get());
            if (first) {
                first = false;
                return;
            }
            bool const user = session.lastWriteByUser();
            if (watched->source == WatchSource::User && !user) {
                return;
            }
            runtime().untracked([&] {
                CommandCtx const ctx{
                    .scope = scope.shared_from_this(),
                    .bindings = std::make_shared<Bindings>(),
                    .cancelClose = std::make_shared<bool>(false),
                    .boundary = nullptr,
                };
                if (user) {
                    session.asUser([&] { runList(watched->body, ctx, {}); });
                } else {
                    runList(watched->body, ctx, {});
                }
            });
        });
    }
}

namespace {

/// @brief What a date input reads from a value: the instant it shows, and whether the value names one at all.
struct DateReading {
    /// @brief The instant shown; empty for an empty field.
    std::optional<time::DateTime> instant;
    /// @brief Whether the value names a date, an instant or nothing; `false` for text that names no date.
    bool readable = true;
};

/// @brief Reads what a date input shows for @p value, in a display zone @p offsetMinutes from UTC.
///
/// The value is read as the date functions read it (`readDateValue`): a timestamp is its instant; a `yyyy-MM-dd`
/// string is midnight of that date in the display zone; a string `DateTime::fromIso8601` reads is its instant; `null`
/// and `""` are empty. Any other value is empty and unreadable.
/// @param value         The value.
/// @param offsetMinutes The display zone's offset from UTC, in minutes.
/// @return The instant, or nothing, and whether the value was readable.
[[nodiscard]] DateReading dateInstantOf(Value const& value, std::int32_t offsetMinutes) {
    DateValue const reading = readDateValue(value);
    switch (reading.form) {
        case DateForm::Absent:
            return {};
        case DateForm::Date:
            return {
                .instant = time::DateTime{std::chrono::sys_days{reading.date} - std::chrono::minutes{offsetMinutes}},
            };
        case DateForm::Timestamp:
        case DateForm::InstantText:
            return {.instant = reading.instant};
        case DateForm::Unreadable:
        default:
            return {.instant = std::nullopt, .readable = false};
    }
}

/// @brief A date input's date text for @p instant in a display zone @p offsetMinutes from UTC.
/// @param instant       The instant.
/// @param offsetMinutes The display zone's offset from UTC, in minutes.
/// @return `yyyy-MM-dd`.
[[nodiscard]] std::string dateTextOf(time::DateTime const& instant, std::int32_t offsetMinutes) {
    auto const local = instant.value + std::chrono::minutes{offsetMinutes};
    std::chrono::year_month_day const date{std::chrono::floor<std::chrono::days>(local)};
    return std::format("{:04}-{:02}-{:02}", static_cast<int>(date.year()), static_cast<unsigned>(date.month()),
                       static_cast<unsigned>(date.day()));
}

/// @brief A value as the places a number is entered with.
/// @param value  The value.
/// @param format The formatting facts.
/// @return The places, 0 to the most a decimal holds; -1 for a value that is not a non-negative integer.
[[nodiscard]] int placesOf(Value const& value, FormatContext const& format) {
    static_cast<void>(format);  // Only to fit `bindProp`'s converter signature.
    std::int64_t const* const whole = value.asInt();
    if (whole == nullptr || *whole < 0) {
        return -1;
    }
    return static_cast<int>(std::min<std::int64_t>(*whole, math::kMaxDecimalPlaces));
}

/// @brief The fraction digits of a number as typed in the active locale.
/// @param text   The typed text.
/// @param format The formatting facts.
/// @return The digits after the decimal separator; 0 for a text that is not a number, which no places rule judges.
[[nodiscard]] std::size_t fractionDigits(std::string_view text, FormatContext const& format) {
    std::optional<std::string> const canonical = render::normalizeLocaleNumber(text, withoutGrouping(format.numeric));
    if (!canonical.has_value()) {
        return 0;
    }
    auto const dot = canonical->find('.');
    return dot == std::string::npos ? 0 : canonical->size() - dot - 1;
}

/// @brief Whether two values show the same in an input: by text for a text input, by instant for a date input, by
///        value otherwise.
/// @param kind   The input's kind.
/// @param lhs    One value.
/// @param rhs    The other.
/// @param format The formatting facts.
/// @return It.
[[nodiscard]] bool sameShown(std::string_view kind, Value const& lhs, Value const& rhs, FormatContext const& format) {
    if (kind == "textInput" || kind == "filePicker") {
        return textOf(lhs, format) == textOf(rhs, format);
    }
    if (kind == "dateTimeInput") {
        return dateInstantOf(lhs, format.utcOffsetMinutes).instant ==
               dateInstantOf(rhs, format.utcOffsetMinutes).instant;
    }
    return lhs == rhs;
}

/// @brief The key of a select option: a record's `key`, `id` or `value` field, or the option itself.
/// @param option The option.
/// @return The key.
[[nodiscard]] Value optionKey(Value const& option) {
    if (option.asRecord() == nullptr) {
        return option;
    }
    for (std::string_view const field : {"key", "id", "value"}) {
        if (Value const* const found = option.field(field)) {
            return *found;
        }
    }
    return Value{};
}
}  // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one rule per source of an error.
void ScreenCore::wireErrors(ElementState& element) {
    RtScope& scope = *element.scope;
    element.errors = &scope.nodes.computed([this, &element] -> Value {
        static_cast<void>(element.checksVersion.get());
        DocNode const& field = *element.node;
        RtScope const& owner = *element.scope;
        std::vector<std::string> errors;
        // A group's `required` is its legend's mark only: a group has no value, so no `Required` is derived from it.
        if (ExprPtr const required = field.kind != "group" ? field.prop("required") : nullptr) {
            Value const requiredNow = evalIn(owner, nullptr, required);
            bool const* const flag = requiredNow.asBool();
            if (flag != nullptr && *flag && !engaged(inputValue(element))) {
                errors.emplace_back("Required");
            }
        }
        for (std::string& text : stringsOf(evalIn(owner, nullptr, field.prop("errors")))) {
            errors.push_back(std::move(text));
        }
        if (ExprPtr const places = field.kind == "textInput" ? field.prop("decimals") : nullptr) {
            int const allowed = placesOf(evalIn(owner, nullptr, places), session.formatting());
            if (allowed >= 0 && fractionDigits(textOf(inputValue(element), session.formatting()),
                                               session.formatting()) > static_cast<std::size_t>(allowed)) {
                // The key and fallback of a form field's `too-precise`, so both read alike in any language.
                errors.emplace_back(render::resolveText(session.formatting().translations, session.formatting().locale,
                                                        std::nullopt, "morph.forms.error.too-precise",
                                                        "Too many decimal places"));
            }
        }
        if (!field.id.empty()) {
            std::vector<std::string> const path{"errors", field.id};
            for (auto const& query : owner.queries) {
                for (std::string& text : stringsOf(walk(query->value(), path))) {
                    errors.push_back(std::move(text));
                }
            }
            for (auto const& mutation : owner.mutations) {
                for (std::string& text : stringsOf(walk(mutation->result(), path))) {
                    errors.push_back(std::move(text));
                }
            }
        }
        for (CheckRuntime const* const check : element.checks) {
            if (check->failing->get()) {
                errors.push_back(check->message->get());
            }
        }
        return listOfStrings(errors);
    });
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one case per kind of element.
void ScreenCore::wireElement(ElementState& element) {
    DocNode const& node = *element.node;
    RtScope& scope = *element.scope;
    std::string_view const kind = node.kind;
    if (kind == "form") {
        wireForm(element);
        return;
    }
    if (kind == "collection" || kind == "wizard") {
        wireComposite(element);
        return;
    }
    if (kind == "table" && !node.name("model").empty()) {
        wireModelTable(element);
        return;
    }
    bool const input = kind == "textInput" || kind == "checkbox" || kind == "select" || kind == "dateTimeInput" ||
                       kind == "slider" || kind == "filePicker";
    if (input) {
        std::string_view key = "value";
        if (kind == "checkbox") {
            key = "checked";
        } else if (kind == "select") {
            key = "selected";
        }
        ExprPtr const expr = kind == "filePicker" ? nullptr : node.prop(key);
        bool const controlled = expr && (expr->kind != ExprKind::Literal || !expr->literal.isNull());
        if (controlled) {
            element.bound = &scope.nodes.computed([&scope, expr] { return evalIn(scope, nullptr, expr); });
            element.draft.set(runtime().untracked([&] { return element.bound->get(); }));
            scope.nodes.effect([this, &element, kind] {
                Value const bound = element.bound->get();
                runtime().untracked([&] {
                    if (!element.edited.peek()) {
                        element.draft.set(bound);
                    } else if (sameShown(kind, element.draft.peek(), bound, session.formatting())) {
                        element.edited.set(false);
                    }
                });
            });
        } else if (kind == "checkbox") {
            element.draft.set(false);
        }
    }
    if (kind == "select") {
        element.rows =
            &scope.nodes.computed([&scope, expr = node.prop("options")] { return evalIn(scope, nullptr, expr); });
        if (element.bound == nullptr && node.name("onStale") == "clear") {
            scope.nodes.effect([this, &element] {
                Value const options = element.rows->get();
                runtime().untracked([&] {
                    Value const selected = element.draft.peek();
                    if (selected.isNull()) {
                        return;
                    }
                    ValueList const* const items = options.asList();
                    bool const kept = items != nullptr && std::ranges::any_of(*items, [&](Value const& option) {
                                          return optionKey(option) == selected;
                                      });
                    if (!kept) {
                        session.noteWrite();
                        element.draft.set(Value{});
                    }
                });
            });
        }
    }
    if (kind == "table" && node.prop("rows")) {
        element.rows =
            &scope.nodes.computed([&scope, expr = node.prop("rows")] { return evalIn(scope, nullptr, expr); });
        // A bound selection is controlled, as a select's `selected` is: any expression but a literal `null` binds it.
        ExprPtr const selection = node.prop("selection");
        if (selection && (selection->kind != ExprKind::Literal || !selection->literal.isNull())) {
            element.bound = &scope.nodes.computed([&scope, selection] { return evalIn(scope, nullptr, selection); });
        }
        // A bound sort is controlled as the selection is.
        ExprPtr const sort = node.prop("sort");
        if (sort && (sort->kind != ExprKind::Literal || !sort->literal.isNull())) {
            element.sortBound = &scope.nodes.computed([&scope, sort] { return evalIn(scope, nullptr, sort); });
        }
        element.rowsTable = makeRowsTable(element, nullptr, true);
        element.view = element.rowsTable->view.get();
    }
}

namespace {

/// @brief A table's sort as its element value reads it: the bound `sort` when the key binds one, else the table's
///        own.
/// @param element The table's element.
/// @return The sort. Tracked.
[[nodiscard]] Value tableSort(ElementState const& element) {
    return element.sortBound != nullptr ? element.sortBound->get() : element.sort.get();
}

/// @brief A table's page values: `viewCount` (the rows it orders, every page), `page`, `pageCount`, and
///        `pageFirst` and `pageLast`, the 1-based positions of the page's first and last rows, both 0 when there are
///        none. A table that does not page reads page 0 of 1, over every row.
/// @param element The table's element.
/// @param name    The value's name.
/// @return The value. Tracked.
[[nodiscard]] Value tablePageValue(ElementState const& element, std::string_view name) {
    if (element.view == nullptr) {
        return name == "pageCount" ? std::int64_t{1} : std::int64_t{0};
    }
    TableView const& view = element.view->get();
    if (name == "viewCount") {
        return static_cast<std::int64_t>(view.rows.size());
    }
    if (name == "page") {
        return view.page;
    }
    if (name == "pageCount") {
        return view.pageCount;
    }
    bool const none = view.first == view.last;
    if (name == "pageFirst") {
        return none ? std::int64_t{0} : static_cast<std::int64_t>(view.first + 1);
    }
    return static_cast<std::int64_t>(view.last);
}

/// @brief A model-backed table's own element values, read from its rows: `viewCount` (the reply's total, `null`
///        before the first), `viewCountKind` (`exact` or `atLeast`), `undecided`, `awaiting`, `failed`, `pending`
///        (its description or a fetch in flight) and `error`.
/// @param table The table.
/// @param name  The value's name.
/// @return The value; nothing for a value the table shares with a table over `rows`. Tracked.
[[nodiscard]] std::optional<Value> modelTableValue(ModelTable const& table, std::string_view name) {
    static_cast<void>(table.version.get());
    auto const strings = [](std::vector<std::string> const& list) {
        ValueList out;
        for (std::string const& item : list) {
            out.emplace_back(item);
        }
        return Value{std::move(out)};
    };
    TableCountsView const counts = table.rows->counts();
    if (name == "viewCount") {
        return counts.view.known ? Value{counts.view.rows} : Value{};
    }
    if (name == "viewCountKind") {
        if (!counts.view.known) {
            return Value{};
        }
        return Value{std::string{counts.view.kind == table::TotalKind::AtLeast ? "atLeast" : "exact"}};
    }
    if (name == "undecided") {
        return Value{counts.undecided};
    }
    if (name == "awaiting") {
        return strings(counts.awaiting);
    }
    if (name == "failed") {
        return strings(counts.failed);
    }
    if (name == "pending") {
        return Value{counts.pending || (table.describe != nullptr && table.describe->pending())};
    }
    if (name == "error") {
        if (auto const& error = table.window->error()) {
            return Value{error->message};
        }
        return table.describe != nullptr ? table.describe->error() : Value{};
    }
    return std::nullopt;
}

/// @brief The selected keys of the rows a table holds on pages other than the one shown, in their order: what an
///        `Extend` report keeps, since the backend holds the shown page alone.
/// @param view      The table's rows and page.
/// @param selection The selection, a list of keys.
/// @param rowKey    A row's key.
/// @return The keys.
[[nodiscard]] ValueList selectedOffPage(TableView const& view, Value const& selection,
                                        std::function<Key(Value const&)> const& rowKey) {
    std::unordered_set<Key> onPage;
    std::unordered_set<Key> offPage;
    for (std::size_t index = 0; index < view.rows.size(); ++index) {
        (index >= view.first && index < view.last ? onPage : offPage).insert(rowKey(view.rows.at(index)));
    }
    ValueList kept;
    if (ValueList const* const selected = selection.asList()) {
        for (Value const& item : *selected) {
            Key const key = keyOf(item);
            if (offPage.contains(key) && !onPage.contains(key)) {
                kept.push_back(item);
            }
        }
    }
    return kept;
}

/// @brief The sortable column part of a table with the id @p columnId.
/// @param table    The table.
/// @param columnId The column's id.
/// @return The part, or `nullptr` when no sortable column has that id.
[[nodiscard]] DocNode const* sortableColumn(DocNode const& table, std::string_view columnId) {
    for (DocNodePtr const& column : table.partList("columns")) {
        if (column->id == columnId && column->flag("sortable")) {
            return column.get();
        }
    }
    return nullptr;
}

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
[[nodiscard]] std::optional<std::vector<SortKey>> sortKeysOf(DocNode const& table, Value const& sort) {
    std::vector<SortKey> keys;
    if (sort.isNull()) {
        return keys;
    }
    ValueList const* const items = sort.asList();
    if (items == nullptr) {
        return std::nullopt;
    }
    for (Value const& item : *items) {
        Value const* const column = item.field("column");
        Value const* const direction = item.field("direction");
        std::string const* const columnId = column == nullptr ? nullptr : column->asString();
        std::string const* const way = direction == nullptr ? nullptr : direction->asString();
        DocNode const* const part = columnId == nullptr ? nullptr : sortableColumn(table, *columnId);
        if (part == nullptr || way == nullptr || (*way != "asc" && *way != "desc")) {
            return std::nullopt;
        }
        keys.push_back(SortKey{.column = part, .descending = *way == "desc"});
    }
    return keys;
}
}  // namespace

TableView ScreenCore::tableView(RowsTable const& table) {
    static_cast<void>(table.version.get());
    DocNode const& node = *table.node;
    RtScope const& scope = *table.where;
    Bindings const* const bindings = table.bindings.get();
    TableView view;
    if (!table.engine->restoring()) {
        table::Engine& engine = table.engine->engine();
        std::size_t const count = engine.viewRowCount();
        view.rows.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            std::optional<TableRowRef> const ref = table.engine->row(static_cast<std::int64_t>(index));
            view.rows.push_back(ref ? ref->row : Value{});
            if (table.grouped) {
                std::optional<std::size_t> const sourceRow = engine.sourceRowOf(index);
                view.runOf.push_back(sourceRow ? table.source->runOf(*sourceRow) : 0);
            }
        }
        if (table.grouped && count > 0) {
            std::span<Value const> const runs = table.source->runRecords();
            view.runs.assign(runs.begin(), runs.end());
        }
    }
    view.last = view.rows.size();
    ExprPtr const sizeExpr = node.prop("pageSize");
    if (!sizeExpr) {
        return view;
    }
    // A `pageSize` of `null` does not page; one that is no positive integer is reported and does not page either.
    Value const size = evalIn(scope, bindings, sizeExpr);
    std::int64_t const* const perPage = size.asInt();
    if (size.isNull() || perPage == nullptr || *perPage < 1) {
        if (!size.isNull()) {
            report(pointerAppend(node.path, "pageSize"),
                   "pageSize is " + canonicalJson(size) + "; a page holds a positive whole number of rows");
        }
        return view;
    }
    // A `page` of `null` is the first; one that is negative or not a whole number is reported and shows the first.
    // One past the last is the last: a filter that shrinks the rows under the user is normal, not an error.
    std::int64_t asked = 0;
    if (ExprPtr const pageExpr = node.prop("page")) {
        Value const page = evalIn(scope, bindings, pageExpr);
        std::int64_t const* const index = page.asInt();
        if (index != nullptr && *index >= 0) {
            asked = *index;
        } else if (!page.isNull()) {
            report(pointerAppend(node.path, "page"),
                   "page is " + canonicalJson(page) + "; a page is a whole number from 0");
        }
    }
    auto const total = static_cast<std::int64_t>(view.rows.size());
    view.pageCount = std::max<std::int64_t>(1, (total + *perPage - 1) / *perPage);
    view.page = std::min(asked, view.pageCount - 1);
    view.first = static_cast<std::size_t>(std::min(total, view.page * *perPage));
    view.last = static_cast<std::size_t>(std::min(total, (view.page + 1) * *perPage));
    return view;
}

namespace {

/// @brief A table's selection as its element value reads it: the bound `selection` when the key binds one, else
///        the table's own. A bound value that is not a list (`null` while state starts empty) selects nothing.
/// @param element The table's element.
/// @return A list of keys. Tracked.
[[nodiscard]] Value tableSelection(ElementState const& element);

/// @brief A table's selection as an action takes it: the selection of the view while a model's table holds one,
///        else the selected keys. Tracked.
/// @param element The table's element.
/// @return The selection.
[[nodiscard]] TableSelection selectionNow(ElementState const& element) {
    if (element.modelTable != nullptr) {
        if (auto const& view = element.modelTable->viewSelection.get()) {
            return *view;
        }
    }
    TableSelection out;
    if (ValueList const* const items = tableSelection(element).asList()) {
        for (Value const& item : *items) {
            out.keys.push_back(keyOf(item));
        }
    }
    return out;
}

[[nodiscard]] Value tableSelection(ElementState const& element) {
    if (element.bound == nullptr) {
        return element.selection.get();
    }
    Value const bound = element.bound->get();
    return bound.asList() != nullptr ? bound : Value{ValueList{}};
}
}  // namespace

[[nodiscard]] Value jsonTextValue(std::string_view text) {
    auto const json = parseJson(text);
    if (!json) {
        return Value{};
    }
    auto value = valueFromJson(*json);
    return value ? *std::move(value) : Value{};
}

namespace {

/// @brief One field of a form as a record: `value` (the encoded value, `null` while blank or invalid), `draft` (the
///        text), `valid`, `required`, `visible`, `readonly` and `errors`. Tracked.
/// @param field The field.
/// @return The record.
[[nodiscard]] Value formFieldValue(forms::FieldState const& field) {
    ValueRecord record;
    forms::Encoding const& encoding = field.encoded().get();
    Value value;
    if (encoding.has_value() && encoding->has_value()) {
        value = (*encoding)->value.isNull() ? jsonTextValue((*encoding)->json) : (*encoding)->value;
    }
    std::optional<forms::FieldError> const& error = field.error().get();
    record.insert_or_assign("value", std::move(value));
    record.insert_or_assign("draft", Value{field.spec().kind == forms::FieldKind::Object ||
                                                   field.spec().kind == forms::FieldKind::ObjectArray
                                               ? std::string{}
                                               : field.draft()});
    record.insert_or_assign("valid", Value{!error.has_value()});
    record.insert_or_assign("required", Value{field.required().get()});
    record.insert_or_assign("visible", Value{field.visible().get()});
    record.insert_or_assign("readonly", Value{field.readonly().get()});
    record.insert_or_assign("errors",
                            error.has_value() ? Value{ValueList{Value{error->message}}} : Value{ValueList{}});
    return Value{std::move(record)};
}

/// @brief A form's element values (spec 5 §6): `ready`, `body` (the record the form would submit, `null` while not
///        ready), `submitted` (whether a submission was sent), `pending`, and each field by its wire name. Tracked.
/// @param element The form's element.
/// @param name    The value's name.
/// @return The value; `null` for a name the form does not have, or a form without a session.
[[nodiscard]] Value formValue(ElementState const& element, std::string_view name) {
    forms::FormSession const* const form = element.form.get();
    if (form == nullptr) {
        return Value{};
    }
    if (name == "ready") {
        return form->ready();
    }
    if (name == "body") {
        std::optional<std::string> const body = form->body();
        return body.has_value() ? jsonTextValue(*body) : Value{};
    }
    if (name == "submitted") {
        return form->submitAttempts() > 0;
    }
    if (name == "pending") {
        return form->pending();
    }
    for (auto const& field : form->fields()) {
        if (field->spec().name == name) {
            return formFieldValue(*field);
        }
    }
    return Value{};
}
}  // namespace

Value ScreenCore::inputValue(ElementState const& element) const {
    std::string_view const kind = element.node->kind;
    if (kind == "checkbox") {
        return elementValue(element, "checked");
    }
    if (kind == "select") {
        return elementValue(element, "selected");
    }
    return elementValue(element, "value");
}

// NOLINTNEXTLINE(misc-no-recursion,readability-function-cognitive-complexity): one case per element value.
Value ScreenCore::elementValue(ElementState const& element, std::string_view name) const {
    DocNode const& node = *element.node;
    RtScope const& scope = *element.scope;
    std::string_view const kind = node.kind;
    auto const flagOf = [&](std::string_view key, bool fallback) -> Value {
        ExprPtr const expr = node.prop(key);
        if (!expr) {
            return fallback;
        }
        Value const value = evalIn(scope, nullptr, expr);
        bool const* const flag = value.asBool();
        return flag != nullptr && *flag;
    };
    auto const errors = [&] -> Value { return element.errors->get(); };
    auto const count = [](Value const& list) -> Value {
        ValueList const* const items = list.asList();
        return static_cast<std::int64_t>(items == nullptr ? 0 : items->size());
    };
    if (name == "visible" || name == "enabled") {
        return flagOf(name, true);
    }
    if (name == "errors") {
        return errors();
    }
    if (name == "valid") {
        Value const current = errors();
        ValueList const* const items = current.asList();
        return items == nullptr || items->empty();
    }
    if (name == "shownErrors") {
        RtScope const& policy = scope.policyScope();
        std::string const mode = policy.showErrors.empty() ? _doc->showErrors : policy.showErrors;
        bool const submittedNow = policy.submitted != nullptr && policy.submitted->get();
        if (mode == "always" || (mode == "submit" && submittedNow) ||
            (mode != "submit" && (submittedNow || element.touched.get() || touchedWithin(element)))) {
            return errors();
        }
        return Value{ValueList{}};
    }
    if (kind == "dialog" || kind == "drawer") {
        if (name == "open") {
            return node.prop("open") ? flagOf("open", false) : Value{element.open.get()};
        }
        if (name == "ready") {
            RtScope const* const inner = element.inner.get();
            return inner != nullptr && inner->ready();
        }
        if (name == "submitted") {
            return element.submitted.get();
        }
        return Value{};
    }
    if (kind == "table" && (name == "layout" || name == "visibleColumns" || name == "hiddenCount")) {
        TableConfig const* const config = element.rowsTable != nullptr     ? &element.rowsTable->config
                                          : element.modelTable != nullptr ? &element.modelTable->config
                                          : element.gridTable != nullptr  ? &element.gridTable->config
                                                                          : nullptr;
        return config == nullptr ? Value{} : layoutElementValue(*config, name);
    }
    if (kind == "table" && name == "filters") {
        TableConfig const* const config = element.rowsTable != nullptr     ? &element.rowsTable->config
                                          : element.modelTable != nullptr ? &element.modelTable->config
                                          : element.gridTable != nullptr  ? &element.gridTable->config
                                                                          : nullptr;
        return config == nullptr || !config->filters ? Value{} : filtersValueOf(config->filters());
    }
    if (kind == "table" && element.gridTable != nullptr) {
        FormGrid const& grid = element.gridTable->grid;
        if (name == "modifiedCount") {
            return Value{static_cast<std::int64_t>(grid.source->modifiedCount())};
        }
        if (name == "modifiedRows") {
            return Value{static_cast<std::int64_t>(grid.source->modifiedRows())};
        }
        if (name == "errorCount") {
            return Value{static_cast<std::int64_t>(grid.source->errorCount())};
        }
        if (name == "editing") {
            std::optional<CellRef> const cell = grid.grid->editing();
            if (!cell) {
                return Value{};
            }
            return Value{ValueRecord{{"key", valueOfKey(cell->key)}, {"column", Value{cell->column}}}};
        }
        if (name == "selection") {
            return cellRangeValue(grid.grid->selection());
        }
    }
    if (kind == "table" && (name == "modifiedCount" || name == "modifiedRows" || name == "errorCount")) {
        return Value{std::int64_t{0}};
    }
    if (kind == "table" && name == "busyCount") {
        if (element.modelTable == nullptr || element.modelTable->rows == nullptr) {
            return Value{std::int64_t{0}};
        }
        static_cast<void>(element.modelTable->marksRevision.get());
        return Value{element.modelTable->rows->states().busyCount()};
    }
    if (kind == "table" && (name == "selectionSet" || name == "selectedCount" || name == "selectedCountKind")) {
        TableSelection const selection = selectionNow(element);
        if (name == "selectionSet") {
            auto const json = parseJson(selectionJsonOf(selection));
            auto read = json ? valueFromJson(*json) : std::expected<Value, std::string>{Value{}};
            return read ? std::move(*read) : Value{};
        }
        SelectedCount counted{.rows = static_cast<std::int64_t>(selection.keys.size()), .kind = CountKind::Exact};
        if (element.modelTable != nullptr && element.modelTable->rows != nullptr) {
            static_cast<void>(element.modelTable->version.get());
            counted = element.modelTable->rows->selectedCount(selection);
        }
        if (name == "selectedCount") {
            return Value{counted.rows};
        }
        return Value{std::string{counted.kind == CountKind::Exact ? "exact" : "approximate"}};
    }
    if (kind == "table" && element.modelTable != nullptr && element.modelTable->window != nullptr) {
        if (std::optional<Value> value = modelTableValue(*element.modelTable, name)) {
            return std::move(*value);
        }
    }
    if (kind == "table") {
        if (name == "selection") {
            return tableSelection(element);
        }
        if (name == "activeKey") {
            return element.activeKey.get();
        }
        if (name == "selectedCount") {
            return count(tableSelection(element));
        }
        if (name == "sort") {
            return tableSort(element);
        }
        if (name == "viewCount" || name == "page" || name == "pageCount" || name == "pageFirst" ||
            name == "pageLast") {
            return tablePageValue(element, name);
        }
        if (name == "sourceCount") {
            return element.rows == nullptr ? Value{std::int64_t{0}} : count(element.rows->get());
        }
        ExprPtr const rows = node.prop("rows");
        if ((name == "pending" || name == "error") && rows && rows->kind == ExprKind::Query) {
            ScopeEnv const env{scope, nullptr};
            return env.control(name == "pending" ? ExprKind::Pending : ExprKind::Error, rows->name);
        }
        return name == "pending" ? Value{false} : Value{};
    }
    if (kind == "tabs") {
        // The shown tab's id: the bound one, else the user's own pick, else the first tab's.
        ExprPtr const selected = node.prop("selected");
        if (name != "selected") {
            return Value{};
        }
        if (selected && (selected->kind != ExprKind::Literal || !selected->literal.isNull())) {
            return evalIn(scope, nullptr, selected);
        }
        if (Value picked = element.draft.get(); !picked.isNull()) {
            return picked;
        }
        auto const tabs = node.partList("tabs");
        return tabs.empty() || tabs.front()->id.empty() ? Value{} : Value{tabs.front()->id};
    }
    if (kind == "form") {
        return formValue(element, name);
    }
    if (name == "draft") {
        return element.draft.get();
    }
    if (name == "dirty") {
        return element.edited.get();
    }
    if (name == "focused") {
        return element.focused.get();
    }
    if (name == "touched") {
        return element.touched.get();
    }
    if (name == "checked" || name == "selected") {
        return element.bound != nullptr ? element.bound->get() : element.draft.get();
    }
    if (name == "selectedOption") {
        Value const selected = elementValue(element, "selected");
        ValueList const* const options = element.rows == nullptr ? nullptr : element.rows->get().asList();
        if (options != nullptr && !selected.isNull()) {
            for (Value const& option : *options) {
                if (optionKey(option) == selected) {
                    return option;
                }
            }
        }
        return Value{};
    }
    if (name == "value") {
        if (element.bound != nullptr) {
            return element.bound->get();
        }
        return element.committed.get();
    }
    return Value{};
}

bool ScreenCore::touchedWithin(ElementState const& element) {
    if (element.node->kind != "group") {
        return false;
    }
    std::string const inside = element.node->path + "/";
    return std::ranges::any_of(element.scope->elements, [&inside](std::unique_ptr<ElementState> const& other) {
        return other->node->path.starts_with(inside) && other->touched.get();
    });
}

// --- Commands --------------------------------------------------------------------------------------------------

// NOLINTBEGIN(misc-no-recursion): command lists nest
void ScreenCore::runList(Commands const& commands, CommandCtx const& ctx, Done done) {
    if (!done) {
        done = [](bool) {};
    }
    if (commands.empty()) {
        done(true);
        return;
    }
    struct Join {
        std::size_t left = 0;
        bool ok = true;
        Done done;
    };
    if (recording) {
        logList(commands);
    }
    auto const join = std::make_shared<Join>(Join{.left = commands.size() + 1, .ok = true, .done = std::move(done)});
    auto const finish = [join](bool succeeded) {
        join->ok = join->ok && succeeded;
        if (--join->left == 0) {
            join->done(join->ok);
        }
    };
    for (Command const& command : commands) {
        runOne(command, ctx, finish);
    }
    finish(true);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one case per command of ui/1.
void ScreenCore::runOne(Command const& command, CommandCtx const& ctx, Done done) {
    if (!done) {
        done = [](bool) {};
    }
    RtScope const& scope = *ctx.scope;
    auto const eval = [&](ExprPtr const& expr) { return evalIn(scope, ctx.bindings.get(), expr); };
    auto const finish = [&done](bool succeeded) { done(succeeded); };
    switch (command.kind) {
        case CommandKind::Set:
            runSet(command, ctx, done);
            return;
        case CommandKind::Reset:
            runReset(command, ctx, done);
            return;
        case CommandKind::Run:
            runMutation(command, ctx, std::move(done));
            return;
        case CommandKind::Refetch: {
            // A loaded document's `refetch` names a query in scope (`loadDocument` checks it), as `run` names a
            // mutation and `open` a dialog.
            scope.find(command.target)->query->refetch();
            finish(true);
            return;
        }
        case CommandKind::Open:
        case CommandKind::Close: {
            ElementState* target = nullptr;
            if (command.kind == CommandKind::Close && command.target == "self") {
                // `self` is the innermost dialog or drawer whose content holds the command, found lexically. A
                // component's fallback scope has no parent, so the walk stops at its boundary as its names do.
                RtScope const* walk = &scope;
                while (walk->owner == nullptr && walk->parent) {
                    walk = walk->parent.get();
                }
                target = walk->owner;
                if (target == nullptr && walk != rootScope.lock().get()) {
                    report(command.path, "`close: self` inside a component names no dialog or drawer");
                    finish(false);
                    return;
                }
                if (target == nullptr) {
                    finish((host != nullptr && host->closeSelf(_doc->id, std::nullopt)) ||
                           unsupported(command, "`close: self`"));
                    return;
                }
            } else {
                target = scope.find(command.target)->element;
            }
            ElementState& element = *target;
            if (element.node->prop("open")) {
                report(command.path, "the dialog's `open` is bound to an expression");
                finish(false);
                return;
            }
            bool const opening = command.kind == CommandKind::Open;
            if (opening && !element.open.peek()) {
                element.submitted.set(false);
            }
            session.noteWrite();
            element.open.set(opening);
            finish(true);
            return;
        }
        case CommandKind::Navigate: {
            NavigateRequest const request{
                .screen = command.target,
                .route = eval(command.extra),
                .params = eval(command.value),
                .as = command.option,
                .call = false,
            };
            if (host == nullptr) {
                finish(unsupported(command, "`navigate`"));
                return;
            }
            // A navigation without `onResult` and `onCancel` asks for no answer, so it files no call: the screen it
            // opens answers nobody, and its `return` is the application's.
            if (command.commands.empty() && command.alternative.empty()) {
                finish(host->navigate(request, {}, {}) || unsupported(command, "`navigate`"));
                return;
            }
            std::weak_ptr<RtScope> const weak = ctx.scope;
            CommandCtx const kept = detached(ctx);
            auto onResult = [this, weak, kept, list = &command.commands](Value const& result) {
                if (auto const held = weak.lock()) {
                    auto const bindings = std::make_shared<Bindings>(*kept.bindings);
                    bindings->push_back(Binding{.name = "event", .read = [result] { return result; }});
                    CommandCtx const next{
                        .scope = held,
                        .bindings = bindings,
                        .cancelClose = kept.cancelClose,
                        .boundary = kept.boundary,
                    };
                    runtime().batch([&] { runList(*list, next, {}); });
                }
            };
            auto onCancel = [this, weak, kept, list = &command.alternative] {
                if (auto const held = weak.lock()) {
                    CommandCtx next = kept;
                    next.scope = held;
                    runtime().batch([&] { runList(*list, next, {}); });
                }
            };
            finish(host->navigate(request, std::move(onResult), std::move(onCancel)) ||
                   unsupported(command, "`navigate`"));
            return;
        }
        case CommandKind::Return:
            finish(host != nullptr && host->closeSelf(_doc->id, eval(command.value))
                       ? true
                       : unsupported(command, "`return`"));
            return;
        case CommandKind::Focus:
            if (focusTarget(command, scope, ctx.bindings.get())) {
                finish(true);
                return;
            }
            report(command.path, "`" + command.target + "` is not mounted");
            finish(false);
            return;
        case CommandKind::Host:
            runHostCommand(command, ctx, done);
            return;
        case CommandKind::SwitchBackend: {
            std::weak_ptr<RtScope> const weak = ctx.scope;
            auto committed = [this, weak, target = command.target, path = command.path,
                              done](std::expected<void, std::string> const& outcome) {
                if (!weak.lock()) {
                    return;
                }
                if (!outcome) {
                    report(path, "backendSwitchFailed: " + outcome.error());
                    done(false);
                    return;
                }
                session.backendChanged(target);
                done(true);
            };
            if (host != nullptr && host->switchBackend(command.target, std::move(committed))) {
                return;
            }
            report(command.path, "backend `" + command.target + "` is not listed");
            finish(false);
            return;
        }
        case CommandKind::ResetPrefs: {
            ValueRecord prefsNow = prefs.peek();
            if (command.target == "all") {
                prefsNow = prefDefaults;
            } else if (auto const found = prefDefaults.find(command.target); found != prefDefaults.end()) {
                prefsNow.insert_or_assign(command.target, found->second);
            }
            prefs.set(std::move(prefsNow));
            if (host != nullptr) {
                host->resetPrefs(_doc->id, command.target);
            }
            finish(true);
            return;
        }
        case CommandKind::SetLocale:
            session.setLocale(command.target);
            if (host != nullptr) {
                host->setLocale(command.target);
            }
            finish(true);
            return;
        case CommandKind::SetContext: {
            // The shell checks the value against the switcher's options; with no switcher it is unsupported.
            Value const requested = eval(command.value);
            if (session.requestContext(requested)) {
                finish(true);
            } else {
                report(command.path,
                       "`setContext` was refused: " + std::string{session.can("context")
                                                                      ? "the value is not one of the scope's options"
                                                                      : "no scope switcher is declared"});
                finish(false);
            }
            return;
        }
        case CommandKind::Notify: {
            std::string const text = textOf(eval(command.value), ctx.scope->formatting());
            finish(host != nullptr && host->notify(text, command.option) ? true : unsupported(command, "`notify`"));
            return;
        }
        case CommandKind::Seq: {
            // A step that waits for a reply keeps the rest of the sequence in that reply's handler, which the
            // dispatcher may hold past the screen: the sequence holds its scope weakly and stops once it is gone.
            CommandCtx const inner = detached(CommandCtx{
                .scope = nullptr,
                .bindings = std::make_shared<Bindings>(*ctx.bindings),
                .cancelClose = ctx.cancelClose,
                .boundary = ctx.boundary,
            });
            std::weak_ptr<RtScope> const weak = ctx.scope;
            auto const step = std::make_shared<std::function<void(std::size_t)>>();
            *step = [this, inner, weak, list = &command.commands, done = std::move(done),
                     weakStep = std::weak_ptr<std::function<void(std::size_t)>>{step}](std::size_t index) {
                auto const held = weak.lock();
                if (index == list->size() || !held) {
                    done(held != nullptr);
                    return;
                }
                auto const self = weakStep.lock();
                CommandCtx current = inner;
                current.scope = held;
                runOne(list->at(index), current, [self, index, done](bool succeeded) {
                    if (!succeeded) {
                        done(false);
                        return;
                    }
                    (*self)(index + 1);
                });
            };
            (*step)(0);
            return;
        }
        case CommandKind::All: {
            CommandCtx const inner{
                .scope = ctx.scope,
                .bindings = std::make_shared<Bindings>(*ctx.bindings),
                .cancelClose = ctx.cancelClose,
                .boundary = ctx.boundary,
            };
            runList(command.commands, inner, std::move(done));
            return;
        }
        case CommandKind::If: {
            Value const condition = eval(command.value);
            bool const* const flag = condition.asBool();
            if (flag == nullptr) {
                finish(true);
                return;
            }
            runList(*flag ? command.commands : command.alternative, ctx, std::move(done));
            return;
        }
        case CommandKind::CancelClose:
            *ctx.cancelClose = true;
            finish(true);
            return;
        case CommandKind::Retry:
            // `retry` is valid only inside a boundary's `error`, which is lowered with its boundary.
            for (QueryRuntime* const query : ctx.boundary->queries) {
                if (!runtime().untracked([&] { return query->error(); }).isNull()) {
                    query->refetch();
                }
            }
            finish(true);
            return;
        case CommandKind::SignIn: {
            NameRef const* const found = scope.find(command.target);
            Value const reply = found != nullptr && found->category == NameCategory::Mutation
                                    ? runtime().untracked([&] { return found->mutation->result(); })
                                    : Value{};
            session.clear();
            finish(host != nullptr && host->signIn(reply) ? true : unsupported(command, "`signIn`"));
            return;
        }
        case CommandKind::SignOut:
            session.clear();
            if (host != nullptr) {
                host->signOut();
            }
            finish(true);
            return;
        case CommandKind::Upload:
        case CommandKind::Download:
        default:
            runTransfer(command, ctx, std::move(done));
            return;
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): a single run, or a run per list item.
void ScreenCore::runMutation(Command const& command, CommandCtx const& ctx, Done done) const {
    RtScope const& scope = *ctx.scope;
    MutationRuntime& mutation = *scope.find(command.target)->mutation;
    RtScope const& policy = scope.policyScope();
    if (policy.submitted != nullptr) {
        policy.submitted->set(true);
    }
    if (policy.owner != nullptr && !runtime().untracked([&] { return policy.ready(); })) {
        done(false);
        return;
    }
    RtScope const& declaring = *[&] -> RtScope const* {
        for (RtScope const* candidate = &scope; candidate != nullptr; candidate = candidate->parent.get()) {
            if (candidate->names.contains(command.target)) {
                return candidate;
            }
        }
        return &scope;
    }();
    auto const bodyFor = [&](Bindings const& bindings) {
        if (command.value) {
            return evalIn(scope, &bindings, command.value);
        }
        if (mutation.decl().body) {
            return evalIn(declaring, &bindings, mutation.decl().body);
        }
        return Value{ValueRecord{}};
    };
    if (!command.extra) {
        Value body = bodyFor(*ctx.bindings);
        mutation.run(std::move(body), std::make_shared<Bindings const>(*ctx.bindings),
                     [bindings = ctx.bindings, bindAs = command.binding, done = std::move(done)](bool succeeded,
                                                                                                 Value const& reply) {
                         if (succeeded && !bindAs.empty()) {
                             bindings->push_back(Binding{.name = bindAs, .read = [reply] { return reply; }});
                         }
                         done(succeeded);
                     });
        return;
    }
    Value const each = evalIn(scope, ctx.bindings.get(), command.extra);
    ValueList const items = each.asList() == nullptr ? ValueList{} : *each.asList();
    if (items.empty()) {
        done(true);
        return;
    }
    struct Each {
        std::size_t left = 0;
        bool ok = true;
        Done done;
    };
    auto const state = std::make_shared<Each>(Each{.left = items.size(), .ok = true, .done = std::move(done)});
    std::vector<Value> bodies;
    std::vector<std::shared_ptr<Bindings const>> bound;
    for (Value const& item : items) {
        Bindings bindings = *ctx.bindings;
        if (!command.binding.empty()) {
            bindings.push_back(Binding{.name = command.binding, .read = [item] { return item; }});
        }
        bodies.push_back(bodyFor(bindings));
        bound.push_back(std::make_shared<Bindings const>(std::move(bindings)));
    }
    if (mutation.decl().concurrency != Concurrency::Exclusive) {
        for (std::size_t index = 0; index < bodies.size(); ++index) {
            mutation.run(bodies.at(index), bound.at(index), [state](bool succeeded, Value const&) {
                state->ok = state->ok && succeeded;
                if (--state->left == 0) {
                    state->done(state->ok);
                }
            });
        }
        return;
    }
    // An exclusive mutation refuses a run while another is in flight, so its elements go one after another.
    auto const step = std::make_shared<std::function<void(std::size_t)>>();
    *step = [&mutation, bodies, bound, state,
             weakStep = std::weak_ptr<std::function<void(std::size_t)>>{step}](std::size_t index) {
        if (index == bodies.size()) {
            state->done(state->ok);
            return;
        }
        auto const self = weakStep.lock();
        mutation.run(bodies.at(index), bound.at(index), [self, state, index](bool succeeded, Value const&) {
            if (!succeeded) {
                state->done(false);
                return;
            }
            (*self)(index + 1);
        });
    };
    (*step)(0);
}

// NOLINTEND(misc-no-recursion)
void ScreenCore::writeSlot(StateSlot& slot, std::span<std::string const> within, Value value, Command const& command,
                           Done const& done) {
    slot.scope->core.initState(*slot.scope, slot);
    Type const& type = slot.decl->type;
    if (within.empty() && !type.isAny() && !type.admits(value)) {
        report(command.path, "`set` writes a value that is not a " + type.name());
        done(false);
        return;
    }
    Value next = withPath(slot.value->peek(), within, std::move(value));
    session.noteWrite();
    slot.value->set(std::move(next));
    done(true);
}

void ScreenCore::runSetApp(Command const& command, std::span<std::string const> rest, Value value, Done const& done) {
    if (RtScope const* const app = session.appScope(); app != nullptr && !rest.empty()) {
        NameRef const* const named = app->find(rest.front());
        if (named != nullptr && named->category == NameCategory::State) {
            writeSlot(*named->state, rest.subspan(1), std::move(value), command, done);
            return;
        }
        report(command.path, "`app." + rest.front() + "` is not state of the app document");
        done(false);
        return;
    }
    std::string name;
    for (std::string const& segment : rest) {
        name += name.empty() ? segment : "." + segment;
    }
    done(host != nullptr && host->setAppValue(name, value) ? true : unsupported(command, "`set` of `app`"));
}

void ScreenCore::runSet(Command const& command, CommandCtx const& ctx, Done const& done) {
    RtScope const& scope = *ctx.scope;
    auto path = splitPath(command.target);
    if (!path || path->empty()) {
        done(false);
        return;
    }
    std::string const head = path->front();
    std::span<std::string const> const rest = std::span<std::string const>{*path}.subspan(1);
    Value value = evalIn(scope, ctx.bindings.get(), command.value);
    if (head == "app") {
        runSetApp(command, rest, std::move(value), done);
        return;
    }
    NameRef const* const found = scope.find(head);
    if (found != nullptr && found->category == NameCategory::State) {
        writeSlot(*found->state, rest, std::move(value), command, done);
        return;
    }
    if (found != nullptr && found->category == NameCategory::Element) {
        ElementState& element = *found->element;
        session.noteWrite();
        element.draft.set(value);
        if (element.bound == nullptr) {
            element.committed.set(std::move(value));
        }
        done(true);
        return;
    }
    report(command.path, "`set` names nothing writable here");
    done(false);
}

void ScreenCore::runReset(Command const& command, CommandCtx const& ctx, Done const& done) {
    RtScope const& scope = *ctx.scope;
    NameRef const* const found = scope.find(command.target);
    auto const resetStates = [this](RtScope const& owner) {
        for (auto const& slot : owner.states) {
            if (slot->value) {
                Value init = runtime().untracked([&] { return evalIn(owner, nullptr, slot->decl->init); });
                slot->value->set(std::move(init));
            }
        }
        for (auto const& element : owner.elements) {
            element->draft.set(element->bound != nullptr ? runtime().untracked([&] { return element->bound->get(); })
                                                         : Value{});
            element->committed.set(Value{});
            element->edited.set(false);
            element->touched.set(false);
        }
    };
    bool known = true;
    session.noteWrite();
    if (found != nullptr && found->category == NameCategory::State) {
        StateSlot& slot = *found->state;
        initState(*slot.scope, slot);
        slot.value->set(runtime().untracked([&] { return evalIn(*slot.scope, nullptr, slot.decl->init); }));
    } else if (found != nullptr && found->category == NameCategory::Element) {
        ElementState& element = *found->element;
        element.submitted.set(false);
        if (RtScope const* const inner = element.inner.peek()) {
            resetStates(*inner);
        }
    } else {
        report(command.path, "`reset` names no state or dialog here");
        known = false;
    }
    done(known);
}

void ScreenCore::runHostCommand(Command const& command, CommandCtx const& ctx, Done const& done) {
    Value const args = evalIn(*ctx.scope, ctx.bindings.get(), command.value);
    if (auto const& run = session.hostCommandRunner()) {
        HostCommandResult const result = run(command.target, args);
        if (result.kind == HostCommandResult::Kind::Missing) {
            report(command.path, "host command `" + command.target + "` is missing");
        } else if (result.kind == HostCommandResult::Kind::Refused) {
            report(command.path, result.reason);
        }
        done(result.kind == HostCommandResult::Kind::Ran);
        return;
    }
    bool const known = host != nullptr && host->hostCommand(command.target, args);
    if (!known) {
        report(command.path, "host command `" + command.target + "` is missing");
    }
    done(known);
}

namespace {

// NOLINTNEXTLINE(readability-function-cognitive-complexity): a download and an upload, each settled.
/// @brief A transfer's share of its screen's pending count.
///
/// A transfer is pending from the host accepting it to its settle: its completion carries on the command list that
/// started it, which the close protocol and save-and-close wait for. The count is taken once the host accepts, so a
/// refused transfer never counts, and a host that settles before it returns leaves nothing taken. It is given back
/// once, also when the transfer settles after its scope is gone, but not after the screen itself is gone, since the
/// count went with it.
class TransferHold {
public:
    /// @brief A hold that counts nothing yet.
    /// @param core  The screen whose count it moves.
    /// @param alive Expires with the screen.
    TransferHold(ScreenCore& core, std::weak_ptr<bool> alive) : _core{&core}, _alive{std::move(alive)} {}

    /// @brief Counts the transfer, unless it has already settled.
    void take() {
        if (_state == State::NotTaken) {
            _state = State::Taken;
            _core->addPending(1);
        }
    }

    /// @brief Gives the count back, if one was taken and the screen is still there; the transfer counts no more.
    void release() {
        if (_state == State::Taken && !_alive.expired()) {
            _core->addPending(-1);
        }
        _state = State::Released;
    }

private:
    enum class State : std::uint8_t { NotTaken, Taken, Released };

    ScreenCore* _core;
    std::weak_ptr<bool> _alive;
    State _state = State::NotTaken;
};

/// @brief Bytes as base64 (RFC 4648, with padding): how a document carries an inline file.
/// @param bytes The bytes.
/// @return The text.
[[nodiscard]] std::string base64Encode(std::string_view bytes) {
    static constexpr std::string_view kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    for (std::size_t index = 0; index < bytes.size(); index += 3) {
        std::size_t const left = std::min<std::size_t>(3, bytes.size() - index);
        std::uint32_t group = 0;
        for (std::size_t offset = 0; offset < 3; ++offset) {
            group <<= 8U;
            if (offset < left) {
                group |= static_cast<unsigned char>(bytes.at(index + offset));
            }
        }
        for (std::size_t digit = 0; digit < 4; ++digit) {
            out.push_back(digit <= left ? kAlphabet.at((group >> (18U - (6U * digit))) & 0x3FU) : '=');
        }
    }
    return out;
}
}  // namespace

[[nodiscard]] std::optional<std::string> base64Decode(std::string_view text) {
    while (!text.empty() && text.back() == '=') {
        text.remove_suffix(1);
    }
    if (text.size() % 4 == 1) {
        return std::nullopt;
    }
    auto const digitOf = [](char chr) -> int {
        if (chr >= 'A' && chr <= 'Z') {
            return chr - 'A';
        }
        if (chr >= 'a' && chr <= 'z') {
            return chr - 'a' + 26;
        }
        if (chr >= '0' && chr <= '9') {
            return chr - '0' + 52;
        }
        if (chr == '+') {
            return 62;
        }
        return chr == '/' ? 63 : -1;
    };
    std::string out;
    out.reserve((text.size() * 3) / 4);
    std::uint32_t group = 0;
    int bits = 0;
    for (char const chr : text) {
        int const digit = digitOf(chr);
        if (digit < 0) {
            return std::nullopt;
        }
        group = (group << 6U) | static_cast<std::uint32_t>(digit);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((group >> static_cast<std::uint32_t>(bits)) & 0xFFU));
        }
    }
    return out;
}

[[nodiscard]] std::size_t base64Size(std::string_view text) {
    while (!text.empty() && text.back() == '=') {
        text.remove_suffix(1);
    }
    return (text.size() * 3) / 4;
}

[[nodiscard]] std::string sizeText(std::size_t bytes) {
    constexpr std::size_t kKiB = 1024;
    if (bytes < kKiB) {
        return std::to_string(bytes) + " bytes";
    }
    bool const mebi = bytes >= kKiB * kKiB;
    std::size_t const unit = mebi ? kKiB * kKiB : kKiB;
    // One decimal place, rounded half up, and none when it is zero.
    std::size_t const tenths = ((bytes * 10) + (unit / 2)) / unit;
    std::string text = std::to_string(tenths / 10);
    if (tenths % 10 != 0) {
        text += "." + std::to_string(tenths % 10);
    }
    return text + (mebi ? " MiB" : " KiB");
}

[[nodiscard]] std::expected<std::string, std::string> inlineBytes(Value const& value) {
    if (std::string const* const text = value.asString()) {
        if (std::optional<std::string> bytes = base64Decode(*text)) {
            return *std::move(bytes);
        }
        return std::unexpected(std::string{"the text is not base64"});
    }
    if (ValueList const* const list = value.asList()) {
        std::string bytes;
        bytes.reserve(list->size());
        for (Value const& item : *list) {
            std::int64_t const* const byte = item.asInt();
            if (byte == nullptr || *byte < 0 || *byte > 255) {
                return std::unexpected(std::string{"a byte list holds integers 0 to 255"});
            }
            bytes.push_back(static_cast<char>(static_cast<unsigned char>(*byte)));
        }
        return bytes;
    }
    return std::unexpected(std::string{"inline bytes are base64 text or a list of bytes"});
}

void ScreenCore::runTransfer(Command const& command, CommandCtx const& ctx, Done done) {
    Value const file = evalIn(*ctx.scope, ctx.bindings.get(), command.value);
    if (command.kind == CommandKind::Download) {
        runDownload(command, ctx, file, std::move(done));
    } else {
        runUpload(command, ctx, file, std::move(done));
    }
}

void ScreenCore::runDownload(Command const& command, CommandCtx const& ctx, Value const& file, Done done) {
    Value const name = evalIn(*ctx.scope, ctx.bindings.get(), command.extra);
    auto const hold = std::make_shared<TransferHold>(*this, _alive);
    auto const settled = [this, weak = std::weak_ptr<RtScope>{ctx.scope}, target = command.target,
                          preview = command.option == "preview", done,
                          hold](std::expected<Value, std::string> const& reply) {
        auto const held = weak.lock();
        if (!held) {
            hold->release();
            return;
        }
        runtime().batch([&] {
            bool const succeeded = reply.has_value();
            NameRef const* const found = succeeded && preview ? held->find(target) : nullptr;
            if (found != nullptr && found->category == NameCategory::State) {
                initState(*found->state->scope, *found->state);
                session.noteWrite();
                found->state->value->set(*reply);
            }
            done(succeeded);
            hold->release();
        });
    };
    if (command.bytes) {
        std::optional<InlineFile> const inlined = inlineDownload(command, ctx, file, name);
        if (!inlined) {
            done(false);
            return;
        }
        if (host == nullptr || !host->saveFile(*inlined, command.option, settled)) {
            unsupported(command, "`download`");
            done(false);
            return;
        }
        hold->take();
        return;
    }
    if (host == nullptr || !host->download(file, command.option, name, settled)) {
        unsupported(command, "`download`");
        done(false);
        return;
    }
    hold->take();
}

// Inline bytes are checked before the host is asked: content over the cap, or that does not decode, never reaches it.
std::optional<InlineFile> ScreenCore::inlineDownload(
    Command const& command, CommandCtx const& ctx,
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): the command's `file` and `name`, in its order.
    Value const& file, Value const& name) {
    std::size_t const cap = session.options().maxInlineFileBytes;
    std::size_t size = 0;
    if (std::string const* const text = file.asString()) {
        size = base64Size(*text);
    } else if (ValueList const* const list = file.asList()) {
        size = list->size();
    }
    std::expected<std::string, std::string> bytes = size > cap ? std::unexpected(std::string{}) : inlineBytes(file);
    if (size > cap || (bytes && bytes->size() > cap)) {
        report(command.path, "download: " + sizeText(bytes ? bytes->size() : size) + " is over the " + sizeText(cap) +
                                 " inline cap; serve it through the file store");
        return std::nullopt;
    }
    if (!bytes) {
        report(command.path, "download: " + bytes.error());
        return std::nullopt;
    }
    return InlineFile{
        .name = textOf(name, ctx.scope->formatting()),
        .mediaType = command.mediaType
                         ? textOf(evalIn(*ctx.scope, ctx.bindings.get(), command.mediaType), ctx.scope->formatting())
                         : std::string{},
        .bytes = *std::move(bytes),
    };
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one branch per outcome an upload can have.
void ScreenCore::runUpload(Command const& command, CommandCtx const& ctx, Value const& file, Done done) {
    RtScope const& scope = *ctx.scope;
    std::weak_ptr<RtScope> const weak = ctx.scope;
    auto const hold = std::make_shared<TransferHold>(*this, _alive);
    NameRef const* const transferName = command.transfer.empty() ? nullptr : scope.find(command.transfer);
    TransferState* const transfer =
        transferName != nullptr && transferName->category == NameCategory::Transfer ? transferName->transfer : nullptr;
    auto const progress = [weak, transfer](Value fraction) {
        if (weak.lock() && transfer != nullptr) {
            transfer->progress.set(std::move(fraction));
        }
    };
    // Settles with the members the mutation's body gets beside its own (the reference or the inline file), or the
    // failure.
    auto const settled = [this, weak, kept = detached(ctx), transfer, command = &command, done,
                          hold](std::expected<ValueRecord, std::string> reply) {
        auto const held = weak.lock();
        if (!held) {
            hold->release();
            return;
        }
        runtime().batch([&] {
            if (transfer != nullptr) {
                transfer->pending.set(false);
                transfer->error.set(reply ? Value{} : Value{reply.error()});
            }
            if (!reply) {
                done(false);
                hold->release();
                return;
            }
            NameRef const* const found = held->find(command->target);
            // The mutation's body cannot name the reference (a body is checked where the mutation is declared),
            // so the reference is added to it under `as`, `file` by default.
            MutationRuntime& mutation = *found->mutation;
            Value const base = evalIn(*held, kept.bindings.get(), mutation.decl().body);
            ValueRecord fields = base.asRecord() == nullptr ? ValueRecord{} : *base.asRecord();
            for (auto& [member, value] : *reply) {
                fields.insert_or_assign(member, std::move(value));
            }
            // The run takes its own count before this one is given back, so pending never reads zero in between.
            mutation.run(Value{std::move(fields)}, std::make_shared<Bindings const>(*kept.bindings),
                         [done](bool succeeded, Value const&) { done(succeeded); });
            hold->release();
        });
    };
    if (host == nullptr) {
        unsupported(command, "`upload`");
        done(false);
        return;
    }
    if (transfer != nullptr) {
        transfer->pending.set(true);
        transfer->progress.set(Value{std::int64_t{0}});
        transfer->error.set(Value{});
    }
    NameRef const* const target = scope.find(command.target);
    UploadRequest request{.file = file, .modelType = {}, .action = {}};
    if (target != nullptr && target->category == NameCategory::Mutation && target->mutation != nullptr) {
        request.modelType = target->mutation->typeId();
        request.action = target->mutation->decl().action;
    }
    std::string const member = command.binding.empty() ? std::string{"file"} : command.binding;
    bool started = false;
    if (command.bytes) {
        std::size_t const cap = session.options().maxInlineFileBytes;
        started = host->readFile(
            file, cap,
            [this, settled, progress, cap, member, command = &command](std::expected<InlineFile, std::string> read) {
                if (!read) {
                    settled(std::unexpected(read.error()));
                    return;
                }
                if (read->bytes.size() > cap) {
                    // The host should have refused it; the cap holds whether or not it did.
                    std::string why = "upload: " + sizeText(read->bytes.size()) + " is over the " + sizeText(cap) +
                                      " inline cap; send it through the file store";
                    report(command->path, why);
                    settled(std::unexpected(std::move(why)));
                    return;
                }
                progress(Value{std::int64_t{1}});
                ValueRecord added;
                added.insert_or_assign(member, Value{base64Encode(read->bytes)});
                if (!command->nameAs.empty()) {
                    added.insert_or_assign(command->nameAs, Value{read->name});
                }
                if (!command->typeAs.empty()) {
                    added.insert_or_assign(command->typeAs, Value{read->mediaType});
                }
                settled(std::move(added));
            });
    } else {
        started = host->uploadFor(request, progress, [settled, member](std::expected<Value, std::string> reference) {
            if (!reference) {
                settled(std::unexpected(reference.error()));
                return;
            }
            ValueRecord added;
            added.insert_or_assign(member, *std::move(reference));
            settled(std::move(added));
        });
    }
    if (!started) {
        if (transfer != nullptr) {
            transfer->pending.set(false);
            transfer->progress.set(Value{});
        }
        unsupported(command, "`upload`");
        done(false);
        return;
    }
    hold->take();
}

}  // namespace detail

namespace detail {
namespace {

// --- Lowering --------------------------------------------------------------------------------------------------

/// @brief A value as a `bool` property: `true` only for `true`.
/// @param value  The value.
/// @param format Unused.
/// @return It.
[[nodiscard]] bool boolOf(Value const& value, FormatContext const& format) {
    static_cast<void>(format);
    bool const* const flag = value.asBool();
    return flag != nullptr && *flag;
}

/// @brief A value as a text property.
/// @param value  The value.
/// @param format The formatting facts.
/// @return Its text.
[[nodiscard]] std::string stringOf(Value const& value, FormatContext const& format) { return textOf(value, format); }
}  // namespace

[[nodiscard]] std::string componentText(Value const& value, std::string_view type, FormatContext const& format) {
    if (type == "json") {
        return canonicalJson(value);
    }
    if (type == "int" || type == "exact") {
        if (std::int64_t const* const whole = value.asInt()) {
            return std::to_string(*whole);
        }
        // A value that cannot be rounded at its own places is written as its exact JSON, never clamped.
        if (Decimal const* const number = value.asDecimal()) {
            return detail::decimalText(number->value, static_cast<std::uint32_t>(number->value.decimalPlaces.value))
                .value_or(canonicalJson(value));
        }
        if (Quantity const* const quantity = value.asQuantity()) {
            auto const text =
                detail::decimalText(quantity->value, static_cast<std::uint32_t>(quantity->value.decimalPlaces.value));
            return text ? *text + " " + quantity->unit : canonicalJson(value);
        }
    }
    return textOf(value, format);
}

namespace {

/// @brief An exact rational as the nearest double, unrounded.
///
/// `Rational::toDouble` rounds to the value's own places, which is a display reading: `29 / 100` keeps its operands'
/// 0 places and would read as 0. The view tree wants the magnitude, so the lowering reads it here.
/// @param value The rational.
/// @return Its numerator divided by its denominator.
[[nodiscard]] double realOf(math::Rational const& value) noexcept {
    return static_cast<double>(value.numerator) / static_cast<double>(value.denominator);
}
}  // namespace

[[nodiscard]] double componentNumber(Value const& value) {
    if (std::int64_t const* const whole = value.asInt()) {
        return static_cast<double>(*whole);
    }
    if (Decimal const* const number = value.asDecimal()) {
        return realOf(number->value);
    }
    if (Quantity const* const quantity = value.asQuantity()) {
        return realOf(quantity->value);
    }
    return 0.0;
}

[[nodiscard]] Value componentPayload(std::string const& payload, Type const* type) {
    std::string_view const text{payload};
    if (text.starts_with("i:")) {
        std::int64_t whole = 0;
        std::string_view const digits = text.substr(2);
        char const* const last = std::to_address(digits.end());
        auto const [end, error] = std::from_chars(std::to_address(digits.begin()), last, whole);
        if (error == std::errc{} && end == last) {
            return Value{whole};
        }
    }
    if (text.starts_with("s:")) {
        return Value{std::string{text.substr(2)}};
    }
    if (auto json = parseJson(text); json && type != nullptr) {
        if (auto value = valueFromJson(*json, *type)) {
            return std::move(*value);
        }
    }
    return Value{payload};
}

[[nodiscard]] std::int64_t intOf(Value const& value, std::int64_t fallback) {
    if (std::int64_t const* const number = value.asInt()) {
        return *number;
    }
    if (Decimal const* const number = value.asDecimal()) {
        // Integer division truncates toward zero, and reads the exact value rather than one rounded to its places.
        return number->value.numerator / number->value.denominator;
    }
    return fallback;
}

namespace {

/// @brief A text role by name.
/// @param name `normal`, `muted`, `heading` (or `title`), `section`, `error`, `success`, `mono`, `display`, `small`,
/// `overline`, `caption`, `label`, `headline`.
/// @return The role; `nullopt` for any other name.
[[nodiscard]] std::optional<TextRole> roleNamed(std::string_view name) {
    if (name == "normal") {
        return TextRole::Normal;
    }
    if (name == "muted") {
        return TextRole::Muted;
    }
    if (name == "heading" || name == "title") {
        return TextRole::Heading;
    }
    if (name == "section") {
        return TextRole::Section;
    }
    if (name == "error") {
        return TextRole::Error;
    }
    if (name == "success") {
        return TextRole::Success;
    }
    if (name == "mono") {
        return TextRole::Mono;
    }
    if (name == "display") {
        return TextRole::Display;
    }
    if (name == "small") {
        return TextRole::Small;
    }
    if (name == "overline") {
        return TextRole::Overline;
    }
    if (name == "caption") {
        return TextRole::Caption;
    }
    if (name == "label") {
        return TextRole::Label;
    }
    if (name == "headline") {
        return TextRole::Headline;
    }
    return std::nullopt;
}

/// @brief What a role that names none is told: the names `roleNamed` reads.
constexpr std::string_view kRoleNames =
    "a role is normal, muted, heading, section, error, success, mono, display, small, overline, caption, label or "
    "headline";

/// @brief A text role by name, as `roleNamed` reads it.
/// @param value  The name.
/// @param format Unused.
/// @return The role; `Normal` for any other name.
[[nodiscard]] TextRole roleOf(Value const& value, FormatContext const& format) {
    return roleNamed(textOf(value, format)).value_or(TextRole::Normal);
}

/// @brief A tone by name.
/// @param value  `neutral`, `info`, `ok` (or `success`), `warn` (or `warning`), `err` (or `error`).
/// @param format Unused.
/// @return The tone; `Neutral` for anything else.
[[nodiscard]] Tone toneOf(Value const& value, FormatContext const& format) {
    std::string const text = textOf(value, format);
    if (text == "info") {
        return Tone::Info;
    }
    if (text == "ok" || text == "success") {
        return Tone::Ok;
    }
    if (text == "warn" || text == "warning") {
        return Tone::Warn;
    }
    if (text == "err" || text == "error") {
        return Tone::Err;
    }
    return Tone::Neutral;
}
}  // namespace

[[nodiscard]] std::optional<Tone> toneNamed(Value const& value) {
    if (value.isNull()) {
        return Tone::Neutral;
    }
    static constexpr std::array<std::pair<std::string_view, Tone>, 5> kTones = {
        {
            {"neutral", Tone::Neutral},
            {"info", Tone::Info},
            {"ok", Tone::Ok},
            {"warn", Tone::Warn},
            {"err", Tone::Err},
        },
    };
    if (std::string const* const text = value.asString()) {
        // A loop, not std::ranges::find: an array's iterator is a pointer on some standard libraries and not on
        // others, so `auto` there reads differently to each.
        for (auto const& [name, tone] : kTones) {
            if (name == *text) {
                return tone;
            }
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::vector<TextSpan> spansOf(Value const& value, FormatContext const& format, bool& dropped,
                                            bool* badTone) {
    std::vector<TextSpan> spans;
    if (value.isNull()) {
        return spans;
    }
    ValueList const* const runs = value.asList();
    if (runs == nullptr) {
        dropped = true;
        return spans;
    }
    auto const flag = [](Value const* field) {
        bool const* const set = field != nullptr ? field->asBool() : nullptr;
        return set != nullptr && *set;
    };
    for (Value const& run : *runs) {
        if (run.asRecord() == nullptr) {
            if (run.isNull() || run.asList() != nullptr) {
                dropped = true;
            } else {
                spans.push_back(TextSpan{.text = textOf(run, format)});
            }
            continue;
        }
        Value const* const text = run.field("text");
        if (text == nullptr) {
            dropped = true;
            continue;
        }
        Tone tone = Tone::Neutral;
        if (Value const* const named = run.field("tone")) {
            if (std::optional<Tone> const known = toneNamed(*named)) {
                tone = *known;
            } else if (badTone != nullptr) {
                *badTone = true;
            }
        }
        spans.push_back(TextSpan{
            .text = textOf(*text, format),
            .strong = flag(run.field("strong")),
            .mono = flag(run.field("mono")),
            .tone = tone,
            .action = flag(run.field("action")),
        });
    }
    return spans;
}

namespace {

/// @brief Whether a scope declares any name, so a node in it reads names the screen's own scope lacks.
/// @param scope The scope.
/// @return True when it declares a model, state, let, query, mutation, watch or check.
[[nodiscard]] bool declaresNames(ScopeDecl const& scope) noexcept {
    return !(scope.models.empty() && scope.state.empty() && scope.lets.empty() && scope.queries.empty() &&
             scope.mutations.empty() && scope.watches.empty() && scope.checks.empty());
}

/// @brief What a span list with a run it drops is told.
constexpr std::string_view kSpansReason =
    "spans is a list of runs, each a value or a record with `text`; a list, a null or a record without `text` is "
    "dropped";

/// @brief What a tone of no known name is told.
constexpr std::string_view kToneNames = "a tone is neutral, info, ok, warn or err, or null for none";

/// @brief What a span list with a run of a tone of no known name is told.
constexpr std::string_view kSpanToneReason =
    "a run's tone is neutral, info, ok, warn or err, or null for none; a run of another tone is drawn untoned";

/// @brief A splitter's `sizes`: a list of numbers, each read as `intOf` reads it.
/// @param value The value.
/// @return The sizes; `nullopt` for anything but a list whose every item is an `int` or a decimal.
[[nodiscard]] std::optional<std::vector<int>> splitterSizesOf(Value const& value) {
    ValueList const* const items = value.asList();
    if (items == nullptr) {
        return std::nullopt;
    }
    std::vector<int> sizes;
    sizes.reserve(items->size());
    for (Value const& item : *items) {
        if (item.asInt() == nullptr && item.asDecimal() == nullptr) {
            return std::nullopt;
        }
        sizes.push_back(static_cast<int>(intOf(item, 0)));
    }
    return sizes;
}
}  // namespace

[[nodiscard]] Sizing sizingOf(Value const& value) {
    if (Value const* const fixed = value.field("fixed")) {
        return Sizing::fixed(static_cast<int>(intOf(*fixed, 0)));
    }
    if (Value const* const percent = value.field("percent")) {
        return Sizing::percent(static_cast<int>(intOf(*percent, 1)));
    }
    if (Value const* const stretch = value.field("stretch")) {
        return Sizing::stretch(static_cast<int>(intOf(*stretch, 1)));
    }
    return Sizing::content();
}

[[nodiscard]] bool isSizing(Value const& value) {
    if (std::string const* const text = value.asString()) {
        return *text == "content";
    }
    ValueRecord const* const record = value.asRecord();
    if (record == nullptr || record->size() != 1 || record->begin()->second.asInt() == nullptr) {
        return false;
    }
    if (record->contains("percent")) {
        std::int64_t const share = *record->begin()->second.asInt();
        return share >= 1 && share <= 100;
    }
    return record->contains("fixed") || record->contains("stretch");
}

namespace {

/// @brief An alignment by name, as the loader checked it: `start`, `center`, `end`, or stretch for none.
/// @param name The name; empty for none.
/// @return The alignment.
[[nodiscard]] Align alignOf(std::string_view name) {
    if (name == "start") {
        return Align::Start;
    }
    if (name == "center") {
        return Align::Center;
    }
    if (name == "end") {
        return Align::End;
    }
    return Align::Stretch;
}

/// @brief A table cell's place by name: across its column (`align`) or down its row (`valign`).
/// @param name `start`, `center` or `end`.
/// @return The place; `nullopt` for any other name, `stretch` included.
[[nodiscard]] std::optional<Align> cellPlaceNamed(std::string_view name) {
    if (name == "start") {
        return Align::Start;
    }
    if (name == "center") {
        return Align::Center;
    }
    if (name == "end") {
        return Align::End;
    }
    return std::nullopt;
}

/// @brief A justification by name, as the loader checked it: `center`, `end`, `between`, or start for none.
/// @param name The name; empty for none.
/// @return The justification.
[[nodiscard]] Justify justifyOf(std::string_view name) {
    if (name == "center") {
        return Justify::Center;
    }
    if (name == "end") {
        return Justify::End;
    }
    if (name == "between") {
        return Justify::Between;
    }
    return Justify::Start;
}

/// @brief An axis by name: `horizontal`, or vertical for anything else.
/// @param value The value.
/// @return The axis.
[[nodiscard]] Axis axisOf(Value const& value) {
    std::string const* const text = value.asString();
    return text != nullptr && *text == "horizontal" ? Axis::Horizontal : Axis::Vertical;
}

/// @brief A step's state by name: `active`, `done`, `failed`, or pending for anything else.
/// @param name The name.
/// @return The state.
[[nodiscard]] StepState stepStateOf(std::string_view name) {
    if (name == "active") {
        return StepState::Active;
    }
    if (name == "done") {
        return StepState::Done;
    }
    return name == "failed" ? StepState::Failed : StepState::Pending;
}

/// @brief The options of a select, from a list of scalars or of records with a key and a `label`.
/// @param options The list.
/// @param format  The formatting facts.
/// @return The options.
[[nodiscard]] std::vector<SelectOption> selectOptions(Value const& options, FormatContext const& format) {
    std::vector<SelectOption> out;
    if (ValueList const* const items = options.asList()) {
        for (Value const& option : *items) {
            Value const key = optionKey(option);
            Value const* const label = option.field("label");
            out.push_back(SelectOption{.key = keyOf(key), .label = textOf(label != nullptr ? *label : key, format)});
        }
    }
    return out;
}

/// @brief What a percent sizes, as a refusal or a report says it.
constexpr std::string_view kPercentPlaces =
    "`percent` sizes a width along a row or a horizontal splitter; nothing here takes a share";
/// @brief Why a percent inside a case or an overlay's content is not a share.
constexpr std::string_view kPercentInHolder = "size the `switch` or the overlay itself; its content fills it";

/// @brief The default spacing step between the children of a document panel or collapsible, the mockups' 8 px.
constexpr int kPanelGap = 2;
/// @brief The default spacing step between the children of a document dialog or drawer, the mockups' 12 px.
constexpr int kDialogGap = 3;

/// @brief A node's size request.
/// @param node The node; may be null.
/// @return Its layout hints, or none for a null node or a kind that has no `Common`.
[[nodiscard]] LayoutHints layoutHintsOf(Node const& node) {
    if (!node) {
        return {};
    }
    return std::visit(
        [](auto const& spec) -> LayoutHints {
            if constexpr (requires { spec.common.layout; }) {
                return spec.common.layout;
            } else {
                return {};
            }
        },
        node->kind);
}

/// @brief A node with its width sizing replaced; a node of a kind with no layout is returned as it is.
/// @param node  The node.
/// @param width The sizing.
/// @return The node.
[[nodiscard]] Node withWidth(Node const& node, Sizing width) {
    if (!node) {
        return node;
    }
    auto copy = std::make_shared<NodeData>(*node);
    std::visit(
        [&width](auto& spec) {
            if constexpr (requires { spec.common.layout; }) {
                spec.common.layout.width = width;
            }
        },
        copy->kind);
    return copy;
}

/// @brief The size request of a holder of @p sizings: a stretch by the largest weight where any is a stretch, and
///        content otherwise (a share of a row is not a holder's: `listColumn` passes a lone child's on).
/// @param sizings One dimension's sizing of each child.
/// @return The holder's sizing.
[[nodiscard]] Sizing holderSizing(std::span<Sizing const> sizings) {
    int weight = 0;
    for (Sizing const& sizing : sizings) {
        if (sizing.kind == Sizing::Kind::Stretch) {
            weight = std::max(weight, sizing.amount);
        }
    }
    return weight > 0 ? Sizing::stretch(weight) : Sizing::content();
}

/// @brief The column that holds a node list. It stretches along an axis when one of its children does, so the
///        content means what its nodes would mean written inline in the holder's parent.
/// @param children The content.
/// @param gap      The spacing step between children.
/// @return The column; bounds and `alignSelf` stay unset, because they belong to a child.
[[nodiscard]] Column listColumn(std::vector<Node> children, int gap = 0) {
    std::vector<Sizing> widths;
    std::vector<Sizing> heights;
    for (Node const& child : children) {
        LayoutHints const hints = layoutHintsOf(child);
        widths.push_back(hints.width);
        heights.push_back(hints.height);
    }
    // A lone child's share of its row is the holder's, and the child fills the holder, so the share is taken once.
    if (children.size() == 1 && widths.front().kind == Sizing::Kind::Percent) {
        Sizing const share = widths.front();
        children.front() = withWidth(children.front(), Sizing::stretch(1));
        Column column{.children = std::move(children), .gap = gap};
        column.common.layout.width = share;
        column.common.layout.height = holderSizing(heights);
        return column;
    }
    Column column{.children = std::move(children), .gap = gap};
    column.common.layout.width = holderSizing(widths);
    column.common.layout.height = holderSizing(heights);
    return column;
}

/// @brief Content whose scope is made when it mounts and released when it unmounts: a one-row `forEach` whose
///        row view builds it.
/// @param build Builds the content, its scope included.
/// @param hints The size request of the content. A slot reads the sizing of its direct child alone, which is this
///        `forEach`, so the content's stretch reaches the slot only through these.
/// @return The node.
[[nodiscard]] Node lazy(std::function<Node()> build, LayoutHints const& hints = {}) {
    Common common;
    common.layout = hints;
    return ui::forEach<int>([] { return std::vector<int>{0}; }, [](int const&) { return Key{std::int64_t{0}}; },
                            [build = std::move(build)](reactive::Signal<int> const&) { return build(); },
                            Axis::Vertical, 0, common);
}

/// @brief A column of content that holds its scope for as long as it is mounted: its `visible` is a binding
///        that keeps the scope alive and reads nothing.
/// @param children The content.
/// @param scope    The scope.
/// @param keys     The chords of the content.
/// @param gap      The spacing step between the children.
/// @return The node.
[[nodiscard]] Node holding(std::vector<Node> children, std::shared_ptr<RtScope> scope,
                           std::vector<KeyBinding> keys = {}, int gap = 0) {
    Column column = listColumn(std::move(children), gap);
    column.common.visible = Prop<bool>{[held = std::move(scope)] { return held != nullptr; }};
    column.common.keys = std::move(keys);
    return ui::column(std::move(column));
}

/// @brief Whether an expression reads a name in @p names.
/// @param expr  The expression.
/// @param names The names.
/// @return Whether any form of the expression names one.
// NOLINTBEGIN(misc-no-recursion): an expression nests its arguments and fields, and the walk follows them.
[[nodiscard]] bool readsAny(ExprPtr const& expr, std::set<std::string, std::less<>> const& names) {
    if (!expr) {
        return false;
    }
    if (!expr->name.empty() && names.contains(expr->name)) {
        return true;
    }
    return std::ranges::any_of(expr->args, [&](ExprPtr const& arg) { return readsAny(arg, names); }) ||
           std::ranges::any_of(expr->fields, [&](auto const& field) { return readsAny(field.second, names); });
}
// NOLINTEND(misc-no-recursion)

/// @brief Collects the element ids of a document subtree.
/// @param node The root.
/// @param out  Receives the ids.
// NOLINTNEXTLINE(misc-no-recursion): a node's children and parts are nodes, and the walk follows them.
void collectIds(DocNode const& node, std::set<std::string, std::less<>>& out) {
    if (!node.id.empty()) {
        out.insert(node.id);
    }
    for (auto const* group : {&node.children, &node.parts}) {
        for (auto const& [key, list] : *group) {
            for (DocNodePtr const& child : list) {
                collectIds(*child, out);
            }
        }
    }
}
}  // namespace

// NOLINTNEXTLINE(misc-no-recursion): lowering a node lowers its children, which come back through here.
Node ScreenCore::lowerAll(std::span<DocNodePtr const> nodes, LowerCtx const& ctx, int gap) {
    LowerCtx listCtx = ctx;
    listCtx.listGap = gap;
    if (nodes.size() == 1) {
        return lower(*nodes.front(), listCtx);
    }
    std::vector<Node> children;
    children.reserve(nodes.size());
    for (DocNodePtr const& child : nodes) {
        children.push_back(lower(*child, listCtx));
    }
    // Several nodes share a holder, and a share of a row is not a share of a holder: each is reported and takes its
    // content size.
    for (auto&& [child, written] : std::views::zip(children, nodes)) {
        if (layoutHintsOf(child).width.kind == Sizing::Kind::Percent) {
            report(written->path,
                   "a percent among several nodes is a share of their holder; " + std::string{kPercentPlaces});
            child = withWidth(child, Sizing::content());
        }
    }
    Column holder = listColumn(std::move(children), gap);
    // The column is the mount's, not the document's: a child that bleeds reaches past the padding of the container
    // the document names, so the column passes that padding on instead of hiding it.
    holder.holder = true;
    return ui::column(std::move(holder));
}

void ScreenCore::fillHolder(std::vector<Node>& children, std::string const& path) {
    for (Node& child : children) {
        if (layoutHintsOf(child).width.kind == Sizing::Kind::Percent) {
            report(path, kPercentInHolder);
            child = withWidth(child, Sizing::stretch(1));
        }
    }
}

LayoutHints ScreenCore::nodeHints(DocNode const& node, std::set<std::string, std::less<>> const& own,
                                  LowerCtx const& ctx) {
    LayoutHints hints;
    if (ExprPtr const sizing = node.prop("sizing")) {
        if (sizing->kind != ExprKind::Literal && readsAny(sizing, own)) {
            report(sizing->path,
                   "this sizing reads a name the case declares, so the case is held at its "
                   "content size");
        } else {
            Value const value = once(ctx, sizing);
            Value const* const width = value.field("width");
            Value const* const height = value.field("height");
            if (width != nullptr || height != nullptr) {
                hints.width = width != nullptr && isSizing(*width) ? sizingOf(*width) : Sizing::content();
                hints.height = height != nullptr && isSizing(*height) ? sizingOf(*height) : Sizing::content();
            } else if (isSizing(value)) {
                hints.width = sizingOf(value);
            }
        }
    }
    return hints;
}

LayoutHints ScreenCore::holderHints(std::span<DocNode const* const> content, ScopeDecl const* decl,
                                    LowerCtx const& ctx) {
    std::set<std::string, std::less<>> own;
    for (DocNode const* const node : content) {
        collectIds(*node, own);
    }
    if (decl != nullptr) {
        for (ModelDecl const& model : decl->models) {
            own.insert(model.name);
        }
        for (StateDecl const& state : decl->state) {
            own.insert(state.name);
        }
        for (LetDecl const& let : decl->lets) {
            own.insert(let.name);
        }
        for (QueryDecl const& query : decl->queries) {
            own.insert(query.name);
        }
        for (MutationDecl const& mutation : decl->mutations) {
            own.insert(mutation.name);
        }
    }
    std::vector<Sizing> widths;
    std::vector<Sizing> heights;
    for (DocNode const* const node : content) {
        LayoutHints const hints = nodeHints(*node, own, ctx);
        widths.push_back(hints.width);
        heights.push_back(hints.height);
    }
    LayoutHints out;
    out.width = holderSizing(widths);
    out.height = holderSizing(heights);
    return out;
}

// NOLINTBEGIN(misc-no-recursion): a menu nests, and lowering descends the document.
Node ScreenCore::lower(DocNode const& node, LowerCtx const& ctx) {
    if (recording) {
        if (loweredNodes.size() >= kPruneLoweredAt && loweredNodes.size() % kPruneLoweredAt == 0) {
            std::erase_if(loweredNodes, [](LoweredNode const& entry) { return entry.scope.expired(); });
        }
        loweredNodes.push_back(LoweredNode{.node = &node, .scope = ctx.scope, .bindings = ctx.bindings});
    }
    // Only a switch reads the gap of the list around it; every other kind's children are held by a holder that sets
    // their gap, so none sees its grandparent's.
    if (ctx.listGap != 0 && node.kind != "switch") {
        LowerCtx plain = ctx;
        plain.listGap = 0;
        return lowerKind(node, plain, elementFor(*ctx.scope, node));
    }
    return lowerKind(node, ctx, elementFor(*ctx.scope, node));
}

Prop<TextRole> ScreenCore::roleProp(LowerCtx const& ctx, ExprPtr const& expr) {
    if (!expr || expr->kind == ExprKind::Literal) {
        return bindProp<TextRole>(ctx, expr, TextRole::Normal, roleOf);
    }
    return slotProp<TextRole>([this, scope = ctx.scope, bindings = ctx.bindings, expr] {
        Value const value = evalIn(*scope, bindings.get(), expr);
        std::optional<TextRole> const role = roleNamed(textOf(value, scope->formatting()));
        if (!role) {
            report(expr->path, "role is " + canonicalJson(value) + "; " + std::string{kRoleNames});
        }
        return role.value_or(TextRole::Normal);
    });
}

Prop<std::string> ScreenCore::surfaceOf(DocNode const& node, LowerCtx const& ctx) {
    ExprPtr const expr = node.prop("surface");
    auto nameOf = [this, path = pointerAppend(node.path, "surface")](Value const& value) -> std::string {
        if (std::string const* const name = value.asString()) {
            return *name;
        }
        if (!value.isNull()) {
            report(path, "a surface is the name of one; " + canonicalJson(value) + " is not a string");
        }
        return {};
    };
    if (!expr || expr->kind == ExprKind::Literal) {
        return expr ? Prop<std::string>{nameOf(expr->literal)} : Prop<std::string>{std::string{}};
    }
    return slotProp<std::string>([scope = ctx.scope, bindings = ctx.bindings, expr, nameOf = std::move(nameOf)] {
        return nameOf(evalIn(*scope, bindings.get(), expr));
    });
}

KeyValueItem ScreenCore::keyValueItemOf(Value const& item, std::string const& path, RtScope const& scope) {
    FormatContext const& format = scope.formatting();
    Value const* const label = item.field("label");
    Value const* const value = item.field("value");
    KeyValueItem out{
        .label = label == nullptr ? std::string{} : textOf(*label, format),
        .value = value == nullptr ? std::string{} : textOf(*value, format),
    };
    if (Value const* const spans = item.field("spans")) {
        bool dropped = false;
        bool badTone = false;
        out.spans = spansOf(*spans, format, dropped, &badTone);
        if (dropped) {
            report(path, kSpansReason);
        }
        if (badTone) {
            report(path, kSpanToneReason);
        }
    }
    if (Value const* const tone = item.field("tone")) {
        std::optional<Tone> const named = toneNamed(*tone);
        if (!named) {
            report(path, "an item's tone is " + canonicalJson(*tone) + "; " + std::string{kToneNames});
        }
        out.tone = named.value_or(Tone::Neutral);
    }
    return out;
}

// A group needs no widget of its own: it lowers to kinds every backend has, so the view tree and the contract do not
// change. Its description is a line read in order after its children, since `Common` carries no accessible
// description.
Node ScreenCore::lowerGroup(DocNode const& node, LowerCtx const& ctx, ElementState* element,
                            std::vector<Node> children) {
    Text legend{
        .text = bindProp<std::string>(ctx, node.prop("label"), std::string{}, stringOf),
        .role = TextRole::Label,
    };
    legend.field.required = bindProp<bool>(ctx, node.prop("required"), false, boolOf);
    std::vector<Node> column;
    column.reserve(children.size() + 3);
    column.push_back(ui::text(std::move(legend)));
    std::ranges::move(children, std::back_inserter(column));
    if (ExprPtr const description = node.prop("description")) {
        Text line{.text = bindProp<std::string>(ctx, description, std::string{}, stringOf), .role = TextRole::Muted};
        line.common.visible = slotProp<bool>([scope = ctx.scope, bindings = ctx.bindings, description] {
            return !textOf(evalIn(*scope, bindings.get(), description), scope->formatting()).empty();
        });
        column.push_back(ui::text(std::move(line)));
    }
    // The shown errors, one line each: a check that targets the group shows its message here.
    auto const shown = [this, element, scope = ctx.scope, bindings = ctx.bindings, errors = node.prop("errors")] {
        return stringsOf(element != nullptr ? elementValue(*element, "shownErrors")
                                            : evalIn(*scope, bindings.get(), errors));
    };
    Text messages{
        .text = slotProp<std::string>([shown] {
            std::string joined;
            for (std::string const& message : shown()) {
                joined += (joined.empty() ? "" : "\n") + message;
            }
            return joined;
        }),
        .role = TextRole::Error,
    };
    messages.common.visible = slotProp<bool>([shown] { return !shown().empty(); });
    column.push_back(ui::text(std::move(messages)));
    Column spec{
        .children = std::move(column),
        .gap = static_cast<int>(intOf(node.prop("gap") ? once(ctx, node.prop("gap")) : Value{}, 0)),
    };
    commonOf(node, ctx, element, spec.common);
    if (spec.common.a11y.role.empty()) {
        spec.common.a11y.role = "group";
    }
    bool const named = std::ranges::any_of(node.partList("a11y"),
                                           [](DocNodePtr const& a11y) { return a11y->prop("name") != nullptr; });
    if (!named) {
        spec.common.a11y.name = bindProp<std::string>(ctx, node.prop("label"), std::string{}, stringOf);
    }
    return ui::column(std::move(spec));
}

Prop<std::vector<TextSpan>> ScreenCore::spansProp(LowerCtx const& ctx, ExprPtr const& expr) {
    return slotProp<std::vector<TextSpan>>([this, scope = ctx.scope, bindings = ctx.bindings, expr] {
        bool dropped = false;
        bool badTone = false;
        std::vector<TextSpan> spans =
            spansOf(evalIn(*scope, bindings.get(), expr), scope->formatting(), dropped, &badTone);
        if (dropped) {
            report(expr->path, kSpansReason);
        }
        if (badTone) {
            report(expr->path, kSpanToneReason);
        }
        return spans;
    });
}

// The size request: `sizing` (the width, or both dimensions), `alignSelf`, and the bounds.
void ScreenCore::layoutOf(DocNode const& node, LowerCtx const& ctx, LayoutHints& layout) {
    if (ExprPtr const sizing = node.prop("sizing")) {
        // The width, or both dimensions from a `{width, height}` record, one left out being content. The loader
        // refuses a literal of another form; a computed one that is none is reported and lowers to content.
        Value const value = once(ctx, sizing);
        Value const* const width = value.field("width");
        Value const* const height = value.field("height");
        bool const record = width != nullptr || height != nullptr;
        bool const valid = record ? (width == nullptr || isSizing(*width)) && (height == nullptr || isSizing(*height))
                                  : isSizing(value);
        if (!valid) {
            report(sizing->path, "sizing is " + canonicalJson(value) +
                                     "; a sizing is \"content\", {\"fixed\": n}, {\"stretch\": w}, {\"percent\": n} "
                                     "with n from 1 to 100, or a record of width and height of them");
        } else if (record) {
            layout.width = width != nullptr ? sizingOf(*width) : Sizing::content();
            layout.height = height != nullptr ? sizingOf(*height) : Sizing::content();
            if (layout.height.kind == Sizing::Kind::Percent) {
                report(sizing->path, kPercentPlaces);
                layout.height = Sizing::content();
            }
        } else {
            layout.width = sizingOf(value);
        }
    }
    if (std::string_view const placed = node.name("alignSelf"); !placed.empty()) {
        layout.alignSelf = alignOf(placed);
    }
    layout.bleed = node.flag("bleed");
    // Literal integers, checked at load.
    for (auto const& [key, bound] : {
             std::pair{"minWidth", &LayoutHints::minWidth},
             std::pair{"maxWidth", &LayoutHints::maxWidth},
             std::pair{"minHeight", &LayoutHints::minHeight},
             std::pair{"maxHeight", &LayoutHints::maxHeight},
         }) {
        if (ExprPtr const expr = node.prop(key); expr != nullptr && expr->literal.asInt() != nullptr) {
            layout.*bound = static_cast<int>(*expr->literal.asInt());
        }
    }
}

namespace {

/// @brief Where a declaration is lowered, as a focus key reads it.
struct FocusPlace {
    /// @brief The focus key of the component use it is lowered for, and `>`; empty outside a component.
    std::string prefix;
    /// @brief The keys of the rows that enclose it, outermost first.
    std::vector<std::string> rows;
};

/// @brief Where @p scope is: the row keys of its chain and the component use it is lowered for.
/// @param scope The scope.
/// @return It.
[[nodiscard]] FocusPlace focusPlace(RtScope const& scope) {
    FocusPlace place;
    for (RtScope const* current = &scope; current != nullptr; current = current->parent.get()) {
        if (current->row != nullptr) {
            place.rows.push_back(current->rowKeyNow ? current->rowKeyNow() : current->rowKey);
        }
        if (current->parent == nullptr) {
            place.prefix = current->usePrefix;
        }
    }
    std::ranges::reverse(place.rows);
    return place;
}

/// @brief A focus key: a declaration's pointer under its component use's prefix, with one key per enclosing row.
/// @param place Where the declaration is lowered.
/// @param path  The declaring node's JSON Pointer.
/// @return The key, unique per declaration, row and component use.
[[nodiscard]] std::string focusKey(FocusPlace const& place, std::string_view path) {
    std::string key = place.prefix;
    key += path;
    for (std::string const& row : place.rows) {
        key += '@';
        key += row;
    }
    return key;
}

/// @brief The focus key of the element @p path declares, lowered in @p scope.
/// @param path  The declaring node's JSON Pointer.
/// @param scope The scope it is lowered in.
/// @return The key.
[[nodiscard]] std::string focusKeyOf(std::string_view path, RtScope const& scope) {
    return focusKey(focusPlace(scope), path);
}
}  // namespace

void ScreenCore::commonOf(DocNode const& node, LowerCtx const& ctx, ElementState* element, Common& common) {
    static_cast<void>(element);
    common.visible = bindProp<bool>(ctx, node.prop("visible"), true, boolOf);
    common.enabled = bindProp<bool>(ctx, node.prop("enabled"), true, boolOf);
    common.tooltip = bindProp<std::string>(ctx, node.prop("tooltip"), std::string{}, stringOf);
    layoutOf(node, ctx, common.layout);
    common.testId = std::string{node.name("testId")};
    if (!node.id.empty()) {
        // Under a scope whose row changes (a pooled table cell), the key follows the row it holds now.
        bool live = false;
        for (RtScope const* current = ctx.scope.get(); current != nullptr; current = current->parent.get()) {
            live = live || static_cast<bool>(current->rowKeyNow);
        }
        if (live) {
            common.focusId = slotProp<std::string>(
                [scope = ctx.scope, path = node.path] { return focusKeyOf(path, *scope); });
        } else {
            common.focusId = focusKeyOf(node.path, *ctx.scope);
        }
    }
    common.surface = surfaceOf(node, ctx);
    if (ExprPtr const autofocus = node.prop("autofocus")) {
        common.autofocus = boolOf(once(ctx, autofocus), ctx.scope->formatting());
    }
    for (DocNodePtr const& a11y : node.partList("a11y")) {
        common.a11y.name = bindProp<std::string>(ctx, a11y->prop("name"), std::string{}, stringOf);
        common.a11y.role = std::string{a11y->name("role")};
    }
    for (DocNodePtr const& drag : node.partList("drag")) {
        ExprPtr const payload = drag->prop("payload");
        common.dragKey =
            slotProp<std::optional<Key>>([scope = ctx.scope, bindings = ctx.bindings, payload] -> std::optional<Key> {
                Value const value = evalIn(*scope, bindings.get(), payload);
                return value.isNull() ? std::nullopt : std::optional<Key>{keyOf(value)};
            });
    }
    for (DocNodePtr const& drop : node.partList("drop")) {
        ExprPtr const accepts = drop->prop("accepts");
        common.accepts = [this, scope = ctx.scope, bindings = ctx.bindings, accepts](Key const& key) {
            if (!accepts) {
                return true;
            }
            Value const accepted = runtime().untracked([&] { return evalIn(*scope, bindings.get(), accepts); });
            if (ValueList const* const items = accepted.asList()) {
                return std::ranges::any_of(*items, [&](Value const& item) { return keyOf(item) == key; });
            }
            return accepted.isNull() || keyOf(accepted) == key;
        };
        common.onDrop = [this, ctx, commands = drop->event("onDrop")](Key const& key) {
            runEvent(commands, ctx, valueOfKey(key));
        };
    }
    for (auto const& [chord, commands] : node.keys) {
        common.keys.push_back(
            KeyBinding{.chord = chord, .onPress = [this, ctx, list = &commands] { runEvent(list, ctx, Value{}); }});
    }
}

void ScreenCore::fieldOf(DocNode const& node, LowerCtx const& ctx, ElementState* element, FieldState& field) {
    field.readonly = bindProp<bool>(ctx, node.prop("readonly"), false, boolOf);
    field.required = bindProp<bool>(ctx, node.prop("required"), false, boolOf);
    field.stale = bindProp<bool>(ctx, node.prop("stale"), false, boolOf);
    if (element != nullptr) {
        field.errors = slotProp<std::vector<std::string>>(
            [this, scope = ctx.scope, element] { return stringsOf(elementValue(*element, "shownErrors")); });
    } else if (ExprPtr const errors = node.prop("errors")) {
        field.errors = slotProp<std::vector<std::string>>([scope = ctx.scope, bindings = ctx.bindings, errors] {
            return stringsOf(evalIn(*scope, bindings.get(), errors));
        });
    }
    // A tone evaluates the value and is not an error: it changes the field's look and nothing of `valid`.
    if (node.prop("description")) {
        field.description = bindProp<std::string>(ctx, node.prop("description"), std::string{}, stringOf);
    }
    if (node.prop("labelHint")) {
        field.labelHint = bindProp<std::string>(ctx, node.prop("labelHint"), std::string{}, stringOf);
    }
    if (ExprPtr const tone = node.prop("tone")) {
        field.tone = slotProp<Tone>([this, scope = ctx.scope, bindings = ctx.bindings, tone] {
            Value const value = evalIn(*scope, bindings.get(), tone);
            std::optional<Tone> const named = toneNamed(value);
            if (!named) {
                report(tone->path, "tone is " + canonicalJson(value) + "; " + std::string{kToneNames});
            }
            return named.value_or(Tone::Neutral);
        });
    }
}

MenuItem ScreenCore::menuItemOf(DocNode const& part, LowerCtx const& ctx) {
    MenuItem item;
    if (part.flag("separator")) {
        item.separator = true;
        return item;
    }
    item.label = bindProp<std::string>(ctx, part.prop("label"), std::string{}, stringOf);
    item.description = bindProp<std::string>(ctx, part.prop("description"), std::string{}, stringOf);
    item.visible = bindProp<bool>(ctx, part.prop("visible"), true, boolOf);
    if (Commands const* const commands = part.event("onClick")) {
        item.onSelect = [this, ctx, commands] { runEvent(commands, ctx, Value{}); };
    }
    if (ExprPtr const icon = part.prop("icon")) {
        item.icon = textOf(once(ctx, icon), ctx.scope->formatting());
    }
    item.keys = std::string{part.name("keys")};
    if (ExprPtr const checked = part.prop("checked")) {
        item.checked = slotProp<std::optional<bool>>(
            [scope = ctx.scope, bindings = ctx.bindings, checked] -> std::optional<bool> {
                Value const value = evalIn(*scope, bindings.get(), checked);
                bool const* const flag = value.asBool();
                return flag == nullptr ? std::nullopt : std::optional<bool>{*flag};
            });
    }
    item.enabled = bindProp<bool>(ctx, part.prop("enabled"), true, boolOf);
    return item;
}

// NOLINTNEXTLINE(misc-no-recursion): a submenu is a menu one level down.
std::vector<MenuItem> ScreenCore::menuItems(std::span<DocNodePtr const> parts, LowerCtx const& ctx) {
    std::vector<MenuItem> items;
    for (DocNodePtr const& part : parts) {
        MenuItem item = menuItemOf(*part, ctx);
        if (!item.separator) {
            item.items = menuItems(part->partList("items"), ctx);
        }
        items.push_back(std::move(item));
    }
    return items;
}

// NOLINTNEXTLINE(misc-no-recursion): a submenu is a menu one level down.
std::vector<HeadMenuModel> ScreenCore::lowerHeadMenu(std::span<DocNodePtr const> parts, LowerCtx const& ctx) {
    std::vector<HeadMenuModel> models;
    models.reserve(parts.size());
    for (DocNodePtr const& part : parts) {
        models.push_back(HeadMenuModel{
            .id = part->id,
            .path = part->path,
            .item = menuItemOf(*part, ctx),
            .items = lowerHeadMenu(part->partList("items"), ctx),
        });
    }
    return models;
}

std::vector<HeadToolModel> ScreenCore::lowerHeadTools(std::span<DocNodePtr const> parts, LowerCtx const& ctx) {
    static constexpr int kSearchWidth = 30;
    std::vector<HeadToolModel> models;
    models.reserve(parts.size());
    for (DocNodePtr const& part : parts) {
        bool const search = part->name("kind") == "search";
        HeadToolModel model{
            .id = part->id,
            .path = part->path,
            .kind = search ? HeadToolKind::Search : HeadToolKind::Select,
            .label = bindProp<std::string>(ctx, part->prop("label"), std::string{}, stringOf),
            .value = bindProp<std::string>(ctx, part->prop("value"), std::string{}, stringOf),
            .placeholder = bindProp<std::string>(ctx, part->prop("placeholder"), std::string{}, stringOf),
            .options = {},
            .selected = {},
            .width = search ? kSearchWidth : 0,
            .enabled = bindProp<bool>(ctx, part->prop("enabled"), true, boolOf),
            .visible = bindProp<bool>(ctx, part->prop("visible"), true, boolOf),
            .edit = {},
            .submit = {},
        };
        if (ExprPtr const width = part->prop("width")) {
            if (std::int64_t const* const units = width->literal.asInt()) {
                model.width = static_cast<int>(*units);
            }
        }
        if (!search) {
            model.options = slotProp<std::vector<SelectOption>>(
                [scope = ctx.scope, bindings = ctx.bindings, options = part->prop("options")] {
                    return selectOptions(evalIn(*scope, bindings.get(), options), scope->formatting());
                });
            model.selected = slotProp<std::optional<Key>>(
                [scope = ctx.scope, bindings = ctx.bindings, selected = part->prop("selected")] -> std::optional<Key> {
                    Value const value = selected ? evalIn(*scope, bindings.get(), selected) : Value{};
                    return value.isNull() ? std::nullopt : std::optional<Key>{keyOf(value)};
                });
        }
        Commands const* const edited = part->event(search ? "onChange" : "onChoose");
        model.edit = [this, ctx, edited](Value event) { runEvent(edited, ctx, std::move(event)); };
        if (search) {
            model.submit = [this, ctx, onSubmit = part->event("onSubmit")](Value event) {
                runEvent(onSubmit, ctx, std::move(event));
            };
        }
        models.push_back(std::move(model));
    }
    return models;
}

ActionModels ScreenCore::lowerActionParts(std::span<DocNodePtr const> parts, LowerCtx const& ctx) {
    ActionModels models;
    models.reserve(parts.size());
    for (DocNodePtr const& part : parts) {
        ActionModel model{
            .id = part->id,
            .path = part->path,
            .label = bindProp<std::string>(ctx, part->prop("label"), std::string{}, stringOf),
            .icon = bindProp<std::string>(ctx, part->prop("icon"), std::string{}, stringOf),
            .iconOnly = bindProp<bool>(ctx, part->prop("iconOnly"), false, boolOf),
            .emphasis = ButtonEmphasis::Normal,
            .enabled = bindProp<bool>(ctx, part->prop("enabled"), true, boolOf),
            .visible = bindProp<bool>(ctx, part->prop("visible"), true, boolOf),
            .run = {},
        };
        model.emphasis = emphasisNamed(part->name("emphasis")).value_or(ButtonEmphasis::Normal);
        if (Commands const* const commands = part->event("onClick")) {
            model.run = [this, ctx, commands] { runEvent(commands, ctx, Value{}); };
        }
        models.push_back(std::move(model));
    }
    return models;
}

Node ScreenCore::actionBarOf(ActionModels actions) {
    ActionBar bar;
    for (ActionModel& action : actions) {
        Button button{
            .label = action.label,
            .emphasis = action.emphasis,
            .onClick = std::move(action.run),
            .common = {},  // NOLINT(readability-trailing-comma): the comma ends the designator, not the empty braces.
            .icon = action.icon.evaluate(),
        };
        if (action.iconOnly.isBound() || action.iconOnly.constant()) {
            // The label is the button's name and tooltip; the caption is empty while the icon stands alone.
            button.label = Prop<std::string>{[label = action.label, iconOnly = action.iconOnly] {
                return iconOnly.evaluate() ? std::string{} : label.evaluate();
            }};
            button.common.a11y.name = action.label;
            button.common.tooltip = action.label;
        }
        button.common.visible = std::move(action.visible);
        button.common.enabled = std::move(action.enabled);
        button.common.testId = "action_" + action.id;
        bar.children.push_back(ui::button(std::move(button)));
    }
    return ui::actionBar(std::move(bar));
}

std::vector<BadgeModel> ScreenCore::lowerBadgeParts(std::span<DocNodePtr const> parts, LowerCtx const& ctx) {
    std::vector<BadgeModel> badges;
    badges.reserve(parts.size());
    for (DocNodePtr const& part : parts) {
        badges.push_back(BadgeModel{
            .text = bindProp<std::string>(ctx, part->prop("text"), std::string{}, stringOf),
            .tone = bindProp<Tone>(ctx, part->prop("tone"), Tone::Neutral, toneOf),
            .icon = bindProp<std::string>(ctx, part->prop("icon"), std::string{}, stringOf),
            .visible = bindProp<bool>(ctx, part->prop("visible"), true, boolOf),
        });
    }
    return badges;
}

// The readers every kind's lowering shares, each bound to the node being lowered.
class ScreenCore::KindKit {
public:
    KindKit(ScreenCore& core, DocNode const& node, LowerCtx const& ctx, ElementState* element)
        : _core{core}, _node{node}, _ctx{ctx}, _element{element}, _format{ctx.scope->formatting()} {}

    // The children under @p key, each lowered with @p gap as the gap of the list around it.
    [[nodiscard]] std::vector<Node> children(std::string_view key, int gap = 0) const {
        LowerCtx listCtx = _ctx;
        listCtx.listGap = gap;
        std::vector<Node> out;
        for (DocNodePtr const& child : _node.nodes(key)) {
            out.push_back(_core.lower(*child, listCtx));
        }
        return out;
    }

    // The children under @p key as one node: none is empty, several share a holder that sets @p gap.
    [[nodiscard]] Node child(std::string_view key, int gap = 0) const {
        auto const nodes = _node.nodes(key);
        return nodes.empty() ? Node{} : _core.lowerAll(nodes, _ctx, gap);
    }

    [[nodiscard]] std::int64_t onceInt(std::string_view key, std::int64_t fallback) const {
        ExprPtr const expr = _node.prop(key);
        return expr ? intOf(_core.once(_ctx, expr), fallback) : fallback;
    }

    [[nodiscard]] bool onceBool(std::string_view key, bool fallback) const {
        ExprPtr const expr = _node.prop(key);
        return expr ? boolOf(_core.once(_ctx, expr), _format) : fallback;
    }

    // A stack's padding: one step, or the loader-checked constant list of the vertical and the horizontal step. A
    // computed padding that yields a list is not a step: it is reported once and taken as 0.
    [[nodiscard]] Padding stackPadding() const {
        ExprPtr const expr = _node.prop("padding");
        if (!expr) {
            return {};
        }
        Value const value = _core.once(_ctx, expr);
        if (ValueList const* const steps = value.asList()) {
            if (expr->kind == ExprKind::Literal && steps->size() == 2) {
                return Padding{static_cast<int>(intOf(steps->at(0), 0)), static_cast<int>(intOf(steps->at(1), 0))};
            }
            _core.report(_node.path + "/padding", "a computed `padding` is one spacing step, not a list; taken as 0");
            return {};
        }
        return Padding{static_cast<int>(intOf(value, 0))};
    }

    [[nodiscard]] std::string onceText(std::string_view key) const {
        ExprPtr const expr = _node.prop(key);
        return expr ? textOf(_core.once(_ctx, expr), _format) : std::string{};
    }

    [[nodiscard]] Prop<std::string> textProp(std::string_view key) const {
        return _core.bindProp<std::string>(_ctx, _node.prop(key), std::string{}, stringOf);
    }

    // A field shows no caption of its own, so its label is what names it to assistive technology. An `a11y` part
    // that gives a name was written for that purpose and wins; one that gives only a role leaves the label.
    void nameByLabel(Common& common) const {
        bool const named = std::ranges::any_of(_node.partList("a11y"),
                                               [](DocNodePtr const& a11y) { return a11y->prop("name") != nullptr; });
        if (_node.prop("label") && !named) {
            common.a11y.name = textProp("label");
        }
    }

    // A standalone input draws its label as a caption only when the document asks for it (`showLabel`).
    template <typename Spec>
    void captionOf(Spec& spec) const {
        if (_node.prop("label")) {
            spec.label = textProp("label");
        }
        spec.showLabel = _node.flag("showLabel");
    }

    [[nodiscard]] Action action(std::string_view key) const {
        Commands const* const commands = _node.event(key);
        if (commands == nullptr) {
            return {};
        }
        return [core = &_core, ctx = _ctx, commands] { core->runEvent(commands, ctx, Value{}); };
    }

    [[nodiscard]] ActionButton actionPart() const {
        ActionButton button;
        for (DocNodePtr const& part : _node.partList("action")) {
            button.label = _core.bindProp<std::string>(_ctx, part->prop("label"), std::string{}, stringOf);
            if (Commands const* const commands = part->event("onClick")) {
                button.onClick = [core = &_core, ctx = _ctx, commands] { core->runEvent(commands, ctx, Value{}); };
            }
        }
        return button;
    }

    // A reader of the element's value under @p name.
    [[nodiscard]] auto read(std::string_view name) const {
        return [element = _element, core = &_core, name = std::string{name}] {
            return core->elementValue(*element, name);
        };
    }

private:
    // NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members): the kit lives inside one lowering call, in
    // which all four outlive it.
    ScreenCore& _core;
    DocNode const& _node;
    LowerCtx const& _ctx;
    ElementState* _element;
    FormatContext const& _format;
    // NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)
};

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one case per kind of ui/1.
std::optional<Node> ScreenCore::lowerWidget(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    std::string_view const kind = node.kind;
    FormatContext const& format = ctx.scope->formatting();
    KindKit const kit{*this, node, ctx, element};
    if (kind == "text") {
        Text spec{.role = roleProp(ctx, node.prop("role"))};
        // A text with spans shows them, so its `text`, an older client's fallback, is not lowered.
        if (ExprPtr const spans = node.prop("spans")) {
            spec.spans = spansProp(ctx, spans);
        } else {
            spec.text = kit.textProp("text");
        }
        if (!node.name("align").empty()) {
            spec.align = textAlignNamed(node.name("align"));
        }
        fieldOf(node, ctx, element, spec.field);
        commonOf(node, ctx, element, spec.common);
        return ui::text(std::move(spec));
    }
    if (kind == "button") {
        Button spec{.label = kit.textProp("label")};
        spec.icon = kit.onceText("icon");
        spec.emphasis = emphasisNamed(node.name("emphasis")).value_or(ButtonEmphasis::Normal);
        if (node.name("style") == "item") {
            spec.style = ButtonStyle::Item;
        }
        spec.shape = shapeNamed(node.name("shape")).value_or(ControlShape::Default);
        spec.size = sizeNamed(node.name("size")).value_or(ButtonSize::Normal);
        spec.mono = node.flag("mono");
        spec.weight = weightNamed(node.name("weight")).value_or(0);
        if (node.prop("selected")) {
            spec.selected = bindProp<bool>(ctx, node.prop("selected"), false, boolOf);
            // An entry's content, drawn in place of its label: one node as it is, several in a column.
            if (std::vector<Node> content = kit.children("children"); content.size() == 1) {
                spec.child = std::move(content.front());
            } else if (!content.empty()) {
                spec.child = ui::column(Column{.children = std::move(content)});
            }
        }
        Commands const* const onClick = node.event("onClick");
        Commands const* const onActivate = node.event("onActivate");
        if (onClick != nullptr || onActivate != nullptr) {
            spec.onClick = [this, ctx, onClick, onActivate] {
                runEvent(onClick, ctx, Value{});
                runEvent(onActivate, ctx, Value{});
            };
        }
        commonOf(node, ctx, element, spec.common);
        return ui::button(std::move(spec));
    }
    if (kind == "textInput") {
        std::string const commitOn = node.name("commitOn").empty() ? "change" : std::string{node.name("commitOn")};
        auto const commit = [this, ctx, element, commitOn, onChange = node.event("onChange")](
                                std::string const& when, std::string const& text) {
            if (commitOn != when) {
                return;
            }
            if (element->bound == nullptr) {
                session.noteWrite();
                element->committed.set(Value{text});
            }
            runEvent(onChange, ctx, Value{text});
        };
        TextInput spec;
        spec.value =
            slotProp<std::string>([this, element] { return textOf(element->draft.get(), session.formatting()); });
        spec.onChange = [this, element, commit](std::string text) {
            session.asUser([&] {
                session.noteWrite();
                element->draft.set(Value{text});
                element->edited.set(true);
            });
            commit("change", text);
        };
        spec.onCommit = [this, ctx, element, commit, onCommit = node.event("onCommit")](std::string const& text) {
            element->touched.set(true);
            commit("blur", text);
            runEvent(onCommit, ctx, Value{text});
        };
        // Only a field that does something on Enter submits: one without `onSubmit` that does not commit on Enter
        // lets Enter go on to its ancestors' chords, a dialog's `default` among them.
        if (Commands const* const onSubmit = node.event("onSubmit"); onSubmit != nullptr || commitOn == "enter") {
            spec.onSubmit = [this, ctx, commit, onSubmit](std::string const& text) {
                commit("enter", text);
                runEvent(onSubmit, ctx, Value{text});
            };
        }
        spec.placeholder = kit.textProp("placeholder");
        std::string const mode = kit.onceText("mode");
        spec.mode = TextInputMode::SingleLine;
        if (mode == "multiline") {
            spec.mode = TextInputMode::Multiline;
        } else if (mode == "password") {
            spec.mode = TextInputMode::Password;
        }
        // A multiline input shows no unit and no icon; a computed mode that turns out multiline is reported and
        // its unit and icon left out, so the mount never meets them there. The loader refuses a literal one.
        for (std::string_view const key : {
                 std::string_view{"unit"},
                 std::string_view{"icon"},
                 std::string_view{"prefix"},
                 std::string_view{"decimals"},
             }) {
            if (ExprPtr const given = node.prop(key); given != nullptr && spec.mode == TextInputMode::Multiline) {
                report(given->path, "a multiline input shows no " + std::string{key});
            }
        }
        spec.mono = node.flag("mono");
        if (spec.mode != TextInputMode::Multiline) {
            spec.emphasis = node.name("emphasis") == "prominent" ? InputEmphasis::Prominent : InputEmphasis::Normal;
            spec.icon = kit.onceText("icon");
            spec.align = alignOf(node.name("align"));
            if (spec.align == Align::Stretch) {
                spec.align = Align::Start;
            }
            if (node.prop("unit")) {
                spec.unit = kit.textProp("unit");
            }
            if (node.prop("prefix")) {
                spec.prefix = kit.textProp("prefix");
            }
            if (ExprPtr const places = node.prop("decimals")) {
                spec.decimals = bindProp<int>(ctx, places, -1, placesOf);
            }
        }
        fieldOf(node, ctx, element, spec.field);
        commonOf(node, ctx, element, spec.common);
        kit.nameByLabel(spec.common);
        kit.captionOf(spec);
        return ui::textInput(std::move(spec));
    }
    if (kind == "checkbox") {
        Checkbox spec{
            .label = kit.textProp("label"),
            .checked = slotProp<bool>([read = kit.read("checked")] {
                Value const value = read();
                bool const* const flag = value.asBool();
                return flag != nullptr && *flag;
            }),
        };
        spec.onToggle = [this, ctx, element, onToggle = node.event("onToggle"),
                         onChange = node.event("onChange")](bool checked) {
            // A toggle is a check box's commit, so it touches the box.
            session.asUser([&] {
                if (element->bound == nullptr) {
                    session.noteWrite();
                    element->draft.set(checked);
                }
                element->touched.set(true);
            });
            runEvent(onToggle, ctx, Value{checked});
            runEvent(onChange, ctx, Value{checked});
        };
        fieldOf(node, ctx, element, spec.field);
        commonOf(node, ctx, element, spec.common);
        return ui::checkbox(std::move(spec));
    }
    if (kind == "select") {
        Select spec;
        spec.options = slotProp<std::vector<SelectOption>>(
            [scope = ctx.scope, element] { return selectOptions(element->rows->get(), scope->formatting()); });
        spec.selected = slotProp<std::optional<Key>>([read = kit.read("selected")] -> std::optional<Key> {
            Value const selected = read();
            return selected.isNull() ? std::nullopt : std::optional<Key>{keyOf(selected)};
        });
        spec.onSelect = [this, ctx, element, onChoose = node.event("onChoose"),
                         onChange = node.event("onChange")](Key const& key) {
            Value const chosen = valueOfKey(key);
            // A choice is a select's commit, so it touches the select.
            session.asUser([&] {
                if (element->bound == nullptr) {
                    session.noteWrite();
                    element->draft.set(chosen);
                }
                element->touched.set(true);
            });
            runEvent(onChoose, ctx, chosen);
            runEvent(onChange, ctx, chosen);
        };
        // A computed style of no known name is reported once and drawn as a dropdown; the loader refuses a constant
        // one.
        if (ExprPtr const style = node.prop("style")) {
            std::string const name = textOf(once(ctx, style), ctx.scope->formatting());
            if (name == "radio") {
                spec.style = SelectStyle::Radio;
            } else if (name == "segmented") {
                spec.style = SelectStyle::Segmented;
            } else if (name != "dropdown") {
                report(style->path, "style is `" + name + "`; a select's style is dropdown, radio or segmented");
            }
        }
        fieldOf(node, ctx, element, spec.field);
        commonOf(node, ctx, element, spec.common);
        kit.nameByLabel(spec.common);
        kit.captionOf(spec);
        return ui::select(std::move(spec));
    }
    if (kind == "slider") {
        Slider spec;
        spec.value = slotProp<std::int64_t>([read = kit.read("value"), element] {
            return intOf(element->bound != nullptr ? read() : element->draft.get(), 0);
        });
        spec.minimum = kit.onceInt("min", 0);
        spec.maximum = kit.onceInt("max", 100);
        spec.step = kit.onceInt("step", 1);
        spec.onChange = [this, ctx, element, onChange = node.event("onChange")](std::int64_t value) {
            session.asUser([&] {
                session.noteWrite();
                element->draft.set(value);
                element->edited.set(true);
                if (element->bound == nullptr) {
                    element->committed.set(value);
                }
            });
            runEvent(onChange, ctx, Value{value});
        };
        spec.onCommit = [this, ctx, element, onCommit = node.event("onCommit")](std::int64_t value) {
            element->touched.set(true);
            runEvent(onCommit, ctx, Value{value});
        };
        fieldOf(node, ctx, element, spec.field);
        commonOf(node, ctx, element, spec.common);
        kit.nameByLabel(spec.common);
        return ui::slider(std::move(spec));
    }
    if (kind == "dateTimeInput") {
        DateTimeInput spec;
        DateMode const mode = kit.onceText("mode") == "date" ? DateMode::Date : DateMode::DateTime;
        std::optional<TypeKind> heldAtLoad;
        if (auto const found = _doc->dateValueKinds.find(&node); found != _doc->dateValueKinds.end()) {
            heldAtLoad = found->second;
        }
        std::string const valuePath = pointerAppend(node.path, "value");
        std::int32_t const offsetMinutes = format.utcOffsetMinutes;
        spec.value = slotProp<std::optional<time::Timestamp>>([this, element, offsetMinutes,
                                                               valuePath] -> std::optional<time::Timestamp> {
            DateReading const reading = dateInstantOf(element->draft.get(), offsetMinutes);
            if (!reading.readable) {
                report(valuePath, "`dateTimeInput` shows a timestamp or an ISO-8601 date or date-time string");
            }
            return reading.instant ? std::optional<time::Timestamp>{time::Timestamp{*reading.instant}} : std::nullopt;
        });
        spec.onChange = [this, ctx, element, heldAtLoad, mode, offsetMinutes, onChange = node.event("onChange"),
                         onCommit = node.event("onCommit")](std::optional<time::Timestamp> value) {
            bool asString = element->bound != nullptr && element->bound->get().asString() != nullptr;
            if (heldAtLoad) {
                asString = *heldAtLoad == TypeKind::String;
            }
            std::optional<time::DateTime> const instant = value ? value->value : std::nullopt;
            Value entered;
            if (!asString) {
                if (instant) {
                    entered = Value{*instant};
                }
            } else if (!instant) {
                entered = Value{std::string{}};
            } else if (mode == DateMode::Date) {
                entered = Value{dateTextOf(*instant, offsetMinutes)};
            } else {
                entered = Value{instant->toIso8601()};
            }
            element->touched.set(true);
            session.asUser([&] {
                session.noteWrite();
                element->draft.set(entered);
                element->edited.set(true);
                if (element->bound == nullptr) {
                    element->committed.set(entered);
                }
            });
            runEvent(onChange, ctx, entered);
            runEvent(onCommit, ctx, entered);
        };
        spec.mode = mode;
        spec.offsetMinutes = offsetMinutes;
        // Tracked: a locale switch changes the pattern the screen's dates are written in, and the input follows.
        spec.pattern = slotProp<std::string>([this, mode] {
            FormatContext const& now = session.formatting();
            return mode == DateMode::Date ? now.datePattern : now.dateTimePattern;
        });
        fieldOf(node, ctx, element, spec.field);
        commonOf(node, ctx, element, spec.common);
        kit.nameByLabel(spec.common);
        kit.captionOf(spec);
        return ui::dateTimeInput(std::move(spec));
    }
    if (kind == "filePicker") {
        FilePicker spec;
        spec.path =
            slotProp<std::string>([this, element] { return textOf(element->draft.get(), session.formatting()); });
        spec.onPicked = [this, ctx, element, onPick = node.event("onPick")](std::string path) {
            session.asUser([&] {
                session.noteWrite();
                element->draft.set(Value{path});
                element->committed.set(Value{path});
            });
            runEvent(onPick, ctx, Value{path});
        };
        spec.mode = kit.onceText("mode") == "save" ? FilePickerMode::Save : FilePickerMode::Open;
        fieldOf(node, ctx, element, spec.field);
        commonOf(node, ctx, element, spec.common);
        kit.nameByLabel(spec.common);
        return ui::filePicker(std::move(spec));
    }
    if (kind == "menu") {
        Menu spec{.items = menuItems(node.partList("items"), ctx)};
        if (node.name("style") == "button") {
            spec.style = MenuStyle::Button;
            spec.label = kit.textProp("label");
            spec.icon = kit.onceText("icon");
            if (node.name("emphasis") == "primary") {
                spec.emphasis = ButtonEmphasis::Primary;
            } else if (node.name("emphasis") == "danger") {
                spec.emphasis = ButtonEmphasis::Danger;
            } else if (node.name("emphasis") == "ghost") {
                spec.emphasis = ButtonEmphasis::Ghost;
            }
        }
        commonOf(node, ctx, element, spec.common);
        return ui::menu(std::move(spec));
    }
    return std::nullopt;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one case per kind of ui/1.
std::optional<Node> ScreenCore::lowerDisplay(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    std::string_view const kind = node.kind;
    KindKit const kit{*this, node, ctx, element};
    if (kind == "banner") {
        Banner spec{.tone = bindProp<Tone>(ctx, node.prop("tone"), Tone::Info, toneOf)};
        if (ExprPtr const spans = node.prop("spans")) {
            spec.spans = spansProp(ctx, spans);
        } else {
            spec.text = kit.textProp("text");
        }
        spec.action = kit.actionPart();
        spec.dismissible = kit.onceBool("dismissible", false);
        spec.onDismiss = kit.action("onDismiss");
        spec.actionStyle = bannerActionStyleNamed(node.name("actionStyle")).value_or(BannerActionStyle::Ghost);
        spec.actionStretch = node.flag("actionStretch");
        commonOf(node, ctx, element, spec.common);
        return ui::banner(std::move(spec));
    }
    if (kind == "badge") {
        Badge spec{
            .tone = bindProp<Tone>(ctx, node.prop("tone"), Tone::Neutral, toneOf),
            .text = kit.textProp("text"),
        };
        spec.icon = kit.onceText("icon");
        std::string_view const style = node.name("style");
        spec.style = BadgeStyle::Filled;
        if (style == "outline") {
            spec.style = BadgeStyle::Outline;
        } else if (style == "quiet") {
            spec.style = BadgeStyle::Quiet;
        }
        spec.shape = shapeNamed(node.name("shape")).value_or(ControlShape::Default);
        spec.mono = node.flag("mono");
        if (Commands const* const onRemove = node.event("onRemove")) {
            spec.onRemove = [this, ctx, onRemove] { runEvent(onRemove, ctx, Value{}); };
            spec.removeLabel = kit.textProp("removeLabel");
        }
        commonOf(node, ctx, element, spec.common);
        return ui::badge(std::move(spec));
    }
    if (kind == "progress") {
        Progress spec;
        spec.value = slotProp<std::optional<double>>(
            [scope = ctx.scope, bindings = ctx.bindings, expr = node.prop("value")] -> std::optional<double> {
                Value const value = evalIn(*scope, bindings.get(), expr);
                if (std::int64_t const* const number = value.asInt()) {
                    return static_cast<double>(*number);
                }
                if (Decimal const* const number = value.asDecimal()) {
                    return realOf(number->value);
                }
                return std::nullopt;
            });
        spec.label = kit.textProp("label");
        commonOf(node, ctx, element, spec.common);
        return ui::progress(std::move(spec));
    }
    if (kind == "steps") {
        Steps spec;
        spec.items =
            slotProp<std::vector<Step>>([scope = ctx.scope, bindings = ctx.bindings, expr = node.prop("items")] {
                std::vector<Step> steps;
                Value const items = evalIn(*scope, bindings.get(), expr);
                if (ValueList const* const list = items.asList()) {
                    for (Value const& item : *list) {
                        Value const* const label = item.field("label");
                        Value const* const state = item.field("state");
                        Value const* const detail = item.field("detail");
                        std::string const name = state == nullptr ? std::string{} : textOf(*state, {});
                        steps.push_back(Step{
                            .label = label == nullptr ? std::string{} : textOf(*label, scope->formatting()),
                            .state = stepStateOf(name),
                            .detail = detail == nullptr ? std::string{} : textOf(*detail, scope->formatting()),
                        });
                    }
                }
                return steps;
            });
        spec.current = slotProp<std::optional<std::size_t>>(
            [scope = ctx.scope, bindings = ctx.bindings, expr = node.prop("current")] -> std::optional<std::size_t> {
                std::int64_t const index = intOf(evalIn(*scope, bindings.get(), expr), -1);
                return index < 0 ? std::nullopt : std::optional<std::size_t>{static_cast<std::size_t>(index)};
            });
        spec.labelled = bindProp<bool>(ctx, node.prop("labelled"), true, boolOf);
        commonOf(node, ctx, element, spec.common);
        return ui::steps(std::move(spec));
    }
    if (kind == "chart") {
        return lowerChart(node, ctx, element);
    }
    if (kind == "keyValue") {
        KeyValue spec;
        spec.captions = node.name("fieldLayout") == "stacked" ? CaptionPlacement::Above : CaptionPlacement::Beside;
        // The loader has checked `captionWidth` is a literal integer.
        if (ExprPtr const width = node.prop("captionWidth")) {
            spec.captionWidth = static_cast<int>(*width->literal.asInt());
        }
        spec.items = slotProp<std::vector<KeyValueItem>>(
            [this, scope = ctx.scope, bindings = ctx.bindings, expr = node.prop("items")] {
                std::vector<KeyValueItem> out;
                Value const items = evalIn(*scope, bindings.get(), expr);
                if (ValueList const* const list = items.asList()) {
                    for (Value const& item : *list) {
                        out.push_back(keyValueItemOf(item, expr->path, *scope));
                    }
                }
                return out;
            });
        commonOf(node, ctx, element, spec.common);
        return ui::keyValue(std::move(spec));
    }
    if (kind == "emptyState") {
        EmptyState spec{.title = kit.textProp("title"), .text = kit.textProp("text")};
        spec.icon = kit.onceText("icon");
        spec.action = kit.actionPart();
        commonOf(node, ctx, element, spec.common);
        return ui::emptyState(std::move(spec));
    }
    return std::nullopt;
}

Node ScreenCore::lowerStack(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    std::string_view const kind = node.kind;
    KindKit const kit{*this, node, ctx, element};
    int const gap = static_cast<int>(kit.onceInt("gap", 0));
    if (std::string const arrangement = arrangedBy(node.id); !arrangement.empty()) {
        return lowerArranged(node, ctx, element, arrangement, kind == "column" ? Axis::Vertical : Axis::Horizontal,
                             gap);
    }
    if (kind == "column") {
        Column spec{
            .children = kit.children("children", gap),
            .gap = gap,
            .align = alignOf(node.name("align")),
            .justify = justifyOf(node.name("justify")),
            .divided = node.flag("divided"),
            .padding = kit.stackPadding(),
        };
        if (node.prop("selected")) {
            spec.selected = bindProp<bool>(ctx, node.prop("selected"), false, boolOf);
        }
        commonOf(node, ctx, element, spec.common);
        return ui::column(std::move(spec));
    }
    Row spec{
        .children = kit.children("children"),
        .gap = gap,
        .align = alignOf(node.name("align")),
        .justify = justifyOf(node.name("justify")),
        .wrap = node.flag("wrap"),
        .divided = node.flag("divided"),
        .padding = kit.stackPadding(),
    };
    if (node.prop("selected")) {
        spec.selected = bindProp<bool>(ctx, node.prop("selected"), false, boolOf);
    }
    commonOf(node, ctx, element, spec.common);
    return ui::row(std::move(spec));
}

Node ScreenCore::lowerGrid(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    KindKit const kit{*this, node, ctx, element};
    Grid spec{
        .columns = static_cast<int>(kit.onceInt("columns", 1)),
        .gap = static_cast<int>(kit.onceInt("gap", 0)),
        .align = alignOf(node.name("align")),
        .minColumnWidth = static_cast<int>(kit.onceInt("minColumnWidth", 0)),
    };
    for (DocNodePtr const& track : node.partList("tracks")) {
        ExprPtr const width = track->prop("width");
        spec.tracks.push_back(GridTrack{
            .width = width ? sizingOf(once(ctx, width)) : Sizing::stretch(),
            .align = cellPlaceNamed(track->name("align")).value_or(Align::Start),
        });
    }
    for (DocNodePtr const& cell : node.nodes("children")) {
        ExprPtr const span = cell->prop("span");
        spec.cells.push_back(
            GridCell{.node = lower(*cell, ctx), .span = static_cast<int>(span ? intOf(once(ctx, span), 1) : 1)});
    }
    commonOf(node, ctx, element, spec.common);
    return ui::grid(std::move(spec));
}

Node ScreenCore::lowerPanel(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    KindKit const kit{*this, node, ctx, element};
    Panel spec{.title = kit.textProp("title"), .padding = static_cast<int>(kit.onceInt("padding", 0))};
    spec.header = kit.child("header");
    spec.child = kit.child("children", static_cast<int>(kit.onceInt("gap", kPanelGap)));
    spec.collapsible = kit.onceBool("collapsible", false);
    spec.collapsed = bindProp<bool>(ctx, node.prop("collapsed"), false, boolOf);
    if (Commands const* const commands = node.event("onToggle")) {
        spec.onToggle = [this, ctx, commands](bool collapsed) { runEvent(commands, ctx, Value{collapsed}); };
    }
    commonOf(node, ctx, element, spec.common);
    return ui::panel(std::move(spec));
}

Node ScreenCore::lowerSplitter(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    KindKit const kit{*this, node, ctx, element};
    ExprPtr const orientation = node.prop("orientation");
    Splitter spec{
        .children = kit.children("children"),
        .orientation =
            orientation && axisOf(once(ctx, orientation)) == Axis::Vertical ? Axis::Vertical : Axis::Horizontal,
    };
    // No `onResize`: a user's drag stays the splitter's own, and the document's sizes are where it starts.
    if (ExprPtr const sizes = node.prop("sizes")) {
        spec.sizes = bindProp<std::vector<int>>(
            ctx, sizes, {}, [this, path = sizes->path](Value const& value, FormatContext const& /*format*/) {
                std::optional<std::vector<int>> parsed = splitterSizesOf(value);
                if (!parsed) {
                    report(path, "sizes is " + canonicalJson(value) + "; sizes is a list of numbers, one per pane");
                }
                return std::move(parsed).value_or(std::vector<int>{});
            });
    }
    commonOf(node, ctx, element, spec.common);
    // A literal `sizes` that matches the panes places them, so a pane's own width is not used.
    if (ExprPtr const sizes = node.prop("sizes"); sizes && sizes->kind == ExprKind::Literal) {
        std::optional<std::vector<int>> const given = splitterSizesOf(sizes->literal);
        if (given && given->size() == spec.children.size()) {
            for (std::size_t at = 0; at < spec.children.size(); ++at) {
                // A stretch pane asks only to fill, which `sizes` leaves it to do; a fixed or percent pane names a
                // size.
                Sizing::Kind const kind = layoutHintsOf(spec.children.at(at)).width.kind;
                if (spec.orientation == Axis::Horizontal &&
                    (kind == Sizing::Kind::Fixed || kind == Sizing::Kind::Percent)) {
                    report(sizes->path, "this splitter's `sizes` places its panes; pane " + std::to_string(at) +
                                            "'s sizing is not used");
                }
            }
        }
    }
    return ui::splitter(std::move(spec));
}

void ScreenCore::reportActionBarGaps(DocNode const& bar, LowerCtx const& ctx) {
    for (DocNodePtr const& child : bar.nodes("children")) {
        if (child->kind != "forEach" || child->prop("gap") == nullptr) {
            continue;
        }
        ExprPtr const axis = child->prop("axis");
        if (axis != nullptr && axis->kind != ExprKind::Literal && axisOf(once(ctx, axis)) == Axis::Horizontal) {
            report(child->path + "/gap", kActionBarGapRefusal);
        }
    }
}

Node ScreenCore::lowerCollapsible(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    KindKit const kit{*this, node, ctx, element};
    Collapsible spec{.title = kit.textProp("title"), .open = bindProp<bool>(ctx, node.prop("open"), true, boolOf)};
    if (Commands const* const commands = node.event("onToggle")) {
        spec.onToggle = [this, ctx, commands](bool open) { runEvent(commands, ctx, Value{open}); };
    }
    spec.header = kit.child("header");
    spec.child = kit.child("children", static_cast<int>(kit.onceInt("gap", kPanelGap)));
    commonOf(node, ctx, element, spec.common);
    return ui::collapsible(std::move(spec));
}

Node ScreenCore::lowerDropZone(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    KindKit const kit{*this, node, ctx, element};
    DropZone spec;
    spec.accept = stringsOf(node.prop("accept") ? once(ctx, node.prop("accept")) : Value{});
    spec.multiple = kit.onceBool("multiple", false);
    if (Commands const* const commands = node.event("onDrop")) {
        spec.onDrop = [this, ctx, commands](std::vector<std::string> const& paths) {
            runEvent(commands, ctx, listOfStrings(paths));
        };
    }
    spec.child = kit.child("children");
    commonOf(node, ctx, element, spec.common);
    return ui::dropZone(std::move(spec));
}

Node ScreenCore::lowerPage(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    KindKit const kit{*this, node, ctx, element};
    Page spec;
    spec.geometry = pageGeometryOf(node);
    std::optional<PageGeometry> const outer = std::exchange(pageBeingLowered, spec.geometry);
    spec.children = kit.children("children");
    pageBeingLowered = outer;
    commonOf(node, ctx, element, spec.common);
    return ui::page(std::move(spec));
}

Node ScreenCore::lowerPageRegion(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    if (!pageBeingLowered) {
        report(node.path, "a page region belongs in a page");
        return ui::text(Text{.text = std::string{}, .role = TextRole::Muted});
    }
    KindKit const kit{*this, node, ctx, element};
    PageRegion spec;
    spec.geometry = regionGeometryOf(node, *pageBeingLowered);
    std::optional<PageGeometry> const outer = std::exchange(pageBeingLowered, std::nullopt);
    spec.children = kit.children("children");
    pageBeingLowered = outer;
    commonOf(node, ctx, element, spec.common);
    return ui::pageRegion(std::move(spec));
}

Node ScreenCore::lowerLocaleScope(DocNode const& node, LowerCtx const& ctx) {
    auto const holder = std::make_shared<RtScope>(*this, ctx.scope, nullptr, ctx.scope->nodes.depth() + 2);
    holder->transparent = true;
    ExprPtr const locale = node.prop("locale");
    holder->localeFormat = &holder->nodes.computed([this, scope = ctx.scope, bindings = ctx.bindings,
                                                    locale] -> std::shared_ptr<FormatContext const> {
        Value const value = evalIn(*scope, bindings.get(), locale);
        std::string const* const tag = value.asString();
        if (tag == nullptr && !value.isNull()) {
            report(locale->path, "the locale is " + canonicalJson(value) + "; a locale is a BCP 47 tag, a string");
        }
        if (tag == nullptr || tag->empty()) {
            return session.sharedFormatting();
        }
        if (session.firstUnlistedLocale(*tag)) {
            report(locale->path, "no formatting facts for locale `" + *tag + "`; numbers, money and dates use en's");
        }
        return session.scopedFormatting(*tag);
    });
    // The children are mounted again whenever the tag changes: a value a node reads once, as it is lowered (a
    // table's column labels, a `once` property, a literal), is written in the facts at that moment, and only a new
    // lowering writes it in the new ones. The key is the tag as the scope resolves it; a tag that is not known yet
    // is the key `""`, apart from every tag.
    std::vector<DocNode const*> content;
    for (DocNodePtr const& child : node.nodes("children")) {
        content.push_back(child.get());
    }
    LayoutHints const hints = holderHints(content, nullptr, ctx);
    Switch spec;
    spec.selector = slotProp<Key>([scope = ctx.scope, bindings = ctx.bindings, locale] {
        Value const value = evalIn(*scope, bindings.get(), locale);
        std::string const* const tag = value.asString();
        return Key{tag == nullptr ? std::string{} : *tag};
    });
    spec.fallback = lazy(
        [this, ctx, holder, content, path = node.path] -> Node {
            auto const inner = std::make_shared<RtScope>(*this, holder, nullptr, holder->nodes.depth() + 2);
            inner->transparent = true;
            LowerCtx const innerCtx{.scope = inner, .bindings = ctx.bindings, .boundary = ctx.boundary, .listGap = 0};
            std::vector<Node> children;
            children.reserve(content.size());
            for (DocNode const* const child : content) {
                children.push_back(lower(*child, innerCtx));
            }
            fillHolder(children, path);
            return holding(std::move(children), inner, {}, 0);
        },
        hints);
    commonOf(node, ctx, nullptr, spec.common);
    return ui::switchOf(std::move(spec));
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one case per kind of ui/1.
Node ScreenCore::lowerKind(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    std::string_view const kind = node.kind;
    KindKit const kit{*this, node, ctx, element};
    if (std::optional<Node> widget = lowerWidget(node, ctx, element)) {
        return std::move(*widget);
    }
    if (kind == "group") {
        return lowerGroup(node, ctx, element, kit.children("children"));
    }
    if (kind == "column" || kind == "row") {
        return lowerStack(node, ctx, element);
    }
    if (kind == "grid") {
        return lowerGrid(node, ctx, element);
    }
    if (kind == "spacer") {
        Spacer spec;
        commonOf(node, ctx, element, spec.common);
        return ui::spacer(std::move(spec));
    }
    if (kind == "panel") {
        return lowerPanel(node, ctx, element);
    }
    if (kind == "scroll") {
        ExprPtr const axis = node.prop("axis");
        Scroll spec{.child = kit.child("children"), .axis = axis ? axisOf(once(ctx, axis)) : Axis::Vertical};
        commonOf(node, ctx, element, spec.common);
        return ui::scroll(std::move(spec));
    }
    if (kind == "switch") {
        return lowerSwitch(node, ctx);
    }
    if (kind == "tabs") {
        return lowerTabs(node, ctx, element);
    }
    if (kind == "dialog" || kind == "drawer") {
        return lowerDialog(node, ctx, element);
    }
    if (kind == "busy") {
        Busy spec{.active = bindProp<bool>(ctx, node.prop("active"), false, boolOf), .label = kit.textProp("label")};
        commonOf(node, ctx, element, spec.common);
        return ui::busy(std::move(spec));
    }
    if (kind == "table" && !node.name("form").empty()) {
        return lowerFormGrid(node, ctx, element);
    }
    if (kind == "table" && !node.name("model").empty()) {
        return lowerModelTable(node, ctx, element);
    }
    if (kind == "forEach" || kind == "table") {
        return lowerRows(node, ctx, element);
    }
    if (std::optional<Node> display = lowerDisplay(node, ctx, element)) {
        return std::move(*display);
    }
    if (kind == "splitter") {
        return lowerSplitter(node, ctx, element);
    }
    if (kind == "collapsible") {
        return lowerCollapsible(node, ctx, element);
    }
    if (kind == "dropZone") {
        return lowerDropZone(node, ctx, element);
    }
    if (kind == "actionBar") {
        KindKit const bar{*this, node, ctx, element};
        ActionBar spec{
            .children = bar.children("children"),
            .align = alignOf(node.name("align")),
            .justify = justifyOf(node.name("justify")),
            .wrap = node.flag("wrap"),
        };
        reportActionBarGaps(node, ctx);
        commonOf(node, ctx, element, spec.common);
        return ui::actionBar(std::move(spec));
    }
    if (kind == "page") {
        return lowerPage(node, ctx, element);
    }
    if (kind == "pageRegion") {
        return lowerPageRegion(node, ctx, element);
    }
    if (kind == "localeScope") {
        return lowerLocaleScope(node, ctx);
    }
    if (kind == "boundary") {
        return lowerBoundary(node, ctx);
    }
    if (kind == "custom") {
        return lowerCustom(node, ctx);
    }
    if (kind == "customize") {
        return lowerCustomize(node, ctx);
    }
    if (kind == "form") {
        return lowerForm(node, ctx, element);
    }
    if (kind == "collection" || kind == "wizard") {
        return lowerComposite(node, ctx, element);
    }
    report(node.path, "`" + node.kind + "` is a node kind this interpreter does not lower");
    return ui::text(Text{.text = std::string{node.kind}, .role = TextRole::Muted});
}

namespace {

/// @brief A document field overlay's expressions, evaluated once, as the engine's overlay.
/// @param scope The scope the form is declared in.
/// @param part  The `fields` entry.
/// @param format How values read as text and numbers.
/// @return The overlay.
[[nodiscard]] forms::FieldOverlay overlayOf(RtScope const& scope, DocNode const& part, FormatContext const& format) {
    auto const read = [&](std::string_view key) -> std::optional<Value> {
        ExprPtr const expr = part.prop(key);
        if (!expr) {
            return std::nullopt;
        }
        return scope.core.runtime().untracked([&] { return evalIn(scope, nullptr, expr); });
    };
    auto const text = [&](std::string_view key) -> std::optional<std::string> {
        auto const value = read(key);
        return value.has_value() && !value->isNull() ? std::optional<std::string>{textOf(*value, format)}
                                                     : std::nullopt;
    };
    auto const flag = [&](std::string_view key) {
        auto const value = read(key);
        bool const* const set = value.has_value() ? value->asBool() : nullptr;
        return set != nullptr && *set;
    };
    forms::FieldOverlay overlay{.omit = flag("omit"), .hidden = flag("hidden")};
    overlay.label = text("label");
    overlay.unit = text("unit");
    overlay.widget = text("widget");
    if (auto const decimals = read("decimals");
        decimals.has_value() && decimals->asInt() != nullptr && *decimals->asInt() >= 0) {
        overlay.decimals = static_cast<std::uint32_t>(std::min<std::int64_t>(*decimals->asInt(), 18));
    }
    if (auto const blankAs = text("blankAs")) {
        overlay.blankAs = *blankAs == "empty" ? forms::BlankAs::Empty : forms::BlankAs::Omit;
    }
    return overlay;
}

/// @brief A lowering context whose scope is held weakly: what an Effect the scope itself owns may keep, so the scope
///        does not keep itself alive.
struct DetachedLower {
    /// @brief The scope, weakly.
    std::weak_ptr<RtScope> scope;
    /// @brief The names bound there.
    std::shared_ptr<Bindings const> bindings;
    /// @brief The innermost boundary.
    std::shared_ptr<BoundaryRuntime> boundary;
};

/// @brief Detaches a lowering context's scope.
/// @param ctx The context.
/// @return The context, its scope held weakly.
[[nodiscard]] DetachedLower detachedLower(LowerCtx const& ctx) {
    return DetachedLower{.scope = ctx.scope, .bindings = ctx.bindings, .boundary = ctx.boundary};
}

/// @brief Puts a detached context's scope back.
/// @param held The detached context.
/// @return The context; its scope is null once the scope is gone.
[[nodiscard]] LowerCtx attached(DetachedLower const& held) {
    return LowerCtx{.scope = held.scope.lock(), .bindings = held.bindings, .boundary = held.boundary};
}
}  // namespace

// Builds the form's session when its element is declared, so its values can be read before the form is drawn.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): one section per kind of form element and its sessions.
void ScreenCore::wireForm(ElementState& element) {
    DocNode const& node = *element.node;
    RtScope& scope = *element.scope;
    std::string const action{node.name("action")};
    Catalog const& catalog = session.options().catalog;
    std::optional<std::string> const schema = catalog.schema ? catalog.schema(action) : std::nullopt;
    if (!schema.has_value()) {
        report(node.path, "no schema for `" + action + "`: the catalog has none for a `form` to build from");
        return;
    }
    NameRef const* const alias = scope.find(node.name("model"));
    if (alias == nullptr || alias->model == nullptr) {
        report(node.path, "`" + std::string{node.name("model")} + "` is not a model alias");
        return;
    }
    forms::FieldOverlays overlays;
    // `fields` is one record part holding a `field` part per overlaid member.
    for (DocNodePtr const& record : node.partList("fields")) {
        for (DocNodePtr const& part : record->partList("fields")) {
            forms::FieldOverlay overlay = overlayOf(scope, *part, session.formatting());
            // A computed `decimals` is carried by the effect below alone: baking the value read now would make it
            // the fallback when the expression later gives no places, where the schema's places belong.
            if (ExprPtr const places = part->prop("decimals"); places && places->kind != ExprKind::Literal) {
                overlay.decimals.reset();
            }
            overlays.insert_or_assign(part->id, std::move(overlay));
        }
    }
    auto model = forms::FormModel::fromSchema(action, *schema, overlays);
    if (!model) {
        report(node.path,
               "the form of `" + action + "` does not build: " + model.error().path + ": " + model.error().message);
        return;
    }
    if (node.name("submit") == "explicit") {
        model = model->withSubmitMode(forms::SubmitMode::Explicit);
    } else if (node.name("submit") == "auto") {
        model = model->withSubmitMode(forms::SubmitMode::Automatic);
    }
    element.form = std::make_unique<forms::FormSession>(runtime(), *std::move(model), aliasSubmits(alias->model),
                                                        aliasCalls(alias->model), engineOptions());
    followFormatting(element, scope);
    deriveForm(element, alias->model);
    // An overlay's `decimals` follows its expression: the field keeps the draft as typed and checks it again against
    // the places the expression gives now.
    for (DocNodePtr const& record : node.partList("fields")) {
        for (DocNodePtr const& part : record->partList("fields")) {
            ExprPtr const places = part->prop("decimals");
            if (!places) {
                continue;
            }
            forms::FieldState* state = nullptr;
            try {
                state = &element.form->field(part->id);
            } catch (std::out_of_range const&) {
                continue;  // The overlay names a field the form does not have (an omitted one).
            }
            scope.nodes.effect([this, &scope, places, state] {
                int const now = placesOf(evalIn(scope, nullptr, places), session.formatting());
                runtime().untracked([&] {
                    state->places().set(now >= 0 ? std::optional<std::uint32_t>{static_cast<std::uint32_t>(now)}
                                                 : std::nullopt);
                });
            });
        }
    }
    if (ExprPtr const prefill = node.prop("prefill")) {
        // A prefill follows its expression. A value of the record being edited (the first value, or one under the
        // same `key`) keeps the fields the user changed; another record, a new `key`, or any later value of a form
        // without one, loads every field.
        forms::FormSession* const form = element.form.get();
        struct Followed {
            bool loaded = false;
            Value key;
        };
        auto const followed = std::make_shared<Followed>();
        scope.nodes.effect([this, &scope, prefill, key = node.prop("key"), form, followed] {
            Value const value = evalIn(scope, nullptr, prefill);
            if (value.isNull()) {
                return;
            }
            runtime().untracked([&] {
                Value const current = key ? evalIn(scope, nullptr, key) : Value{};
                bool const same = !followed->loaded || (key && current == followed->key);
                followed->loaded = true;
                followed->key = current;
                std::string const json = glz::write_json(valueToJson(value)).value_or(std::string{});
                if (same) {
                    form->refill(json);
                } else {
                    form->prefill(json);
                }
            });
        });
    }
}

namespace {

// Whether a field, or a member of it at any depth, is computed: what an evaluation round would bring back.
// NOLINTNEXTLINE(misc-no-recursion): an object's and a row's members are fields.
[[nodiscard]] bool computesAny(std::span<forms::FieldSpec const> fields) {
    return std::ranges::any_of(fields, [](forms::FieldSpec const& field) {
        return field.computed || computesAny(field.children);
    });
}

}  // namespace

// A form's rounds of derived values: its action's evaluation while a member is computed, and its `derive` action when
// it names one, each sent through the form's alias, `deriveDebounce` (150 ms when absent) after the last commit on the
// session's timers. A path a derive reply names that is no derived member is reported once.
void ScreenCore::deriveForm(ElementState& element, ModelAlias* alias) {
    DocNode const& node = *element.node;
    std::string const action{node.name("action")};
    std::string const derive{node.name("derive")};
    bool const computes = computesAny(element.form->model().fields());
    if (derive.empty() && !computes) {
        return;
    }
    auto const call = [this, alias](std::string actionId, bool evaluate) {
        return [this, alias, actionId = std::move(actionId), evaluate](std::string body) -> async::Completion<std::string> {
            std::shared_ptr<ModelChannel> const channel = channelOf(*alias, *this);
            if (channel == nullptr) {
                auto [failed, promise] = async::Completion<std::string>::makeSettleable(&runtime().owner());
                promise.reject(std::make_exception_ptr(std::runtime_error{"the node's model alias is not bound"}));
                return std::move(failed);
            }
            return channel->execute(actionId, body, CallOptions{.evaluate = evaluate});
        };
    };
    forms::DeriveOptions options;
    if (computes) {
        options.evaluate = call(action, true);
    }
    if (!derive.empty()) {
        options.derive = call(derive, false);
    }
    options.scheduler = session.options().scheduler;
    if (ExprPtr const debounce = node.prop("deriveDebounce"); debounce && debounce->kind == ExprKind::Literal) {
        if (std::int64_t const* const millis = debounce->literal.asInt(); millis != nullptr && *millis >= 0) {
            options.debounce = std::chrono::milliseconds{*millis};
        }
    }
    options.refused = [this, path = node.path, derive](std::vector<std::string> const& paths) {
        for (std::string const& refused : paths) {
            report(path + "/derive",
                   "the reply of `" + derive + "` names `" + refused + "`, which is not a derived member of the form");
        }
    };
    element.form->deriveWith(std::move(options));
}

Node ScreenCore::lowerForm(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    forms::FormSession* const form = element == nullptr ? nullptr : element->form.get();
    if (form == nullptr) {
        return ui::text(Text{.text = std::string{node.name("action")}, .role = TextRole::Muted});
    }
    // The form's events, each run once per change of the count it watches.
    auto const watch = [this, &ctx](Commands const* commands, auto count, auto payload) {
        if (commands == nullptr || commands->empty()) {
            return;
        }
        auto const seen = std::make_shared<std::uint64_t>(runtime().untracked(count));
        ctx.scope->nodes.effect([this, held = detachedLower(ctx), commands, count, payload, seen] {
            std::uint64_t const now = count();
            if (now == *seen) {
                return;
            }
            *seen = now;
            runtime().untracked([&] { runEvent(commands, attached(held), payload()); });
        });
    };
    watch(
        node.event("onSubmit"), [form] { return form->submitAttempts(); },
        [form] {
            std::optional<std::string> const body = form->body();
            return body.has_value() ? jsonTextValue(*body) : Value{};
        });
    watch(
        node.event("onSuccess"), [form] { return form->successCount(); },
        [form] {
            std::optional<std::string> const& reply = form->lastReply();
            return reply.has_value() ? jsonTextValue(*reply) : Value{};
        });
    if (Commands const* const onError = node.event("onError"); onError != nullptr && !onError->empty()) {
        // Each failure is a new exception: run once per failure, with its text.
        auto const seen =
            std::make_shared<std::exception_ptr>(runtime().untracked([form] { return form->lastError(); }));
        ctx.scope->nodes.effect([this, held = detachedLower(ctx), onError, form, seen] {
            std::exception_ptr const error = form->lastError();
            if (error == nullptr || error == *seen) {
                return;
            }
            *seen = error;
            runtime().untracked([&] { runEvent(onError, attached(held), Value{failureText(error)}); });
        });
    }
    forms::FormViewOptions options{.showTitle = false};
    presentForm(node, ctx, options);
    options.title = bindProp<std::string>(ctx, node.prop("title"), std::string{}, stringOf);
    // A field whose overlay names a component is drawn by it; only top-level fields take one, as every overlay.
    for (DocNodePtr const& record : node.partList("fields")) {
        for (DocNodePtr const& overlay : record->partList("fields")) {
            if (!overlay->name("component").empty()) {
                options.overrides.byField(overlay->id,
                                          [this, ctx, field = overlay.get()](forms::FieldView const& view) {
                                              return fieldComponent(view, *field, ctx);
                                          });
            }
            // A `grid` part makes the field a grid table, kept on the form's element by the field's name so every
            // mount of the form shows the same grid; the scope is held weakly, as the grid's is.
            if (std::span<DocNodePtr const> const grid = overlay->partList("grid"); !grid.empty()) {
                options.overrides.byField(
                    overlay->id, [this, element, detached = detachedLower(ctx), part = grid.front().get(),
                                  name = overlay->id](forms::FieldView const& view) {
                        LowerCtx const live = attached(detached);
                        if (live.scope == nullptr) {
                            return ui::text(Text{.text = view.state().path(), .role = TextRole::Muted});
                        }
                        std::unique_ptr<ElementState>& slot = element->fieldGrids[name];
                        if (slot == nullptr) {
                            slot = std::make_unique<ElementState>(runtime());
                            slot->node = part;
                            slot->scope = element->scope;
                        }
                        return formGridTable(*part, live, *slot, view.session(), view.state());
                    });
            }
        }
    }
    Column column{.children = {forms::formView(*form, std::move(options))}};
    commonOf(node, ctx, element, column.common);
    return ui::column(std::move(column));
}

// What a grid's part gives beyond its table: its columns, each a member its `field` names or a display column whose
// `value` is read with `row` the row's record, every member of the row type in schema order when it gives none;
// `enterMoves`, `walk`, and `onActivate`, run with the row's key on a read-only cell. What the columns and
// `onActivate` keep holds the scope weakly: the grid lives as long as the element the scope owns.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): one section per key of a grid column.
void ScreenCore::gridParts(DocNode const& part, LowerCtx const& ctx, forms::FieldState const& array,
                           std::vector<GridColumn>& columns, GridOptions& options) {
    DetachedLower const detached = detachedLower(ctx);
    // An expression read in a row's scope, `row` its record of encoded members; null once the scope is gone. Tracked.
    auto const inRow = [detached](ExprPtr expr) {
        return [detached, expr](forms::RowState const& row) -> std::optional<std::pair<Value, FormatContext>> {
            LowerCtx const live = attached(detached);
            if (live.scope == nullptr) {
                return std::nullopt;
            }
            ValueRecord record;
            for (auto const& member : row.fields()) {
                forms::Encoding const& encoding = member->encoded().get();
                record.insert_or_assign(member->spec().name, encoding.has_value() && encoding->has_value()
                                                                 ? (*encoding)->value
                                                                 : Value{});
            }
            Bindings bindings = live.bindings != nullptr ? *live.bindings : Bindings{};
            bindings.push_back(Binding{.name = "row", .read = [record] { return Value{record}; }});
            return std::pair{evalIn(*live.scope, &bindings, expr), live.scope->formatting()};
        };
    };
    for (DocNodePtr const& column : part.partList("columns")) {
        ExprPtr const label = column->prop("label");
        GridColumn made{.id = column->id,
                        .label = label ? textOf(once(ctx, label), ctx.scope->formatting()) : column->id,
                        .field = std::string{column->name("field")},
                        .value = {}};
        if (ExprPtr const field = column->prop("field")) {
            // One member per row: a name that is no member of the row type is reported once, and its cell is read-only.
            std::vector<std::string> members;
            for (forms::FieldSpec const& member : array.spec().children) {
                members.push_back(member.name);
            }
            made.fieldOf = [this, read = inRow(field), members = std::move(members), path = field->path](
                               forms::RowState const& row) {
                auto const read_ = read(row);
                std::string const name = read_ ? textOf(read_->first, read_->second) : std::string{};
                if (!name.empty() && std::ranges::find(members, name) == members.end()) {
                    report(path, "`" + name + "` names no member of the row");
                }
                return name;
            };
        }
        ExprPtr const value = column->prop("value");
        if (made.field.empty() && !made.fieldOf && value) {
            made.value = [read = inRow(value)](forms::RowState const& row) {
                auto const read_ = read(row);
                return read_ ? textOf(read_->first, read_->second) : std::string{};
            };
        }
        // Overlays per row, as a form field's overlays, each read in the row's scope.
        if (ExprPtr const decimals = column->prop("decimals")) {
            made.decimals = [read = inRow(decimals)](forms::RowState const& row) -> std::optional<std::uint32_t> {
                auto const read_ = read(row);
                int const places = read_ ? placesOf(read_->first, read_->second) : -1;
                return places >= 0 ? std::optional{static_cast<std::uint32_t>(places)} : std::nullopt;
            };
        }
        if (ExprPtr const unit = column->prop("unit")) {
            made.unit = [read = inRow(unit)](forms::RowState const& row) -> std::optional<std::string> {
                auto const read_ = read(row);
                std::string const* const id = read_ ? read_->first.asString() : nullptr;
                return id != nullptr && !id->empty() ? std::optional{*id} : std::nullopt;
            };
        } else if (std::string_view const unitName = column->name("unit"); !unitName.empty()) {
            made.unit = [id = std::string{unitName}](forms::RowState const&) { return std::optional{id}; };
        }
        if (ExprPtr const readonly = column->prop("readonly")) {
            made.readonly = [read = inRow(readonly)](forms::RowState const& row) {
                auto const read_ = read(row);
                bool const* const flag = read_ ? read_->first.asBool() : nullptr;
                return flag != nullptr && *flag;
            };
        }
        if (ExprPtr const offered = column->prop("options")) {
            made.options = [read = inRow(offered)](forms::RowState const& row) -> std::optional<std::vector<std::string>> {
                auto const read_ = read(row);
                ValueList const* const items = read_ ? read_->first.asList() : nullptr;
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
        if (ExprPtr const placeholder = column->prop("placeholder")) {
            made.placeholder = [read = inRow(placeholder)](forms::RowState const& row) {
                auto const read_ = read(row);
                return read_ ? textOf(read_->first, read_->second) : std::string{};
            };
        }
        columns.push_back(std::move(made));
    }
    if (columns.empty()) {
        for (forms::FieldSpec const& member : array.spec().children) {
            columns.push_back(GridColumn{.id = member.name, .label = member.title, .field = member.name, .value = {}});
        }
    }
    std::string_view const enter = part.name("enterMoves");
    options.enterMoves = enter == "right" ? EnterMoves::Right : (enter == "none" ? EnterMoves::None : EnterMoves::Down);
    options.walk = part.name("walk") == "columns" ? Walk::Columns : Walk::Rows;
    options.appendRows = part.flag("appendRows");
    options.rowsFixed = part.flag("rowsFixed");
    options.maxPasteCells = session.options().maxPasteCells;
    options.maxCopyRows = session.options().maxCopyRows;
    options.scheduler = session.options().scheduler;
    options.undoHold = kStatusMessageHold;
    if (Commands const* const commands = part.event("onActivate"); commands != nullptr && !commands->empty()) {
        options.onActivate = [this, detached, commands](Key const& key) {
            LowerCtx const live = attached(detached);
            if (live.scope != nullptr) {
                runEvent(commands, live, valueOfKey(key));
            }
        };
    }
}

// A grid of a form named by a table's `form` and `field`.
Node ScreenCore::lowerFormGrid(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    NameRef const* const named = ctx.scope->find(node.name("form"));
    forms::FormSession* const form =
        named != nullptr && named->element != nullptr ? named->element->form.get() : nullptr;
    if (form == nullptr || element == nullptr) {
        return ui::text(Text{.text = std::string{node.name("field")}, .role = TextRole::Muted});
    }
    forms::FieldState* array = nullptr;
    try {
        array = &form->field(node.name("field"));
    } catch (std::out_of_range const&) {
        report(node.path, "`" + std::string{node.name("field")} + "` is not a field of the form");
        return ui::text(Text{.text = std::string{node.name("field")}, .role = TextRole::Muted});
    }
    return formGridTable(node, ctx, *element, *form, *array);
}

// A grid of a form, made once per element: every mount of the table shows the same grid, so its focus, its open editor,
// its messages, its layout, its sort and its filters outlive a remount. Its columns and walk are `gridParts`'; its
// header, its menus, its cell menu and its arrangement are a table's (`configureTable`): an edit column whose part says
// `sortable` sorts, every edit column filters unless its part says `filterable: false`, and a display column does
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
        config.columns.push_back(LayoutColumn{.id = column.id,
                                              .label = column.label,
                                              .locked = false,
                                              .kind = edits && !perRow ? detail::columnKindOf(member->kind)
                                                                       : table::ColumnKind::Text});
        config.sortable.push_back(edits && at < parts.size() && parts[at]->flag("sortable"));
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
    config.filterTarget = [held, filterable](std::string const& id) -> std::optional<FilterTarget> {
        auto const found = std::ranges::find(held->config.columns, id, &LayoutColumn::id);
        auto const at = static_cast<std::size_t>(found - held->config.columns.begin());
        if (held->grid.source == nullptr || found == held->config.columns.end() || !filterable.at(at)) {
            return std::nullopt;
        }
        std::span<table::ColumnInfo const> const infos = held->grid.source->view().source().columns();
        auto const info = std::ranges::find(infos, id, &table::ColumnInfo::id);
        if (info == infos.end()) {
            return std::nullopt;
        }
        FilterTarget target;
        target.column = id;
        target.label = found->label;
        target.shown = table::TableColumn{.info = *info};
        target.listed = true;
        return target;
    };
    // A cell a menu opens on: the member as its column's filter reads it, and the row's record.
    config.heldCell = [held](Key const& key, std::string const& id) -> std::optional<HeldCell> {
        FormGridSource const* const source = held->grid.source.get();
        if (source == nullptr) {
            return std::nullopt;
        }
        std::optional<std::size_t> const at = source->positionOf(key);
        std::optional<TableRowRef> const ref =
            at ? held->grid.source->view().row(static_cast<std::int64_t>(*at)) : std::nullopt;
        if (!ref) {
            return std::nullopt;
        }
        std::optional<std::pair<table::Cell, table::TableColumn>> cell = source->filterCell(key, id);
        if (!cell) {
            return HeldCell{.record = ref->row, .cell = table::Cell{}, .shown = {}, .units = {}, .keep = nullptr};
        }
        return HeldCell{
            .record = ref->row, .cell = std::move(cell->first), .shown = std::move(cell->second), .units = {}, .keep = nullptr};
    };
    // A copy: each cell's text as the grid shows it, under its column's label.
    config.gather = [this, held](std::vector<Key> keys, std::vector<std::string> const& ids, CopyDone done) {
        CopyGathered gathered;
        std::vector<std::string> kept;
        for (std::string const& id : ids) {
            auto const found = std::ranges::find(held->config.columns, id, &LayoutColumn::id);
            if (found != held->config.columns.end()) {
                kept.push_back(id);
                gathered.headers.push_back(found->label);
            }
        }
        FormGridSource const& source = *held->grid.source;
        runtime().untracked([&] {
            for (Key const& key : keys) {
                std::optional<std::size_t> const at = source.positionOf(key);
                if (!at) {
                    continue;
                }
                CopyRow row{.viewRow = static_cast<std::int64_t>(*at), .cells = {}};
                for (std::string const& id : kept) {
                    row.cells.emplace_back(source.text(key, id));
                }
                gathered.rows.push_back(std::move(row));
            }
        });
        done(std::move(gathered));
    };
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
        held->grid.grid->onSelectionChange([this, detached = detachedLower(ctx), commands](
                                               std::optional<CellRange> const& range) {
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
                        bool const descending =
                            direction != nullptr && direction->asString() != nullptr && *direction->asString() == "desc";
                        chain.push_back(table::SortKey{.column = *column->asString(),
                                                       .dir = descending ? table::SortDirection::Descending
                                                                         : table::SortDirection::Ascending});
                    }
                }
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
            for (StatusEntry const& entry : offered) {
                entries.emplace_back(entry, [held, id = entry.id] { held->grid.grid->chooseEntry(id); });
            }
            tableMessage(element, text, std::move(entries));
        });
    });
    return gridTableNode(part, ctx, element);
}

// The table node of a grid kept in @p element: its status line, its configuring and its columns' filter buttons.
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
    table.onStatusEntry = [held](std::string const& id) {
        if (auto const handler = held->entryHandlers.find(id); handler != held->entryHandlers.end()) {
            std::function<void()> const run = handler->second;
            run();
            return;
        }
        held->grid.grid->chooseEntry(id);
    };
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
        table.onMenu = [this, own, rowEntries, chosen = std::move(table.onMenu)](std::optional<std::size_t> column,
                                                                                std::vector<std::size_t> const& path) {
            if (!column && !path.empty()) {
                std::size_t const shown = own(column).size();
                std::size_t const first = shown + (shown > 0 ? 1 : 0);
                if (path.front() >= first) {
                    std::vector<MenuItem> const rows = rowEntries();
                    std::size_t const at = path.front() - first;
                    if (at < rows.size() && runtime().untracked([&] { return rows.at(at).enabled.evaluate(); }) &&
                        rows.at(at).onSelect) {
                        rows.at(at).onSelect();
                    }
                    return;
                }
            }
            chosen(column, path);
        };
    }
    std::vector<TableColumn> columns = held->shown;
    markFilterable(held->config, columns, table);
    return besideDialogs(table.common, std::move(dialogs),
                         [&] { return held->grid.grid->node(std::move(columns), std::move(table)); });
}

[[nodiscard]] std::expected<forms::detail::engine::Json, std::string> changeWire(std::string const& payload,
                                                                                 std::string_view type) {
    std::string_view text{payload};
    if (type == "string") {
        if (text.starts_with("s:")) {
            text.remove_prefix(2);
        }
        // Parentheses: braces would pick the initializer-list constructor and make a one-element array.
        return forms::detail::engine::Json(std::string{text});
    }
    if (text.starts_with("i:")) {
        text.remove_prefix(2);
    }
    std::optional<forms::detail::engine::Json> json = forms::detail::engine::parseJson(text);
    if (!json) {
        return std::unexpected(std::string{"it is not JSON"});
    }
    bool const fits = type == "json" || json->is_null() || (type == "bool" && json->is_boolean()) ||
                      ((type == "int" || type == "number") && json->is_number()) ||
                      (type == "exact" && (json->is_number() || json->is_string()));
    if (!fits) {
        bool const vowel = type == "int" || type == "exact";
        return std::unexpected("it is not " + std::string{vowel ? "an " : "a "} + std::string{type});
    }
    return *std::move(json);
}

namespace {

/// @brief What a form fills of a field component's props: the field's value, errors, read-only and required state,
///        and label, each a tracked read of the field.
/// @param prop  The prop's name.
/// @param state The field.
/// @return The read, or empty for a prop the form does not fill.
[[nodiscard]] std::function<Value()> formFilled(std::string const& prop, forms::FieldState const* state) {
    if (prop == "value") {
        return [state] {
            forms::Encoding const& encoding = state->encoded().get();
            if (!encoding.has_value() || !encoding->has_value()) {
                return Value{};
            }
            return (*encoding)->value.isNull() ? jsonTextValue((*encoding)->json) : (*encoding)->value;
        };
    }
    if (prop == "errors") {
        return [state] {
            std::optional<forms::FieldError> const& error = state->error().get();
            return error.has_value() ? Value{ValueList{Value{error->message}}} : Value{ValueList{}};
        };
    }
    if (prop == "readonly") {
        return [state] { return Value{state->readonly().get()}; };
    }
    if (prop == "required") {
        return [state] { return Value{state->required().get()}; };
    }
    if (prop == "label") {
        return [state] { return Value{state->label()}; };
    }
    return {};
}
}  // namespace

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a prop's name, then its declared type.
CustomProp ScreenCore::componentProp(std::string const& prop, std::string const& type, std::function<Value()> read) {
    if (type == "bool") {
        return CustomProp{
            .name = prop,
            .value = slotProp<bool>([this, read = std::move(read)] { return boolOf(read(), session.formatting()); }),
        };
    }
    if (type == "number") {
        return CustomProp{
            .name = prop,
            .value = slotProp<double>([read = std::move(read)] { return componentNumber(read()); }),
        };
    }
    return CustomProp{
        .name = prop,
        .value = slotProp<std::string>(
            [this, read = std::move(read), type] { return componentText(read(), type, session.formatting()); }),
    };
}

void ScreenCore::changeField(forms::FieldState& state, forms::FieldSpec const& spec, std::string const& path,
                             std::string_view valueType, std::string const& payload) {
    auto wire = changeWire(payload, valueType);
    // A base64 member holds a file's bytes, which a document moves inline only under the session's cap.
    if (std::size_t const cap = session.options().maxInlineFileBytes;
        wire && spec.bytes && wire->is_string() && base64Size(wire->get_string()) > cap) {
        report(path, "field " + spec.name + ": " + std::to_string(base64Size(wire->get_string())) +
                         " bytes is over the " + std::to_string(cap) + " inline cap");
        return;
    }
    if (wire) {
        if (auto edited = state.edit(*wire); !edited) {
            wire = std::unexpected(edited.error());
        }
    }
    if (!wire) {
        report(path, "field " + spec.name + ": the component's change does not decode: " + wire.error());
    }
}

Node ScreenCore::fieldComponent(forms::FieldView const& view, DocNode const& overlay, LowerCtx const& ctx) {
    std::string const name{overlay.name("component")};
    // A loaded document's field component is declared, with a `value` prop and a `change` event.
    Component const& component = _doc->components.at(name);
    forms::FieldState* const state = &view.state();
    Custom spec{
        .name = name,
        .fallback = view.defaultControl(),
        .props = {},
        .minVersion =
            static_cast<int>(std::clamp<std::int64_t>(component.version, 0, std::numeric_limits<int>::max())),
        .onEvent = {},
    };
    for (auto const& [prop, type] : component.props) {
        if (prop == "value" && type == "json") {
            // The wire JSON itself, so an object's or a collection's every digit reaches the component.
            spec.props.push_back(CustomProp{
                .name = prop,
                .value = slotProp<std::string>([state] {
                    forms::Encoding const& encoding = state->encoded().get();
                    return encoding.has_value() && encoding->has_value() ? (*encoding)->json : std::string{"null"};
                }),
            });
            continue;
        }
        std::function<Value()> read = formFilled(prop, state);
        if (!read) {
            ExprPtr given;
            for (DocNodePtr const& record : overlay.partList("props")) {
                if (ExprPtr const expr = record->prop(prop)) {
                    given = expr;
                }
            }
            read = [scope = ctx.scope, bindings = ctx.bindings, given] {
                return given ? evalIn(*scope, bindings.get(), given) : Value{};
            };
        }
        spec.props.push_back(componentProp(prop, type, std::move(read)));
    }
    spec.onEvent = [this, state, path = overlay.path, fieldSpec = &view.spec(),
                    valueType = component.props.at("value")](std::string const& event, std::string const& payload) {
        if (event == "change") {
            changeField(*state, *fieldSpec, path, valueType, payload);
        }
    };
    return ui::custom(std::move(spec));
}

// Every call goes through the alias's channel, bound for its current instance when the call is made.
forms::Submitter ScreenCore::aliasCalls(ModelAlias* alias) {
    return [this, alias](std::string_view actionId, std::string const& body) -> async::Completion<std::string> {
        std::shared_ptr<ModelChannel> const channel = channelOf(*alias, *this);
        if (channel == nullptr) {
            auto [failed, promise] = async::Completion<std::string>::makeSettleable(&runtime().owner());
            promise.reject(std::make_exception_ptr(std::runtime_error{"the node's model alias is not bound"}));
            return std::move(failed);
        }
        return channel->execute(std::string{actionId}, body, CallOptions{});
    };
}

// The calls of a form, collection or wizard: a success other than of @p read, the collection's own query, tells the
// session, which applies the action's effects, as a mutation's success does.
forms::Submitter ScreenCore::aliasSubmits(ModelAlias* alias, std::string read) {
    return [this, alias, call = aliasCalls(alias), read = std::move(read)](
               std::string_view actionId, std::string body) -> async::Completion<std::string> {
        async::Completion<std::string> completion = call(actionId, std::move(body));
        if (actionId != read) {
            completion.then([this, alive = std::weak_ptr<bool>{_alive}, alias, typeId = alias->decl->type,
                             action = std::string{actionId}](std::string const& reply) {
                if (alive.expired()) {
                    return;
                }
                auto json = parseJson(reply);
                auto value = json ? valueFromJson(*json) : std::unexpected(json.error());
                std::string const token = runtime().untracked([&] { return alias->token->get(); });
                session.actionSucceeded(typeId, action, value ? *value : Value{}, ReplyUse::Applied, token);
            });
        }
        return completion;
    };
}

// The session's locale, translations, display zone and date patterns, as every form the engine builds here reads them.
forms::FormSessionOptions ScreenCore::engineOptions() const {
    FormatContext const& format = session.formatting();
    forms::FormSessionOptions options{};
    options.locale = withoutGrouping(format.numeric);
    options.bcp47 = format.locale;
    options.translations = format.translations;
    options.displayOffsetMinutes = format.utcOffsetMinutes;
    options.datePattern = format.datePattern;
    options.dateTimePattern = format.dateTimePattern;
    return options;
}

// Keeps a form's, collection's or wizard's number locale and date patterns at the session's current formatting. The
// engine's number locale holds views into the facts it was given, so the effect also holds the facts it last applied.
void ScreenCore::followFormatting(ElementState& element, RtScope& scope) {
    auto const held = std::make_shared<std::shared_ptr<FormatContext const>>();
    forms::FormSession* const form = element.form.get();
    forms::CollectionSession* const collection = element.collection.get();
    forms::WizardSession* const wizard = element.wizard.get();
    scope.nodes.effect([this, held, form, collection, wizard] {
        std::shared_ptr<FormatContext const> const format = session.formattingHandle();
        runtime().untracked([&] {
            render::NumericLocale const locale = withoutGrouping(format->numeric);
            if (form != nullptr) {
                form->setFormat(locale, format->datePattern, format->dateTimePattern);
            }
            if (collection != nullptr) {
                collection->setFormat(locale, format->datePattern, format->dateTimePattern);
            }
            if (wizard != nullptr) {
                wizard->setFormat(locale, format->datePattern, format->dateTimePattern);
            }
            *held = format;
        });
    });
}

// Builds a collection's or wizard's session when its element is declared. The node's `action` is the collection's
// query or the wizard's first step; the catalog answers the document for it, and every call (the query, the actions,
// every step's submission) goes through the node's model alias.
void ScreenCore::wireComposite(ElementState& element) {
    DocNode const& node = *element.node;
    RtScope& scope = *element.scope;
    bool const collection = node.kind == "collection";
    std::string const action{node.name("action")};
    Catalog const& catalog = session.options().catalog;
    auto const& lookup = collection ? catalog.view : catalog.wizard;
    std::optional<std::string> const document = lookup ? lookup(action) : std::nullopt;
    if (!document.has_value()) {
        report(node.path, std::string{"no "} + (collection ? "view" : "wizard") + " document for `" + action +
                              "`: the catalog has none for a `" + node.kind + "` to build from");
        return;
    }
    NameRef const* const alias = scope.find(node.name("model"));
    if (alias == nullptr || alias->model == nullptr) {
        report(node.path, "`" + std::string{node.name("model")} + "` is not a model alias");
        return;
    }
    forms::SchemaLookup const schemas = catalog.schema ? forms::SchemaLookup{catalog.schema} : forms::SchemaLookup{};
    forms::Submitter const call = aliasCalls(alias->model);
    if (collection) {
        auto model = forms::CollectionModel::fromSchema(*document);
        if (!model) {
            report(node.path, "the collection of `" + action + "` does not build: " + model.error().path + ": " +
                                  model.error().message);
            return;
        }
        if (model->query() != action) {
            report(node.path, "the view document of `" + action + "` lists `" + std::string{model->query()} + "`");
            return;
        }
        std::string query{model->query()};
        element.collection = std::make_unique<forms::CollectionSession>(runtime(), *std::move(model), schemas,
                                                                        aliasSubmits(alias->model, std::move(query)),
                                                                        call, engineOptions());
        followFormatting(element, scope);
        wireCollectionTable(element, alias->model->decl->type);
        return;
    }
    // The wizard's id, the stem of its translation keys, is the action it starts with.
    auto model = forms::WizardModel::fromSchema(action, *document);
    if (!model) {
        report(node.path,
               "the wizard of `" + action + "` does not build: " + model.error().path + ": " + model.error().message);
        return;
    }
    if (model->steps().front().action != action) {
        report(node.path,
               "the wizard document of `" + action + "` starts with `" + model->steps().front().action + "`");
        return;
    }
    try {
        element.wizard = std::make_unique<forms::WizardSession>(runtime(), *std::move(model), schemas,
                                                                aliasSubmits(alias->model), call, engineOptions());
        followFormatting(element, scope);
    } catch (std::invalid_argument const& error) {
        report(node.path, error.what());
    }
}

// The presentation keys a form, a collection and a wizard share, but the title: the label of the explicit-mode button
// and the status line naming it, bound in the node's scope; whether the status line shows, the grid's columns and the
// field layout and the inline caption column's width, as read at load.
void ScreenCore::presentForm(DocNode const& node, LowerCtx const& ctx, forms::FormViewOptions& options) {
    if (ExprPtr const label = node.prop("submitLabel")) {
        options.submitLabel = bindProp<std::string>(ctx, label, std::string{}, stringOf);
    }
    if (auto const shown = node.flags.find("showStatus"); shown != node.flags.end()) {
        options.showStatus = shown->second;
    }
    if (ExprPtr const columns = node.prop("columns"); columns != nullptr && columns->literal.asInt() != nullptr) {
        options.gridColumns = static_cast<int>(*columns->literal.asInt());
        options.flatGridColumns = options.gridColumns;
    }
    if (node.name("fieldLayout") == "inline") {
        options.fieldLayout = forms::FieldLayout::Inline;
    }
    // The loader has checked `captionWidth` is a literal integer.
    if (ExprPtr const width = node.prop("captionWidth")) {
        options.inlineLabelWidth = static_cast<int>(*width->literal.asInt());
    }
}

Node ScreenCore::lowerComposite(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    // The editor's and the steps' titles are the dialog's and the header's; the forms do not repeat them. A `title`
    // replaces the view's own heading.
    forms::FormViewOptions form{};
    form.showTitle = false;
    presentForm(node, ctx, form);
    Prop<std::string> title = bindProp<std::string>(ctx, node.prop("title"), std::string{}, stringOf);
    Node view;
    if (element != nullptr && element->collection != nullptr) {
        forms::CollectionViewOptions options{};
        options.form = std::move(form);
        options.title = std::move(title);
        // The rows are a table as every other table of the screen is.
        if (element->rowsTable != nullptr) {
            options.chrome.rows = [this, ctx, element](forms::CollectionSession& /*collection*/,
                                                       std::function<Node(std::string const&)> const& buttons) {
                return lowerCollectionRows(*element, ctx, buttons);
            };
        }
        view = forms::collectionView(*element->collection, std::move(options));
    } else if (element != nullptr && element->wizard != nullptr) {
        forms::WizardViewOptions options{};
        options.form = std::move(form);
        options.title = std::move(title);
        view = forms::wizardView(*element->wizard, options);
    } else {
        return ui::text(Text{.text = std::string{node.name("action")}, .role = TextRole::Muted});
    }
    Column column{.children = {std::move(view)}};
    commonOf(node, ctx, element, column.common);
    return ui::column(std::move(column));
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): an overlay's scope, content, actions and keys are one lowering.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): an overlay's scope, content, actions and keys are one lowering.
Node ScreenCore::lowerDialog(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    Prop<bool> open = node.prop("open") ? bindProp<bool>(ctx, node.prop("open"), false, boolOf)
                                        : slotProp<bool>([element] { return element->open.get(); });
    std::vector<DocNode const*> bodyContent;
    for (DocNodePtr const& child : node.nodes("children")) {
        bodyContent.push_back(child.get());
    }
    int const bodyGap =
        node.prop("gap") ? static_cast<int>(intOf(once(ctx, node.prop("gap")), kDialogGap)) : kDialogGap;
    LayoutHints const bodyHints = holderHints(bodyContent, node.scope ? &*node.scope : nullptr, ctx);
    Node body = lazy(
        [this, ctx, &node, element, bodyGap] -> Node {
            auto const inner = std::make_shared<RtScope>(*this, ctx.scope, nullptr, ctx.scope->nodes.depth() + 2);
            inner->owner = element;
            addPending(1);
            inner->submitted = &element->submitted;
            inner->showErrors = std::string{node.name("showErrors")};
            std::vector<DocNode const*> content;
            for (DocNodePtr const& child : node.nodes("children")) {
                content.push_back(child.get());
            }
            std::vector<Commands const*> lists;
            for (std::string_view const key : {"default", "cancel"}) {
                if (Commands const* const commands = node.event(key)) {
                    lists.push_back(commands);
                }
            }
            for (DocNodePtr const& action : node.partList("actions")) {
                if (Commands const* const commands = action->event("onClick")) {
                    lists.push_back(commands);
                }
            }
            declare(*inner, node.scope ? &*node.scope : nullptr, content, lists);
            element->inner.set(inner.get());
            LowerCtx const innerCtx{
                .scope = inner,
                .bindings = ctx.bindings,
                .boundary = ctx.boundary,
                .listGap = bodyGap,
            };
            std::vector<Node> children;
            children.reserve(content.size());
            for (DocNode const* const child : content) {
                children.push_back(lower(*child, innerCtx));
            }
            // The overlay's actions end its scoped content, so they bind in its scope; the mount puts them in its
            // footer.
            if (ActionModels actions = lowerActionParts(node.partList("actions"), innerCtx); !actions.empty()) {
                children.push_back(actionBarOf(std::move(actions)));
            }
            std::vector<KeyBinding> keys;
            if (Commands const* const commands = node.event("default")) {
                keys.push_back(KeyBinding{
                    .chord = "Enter",
                    .onPress = [this, innerCtx, commands] { runEvent(commands, innerCtx, Value{}); },
                });
            }
            if (Commands const* const commands = node.event("cancel")) {
                keys.push_back(KeyBinding{
                    .chord = "Esc",
                    .onPress = [this, innerCtx, commands] { runEvent(commands, innerCtx, Value{}); },
                });
            }
            fillHolder(children, node.path);
            return holding(std::move(children), inner, std::move(keys), bodyGap);
        },
        bodyHints);
    Action onDismiss = [this, ctx, element, commands = node.event("onDismiss")] {
        session.asUser([&] {
            session.noteWrite();
            element->open.set(false);
        });
        runEvent(commands, ctx, Value{});
    };
    if (node.kind == "drawer") {
        ExprPtr const side = node.prop("side");
        Drawer spec{
            .open = std::move(open),
            .title = bindProp<std::string>(ctx, node.prop("title"), std::string{}, stringOf),
            .description = bindProp<std::string>(ctx, node.prop("description"), std::string{}, stringOf),
            .child = std::move(body),
            .onDismiss = std::move(onDismiss),
            .side = side && textOf(once(ctx, side), ctx.scope->formatting()) == "start" ? Side::Start : Side::End,
            .headNote = bindProp<std::string>(ctx, node.prop("headNote"), std::string{}, stringOf),
            .footerNote = bindProp<std::string>(ctx, node.prop("footerNote"), std::string{}, stringOf),
            .closeButton = bindProp<bool>(ctx, node.prop("closeButton"), false, boolOf),
            .dismissLabel = bindProp<std::string>(ctx, node.prop("dismissLabel"), std::string{"Close"}, stringOf),
        };
        if (ExprPtr const spans = node.prop("descriptionSpans")) {
            spec.descriptionSpans = spansProp(ctx, spans);
        }
        commonOf(node, ctx, element, spec.common);
        return ui::drawer(std::move(spec));
    }
    Dialog spec{
        .open = std::move(open),
        .title = bindProp<std::string>(ctx, node.prop("title"), std::string{}, stringOf),
        .description = bindProp<std::string>(ctx, node.prop("description"), std::string{}, stringOf),
        .child = std::move(body),
        .onDismiss = std::move(onDismiss),
        .common = {},  // NOLINT(readability-trailing-comma): the comma ends the designator, not the empty braces.
        .sheet = bindProp<bool>(ctx, node.prop("sheet"), false, boolOf),
        .dismissLabel = bindProp<std::string>(ctx, node.prop("dismissLabel"), std::string{"Close"}, stringOf),
        .headNote = bindProp<std::string>(ctx, node.prop("headNote"), std::string{}, stringOf),
        .footerNote = bindProp<std::string>(ctx, node.prop("footerNote"), std::string{}, stringOf),
        .closeButton = bindProp<bool>(ctx, node.prop("closeButton"), false, boolOf),
    };
    if (ExprPtr const spans = node.prop("descriptionSpans")) {
        spec.descriptionSpans = spansProp(ctx, spans);
    }
    commonOf(node, ctx, element, spec.common);
    return ui::dialog(std::move(spec));
}

Node ScreenCore::lowerSwitch(DocNode const& node, LowerCtx const& ctx) {
    std::vector<ExprPtr> whens;
    Switch spec;
    std::int64_t index = 0;
    for (DocNodePtr const& part : node.partList("cases")) {
        whens.push_back(part->prop("when"));
        DocNode const* const casePart = part.get();
        std::vector<DocNode const*> caseContent;
        for (DocNodePtr const& child : casePart->nodes("children")) {
            caseContent.push_back(child.get());
        }
        LayoutHints const caseHints = holderHints(caseContent, casePart->scope ? &*casePart->scope : nullptr, ctx);
        spec.cases.push_back(SwitchCase{
            .key = Key{index++},
            .node = lazy(
                [this, ctx, casePart, caseGap = ctx.listGap] -> Node {
                    auto const inner =
                        std::make_shared<RtScope>(*this, ctx.scope, nullptr, ctx.scope->nodes.depth() + 2);
                    std::vector<DocNode const*> content;
                    for (DocNodePtr const& child : casePart->nodes("children")) {
                        content.push_back(child.get());
                    }
                    declare(*inner, casePart->scope ? &*casePart->scope : nullptr, content, {});
                    LowerCtx const innerCtx{
                        .scope = inner,
                        .bindings = ctx.bindings,
                        .boundary = ctx.boundary,
                        .listGap = caseGap,
                    };
                    std::vector<Node> children;
                    children.reserve(content.size());
                    for (DocNode const* const child : content) {
                        children.push_back(lower(*child, innerCtx));
                    }
                    fillHolder(children, casePart->path);
                    return holding(std::move(children), inner, {}, caseGap);
                },
                caseHints),
        });
    }
    spec.selector = slotProp<Key>(
        [scope = ctx.scope, bindings = ctx.bindings, selected = node.prop("on"), whens = std::move(whens)] {
            Value const value = evalIn(*scope, bindings.get(), selected);
            for (std::size_t at = 0; at < whens.size(); ++at) {
                if (evalIn(*scope, bindings.get(), whens.at(at)) == value) {
                    return Key{static_cast<std::int64_t>(at)};
                }
            }
            return Key{std::int64_t{-1}};
        });
    auto const fallback = node.nodes("default");
    spec.fallback = fallback.empty() ? Node{} : lowerAll(fallback, ctx, ctx.listGap);
    commonOf(node, ctx, nullptr, spec.common);
    return ui::switchOf(std::move(spec));
}

// The tab a bound `selected` names. A value that names no tab shows the first, and is reported once.
Prop<std::size_t> ScreenCore::controlledTabSelection(DocNode const& node, LowerCtx const& ctx, ExprPtr const& selected,
                                                     std::shared_ptr<std::vector<std::string>> const& ids) {
    auto const unknownReported = std::make_shared<bool>(false);
    return Prop<std::size_t>{[this, scope = ctx.scope, bindings = ctx.bindings, selected, ids, unknownReported,
                              path = pointerAppend(node.path, "selected")] -> std::size_t {
        Value const value = evalIn(*scope, bindings.get(), selected);
        std::string const* const named = value.asString();
        auto const found = named == nullptr ? ids->end() : std::ranges::find(*ids, *named);
        if (found != ids->end()) {
            return static_cast<std::size_t>(std::distance(ids->begin(), found));
        }
        if (!*unknownReported) {
            *unknownReported = true;
            report(path, canonicalJson(value) + " names no tab; the first is shown");
        }
        return 0;
    }};
}

// A document names tabs by id; the view tree by index, so the ids map to indices once. A bound `selected` is the
// controlled index: a pick runs `onChoose` and the page follows `selected` alone. Without it the node keeps the
// user's pick itself, as a bound index too, whose page follows the pick: a constant would move only the highlight.
Node ScreenCore::lowerTabs(DocNode const& node, LowerCtx const& ctx, ElementState* element) {
    Tabs spec;
    auto const ids = std::make_shared<std::vector<std::string>>();
    for (DocNodePtr const& tab : node.partList("tabs")) {
        auto const nodes = tab->nodes("children");
        ids->push_back(tab->id);
        spec.tabs.push_back(Tab{
            .label = bindProp<std::string>(ctx, tab->prop("label"), std::string{}, stringOf),
            .node = nodes.empty() ? Node{} : lowerAll(nodes, ctx),
        });
    }
    spec.moreLabel = bindProp<std::string>(ctx, node.prop("moreLabel"), std::string{"More"}, stringOf);
    if (ExprPtr const inset = node.prop("inset")) {
        spec.inset = static_cast<int>(intOf(once(ctx, inset), 0));
    }
    if (ExprPtr const pageInset = node.prop("pageInset")) {
        spec.pageInset = static_cast<int>(intOf(once(ctx, pageInset), 0));
    }
    spec.stripSurface = std::string{node.name("stripSurface")};
    ExprPtr const selected = node.prop("selected");
    Commands const* const onChoose = node.event("onChoose");
    bool const controlled = selected && (selected->kind != ExprKind::Literal || !selected->literal.isNull());
    auto const picked = std::make_shared<reactive::Signal<std::size_t>>(runtime(), std::size_t{0});
    spec.selected = controlled ? controlledTabSelection(node, ctx, selected, ids)
                               : Prop<std::size_t>{[picked] { return picked->get(); }};
    spec.onSelect = [this, ctx, onChoose, ids, picked, controlled, element](std::size_t index) {
        if (!controlled) {
            picked->set(index);
            if (element != nullptr && index < ids->size()) {
                element->draft.set(Value{ids->at(index)});
            }
        }
        if (onChoose != nullptr && index < ids->size()) {
            runEvent(onChoose, ctx, Value{ids->at(index)});
        }
    };
    commonOf(node, ctx, element, spec.common);
    return ui::tabs(std::move(spec));
}

// Focuses the declaration the command's target resolved to: of several candidates (cases of one switch), the one
// that is mounted. Each candidate is keyed by the command's own enclosing row keys, as many as enclose the
// candidate; with `row`, the innermost of them is the row's key instead.
bool ScreenCore::focusTarget(Command const& command, RtScope const& scope, Bindings const* bindings) {
    if (!focusElement) {
        return false;
    }
    auto const found = _doc->focusTargets.find(command.path);
    if (found == _doc->focusTargets.end()) {
        return focusElement(command.target);
    }
    FocusPlace const place = focusPlace(scope);
    std::optional<std::string> row;
    if (found->second.row) {
        Value const value = evalIn(scope, bindings, command.extra);
        if (value.isNull()) {
            return false;
        }
        row = canonicalJson(valueOfKey(keyOf(value)));
    }
    for (FocusCandidate const& candidate : found->second.candidates) {
        std::size_t const kept = row ? candidate.rows - 1 : candidate.rows;
        if (candidate.rows == 0 && row) {
            continue;
        }
        if (kept > place.rows.size()) {
            continue;
        }
        FocusPlace candidatePlace{
            .prefix = place.prefix,
            .rows = {place.rows.begin(), std::next(place.rows.begin(), static_cast<std::ptrdiff_t>(kept))},
        };
        if (row) {
            candidatePlace.rows.push_back(*row);
        }
        if (focusElement(focusKey(candidatePlace, candidate.path))) {
            return true;
        }
    }
    return false;
}

std::shared_ptr<RtScope> ScreenCore::rowScope(DocNode const& node, LowerCtx const& ctx,
                                              reactive::Signal<Value> const& row,
                                              std::vector<DocNode const*> const& content, std::string key) {
    auto scope = std::make_shared<RtScope>(*this, ctx.scope, &row, ctx.scope->nodes.depth() + 2);
    scope->rowKey = std::move(key);
    declare(*scope, node.scope ? &*node.scope : nullptr, content, {});
    return scope;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): a list of rows and a table.
namespace {

/// The tooltip of a cell whose value cannot be read as its column's kind.
constexpr std::string_view kUnreadable = "The value could not be read";

/// Whether a column part gives any of a column's display parts.
[[nodiscard]] bool givesDisplay(DocNode const& part) {
    return part.prop("decimals") != nullptr || part.prop("labels") != nullptr || part.names.contains("rounding") ||
           part.names.contains("empty") || part.names.contains("unit") || part.names.contains("date") ||
           part.flags.contains("padPlaces") || part.flags.contains("grouping");
}

/// A column's display with a column part's display parts over it, member by member.
/// @param part    The column part.
/// @param display The display the part overrides.
/// @param read    Reads one of the part's expressions once.
[[nodiscard]] table::ColumnDisplay displayOf(DocNode const& part, table::ColumnDisplay display,
                                             std::function<Value(ExprPtr const&)> const& read) {
    if (ExprPtr const decimals = part.prop("decimals")) {
        constexpr std::int64_t kMostPlaces = 18;
        Value const places = read(decimals);
        if (std::int64_t const* const count = places.asInt(); count != nullptr && *count >= 0 && *count <= kMostPlaces) {
            display.decimals = static_cast<std::uint32_t>(*count);
        }
    }
    if (auto const rounding = part.names.find("rounding"); rounding != part.names.end()) {
        display.rounding = rounding->second == "halfUp" ? table::Rounding::HalfUp : table::Rounding::HalfEven;
    }
    if (auto const pad = part.flags.find("padPlaces"); pad != part.flags.end()) {
        display.padPlaces = pad->second;
    }
    if (auto const grouping = part.flags.find("grouping"); grouping != part.flags.end()) {
        display.grouping = grouping->second;
    }
    for (auto [key, member] : {std::pair{"empty", &display.empty}, std::pair{"unit", &display.unit},
                               std::pair{"date", &display.date}}) {
        if (auto const found = part.names.find(std::string_view{key}); found != part.names.end()) {
            *member = found->second;
        }
    }
    if (ExprPtr const labels = part.prop("labels")) {
        display.labels.clear();
        Value const list = read(labels);
        for (Value const& item : list.asList() != nullptr ? *list.asList() : ValueList{}) {
            Value const* const operand = item.field("operand");
            Value const* const text = item.field("text");
            if (operand == nullptr || text == nullptr || text->asString() == nullptr) {
                continue;
            }
            display.labels.push_back(table::ValueLabel{
                .operand = operand->asString() != nullptr ? *operand->asString() : canonicalJson(*operand),
                .text = *text->asString()});
        }
    }
    return display;
}

/// A record's field as a cell of its column: what `formatCell` reads. The record holds a `Date` as `YYYY-MM-DD`, a
/// `DateTime` as a timestamp and a measure as a `Quantity` named by its unit's id, and holds `null` for an empty
/// cell and for an unloaded one alike: the placeholder of an unloaded cell is the mount's (`setCellLoad`).
/// @param value  The field; null when the record lacks it.
/// @param column The column.
/// @param units  The registry a measure's unit is found in; null for none.
[[nodiscard]] table::Cell cellOfRecord(Value const* value, table::TableColumn const& column,
                                       table::UnitRegistry const* units) {
    using table::ColumnKind;
    if (value == nullptr || value->isNull()) {
        return std::monostate{};
    }
    bool const measure = column.info.kind == ColumnKind::Measure;
    if (std::int64_t const* const integer = value->asInt()) {
        return *integer;
    }
    if (Decimal const* const decimal = value->asDecimal()) {
        if (measure) {
            return table::Measure{.amount = decimal->value, .unit = table::kUnknownUnit};
        }
        return decimal->value;
    }
    if (bool const* const flag = value->asBool()) {
        return *flag;
    }
    if (Quantity const* const quantity = value->asQuantity()) {
        if (measure) {
            std::optional<table::UnitId> const unit = units != nullptr ? units->find(quantity->unit) : std::nullopt;
            return table::Measure{.amount = quantity->value, .unit = unit.value_or(table::kUnknownUnit)};
        }
        return quantity->value;
    }
    if (time::DateTime const* const instant = value->asTimestamp()) {
        return std::int64_t{
            std::chrono::duration_cast<std::chrono::seconds>(instant->value.time_since_epoch()).count()};
    }
    if (std::string const* const text = value->asString()) {
        if (column.info.kind == ColumnKind::Date && text->size() == 10 && (*text)[4] == '-' && (*text)[7] == '-') {
            int year = 0;
            unsigned month = 0;
            unsigned day = 0;
            char const* const at = text->data();
            bool const read = std::from_chars(at, at + 4, year).ec == std::errc{} &&
                              std::from_chars(at + 5, at + 7, month).ec == std::errc{} &&
                              std::from_chars(at + 8, at + 10, day).ec == std::errc{};
            std::chrono::year_month_day const date{std::chrono::year{year}, std::chrono::month{month},
                                                   std::chrono::day{day}};
            if (read && date.ok()) {
                return std::int64_t{std::chrono::sys_days{date}.time_since_epoch().count()};
            }
        }
        return *text;
    }
    return canonicalJson(*value);
}

/// What a rows table's column shows for a value, by the kind the value has: a rows table declares no kinds.
[[nodiscard]] table::TableColumn columnFor(Value const& value, table::ColumnDisplay const& display) {
    using table::ColumnKind;
    ColumnKind const kind = value.asInt() != nullptr         ? ColumnKind::Integer
                            : value.asDecimal() != nullptr   ? ColumnKind::Decimal
                            : value.asQuantity() != nullptr  ? ColumnKind::Decimal
                            : value.asBool() != nullptr      ? ColumnKind::Bool
                            : value.asTimestamp() != nullptr ? ColumnKind::DateTime
                                                             : ColumnKind::Text;
    return table::TableColumn{.info = {.id = {}, .kind = kind}, .display = display};
}

/// What a model-backed table's cell reads beside its column: the document's own tone and tooltip expressions.
struct CellExprs {
    ExprPtr tone;
    ExprPtr tooltip;
    std::string path;
};

}  // namespace

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
                       "`" + std::string{key} + "` is drawn on a model-backed table's cells; a table over `rows` "
                       "draws none");
            }
        }
        cells.push_back(part.get());
        fields.emplace_back(part->id);
        roles.push_back(columnRole);
    }
    // A column's display parts, read once; a rows table's columns have no other source of them.
    std::vector<std::optional<table::ColumnDisplay>> displays;
    for (DocNode const* const column : cells) {
        displays.push_back(column != nullptr && givesDisplay(*column)
                               ? std::optional{displayOf(*column, {}, [&](ExprPtr const& expr) { return once(ctx, expr); })}
                               : std::nullopt);
    }
    // A column's filter menu: a column part's values, of the kind its cells infer, shown by its display parts. A
    // derived column and one whose part says `filterable: false` have none.
    table->config.beginMount();
    table->config.filterTarget = [table, cells, displays](std::string const& id) -> std::optional<FilterTarget> {
        std::vector<LayoutColumn> const& arranged = table->config.columns;
        auto const found = std::ranges::find(arranged, id, &LayoutColumn::id);
        if (found == arranged.end() || table->source == nullptr) {
            return std::nullopt;
        }
        auto const at = static_cast<std::size_t>(found - arranged.begin());
        DocNode const* const part = at < cells.size() ? cells.at(at) : nullptr;
        if (part == nullptr || part->id.empty() || !part->flag("filterable", true)) {
            return std::nullopt;
        }
        std::span<table::ColumnInfo const> const infos = table->source->columns();
        auto const info = std::ranges::find(infos, id, &table::ColumnInfo::id);
        if (info == infos.end()) {
            return std::nullopt;
        }
        FilterTarget target;
        target.column = id;
        target.label = found->label;
        target.shown = table::TableColumn{.info = *info, .display = displays.at(at).value_or(table::ColumnDisplay{})};
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
    source.rowView = [this, ctx, &node, cellContent, registry, keyText, toneReported, spansReported, fields,
                      own](reactive::Signal<Value> const& row) -> TableRowLook {
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
            look.spans = slotProp<std::vector<RowSpan>>([this, scope, bindings = ctx.bindings, spansExpr,
                                                         spansReported, fields] {
                return rowSpansOf(evalIn(*scope, bindings.get(), spansExpr), fields, spansExpr->path, *spansReported);
            });
        }
        return look;
    };
    // A field's text as a column's default cell shows it: by the column's display parts, of the kind the value has,
    // or the value's own text. A copy writes it for every column, one with its own `cell` too.
    auto const plainText = [](Value const* value, std::optional<table::ColumnDisplay> const& display,
                              FormatContext const& format) {
        if (display) {
            Value const shown = value == nullptr ? Value{} : *value;
            table::TableColumn const kind = columnFor(shown, *display);
            table::Cell const cell = cellOfRecord(value, kind, nullptr);
            std::string text = formatCell(cell, kind, format);
            // A quantity keeps its unit after its formatted amount, as its own text writes it; a label's text stands
            // for the value whole and takes none.
            std::optional<std::string> const operand = operandOf(cell, kind);
            bool const labelled = operand && std::ranges::any_of(display->labels, [&](table::ValueLabel const& label) {
                                      return detail::cells::sameOperand(label.operand, *operand, kind.info.kind);
                                  });
            if (Quantity const* const quantity = shown.asQuantity(); quantity != nullptr && !text.empty() && !labelled) {
                auto const unit = format.units.find(quantity->unit);
                bool const named = unit != format.units.end() && !unit->second.display.empty();
                text += " " + (named ? unit->second.display : quantity->unit);
            }
            return text;
        }
        return value == nullptr ? std::string{} : textOf(*value, format);
    };
    source.cellView = [this, ctx, &node, cells, fields, roles, keyText, registry, displays, plainText](
                          std::size_t column, reactive::Signal<Value> const& row) -> Node {
        DocNode const* const part = column < cells.size() ? cells.at(column) : nullptr;
        auto const nodes = part == nullptr ? std::span<DocNodePtr const>{} : part->nodes("cell");
        if (nodes.empty()) {
            return ui::text(Text{
                .text = slotProp<std::string>([scope = ctx.scope, &row, field = fields.at(column),
                                               display = displays.at(column), plainText] {
                    return plainText(row.get().field(field), display, scope->formatting());
                }),
                .role = roles.at(column),
            });
        }
        std::string key = keyText(row.peek());
        auto const found = registry->scopes.find(key);
        std::shared_ptr<RtScope> scope = found == registry->scopes.end() ? nullptr : found->second.lock();
        if (scope == nullptr) {
            std::vector<DocNode const*> own;
            for (DocNodePtr const& cell : nodes) {
                own.push_back(cell.get());
            }
            scope = rowScope(node, ctx, row, own, std::move(key));
        }
        LowerCtx const rowCtx{.scope = scope, .bindings = ctx.bindings, .boundary = ctx.boundary};
        // The cell owns its row's scope as long as its node lives.
        auto const owner =
            std::make_shared<std::pair<Node, std::shared_ptr<RtScope>> const>(lowerAll(nodes, rowCtx), scope);
        return Node{owner, owner->first.get()};
    };
    TableOptions options;
    options.showHeader = node.flag("showHeader", true);
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
        ValueList const* const selected = tableSelection(*element).asList();
        status.counts = countsText(table->rows->counts(),
                                   selected == nullptr ? 0 : static_cast<std::int64_t>(selected->size()),
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
            auto selection = table::Selection::create(table::SelectionOptions{
                .mode = table::SelectionMode::Multiple, .pruneOnFilter = false, .tableMode = table::TableMode::Client});
            if (!selection || !selection->selectAll(table->engine->engine())) {
                return;
            }
            std::unordered_set<Key> held;
            runtime().untracked([&] {
                if (ValueList const* const items = tableSelection(*element).asList()) {
                    for (Value const& item : *items) {
                        held.insert(keyOf(item));
                    }
                }
            });
            bool const every = std::ranges::all_of(selection->keys(), [&held](table::RowId const& key) {
                return held.contains(key);
            });
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
                                                       std::string const& id) -> std::optional<HeldCell> {
        std::vector<LayoutColumn> const& arranged = table->config.columns;
        auto const found = std::ranges::find(arranged, id, &LayoutColumn::id);
        auto const at = static_cast<std::size_t>(found - arranged.begin());
        if (found == arranged.end() || at >= fields.size() || table->source == nullptr || table->engine == nullptr) {
            return std::nullopt;
        }
        std::span<table::ColumnInfo const> const infos = table->source->columns();
        auto const info = std::ranges::find(infos, id, &table::ColumnInfo::id);
        if (info == infos.end()) {
            return std::nullopt;
        }
        table::TableColumn const shown{.info = *info, .display = displays.at(at).value_or(table::ColumnDisplay{})};
        EngineRows const& rows = *table->engine;
        std::int64_t const total = rows.total().rows;
        for (std::int64_t row = 0; row < total; ++row) {
            std::optional<TableRowRef> const ref = rows.row(row);
            if (ref && ref->key == key) {
                return HeldCell{.record = ref->row,
                                .cell = cellOfRecord(ref->row.field(fields.at(at)), shown, nullptr),
                                .shown = shown,
                                .units = {},
                                .keep = nullptr};
            }
        }
        return std::nullopt;
    };
    // The engine's view holds every row, its record the row as `rows` gave it.
    table->config.gather = [table, fields, displays, plainText, headers, scope = ctx.scope](
                               std::vector<Key> keys, std::vector<std::string> const& ids, CopyDone done) {
        std::vector<std::size_t> places;
        CopyGathered gathered;
        std::vector<LayoutColumn> const& arranged = table->config.columns;
        for (std::string const& id : ids) {
            auto const found = std::ranges::find(arranged, id, &LayoutColumn::id);
            auto const place = static_cast<std::size_t>(found - arranged.begin());
            if (found != arranged.end() && place < fields.size() && place < headers->size()) {
                places.push_back(place);
                gathered.headers.push_back(headers->at(place));
            }
        }
        FormatContext const& format = scope->formatting();
        std::unordered_set<Key> wanted{keys.begin(), keys.end()};
        EngineRows const& rows = *table->engine;
        std::int64_t const total = rows.total().rows;
        for (std::int64_t at = 0; at < total && !wanted.empty(); ++at) {
            std::optional<TableRowRef> const ref = rows.row(at);
            if (!ref || wanted.erase(ref->key) == 0) {
                continue;
            }
            CopyRow copied{.viewRow = at, .cells = {}};
            for (std::size_t const place : places) {
                copied.cells.emplace_back(plainText(ref->row.field(fields.at(place)), displays.at(place), format));
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
        return besideDialogs(options.common, std::move(dialogs), [&] {
            return ui::table(std::move(columns), std::move(source), std::move(options));
        });
    }
    // With `empty`, a column holds the table and the content shown in its place while `rows` is an empty list; a
    // `null` (a query not answered yet) shows the table. The column is placed as the table would be: it takes the
    // node's visibility and layout, and every other common property stays on the table, so its test id and the
    // element id `focus` finds name the table.
    auto const listed = [scope = ctx.scope, bindings = ctx.bindings, rowsExpr] {
        ValueList const* const items = evalIn(*scope, bindings.get(), rowsExpr).asList();
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

namespace {

/// @brief A sort as a table's element value holds it, a list of `{column, direction}`, as a sort chain; an entry
///        that names no column is left out.
/// @param value The list.
/// @return The chain.
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
        chain.push_back(table::SortKey{.column = *column->asString(),
                                       .dir = descending ? table::SortDirection::Descending
                                                         : table::SortDirection::Ascending});
    }
    return chain;
}

/// @brief A sort chain as a table's element value holds it.
/// @param chain The chain.
/// @return The list of `{column, direction}`.
[[nodiscard]] Value sortValueOf(SortChain const& chain) {
    ValueList keys;
    for (table::SortKey const& key : chain) {
        ValueRecord entry;
        entry.insert_or_assign("column", Value{key.column});
        entry.insert_or_assign(
            "direction", Value{std::string{key.dir == table::SortDirection::Descending ? "desc" : "asc"}});
        keys.emplace_back(std::move(entry));
    }
    return Value{std::move(keys)};
}

/// @brief What a table's expressions read of its row: the fields named (`{"row": "id"}`), and whether the row is
///        read whole (`{"row": ""}` passed on as a value). A path read out of the whole row with `get` is neither:
///        it is the row's own read, which a binding names at run time.
struct RowReads {
    std::set<std::string, std::less<>> ids;
    bool whole = false;
};

// NOLINTNEXTLINE(misc-no-recursion): one level per nested expression.
void rowReadsOf(Expr const& expr, RowReads& out, bool readFrom = false) {
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

// NOLINTNEXTLINE(misc-no-recursion): one level per nested command list.
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

// NOLINTNEXTLINE(misc-no-recursion): one level per nested node.
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

/// @brief The status line's words for a level: its id with the first letter raised.
/// @param level The level's id.
/// @return The words.
[[nodiscard]] std::string levelWords(std::string level) {
    if (!level.empty() && level.front() >= 'a' && level.front() <= 'z') {
        level.front() = static_cast<char>(level.front() - 'a' + 'A');
    }
    return level;
}

}  // namespace


namespace {

/// @brief The column kind a column part's literal `kind` names.
/// @param name The name; empty for none.
/// @return The kind; nothing for no name, which infers the kind from the cells.
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
    auto const found = std::ranges::find(kKinds, name, &std::pair<std::string_view, table::ColumnKind>::first);
    return found == kKinds.end() ? std::nullopt : std::optional{found->second};
}

/// @brief The registry a table over `rows` reads its quantities' units from: each unit of the screen's table in the
///        dimension of its canonical unit, each canonical unit the table names but does not list, and each unit the
///        rows brought that the table does not know, which is its own canonical unit, as an expression reads it.
/// @param units The screen's units.
/// @param seen  The units the rows brought.
/// @return The registry.
[[nodiscard]] std::shared_ptr<table::UnitRegistry const> registryOfUnits(
    UnitTable const& units, std::set<std::string, std::less<>> const& seen) {
    table::UnitRegistryBuilder builder;
    auto const add = [&builder](std::string const& id, std::string const& canonical, math::Rational factor) {
        static_cast<void>(builder.add(table::UnitInfo{
            .id = id, .symbol = id, .dimension = canonical, .toBase = factor, .decimals = 0}));
    };
    for (auto const& [id, info] : units) {
        add(id, info.canonical.empty() ? id : info.canonical, info.toCanonical);
    }
    for (auto const& [id, info] : units) {
        if (!info.canonical.empty() && !units.contains(info.canonical)) {
            add(info.canonical, info.canonical, math::Rational{1, math::DecimalPlaces{0}});
        }
    }
    // A unit already added (a canonical one) is refused by the builder and keeps its first entry.
    for (std::string const& id : seen) {
        if (!units.contains(id)) {
            add(id, id, math::Rational{1, math::DecimalPlaces{0}});
        }
    }
    return std::move(builder).build();
}

/// @brief Gives a table over `rows` a new source and engine over the units known now. The columns in use, the
///        groups, the sort and whether the rows are held back carry over from the ones they replace; the caller
///        assigns the rows.
/// @param table   The table.
/// @param runtime The screen's runtime.
void startRowsEngine(RowsTable& table, reactive::Runtime& runtime) {
    auto registry = registryOfUnits(table.units, table.unitsSeen);
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
    auto engine = std::make_shared<EngineRows>(source, source->recordOf(), std::move(options));
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
        ordered.push_back(table::SortKey{.column = std::string{kGroupRunColumn}, .dir = table::SortDirection::Ascending});
    }
    ordered.insert(ordered.end(), table.sort.begin(), table.sort.end());
    static_cast<void>(engine->setSort(std::move(ordered)));
    if (!filter.empty()) {
        static_cast<void>(engine->setFilter(filter));
    }
}

/// @brief The columns a table over `rows` keeps in use: those a sort, filters and an open values list read, each once.
/// @param table  The table, whose declared groups and open list are read.
/// @param sort   The sort.
/// @param filter The filters.
/// @return The columns' ids.
[[nodiscard]] std::vector<std::string> usedColumns(RowsTable const& table, SortChain const& sort,
                                                   table::FilterSpec const& filter) {
    std::vector<std::string> out;
    auto const add = [&out](std::string const& id) {
        if (!std::ranges::contains(out, id)) {
            out.push_back(id);
        }
    };
    for (table::SortKey const& key : sort) {
        add(key.column);
    }
    for (auto const& [id, column] : filter.columns) {
        static_cast<void>(column);
        add(id);
    }
    for (table::GroupFilter const& group : filter.groups) {
        auto const declared = std::ranges::find(table.groups, group.group, &table::ColumnGroupInfo::id);
        std::vector<std::string> const members = !group.group.empty() && declared != table.groups.end()
                                                     ? declared->columns
                                                     : group.columns;
        for (std::string const& id : members) {
            add(id);
        }
    }
    for (std::string const& id : table.listedColumns) {
        add(id);
    }
    return out;
}

/// @brief Puts columns in use and takes the others out: the new ones first, so a column both read keeps its cells.
/// @param table The table.
/// @param next  The columns in use from now.
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

/// @brief A count of rows with its thousands grouped by commas, as a diagnostic writes it.
/// @param count The count.
/// @return The text.
[[nodiscard]] std::string groupedCount(std::size_t count) {
    std::string digits = std::to_string(count);
    for (auto at = static_cast<std::ptrdiff_t>(digits.size()) - 3; at > 0; at -= 3) {
        digits.insert(static_cast<std::size_t>(at), ",");
    }
    return digits;
}

/// @brief The most rows, and the most cells, a table over `rows` orders on the client without a warning.
inline constexpr std::size_t kClientRows = 20'000;
inline constexpr std::size_t kClientCells = 1'000'000;

}  // namespace

namespace {

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
                       std::function<void(std::optional<std::string> const&, std::string const&)> done) {
    struct Joined {
        int waiting = 2;
        std::optional<std::string> json;
        std::string failure;
        std::function<void(std::optional<std::string> const&, std::string const&)> done;
    };
    auto const joined = std::make_shared<Joined>(Joined{.waiting = 2, .json = {}, .failure = {}, .done = std::move(done)});
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

}  // namespace

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
    ExprPtr const by = groups.empty() ? nullptr : groups.front()->prop("by");
    auto const byOf = [&scope, bindings, by](Value const& row) {
        return evaluate(*by, ScopeEnv{scope, bindings.get(), &row});
    };
    if (by) {
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
        auto const add = [&config](std::string id, std::string label, bool locked, bool sortable,
                                   table::ColumnKind kind) {
            if (id.empty()) {
                id = "#" + std::to_string(config.columns.size());
                locked = true;
            }
            config.columns.push_back(
                LayoutColumn{.id = std::move(id), .label = std::move(label), .locked = locked, .kind = kind});
            config.sortable.push_back(sortable);
        };
        if (ExprPtr const derived = node.prop("columns")) {
            Value const list = evalIn(scope, bindings.get(), derived);
            if (ValueList const* const items = list.asList()) {
                for (Value const& item : *items) {
                    Value const* const field = item.field("id");
                    Value const* const label = item.field("label");
                    std::string id = field == nullptr ? std::string{} : textOf(*field, format);
                    std::string text = label != nullptr ? textOf(*label, format) : id;
                    add(std::move(id), std::move(text), false, false, table::ColumnKind::Text);
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
            group.label = runtime().untracked([&] { return textOf(evalIn(scope, bindings.get(), label), scope.formatting()); });
        }
        if (ExprPtr const members = part->prop("columns"); members && members->kind == ExprKind::Literal) {
            if (ValueList const* const ids = members->literal.asList()) {
                for (Value const& id : *ids) {
                    if (std::string const* const name = id.asString()) {
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
            column.info = infos[at];
            column.label = parts[at]->id;
            column.sortable = parts[at]->flag("sortable");
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
        for (std::string& id :
             usedColumns(table, document.sort.value_or(SortChain{}), document.filters.value_or(table::FilterSpec{}))) {
            if (!std::ranges::contains(next, id)) {
                next.push_back(std::move(id));
            }
        }
        useColumns(table, std::move(next));
    };
    // A bound layout is controlled, as a bound sort is: any expression but a literal `null` binds it.
    if (ExprPtr const layout = node.prop("layout");
        layout && (layout->kind != ExprKind::Literal || !layout->literal.isNull())) {
        config.bound = &scope.nodes.computed([&scope, bindings, layout] { return evalIn(scope, bindings.get(), layout); });
    }
    // Shows the page of @p view: each group's heading entry before the first of its rows the page holds, and the
    // engine's own operations when the page is the whole ungrouped view and they made it, a reset otherwise.
    auto const show = [&table](TableView const& view, table::ViewChange const* change) {
        std::vector<PageRows::Entry> entries;
        entries.reserve(view.last - view.first);
        table::Engine& engine = table.engine->engine();
        for (std::size_t index = view.first; index < view.last; ++index) {
            if (!view.runOf.empty() && (index == view.first || view.runOf.at(index) != view.runOf.at(index - 1))) {
                Value const& heading = view.runs.at(view.runOf.at(index));
                Value const* const key = heading.field(kGroupHeadingField);
                entries.push_back(PageRows::Entry{
                    .key = table::RowId{key != nullptr && key->asString() != nullptr ? *key->asString() : std::string{}},
                    .row = heading});
            }
            entries.push_back(PageRows::Entry{.key = engine.rowIdAt(index), .row = view.rows.at(index)});
        }
        bool const whole = view.runOf.empty() && view.first == 0 && view.last == view.rows.size();
        table::ViewChange reset;
        reset.ops.push_back(table::ViewOp{.kind = table::ViewOp::Kind::Reset, .first = 0, .count = 0, .to = 0});
        bool const passed = whole && change != nullptr && !change->ops.empty();
        table.shown = RowsTable::Shown{
            .version = table.version.peek(), .first = view.first, .last = view.last, .any = true};
        table.rows->show(std::move(entries), passed ? *change : reset);
    };
    table.showPage = show;
    table.heard = [this, &table](table::ViewChange const& change) {
        table.bump();
        if (table.engine->restoring()) {
            // The page stays empty while the rows are held back; the mount draws the engine's rows as placeholders.
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
    table.view = std::make_unique<reactive::Computed<TableView>>(runtime(), [this, &table] { return tableView(table); });
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
                    std::string const& id = table.node->id;
                    report(table.node->path,
                           "table " + (id.empty() ? table.node->path : id) + " holds " + groupedCount(rows.size()) +
                               " rows; a table over rows is ordered on the client up to " + groupedCount(kClientRows) +
                               " rows or " + groupedCount(kClientCells) +
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
    if (by) {
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
            chain.push_back(table::SortKey{.column = key.column->id,
                                           .dir = key.descending ? table::SortDirection::Descending
                                                                 : table::SortDirection::Ascending});
        }
    }
    // The columns the new keys read are in use before the old ones leave, so a column both read keeps its cells.
    useColumns(table, usedColumns(table, chain, table.filter));
    SortChain ordered;
    if (table.grouped) {
        ordered.push_back(table::SortKey{.column = std::string{kGroupRunColumn}, .dir = table::SortDirection::Ascending});
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
    if (!json) {
        table.states->start(*table.stateKey, document);
    }
    if (element.sortBound == nullptr && document.sort) {
        element.sort.set(sortValueOf(*document.sort));
    }
    // The filters go to the engine in a task of their own: it takes in the kinds of the columns just put in use when
    // it flushes the change that put them there, and compiles a filter against the kinds it has taken in.
    if (document.filters && !document.filters->empty()) {
        runtime().owner().post(table.scope.token().guard([this, &table, filters = *document.filters] {
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

/// @brief The cells a label takes in a monospace font: a combining mark none, an East Asian wide or full-width
///        character two, any other character one. Malformed UTF-8 counts a cell per byte.
/// @param text UTF-8 text.
/// @return The cells.
[[nodiscard]] int labelCells(std::string_view text) {
    int cells = 0;
    for (std::size_t at = 0; at < text.size();) {
        auto const lead = static_cast<unsigned char>(text[at]);
        std::size_t const length = lead < 0x80U ? 1 : (lead >> 5U) == 0x6U ? 2 : (lead >> 4U) == 0xEU ? 3
                                                    : (lead >> 3U) == 0x1EU ? 4 : 1;
        if (length == 1 || at + length > text.size()) {
            ++cells;
            ++at;
            continue;
        }
        char32_t code = lead & (0xFFU >> (length + 1));
        for (std::size_t next = 1; next < length; ++next) {
            code = (code << 6U) | (static_cast<unsigned char>(text[at + next]) & 0x3FU);
        }
        at += length;
        bool const combining = (code >= 0x0300 && code <= 0x036F) || (code >= 0x200B && code <= 0x200F) ||
                               (code >= 0xFE00 && code <= 0xFE0F);
        bool const wide = (code >= 0x1100 && code <= 0x115F) || (code >= 0x2E80 && code <= 0xA4CF) ||
                          (code >= 0xAC00 && code <= 0xD7A3) || (code >= 0xF900 && code <= 0xFAFF) ||
                          (code >= 0xFE30 && code <= 0xFE4F) || (code >= 0xFF00 && code <= 0xFF60) ||
                          (code >= 0xFFE0 && code <= 0xFFE6) || (code >= 0x1F300 && code <= 0x1FAFF) ||
                          (code >= 0x20000 && code <= 0x3FFFD);
        cells += combining ? 0 : wide ? 2 : 1;
    }
    return cells;
}

}  // namespace

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
    RecordOf recordOf = [rt = &runtime(), session = &collection, records](table::RowSnapshot const& snapshot,
                                                                          std::size_t sourceRow) {
        table::RowId key = snapshot.rowId(sourceRow);
        ValueRecord record;
        record.insert_or_assign(std::string{kCollectionKeyField}, Value{collectionKeyText(key)});
        rt->untracked([&] {
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
    if (!json) {
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
    // A table over a source measures no cell to size a column: the buttons' column is as wide as its buttons, two
    // units apart. A button is its label's cells and four units of frame, at least the eight units a framed button
    // keeps on every backend, and two units of side padding beyond, which a proportional font's padding needs.
    if (buttons) {
        forms::CollectionViewOptions const defaults{};
        int units = 0;
        auto const add = [&units](std::string_view label) {
            units += (units == 0 ? 0 : 2) + std::max(labelCells(label) + 4, 8) + 2;
        };
        if (collection.model().rowAction().has_value()) {
            add(defaults.openLabel);
        }
        for (forms::CollectionAction const& action : collection.model().actions()) {
            if (action.scope == forms::CollectionActionScope::Row) {
                add(action.label);
            }
        }
        columns.push_back(TableColumn{.label = std::string{}, .width = Sizing::fixed(units)});
    }
    config.beginMount();
    config.filterTarget = [&table, &collection](std::string const& id) -> std::optional<FilterTarget> {
        std::vector<LayoutColumn> const& arranged = table.config.columns;
        auto const found = std::ranges::find(arranged, id, &LayoutColumn::id);
        std::span<table::ColumnInfo const> const infos = collection.source()->columns();
        auto const info = std::ranges::find(infos, id, &table::ColumnInfo::id);
        if (found == arranged.end() || info == infos.end()) {
            return std::nullopt;
        }
        FilterTarget target;
        target.column = id;
        target.label = found->label;
        target.shown = table::TableColumn{.info = *info, .display = table::ColumnDisplay{}};
        target.listed = true;
        return target;
    };
    // A copy reads the engine's view, which holds every row, each cell as the session formats it.
    config.gather = [&table](std::vector<Key> keys, std::vector<std::string> const& ids, CopyDone done) {
        CopyGathered gathered;
        std::vector<std::string> read;
        for (std::string const& id : ids) {
            auto const found = std::ranges::find(table.config.columns, id, &LayoutColumn::id);
            if (found != table.config.columns.end()) {
                gathered.headers.push_back(found->label);
                read.push_back(id);
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
            for (std::string const& id : read) {
                Value const* const text = ref->row.field(id);
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
    forms::CollectionSession* const view = &collection;
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
        options.arrangement = slotProp<TableArrangement>([inner = std::move(options.arrangement), laidOut] {
            TableArrangement arranged = inner.evaluate();
            if (!arranged.visible.empty()) {
                arranged.visible.push_back(laidOut);
            }
            return arranged;
        });
    }
    return besideDialogs(options.common, std::move(dialogs), [&] {
        return ui::table(std::move(columns), std::move(source), std::move(options));
    });
}

namespace {

/// What a table over a typed list action is built from: its description, the columns whose kind its first reply
/// decides, the request member its query goes in, the records it watches, and what stops it being paged.
struct ActionTable {
    table::TableDescription description;
    std::vector<std::string> inferKinds;
    std::string queryMember;
    std::vector<std::string> records;
    /// Empty when the table can be paged.
    std::string problem;
    /// Empty when the node's `key`, if any, reads one row field.
    std::string keyProblem;
    /// What the view document's units say that is not taken: a factor of zero or less, or a unit two columns
    /// declare differently.
    std::vector<std::string> unitProblems;
};

/// One `v-columns` entry of a view document, as a table over the action reads it.
struct ViewColumn {
    std::string label;
    std::optional<table::ColumnKind> kind;
    bool hidden = false;
    /// The units the column's quantities are in: its `ExtUnits` and each `x-unitAlternatives` entry.
    std::vector<table::UnitInfo> units;
};

/// The units a `v-columns` entry declares: its `ExtUnits` (`unitAscii` the id, `unitUnicode` the symbol) as the
/// canonical unit, at `x-decimalPlaces`, and each `x-unitAlternatives` entry (`id`, `display`, `decimals`, and
/// `num`/`den`, its factor to the canonical unit, 1 when absent), all in one dimension named by the canonical unit's
/// id. An alternative whose factor is zero or less is left out and said in @p problems.
std::vector<table::UnitInfo> viewUnitsOf(Json::object_t const& fields, std::vector<std::string>& problems) {
    std::vector<table::UnitInfo> out;
    auto const member = [](Json::object_t const& object, std::string_view name) -> Json const* {
        auto const found = object.find(name);
        return found == object.end() ? nullptr : &found->second;
    };
    auto const textOf = [&member](Json::object_t const& object, std::string_view name) -> std::string const* {
        Json const* const value = member(object, name);
        return value == nullptr ? nullptr : std::get_if<std::string>(&value->data);
    };
    auto const integerOf = [&member](Json::object_t const& object, std::string_view name) -> std::optional<std::int64_t> {
        Json const* const value = member(object, name);
        if (value == nullptr) {
            return std::nullopt;
        }
        if (auto const* const signedValue = std::get_if<std::int64_t>(&value->data)) {
            return *signedValue;
        }
        if (auto const* const unsignedValue = std::get_if<std::uint64_t>(&value->data);
            unsignedValue != nullptr && *unsignedValue <= static_cast<std::uint64_t>(INT64_MAX)) {
            return static_cast<std::int64_t>(*unsignedValue);
        }
        return std::nullopt;
    };
    auto const places = [](std::optional<std::int64_t> value) {
        return static_cast<std::uint32_t>(std::clamp<std::int64_t>(value.value_or(0), 0, math::kMaxDecimalPlaces));
    };
    Json const* const ext = member(fields, "ExtUnits");
    auto const* const extFields = ext != nullptr ? std::get_if<Json::object_t>(&ext->data) : nullptr;
    std::string const* const ascii = extFields != nullptr ? textOf(*extFields, "unitAscii") : nullptr;
    if (ascii == nullptr || ascii->empty()) {
        return out;
    }
    std::string const* const unicode = textOf(*extFields, "unitUnicode");
    out.push_back(table::UnitInfo{.id = *ascii,
                                  .symbol = unicode != nullptr && !unicode->empty() ? *unicode : *ascii,
                                  .dimension = *ascii,
                                  .toBase = math::Rational{1, math::DecimalPlaces{0}},
                                  .decimals = places(integerOf(fields, "x-decimalPlaces"))});
    Json const* const alternatives = member(fields, "x-unitAlternatives");
    auto const* const entries = alternatives != nullptr ? std::get_if<Json::array_t>(&alternatives->data) : nullptr;
    for (Json const& entry : entries != nullptr ? std::span<Json const>{*entries} : std::span<Json const>{}) {
        auto const* const alternative = std::get_if<Json::object_t>(&entry.data);
        std::string const* const id = alternative != nullptr ? textOf(*alternative, "id") : nullptr;
        if (id == nullptr || id->empty() || std::ranges::contains(out, *id, &table::UnitInfo::id)) {
            continue;
        }
        auto const num = integerOf(*alternative, "num").value_or(1);
        auto const den = integerOf(*alternative, "den").value_or(1);
        if (num <= 0 || den <= 0) {
            problems.push_back("the unit `" + *id + "` has a factor of zero or less; it is not listed");
            continue;
        }
        std::string const* const display = textOf(*alternative, "display");
        out.push_back(table::UnitInfo{.id = *id,
                                      .symbol = display != nullptr && !display->empty() ? *display : *id,
                                      .dimension = *ascii,
                                      .toBase = math::Rational{math::Numerator{num}, math::Denominator{den},
                                                               math::DecimalPlaces{math::kMaxDecimalPlaces}},
                                      .decimals = places(integerOf(*alternative, "decimals"))});
    }
    return out;
}

/// Lists one column's units beside the units listed already, so the order of the columns does not matter: a column
/// one of whose units is listed joins that unit's dimension, its factors scaled through that unit; any other starts
/// its own. A unit listed already with another dimension or factor is said in @p problems and kept as first listed.
void listViewUnits(std::span<table::UnitInfo const> column, std::vector<table::UnitInfo>& listed,
                   std::vector<std::string>& problems) {
    if (column.empty()) {
        return;
    }
    std::string dimension = column.front().dimension;
    math::Rational scale{1, math::DecimalPlaces{0}};
    for (table::UnitInfo const& unit : column) {
        auto const found = std::ranges::find(listed, unit.id, &table::UnitInfo::id);
        if (found != listed.end()) {
            dimension = found->dimension;
            scale = (found->toBase / unit.toBase).value_or(scale);
            break;
        }
    }
    for (table::UnitInfo unit : column) {
        unit.dimension = dimension;
        unit.toBase = unit.toBase * scale;
        auto const found = std::ranges::find(listed, unit.id, &table::UnitInfo::id);
        if (found == listed.end()) {
            listed.push_back(std::move(unit));
        } else if (found->dimension != unit.dimension || found->toBase != unit.toBase) {
            problems.push_back("the unit `" + unit.id + "` is declared differently by two columns; the first is kept");
        }
    }
}

/// The table a typed list action's node describes. The columns are the node's `columns` parts, else the view
/// document's `v-columns`; the key is the node's `key` when it reads one row field, else the view document's
/// `v-rowKey`, else `id`; a column's kind is the part's literal `kind`, else its `v-columns` entry's, else the first
/// reply's. A catalog carries no reply schema, so nothing else can say what the rows hold.
// NOLINTNEXTLINE(readability-function-cognitive-complexity): the schema, the view document, then the columns.
ActionTable actionTableOf(DocNode const& node, Catalog const& catalog) {
    ActionTable out;
    std::string const action{node.name("action")};
    if (catalog.action) {
        if (std::optional<ActionInfo> const info = catalog.action(action)) {
            for (ActionRecordInfo const& record : info->records) {
                out.records.push_back(record.record);
            }
        }
    }
    if (catalog.schema) {
        if (std::optional<std::string> const schema = catalog.schema(action)) {
            std::vector<std::string> const members = tableQueryMembers(*schema);
            if (members.size() == 1) {
                out.queryMember = members.front();
            }
        }
    }
    if (out.queryMember.empty()) {
        out.problem = "a table over an action needs a query member marked x-table";
        out.queryMember = "query";
    }
    std::vector<std::pair<std::string, ViewColumn>> viewColumns;
    std::string viewKey;
    std::optional<std::string> const view = catalog.view ? catalog.view(action) : std::nullopt;
    if (view) {
        auto const json = parseJson(*view);
        auto const* const object = json ? std::get_if<Json::object_t>(&json->data) : nullptr;
        if (object != nullptr) {
            if (auto const key = object->find("v-rowKey"); key != object->end()) {
                if (auto const* const text = std::get_if<std::string>(&key->second.data)) {
                    viewKey = *text;
                }
            }
            auto const columns = object->find("v-columns");
            auto const* const entries =
                columns != object->end() ? std::get_if<Json::array_t>(&columns->second.data) : nullptr;
            for (Json const& entry : entries != nullptr ? std::span<Json const>{*entries} : std::span<Json const>{}) {
                auto const* const fields = std::get_if<Json::object_t>(&entry.data);
                auto const text = [fields](std::string_view name) -> std::string const* {
                    auto const found = fields->find(name);
                    return found == fields->end() ? nullptr : std::get_if<std::string>(&found->second.data);
                };
                std::string const* const field = fields != nullptr ? text("field") : nullptr;
                if (field == nullptr) {
                    continue;
                }
                ViewColumn column;
                std::string const* const label = text("label");
                column.label = label != nullptr ? *label : *field;
                if (std::string const* const kind = text("kind")) {
                    table::ColumnKind parsed{};
                    if (!glz::read_json(parsed, "\"" + *kind + "\"")) {
                        column.kind = parsed;
                    }
                }
                auto const hidden = fields->find("v-hidden");
                column.hidden = hidden != fields->end() && std::get_if<bool>(&hidden->second.data) != nullptr &&
                                *std::get_if<bool>(&hidden->second.data);
                // A column of quantities with units is read as measures, each cell naming its unit.
                column.units = viewUnitsOf(*fields, out.unitProblems);
                if (!column.units.empty() && column.kind == table::ColumnKind::Quantity) {
                    column.kind = table::ColumnKind::Measure;
                }
                viewColumns.emplace_back(*field, std::move(column));
            }
        }
    }
    auto const viewColumn = [&viewColumns](std::string_view id) -> ViewColumn const* {
        auto const found = std::ranges::find(viewColumns, id, [](auto const& entry) -> auto const& { return entry.first; });
        return found == viewColumns.end() ? nullptr : &found->second;
    };
    auto const add = [&out](std::string id, std::string label, std::optional<table::ColumnKind> kind, bool shown,
                            ViewColumn const* described) {
        listViewUnits(described != nullptr ? std::span<table::UnitInfo const>{described->units}
                                           : std::span<table::UnitInfo const>{},
                      out.description.units, out.unitProblems);
        if (!kind) {
            out.inferKinds.push_back(id);
        }
        out.description.columns.push_back(table::TableColumn{
            .info = table::ColumnInfo{.id = std::move(id),
                                      .kind = kind.value_or(table::ColumnKind::Text),
                                      .comparator = {},
                                      .format = {},
                                      .level = {}},
            .label = std::move(label),
            .group = {},
            .sortable = true,
            .filterable = true,
            .shown = shown});
    };
    if (auto const parts = node.partList("columns"); !parts.empty()) {
        for (DocNodePtr const& part : parts) {
            ViewColumn const* const described = viewColumn(part->id);
            std::optional<table::ColumnKind> kind = columnKindNamed(part->name("kind"));
            if (!kind && described != nullptr) {
                kind = described->kind;
            }
            add(part->id, described != nullptr ? described->label : part->id, kind, true, described);
        }
    } else if (view) {
        for (auto const& [id, column] : viewColumns) {
            add(id, column.label, column.kind, !column.hidden, &column);
        }
    } else if (out.problem.empty()) {
        out.problem = "a table over an action needs its columns, or a view document for the action";
    }
    ExprPtr const key = node.prop("key");
    if (key && key->kind == ExprKind::Row && key->fieldPath.size() == 1) {
        out.description.key = key->fieldPath.front();
    } else {
        out.description.key = viewKey.empty() ? std::string{"id"} : viewKey;
        if (key) {
            out.keyProblem = "the key of a table over an action is one row field, {\"row\": \"<id>\"}; `" +
                             out.description.key + "` is used";
        }
    }
    out.description.id = action;
    return out;
}

/// @brief The id of the status line's "Show only matching rows", which drops the retained rows.
constexpr std::string_view kShowMatching = "retained.showMatching";
/// @brief The ids of the status line's "Select them", which selects the rows an action could not change, and "Clear
///        marks", which drops their marks.
constexpr std::string_view kSelectFailed = "failed.select";
constexpr std::string_view kClearMarks = "failed.clear";

/// @brief Keeps a model table's selection of the view true to its rows after they changed: a selection of another
///        filter than the rows hold ends, the description's new revision having dropped a filter included, and a
///        key of `except` a reply left out of the view is dropped from it.
/// @param core    The screen.
/// @param element The table's element.
/// @param model   The table.
void fitViewSelection(ScreenCore& core, ElementState& element, ModelTable& model) {
    std::optional<TableSelection> const& current = model.viewSelection.peek();
    if (!current || !current->view) {
        return;
    }
    if (model.window->filter() != current->view->filters) {
        core.runtime().batch([&] {
            model.viewSelection.set(std::nullopt);
            if (element.bound == nullptr) {
                core.session.asUser([&] {
                    core.session.noteWrite();
                    element.selection.set(Value{ValueList{}});
                });
            }
        });
        return;
    }
    TableSelection next = *current;
    auto const dropped = std::erase_if(next.except, [&model](table::RowId const& key) { return model.rows->leftView(key); });
    if (dropped > 0) {
        model.viewSelection.set(std::move(next));
    }
}

/// @brief A model table's ear on the session's runs: the rows a run works on are busy until it settles, the rows its
///        reply could not change are marked failed, a row it gave a new key keeps what this session held by its old
///        one, and a run refused because the rows of its selection of this table's view changed is offered again, on
///        the view as it is now, through the status line.
///
/// Holds the table and its element by reference: the table owns it and removes it before either goes. The screen's
/// scope is held weakly, so the observer keeps nothing of the screen alive.
class TableRuns final : public RunObserver {
public:
    TableRuns(ScreenCore& core, ElementState& element, ModelTable& model)
        : _core{&core}, _element{&element}, _model{&model} {}

    // The rows the run's input names become busy, and their failed marks go. A run of an action that declares an
    // effect on the table's record defers the drop of selected keys that leave the view until it settles, and on a
    // table with `retainChanged` holds the refetches its change events cause.
    void started(RunStarted const& run) override {
        if (_model->rows == nullptr) {
            return;
        }
        markBusy(run);
        auto const& catalog = _core->session.options().catalog;
        std::optional<ActionInfo> const info = catalog.action ? catalog.action(run.action) : std::nullopt;
        if (!info || !info->effects) {
            return;
        }
        std::string const record = recordName();
        if (std::ranges::none_of(*info->effects, [&record](ActionEffect const& effect) {
                return effect.record && *effect.record == record;
            })) {
            return;
        }
        _changing.insert(run.run);
        if (_model->retainChanged) {
            _held.insert(run.run);
            _model->rows->holdRefetches(run.run);
        }
    }

    // A selected key that left the view is dropped from the selection and its failed mark goes; while a run that
    // changes the table is in flight the drop waits for it to settle, since its reply may name the key as a row's
    // old key, and its own change event can come before it.
    void rowsChanged() {
        if (_model->rows == nullptr || _model->viewSelection.peek()) {
            return;
        }
        std::vector<Key> left;
        for (Key const& key : selectedKeys()) {
            if (_model->rows->leftView(key)) {
                left.push_back(key);
            }
        }
        if (left.empty()) {
            return;
        }
        if (!_changing.empty()) {
            _deferred.insert(left.begin(), left.end());
            return;
        }
        drop(left);
    }

    void settled(RunSettled const& run) override {
        bool const held = _held.erase(run.run) > 0;
        if (_model->rows != nullptr) {
            _model->rows->states().settle(run.run);
            std::vector<Key> moved;
            if (run.succeeded) {
                applyReply(run, moved);
            }
            if (held) {
                keepChanged(run);
            }
            if (_changing.erase(run.run) > 0 && _changing.empty()) {
                std::vector<Key> left;
                for (Key const& key : std::exchange(_deferred, {})) {
                    if (_model->rows->leftView(key) && !std::ranges::contains(moved, key)) {
                        left.push_back(key);
                    }
                }
                drop(left);
            }
            _model->marksRevision.set(_model->marksRevision.peek() + 1);
            if (held) {
                _model->rows->releaseRefetches(run.run);
            }
        }
        if (run.refusalCode != "morph.table.viewChanged" || _model->rows == nullptr) {
            return;
        }
        auto const& catalog = _core->session.options().catalog;
        std::optional<std::string> const schema = catalog.schema ? catalog.schema(run.action) : std::nullopt;
        ValueRecord const* const body = run.body.asRecord();
        if (!schema || body == nullptr) {
            return;
        }
        std::string const record = _model->modelType + "." + _model->id;
        for (TableInput const& input : tableInputsOf(*schema)) {
            if (input.kind != TableInputKind::Selection || input.record != record) {
                continue;
            }
            auto const member = body->find(input.member);
            Value const* const view = member == body->end() ? nullptr : member->second.field("view");
            if (view == nullptr || view->isNull()) {
                continue;
            }
            // Offered only by a table whose view is the one refused: another table of the record, or this one
            // under another filter now, would run the action over rows the user did not select for it.
            table::FilterSpec refused;
            Value const* const filters = view->field("filters");
            if (filters == nullptr || glz::read_json(refused, canonicalJson(*filters)) ||
                refused != _model->window->filter()) {
                continue;
            }
            auto taken = _model->rows->selectView();
            if (!taken) {
                continue;
            }
            TableSelection again{.keys = {}, .view = std::move(*taken), .except = {}};
            if (Value const* const except = member->second.field("except"); except != nullptr) {
                if (ValueList const* const keys = except->asList()) {
                    for (Value const& key : *keys) {
                        again.except.push_back(keyOf(key));
                    }
                }
            }
            auto const json = parseJson(selectionJsonOf(again));
            auto selection = json ? valueFromJson(*json) : std::expected<Value, std::string>{Value{}};
            if (!selection) {
                return;
            }
            ValueRecord nextBody = *body;
            nextBody.insert_or_assign(input.member, std::move(*selection));
            std::int64_t const rows = _model->rows->selectedCount(again).rows;
            std::string const id = "rerun." + std::to_string(run.run);
            auto const rerun = [model = _model, again, rerun = run.rerun, nextBody = Value{std::move(nextBody)}] {
                model->viewSelection.set(again);
                rerun(nextBody);
            };
            _core->tableMessage(*_element,
                                "The rows changed since you selected them: " +
                                    render::formatCanonicalNumber(std::to_string(rows), _core->session.formatting().numeric) +
                                    (rows == 1 ? " row now. Run on these?" : " rows now. Run on these?"),
                                {{StatusEntry{.id = id, .label = "Run on these"}, rerun}});
            return;
        }
    }

private:
    // Retains the rows the run's reply says it changed, all or none: past the bound none is, and the status line
    // says so.
    void keepChanged(RunSettled const& run) {
        if (!run.succeeded) {
            return;
        }
        auto const& catalog = _core->session.options().catalog;
        std::optional<ActionInfo> const info = catalog.action ? catalog.action(run.action) : std::nullopt;
        if (!info) {
            return;
        }
        ChangedRowsValue const changed = changedRowsOf(run.reply, *info, _model->modelType + "." + _model->id);
        std::vector<table::RowId> keys;
        keys.reserve(changed.rows.size());
        for (table::ChangedRow const& row : changed.rows) {
            keys.push_back(row.key);
        }
        if (keys.empty() || _model->rows->retain(keys) != RetainResult::OverBound) {
            return;
        }
        std::string const bound = render::formatCanonicalNumber(std::to_string(table::QueryLimits{}.maxRetained),
                                                                _core->session.formatting().numeric);
        _core->tableMessage(*_element, "The changed rows are not kept in view: a table keeps at most " + bound);
    }

    [[nodiscard]] std::string recordName() const { return _model->modelType + "." + _model->id; }

    // The selected keys, while the selection is keys.
    [[nodiscard]] std::vector<Key> selectedKeys() const {
        std::vector<Key> out;
        Value const selected = _core->runtime().untracked([&] { return tableSelection(*_element); });
        if (ValueList const* const items = selected.asList()) {
            for (Value const& item : *items) {
                out.push_back(keyOf(item));
            }
        }
        return out;
    }

    // Selects @p keys as the user's selection does.
    void select(std::vector<Key> const& keys) {
        if (_model->config.selectKeys) {
            _model->config.selectKeys(keys);
        }
    }

    // Drops keys that left the view from the selection and from the failed marks.
    void drop(std::vector<Key> const& left) {
        if (left.empty()) {
            return;
        }
        _model->rows->states().clearFailed(left);
        std::vector<Key> kept = selectedKeys();
        if (std::erase_if(kept, [&left](Key const& key) { return std::ranges::contains(left, key); }) > 0) {
            select(kept);
        }
        _model->marksRevision.set(_model->marksRevision.peek() + 1);
    }

    // The keys a member of a run's input names, as a key list or a row's key.
    [[nodiscard]] static std::vector<Key> keysIn(Value const* list) {
        std::vector<Key> out;
        if (ValueList const* const items = list == nullptr ? nullptr : list->asList()) {
            for (Value const& item : *items) {
                out.push_back(keyOf(item));
            }
        }
        return out;
    }

    // The rows a run's input names for this table become busy for it: a `RowSelection` member's keys, or every row
    // of its view but `except`, and a `RowRef` member's row. Their failed marks go.
    void markBusy(RunStarted const& run) {
        auto const& catalog = _core->session.options().catalog;
        std::optional<std::string> const schema = catalog.schema ? catalog.schema(run.action) : std::nullopt;
        ValueRecord const* const body = run.body.asRecord();
        if (!schema || body == nullptr) {
            return;
        }
        std::string const record = recordName();
        RowStates& states = _model->rows->states();
        bool marked = false;
        for (TableInput const& input : tableInputsOf(*schema)) {
            auto const member = body->find(input.member);
            if (input.record != record || member == body->end() || member->second.isNull()) {
                continue;
            }
            Value const& named = member->second;
            if (input.kind == TableInputKind::Row) {
                std::vector<Key> const one{keyOf(named)};
                states.clearFailed(one);
                states.busyKeys(run.run, one);
            } else if (Value const* const view = named.field("view"); view != nullptr && !view->isNull()) {
                std::vector<Key> except = keysIn(named.field("except"));
                std::int64_t const count =
                    _model->rows->selectedCount(TableSelection{.keys = {}, .view = ViewSelection{}, .except = except})
                        .rows;
                std::vector<Key> marks = states.failedKeys();
                std::erase_if(marks, [&except](Key const& key) { return std::ranges::contains(except, key); });
                states.clearFailed(marks);
                states.busyView(run.run, std::move(except), count);
            } else {
                std::vector<Key> keys = keysIn(named.field("keys"));
                states.clearFailed(keys);
                states.busyKeys(run.run, std::move(keys));
            }
            marked = true;
        }
        if (marked) {
            _model->marksRevision.set(_model->marksRevision.peek() + 1);
        }
    }

    // Whether this session holds anything by a key: the selection, the focus, a retained row, a busy or failed state,
    // or a drop waiting for the runs to settle.
    [[nodiscard]] bool holds(Key const& key) const {
        RowStates const& states = _model->rows->states();
        std::optional<TableSelection> const& view = _model->viewSelection.peek();
        bool const selected = view ? std::ranges::contains(view->except, key)
                                   : std::ranges::contains(selectedKeys(), key);
        return selected || (_model->config.focused && _model->config.focused->key == key) ||
               std::ranges::contains(_model->window->retained(), key) || states.isBusy(key) ||
               states.failure(key).has_value() || _deferred.contains(key);
    }

    // A reply's failed rows are marked, and each changed row with a `from` this session holds moves what it holds
    // from that key to the new one: the selection, the retained rows and the states. Every `from` goes to the mount,
    // which moves its focus and highlight by its own keys: focus the action's own event already moved off the old row
    // is held only there. @p moved gets each old key moved here.
    void applyReply(RunSettled const& run, std::vector<Key>& moved) {
        auto const& catalog = _core->session.options().catalog;
        std::optional<ActionInfo> const info = catalog.action ? catalog.action(run.action) : std::nullopt;
        if (!info) {
            return;
        }
        ChangedRowsValue const changed = changedRowsOf(run.reply, *info, recordName());
        _model->rows->states().fail(changed.failed);
        std::vector<std::pair<Key, Key>> moves;
        std::vector<std::pair<Key, Key>> told;
        for (table::ChangedRow const& row : changed.rows) {
            if (!row.from || *row.from == row.key) {
                continue;
            }
            told.emplace_back(*row.from, row.key);
            if (holds(*row.from)) {
                moves.emplace_back(*row.from, row.key);
            }
        }
        // The mount is told after the selection is rewritten: a handler reading both in one turn sees them agree.
        auto const tell = [this, &told] {
            if (!told.empty()) {
                _model->keyMoves.set(
                    KeyMoves{.generation = _model->keyMoves.peek().generation + 1, .moves = std::move(told)});
            }
        };
        if (moves.empty()) {
            tell();
            return;
        }
        std::optional<TableSelection> view = _model->viewSelection.peek();
        std::vector<Key> keys = view ? std::vector<Key>{} : selectedKeys();
        bool reselect = false;
        for (auto const& [from, to] : moves) {
            moved.push_back(from);
            _deferred.erase(from);
            _model->rows->moveKey(from, to);
            if (view) {
                // A key left out of the view's selection leaves its new key out; a selected one selects it.
                if (auto const out = std::ranges::find(view->except, from); out != view->except.end()) {
                    if (std::ranges::contains(view->except, to)) {
                        view->except.erase(out);
                    } else {
                        *out = to;
                    }
                } else {
                    std::erase(view->except, to);
                }
            } else if (auto const at = std::ranges::find(keys, from); at != keys.end()) {
                reselect = true;
                if (std::ranges::contains(keys, to)) {
                    keys.erase(at);
                } else {
                    *at = to;
                }
            }
        }
        if (view) {
            _model->viewSelection.set(std::move(view));
        } else if (reselect) {
            select(keys);
        }
        tell();
    }

    ScreenCore* _core;
    ElementState* _element;
    ModelTable* _model;
    // The runs holding the rows' refetches.
    std::set<std::uint64_t> _held;
    // The runs in flight of actions that change the table's record, and the selected keys that left the view while
    // one was: dropped when the last settles, unless a reply named them as a row's old key.
    std::set<std::uint64_t> _changing;
    std::set<Key> _deferred;
};

}  // namespace

void ScreenCore::wireModelTable(ElementState& element) {
    DocNode const& node = *element.node;
    RtScope& scope = *element.scope;
    auto made = std::make_unique<ModelTable>(runtime());
    ModelTable& model = *made;
    element.modelTable = std::move(made);
    model.core = this;
    model.element = &element;
    modelTables.push_back(&element);
    NameRef const* const named = scope.find(node.name("model"));
    ModelAlias* const alias = named != nullptr && named->category == NameCategory::Model ? named->model : nullptr;
    if (alias == nullptr) {
        report(node.path, "the table's `model` names no model alias of this scope");
        return;
    }
    std::string const action{node.name("action")};
    model.id = action.empty() ? std::string{node.name("table")} : action;
    model.modelType = alias->decl->type;
    model.statusShown = node.flag("status", true);
    model.retainChanged = node.flag("retainChanged");
    TableConfig& config = model.config;
    config.element = &element;
    auto const [allowed, menus] = configurableOf(node);
    config.allowed = allowed;
    config.menus = menus;
    // The bound the server's `limits` set, which the description carries.
    config.maxSortKeys = [&model] {
        table::TableDescription const* const description = model.window->description();
        return description == nullptr ? std::size_t{8} : std::size_t{description->capabilities.maxSortKeys};
    };
    config.message = [this, &element](std::string text) { tableMessage(element, std::move(text)); };
    config.touch = [this, &element] { tableGesture(element); };
    config.save = [&model](table::StateDocument const& document, StateParts parts) {
        if (model.stateKey) {
            model.states->changed(*model.stateKey, document, parts);
        }
    };
    // The filters the rows hold; the user's go to the rows through `ObservedRows`, which marks them the user's.
    config.filters = [&model] {
        static_cast<void>(model.version.get());
        return model.filter;
    };
    config.setFilters = [this, &model, &element](table::FilterSpec const& filter) {
        if (auto const set = model.rows->setFilter(filter); !set) {
            tableMessage(element, "The filter is refused: " + set.error().message);
            return false;
        }
        model.filter = filter;
        model.heldFilter.reset();
        model.bump();
        return true;
    };
    config.rows = [&model]() -> TableRows* { return model.rows.get(); };
    config.declaredGroups = [&model] {
        table::TableDescription const* const description = model.window->description();
        return description == nullptr ? std::vector<table::ColumnGroupInfo>{} : description->groups;
    };
    config.description = [&model]() -> std::optional<table::TableDescription> {
        table::TableDescription const* const description = model.window->description();
        return description == nullptr ? std::nullopt : std::optional<table::TableDescription>{*description};
    };
    // A bound layout is controlled, as a bound sort is: any expression but a literal `null` binds it.
    if (ExprPtr const layout = node.prop("layout");
        layout && (layout->kind != ExprKind::Literal || !layout->literal.isNull())) {
        config.bound = &scope.nodes.computed([&scope, layout] { return evalIn(scope, nullptr, layout); });
    }
    std::string qualifier = _doc->id;
    bool persists = true;
    if (ExprPtr const persist = node.prop("persist"); persist && persist->kind == ExprKind::Literal) {
        if (bool const* const flag = persist->literal.asBool()) {
            persists = *flag;
        } else if (std::string const* const name = persist->literal.asString()) {
            qualifier = *name;
        }
    }
    model.states = session.tableStates();
    if (persists && model.states != nullptr) {
        model.stateKey = table::TableStateKey{.application = session.options().application,
                                              .model = model.modelType,
                                              .table = model.id,
                                              .qualifier = std::move(qualifier),
                                              .part = "state"};
    }
    // A model's table is described by its `describe` action; a typed list action by the client, from the node's
    // columns or the action's view document.
    std::optional<ActionTable> typed;
    if (action.empty()) {
        model.describeDecl = QueryDecl{.name = {},
                                       .path = node.path,
                                       .model = std::string{node.name("model")},
                                       .action = "Table." + model.id + ".describe",
                                       .evaluate = false,
                                       .body = nullptr,
                                       .when = nullptr,
                                       .debounce = 0,
                                       .refreshEvery = 0,
                                       .staleTime = 0};
        model.describe = std::make_unique<QueryRuntime>(model.describeDecl, scope, alias);
    } else {
        typed = actionTableOf(node, session.options().catalog);
        if (!typed->problem.empty()) {
            report(node.path, typed->problem);
        }
        if (!typed->keyProblem.empty()) {
            report(pointerAppend(node.path, "key"), typed->keyProblem);
        }
        for (std::string const& problem : typed->unitProblems) {
            report(node.path, problem);
        }
    }
    Value const body = node.prop("body") ? runtime().untracked([&] { return evalIn(scope, nullptr, node.prop("body")); })
                                         : Value{};
    // The table owns these members of the query; a body that gives one has it replaced, which is said, since the
    // body is the query's fixed part and the author may have meant it to restrict the rows.
    if (ValueRecord const* const fixed = body.asRecord()) {
        if (auto const query = fixed->find("query"); query != fixed->end() && query->second.asRecord() != nullptr) {
            for (std::string_view const owned : {"sort", "filters", "page", "columns", "keys", "retry"}) {
                if (query->second.asRecord()->contains(owned)) {
                    report(pointerAppend(pointerAppend(pointerAppend(node.path, "body"), "query"), owned),
                           "`" + std::string{owned} + "` is the table's own; the body's is not used");
                }
            }
        }
    }
    model.waiting = model.stateKey.has_value();
    model.window = std::make_shared<WindowRows>(
        session, WindowRowsOptions{.channel = channelOf(*alias, *this),
                                   .table = typed ? std::string{} : model.id,
                                   .action = action,
                                   .queryMember = typed ? typed->queryMember : std::string{},
                                   .inferKinds = typed ? typed->inferKinds : std::vector<std::string>{},
                                   .records = typed ? typed->records
                                                    : std::vector<std::string>{model.modelType + "." + model.id},
                                   .body = body,
                                   .pageSize = 200,
                                   .maxPages = 8,
                                   .maxKeys = 1000,
                                   .maxColumns = 96,
                                   .restoring = model.waiting});
    element.tableWindow = model.window;
    model.rows = std::make_shared<ObservedRows>(
        model.window, model.scope.token().guard([this, &model, &element] {
            model.bump();
            model.marksRevision.set(model.marksRevision.peek() + 1);
            fitViewSelection(*this, element, model);
            if (model.runs != nullptr) {
                static_cast<TableRuns&>(*model.runs).rowsChanged();
            }
        }),
        model.scope.token().guard([this, &element] { tableGesture(element); }),
        model.scope.token().guard([&model] { model.resets.set(model.resets.peek() + 1); }),
        model.scope.token().guard([this, &model, &element] {
            model.userFiltered = true;
            tableGesture(element);
        }));
    element.tableRows = model.rows;
    model.runs = std::make_unique<TableRuns>(*this, element, model);
    session.addRunObserver(model.runs.get());
    if (model.describe != nullptr) {
        model.window->onStaleDescription(model.scope.token().guard([&model] { model.describe->invalidate(); }));
    }
    // Heard while the first reply is read: the filter held meanwhile goes once that reply is taken.
    model.window->onInferred(model.scope.token().guard([this, &model, &element] {
        runtime().owner().post(model.scope.token().guard([this, &element] { inferredTable(element); }));
    }));
    // Called inside a binding, where no signal may be written: the misses of one turn are fetched by one task.
    model.onMissing = [this, &model](std::string_view field) {
        bool const first = model.missing.empty();
        model.missing.emplace(field);
        if (!first) {
            return;
        }
        // A field no column of the description has is dropped by the rows, which never name it to the server.
        runtime().owner().post(model.scope.token().guard([&model] {
            std::set<std::string, std::less<>> const missed = std::exchange(model.missing, {});
            model.rows->fetchAlso(std::vector<std::string>{missed.begin(), missed.end()});
        }));
    };
    // A literal sort is the default a stored one replaces; any other expression binds it.
    if (ExprPtr const sort = node.prop("sort"); sort && !(sort->kind == ExprKind::Literal && sort->literal.isNull())) {
        if (sort->kind == ExprKind::Literal) {
            element.sort.set(sort->literal);
        } else {
            element.sortBound = &scope.nodes.computed([&scope, sort] { return evalIn(scope, nullptr, sort); });
        }
    }
    if (ExprPtr const selection = node.prop("selection");
        selection && (selection->kind != ExprKind::Literal || !selection->literal.isNull())) {
        element.bound = &scope.nodes.computed([&scope, selection] { return evalIn(scope, nullptr, selection); });
    }
    // A literal filter is the default a stored one replaces; an expression binds the filters.
    if (std::string_view const literal = node.name("filters"); !literal.empty()) {
        table::FilterSpec filter;
        if (!glz::read_json(filter, literal)) {
            if (model.window->inferring()) {
                model.heldFilter = std::move(filter);
            } else {
                model.filter = filter;
                static_cast<void>(model.window->setFilter(std::move(filter)));
            }
        }
    } else if (ExprPtr const filters = node.prop("filters")) {
        model.filtersBound = true;
        config.filtersBound = true;
        scope.nodes.effect([this, &scope, &element, filters] {
            Value const value = evalIn(scope, nullptr, filters);
            runtime().untracked([&] {
                ModelTable& table = *element.modelTable;
                table::FilterSpec filter;
                if (!value.isNull()) {
                    std::string const text = canonicalJson(value);
                    if (auto const failed = glz::read_json(filter, text)) {
                        report(pointerAppend(element.node->path, "filters"),
                               "`filters` is not a filter: " + glz::format_error(failed, text));
                        return;
                    }
                }
                if (table.window->inferring()) {
                    table.heldFilter = std::move(filter);
                    table.heldStored = false;
                    return;
                }
                if (filter == table.filter) {
                    return;
                }
                if (auto const set = table.window->setFilter(filter); !set) {
                    report(pointerAppend(element.node->path, "filters"), "the filter is refused: " + set.error().message);
                    return;
                }
                table.filter = std::move(filter);
            });
        });
    }
    if (model.describe != nullptr) {
        model.describe->start();
    }
    // The sort the table shows: the bound one, or the table's own, which a header click and a stored state set.
    scope.nodes.effect([this, &element] {
        Value const value = tableSort(element);
        runtime().untracked([&] {
            ModelTable& table = *element.modelTable;
            SortChain chain = sortChainOf(value);
            if (chain == table.sort) {
                return;
            }
            if (auto const set = table.window->setSort(chain); !set) {
                report(pointerAppend(element.node->path, "sort"), "the sort is refused: " + set.error().message);
                return;
            }
            table.sort = std::move(chain);
        });
    });
    if (typed) {
        model.window->setDescription(std::move(typed->description));
        if (table::TableDescription const* const now = model.window->description()) {
            model.keyColumn = now->key;
            for (table::TableColumn const& column : now->columns) {
                model.described.insert(column.info.id);
            }
        }
        modelLayoutColumns(element);
        model.bump();
    } else {
        scope.nodes.effect([this, &element] {
            Value const described = element.modelTable->describe->value();
            runtime().untracked([&] { describedTable(element, described); });
        });
    }
    // A highlight: each non-null key, once a description is held, is located in the view by a keyed fetch naming no
    // columns, which stops the one an earlier key still has in flight; a key the view lacks, or null, marks nothing.
    // A typed list action's description is held from here on.
    if (ExprPtr const highlight = node.prop("highlightKey")) {
        scope.nodes.effect([this, &scope, &element, highlight, typedTable = typed.has_value()] {
            Value const key = evalIn(scope, nullptr, highlight);
            bool const described = typedTable || !element.modelTable->revision.get().empty();
            // A new query (sort, filter) moves the row: located again, the mark cleared meanwhile.
            static_cast<void>(element.modelTable->resets.get());
            runtime().untracked([&] {
                ModelTable& table = *element.modelTable;
                if (key.isNull() || !described) {
                    table.window->stopLocating();
                    table.highlight.set(std::nullopt);
                    return;
                }
                Key const shown = keyOf(key);
                table.highlight.set(std::nullopt);
                table.window->locate(std::visit([](auto const& value) { return table::RowId{value}; }, shown),
                                     [&table, shown](std::optional<std::int64_t> index) {
                                         table.highlight.set(index && *index >= 0
                                                                 ? std::optional<TableHighlight>{TableHighlight{
                                                                       .key = shown,
                                                                       .row = static_cast<std::size_t>(*index)}}
                                                                 : std::nullopt);
                                     });
            });
        });
    }
    if (!model.stateKey) {
        return;
    }
    // The first query waits for the stored state, at most `tableStateTimeout`; a state that comes later is applied
    // unless the user changed the table meanwhile.
    if (reactive::Scheduler* const scheduler = session.options().scheduler) {
        model.loadTimer = scheduler->after(session.options().tableStateTimeout,
                                           model.scope.token().guard([&model] { model.release(); }));
    }
    model.views = std::make_unique<TableViews>(*model.states, *model.stateKey, runtime().owner());
    model.config.views = model.views.get();
    loadStateAndViews(*model.states, *model.stateKey, *model.views, model.scope,
                      [this, &element](std::optional<std::string> const& json, std::string const& failure) {
                          storedTableState(element, json, failure);
                      });
}

void ScreenCore::storedTableState(ElementState& element, std::optional<std::string> const& json,
                                  std::string const& failure) {
    ModelTable& model = *element.modelTable;
    DocNode const& node = *element.node;
    bool const late = !model.waiting;
    if (!failure.empty()) {
        report(node.path, "the stored state of table `" + node.id + "` could not be loaded: " + failure);
        model.release();
        return;
    }
    std::optional<table::StateDocument> stored;
    if (json) {
        auto read = table::readState(*json);
        if (!read) {
            report(node.path, "the stored state of table `" + node.id + "` is not used: " + read.error());
            model.release();
            return;
        }
        stored = std::move(*read);
    }
    // The default view stands in for a state never stored, and its sort and filters for the stored ones when the
    // user starts from it.
    std::optional<table::StateDocument> start =
        model.views != nullptr ? stateAtMount(std::move(stored), model.views->own()) : std::move(stored);
    if (!start) {
        model.release();
        return;
    }
    if (late && model.touched) {
        return;
    }
    table::StateDocument document = std::move(*start);
    // Kinds still to be inferred are placeholders: the stored filters are fitted to the kinds the first reply gives.
    if (model.window->inferring()) {
        if (!model.filtersBound && document.filters) {
            model.heldFilter = std::exchange(document.filters, std::nullopt);
            model.heldStored = true;
        }
    }
    if (table::TableDescription const* const description = model.window->description()) {
        table::FitReport fitted;
        document = table::fitState(std::move(document), *description, fitted);
        for (table::FitDrop const& drop : fitted.dropped) {
            if (drop.what == table::FitDrop::What::Filter || drop.what == table::FitDrop::What::GroupFilter) {
                report(node.path, "the stored filter on `" + drop.id + "` is not applied: the table has no such column");
            }
        }
    }
    // A table started from the default view, with nothing stored, holds it as its state: the next change saves on
    // top of it rather than over nothing.
    if (!json) {
        model.states->start(*model.stateKey, document);
    }
    if (element.sortBound == nullptr && document.sort) {
        if (model.window->setSort(*document.sort)) {
            model.sort = *document.sort;
            element.sort.set(sortValueOf(model.sort));
        }
    }
    if (!model.filtersBound && document.filters) {
        if (model.window->setFilter(*document.filters)) {
            model.filter = *document.filters;
            model.heldFilter.reset();
        }
    }
    // A bound layout is the document's: the stored one is neither applied nor saved over it.
    bool const arranged = document.layout && model.config.bound == nullptr;
    if (arranged) {
        model.config.held = std::move(document.layout);
        model.config.version.set(model.config.version.peek() + 1);
    }
    if (document.view) {
        model.config.savedView = std::move(document.view);
        model.config.viewShared = false;
    }
    tablePoints(element);
    if (late || !arranged) {
        model.release();
        return;
    }
    // A mounted table takes the new arrangement in a pass of its own, which tells the rows the columns it shows: the
    // first query goes after it, so it names those columns.
    runtime().owner().post(model.scope.token().guard([&model] { model.release(); }));
}

void ScreenCore::describedTable(ElementState& element, Value const& described) {
    ModelTable& model = *element.modelTable;
    if (described.isNull()) {
        return;
    }
    table::TableDescription const* const held = model.window->description();
    std::string const before = held != nullptr ? held->revision : std::string{};
    // The filters a new revision drops are said once, at the table; a dropped sort key is not.
    if (held != nullptr) {
        table::TableDescription next;
        std::string const text = canonicalJson(described);
        if (!glz::read<glz::opts{.error_on_unknown_keys = false}>(next, text) && next.revision != before) {
            table::FitReport fitted;
            static_cast<void>(table::fitState(
                table::StateDocument{.version = table::kStateVersion,
                                     .layout = {},
                                     .sort = model.sort,
                                     .filters = model.filter,
                                     .view = {},
                                     .carried = {}},
                next, fitted));
            for (table::FitDrop const& drop : fitted.dropped) {
                if (drop.what == table::FitDrop::What::Filter || drop.what == table::FitDrop::What::GroupFilter ||
                    drop.what == table::FitDrop::What::Unit) {
                    report(element.node->path,
                           "the filter on `" + drop.id + "` is dropped: the table's new description has no such column");
                }
            }
        }
    }
    model.window->setDescription(described);
    table::TableDescription const* const now = model.window->description();
    if (now == nullptr) {
        model.bump();
        return;
    }
    model.keyColumn = now->key;
    model.described.clear();
    for (table::TableColumn const& column : now->columns) {
        model.described.insert(column.info.id);
    }
    modelLayoutColumns(element);
    // The fitted sort and filters are what the rows hold now.
    table::FitReport ignored;
    table::StateDocument const fitted = table::fitState(
        table::StateDocument{
            .version = table::kStateVersion, .layout = {}, .sort = model.sort, .filters = model.filter, .view = {},
            .carried = {}},
        *now, ignored);
    model.sort = fitted.sort.value_or(SortChain{});
    model.filter = fitted.filters.value_or(table::FilterSpec{});
    if (element.sortBound == nullptr) {
        element.sort.set(sortValueOf(model.sort));
    }
    model.revision.set(now->revision);
    model.bump();
}

void ScreenCore::inferredTable(ElementState& element) {
    ModelTable& model = *element.modelTable;
    table::TableDescription const* const description = model.window->description();
    if (description == nullptr) {
        return;
    }
    // The held filter, unless the user filtered meanwhile: a bound filter is the document's and always wins; a
    // literal or a stored one gives way only to a filter the user set, since a stored state held here arrived in
    // time and a sort or a scroll since does not undo it. Without one, the filter the rows hold, which may have been
    // compiled against the placeholders, is fitted again.
    std::optional<table::FilterSpec> held = std::exchange(model.heldFilter, std::nullopt);
    bool const stored = std::exchange(model.heldStored, false);
    bool const yields = !model.filtersBound && model.userFiltered;
    if (held && yields) {
        held.reset();
    }
    table::FilterSpec filter = held ? *held : model.window->filter();
    bool const fromStore = held && stored;
    table::FitReport fitted;
    table::StateDocument const document = table::fitState(
        table::StateDocument{
            .version = table::kStateVersion, .layout = {}, .sort = {}, .filters = filter, .view = {}, .carried = {}},
        *description, fitted);
    for (table::FitDrop const& drop : fitted.dropped) {
        if (drop.what == table::FitDrop::What::Filter || drop.what == table::FitDrop::What::GroupFilter) {
            report(element.node->path, std::string{fromStore ? "the stored filter" : "the filter"} + " on `" +
                                           drop.id + "` is not applied: " +
                                           (drop.carried ? "the table has no such column"
                                                         : "it does not apply to the column's kind"));
        }
    }
    filter = document.filters.value_or(table::FilterSpec{});
    if (filter == model.window->filter()) {
        model.filter = std::move(filter);
        return;
    }
    if (auto const set = model.window->setFilter(filter); !set) {
        report(pointerAppend(element.node->path, "filters"), "the filter is refused: " + set.error().message);
        return;
    }
    model.filter = std::move(filter);
}

void ScreenCore::tableGesture(ElementState& element) {
    if (element.rowsTable != nullptr) {
        element.rowsTable->touched = true;
        tableMessage(element, {});
        return;
    }
    if (element.modelTable == nullptr) {
        return;
    }
    element.modelTable->touched = true;
    tableMessage(element, {});
}

void ScreenCore::rowsTableMessage(RowsTable& rows, std::string text) {
    if (text.empty()) {
        rows.messageTimer.cancel();
        rows.message.set({});
        return;
    }
    if (rows.status == false) {
        if (host != nullptr) {
            static_cast<void>(host->notify(text, "warning"));
        }
        return;
    }
    rows.message.set(text);
    rows.messageTimer.cancel();
    if (reactive::Scheduler* const scheduler = session.options().scheduler) {
        rows.messageTimer = scheduler->after(
            kStatusMessageHold, rows.scope.token().guard([&rows, shown = std::move(text)] {
                if (rows.message.peek() == shown) {
                    rows.message.set({});
                }
            }));
    }
}

void ScreenCore::tableMessage(ElementState& table, std::string text) {
    if (RowsTable* const rows = table.rowsTable.get()) {
        rowsTableMessage(*rows, std::move(text));
        return;
    }
    if (GridTable* const grid = table.gridTable.get()) {
        grid->entryHandlers.clear();
        grid->entries.set({});
        grid->messageTimer.cancel();
        grid->message.set(text);
        if (reactive::Scheduler* const scheduler = session.options().scheduler; scheduler != nullptr && !text.empty()) {
            grid->messageTimer = scheduler->after(kStatusMessageHold, grid->scope.token().guard([grid, shown = text] {
                if (grid->message.peek() == shown) {
                    grid->message.set({});
                    grid->entries.set({});
                    grid->entryHandlers.clear();
                }
            }));
        }
        return;
    }
    ModelTable* const model = table.modelTable.get();
    if (model == nullptr) {
        return;
    }
    model->entryHandlers.clear();
    model->entries.set({});
    if (text.empty()) {
        model->messageTimer.cancel();
        model->message.set({});
        return;
    }
    if (!model->statusShown) {
        if (host != nullptr) {
            static_cast<void>(host->notify(text, "warning"));
        }
        return;
    }
    model->message.set(text);
    model->messageTimer.cancel();
    if (reactive::Scheduler* const scheduler = session.options().scheduler) {
        model->messageTimer = scheduler->after(
            kStatusMessageHold, model->scope.token().guard([model, shown = std::move(text)] {
                if (model->message.peek() == shown) {
                    model->message.set({});
                    model->entries.set({});
                    model->entryHandlers.clear();
                }
            }));
    }
}

void ScreenCore::tableMessage(ElementState& table, std::string text,
                              std::vector<std::pair<StatusEntry, std::function<void()>>> entries) {
    tableMessage(table, std::move(text));
    if (GridTable* const grid = table.gridTable.get()) {
        if (grid->message.peek().empty()) {
            return;
        }
        std::vector<StatusEntry> shown;
        for (auto& [entry, handler] : entries) {
            grid->entryHandlers.insert_or_assign(entry.id, std::move(handler));
            shown.push_back(std::move(entry));
        }
        grid->entries.set(std::move(shown));
        return;
    }
    ModelTable* const model = table.modelTable.get();
    if (model == nullptr || !model->statusShown || model->message.peek().empty()) {
        return;
    }
    std::vector<StatusEntry> shown;
    for (auto& [entry, handler] : entries) {
        model->entryHandlers.insert_or_assign(entry.id, std::move(handler));
        shown.push_back(std::move(entry));
    }
    model->entries.set(std::move(shown));
}

void ScreenCore::tablePoints(ElementState& table) {
    ModelTable const* const model = table.modelTable.get();
    // A table over `rows` that keeps a state shows its layout at a `columns` point and its saved view's name at a
    // `saved` one, as a model-backed table does.
    RowsTable const* const rows = model == nullptr && table.rowsTable != nullptr && table.rowsTable->stateKey &&
                                          table.node != nullptr && table.node->kind == "table"
                                      ? table.rowsTable.get()
                                      : nullptr;
    if ((model == nullptr && rows == nullptr) || table.node == nullptr) {
        return;
    }
    TableConfig const& config = model != nullptr ? model->config : rows->config;
    ValueRecord next = prefs.peek();
    bool changed = false;
    for (auto const& [name, point] : doc().customize) {
        // A point without `of` names no table, an id-less one included.
        if (point.of.empty() || point.of != table.node->id || (point.kind != "columns" && point.kind != "saved")) {
            continue;
        }
        Value value;
        if (point.kind == "saved") {
            value = config.savedView ? Value{*config.savedView} : Value{};
        } else if (config.held) {
            auto const json = parseJson(glz::write_json(*config.held).value_or("null"));
            auto read = json ? valueFromJson(*json) : std::expected<Value, std::string>{Value{}};
            value = read ? std::move(*read) : Value{};
        } else {
            continue;
        }
        if (auto const held = next.find(name); held == next.end() || held->second != value) {
            next.insert_or_assign(name, std::move(value));
            changed = true;
        }
    }
    if (changed) {
        prefs.set(std::move(next));
    }
}

std::optional<bool> ScreenCore::tablePoint(CustomizePoint const& point, Value const& value) {
    if (point.kind != "columns" && point.kind != "saved") {
        return std::nullopt;
    }
    // A point without `of` names no table. A model-backed table, and a table over `rows` that keeps a state, hold both
    // points' values in that state.
    if (point.of.empty()) {
        return std::nullopt;
    }
    auto const found = std::ranges::find_if(modelTables, [&point](ElementState const* element) {
        if (element->node == nullptr || element->node->id != point.of) {
            return false;
        }
        return element->modelTable != nullptr ||
               (element->rowsTable != nullptr && element->rowsTable->stateKey && element->node->kind == "table");
    });
    if (found == modelTables.end()) {
        return std::nullopt;
    }
    ModelTable* const model = (*found)->modelTable.get();
    RowsTable* const rows = model == nullptr ? (*found)->rowsTable.get() : nullptr;
    TableConfig& config = model != nullptr ? model->config : rows->config;
    table::StateDocument changed;
    StateParts part = StateParts::Layout;
    if (point.kind == "saved") {
        std::string const* const name = value.asString();
        if (name == nullptr && !value.isNull()) {
            return false;
        }
        config.savedView = name == nullptr ? std::nullopt : std::optional<std::string>{*name};
        config.viewShared = false;
        changed.view = config.savedView;
        part = StateParts::View;
    } else {
        table::ColumnLayoutValue layout;
        if (glz::read_json(layout, canonicalJson(value))) {
            return false;
        }
        config.held = std::move(layout);
        config.version.set(config.version.peek() + 1);
        changed.layout = config.held;
    }
    std::optional<table::TableStateKey> const& key = model != nullptr ? model->stateKey : rows->stateKey;
    TableStates* const states = model != nullptr ? model->states : rows->states;
    (model != nullptr ? model->touched : rows->touched) = true;
    if (key) {
        states->changed(*key, changed, part);
    }
    return true;
}

std::size_t ScreenCore::tableRowScopes(ElementState const& table) {
    return table.modelTable == nullptr ? 0 : table.modelTable->rowScopes.size();
}

void ScreenCore::tableSaveFailed(table::TableStateKey const& key, table::SaveResult const& result) {
    std::string const text = result.reason.empty()
                                 ? std::string{"The table's arrangement could not be saved"}
                                 : "The table's arrangement could not be saved: " + result.reason;
    std::vector<ElementState*> tables;
    for (ElementState* const element : modelTables) {
        std::optional<table::TableStateKey> const* const held =
            element->modelTable != nullptr ? &element->modelTable->stateKey
            : element->rowsTable != nullptr ? &element->rowsTable->stateKey
                                            : nullptr;
        if (held != nullptr && *held == key) {
            tables.push_back(element);
        }
    }
    for (ElementState* const table : tables) {
        tableMessage(*table, text);
    }
}

namespace {

/// The description's columns a model-backed table's `Table::columns` are, in order: those its column parts name, or
/// without parts every column but the companions, which travel with their columns and are never one of them.
[[nodiscard]] std::vector<table::TableColumn const*> arrangedColumns(DocNode const& node,
                                                                    table::TableDescription const& description) {
    std::vector<std::string> const companions = table::companionColumns(description);
    std::vector<table::TableColumn const*> out;
    if (auto const parts = node.partList("columns"); !parts.empty()) {
        for (DocNodePtr const& part : parts) {
            auto const found = std::ranges::find(description.columns, part->id,
                                                 [](table::TableColumn const& column) -> auto const& {
                                                     return column.info.id;
                                                 });
            if (found != description.columns.end() && !std::ranges::binary_search(companions, part->id)) {
                out.push_back(&*found);
            }
        }
        return out;
    }
    for (table::TableColumn const& column : description.columns) {
        if (!std::ranges::binary_search(companions, column.info.id)) {
            out.push_back(&column);
        }
    }
    return out;
}

}  // namespace

void ScreenCore::modelLayoutColumns(ElementState& element) {
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

// NOLINTNEXTLINE(readability-function-cognitive-complexity): the sort, the layout, then the menus that reach both.
Node ScreenCore::configureTable(TableConfig& config, LowerCtx const& ctx, TableOptions& options,
                                std::shared_ptr<void const> keep) {
    ElementState& element = *config.element;
    DocNode const& node = *element.node;
    // This mount's columns: a new description mounts the table again, with its own.
    auto const columns = std::make_shared<std::vector<LayoutColumn> const>(config.columns);
    auto const sortable = std::make_shared<std::vector<bool> const>(config.sortable);
    auto const hidden = std::make_shared<std::vector<std::string> const>(config.declaredHidden);
    auto const layoutHeld = [&config, columns, hidden] {
        return layoutOver(*columns, *hidden, layoutNow(config));
    };
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
            std::string const* const id = column == nullptr ? nullptr : column->asString();
            std::string const* const way = direction == nullptr ? nullptr : direction->asString();
            auto const found = id == nullptr ? columns->end() : std::ranges::find(*columns, *id, &LayoutColumn::id);
            if (found == columns->end() || way == nullptr || (*way != "asc" && *way != "desc")) {
                return std::vector<TableSort>{};
            }
            auto const index = static_cast<std::size_t>(found - columns->begin());
            if (!sortable->at(index)) {
                return std::vector<TableSort>{};
            }
            out.push_back(TableSort{.column = index,
                                    .direction = *way == "desc" ? SortDirection::Descending : SortDirection::Ascending});
        }
        return out;
    });
    options.arrangement = slotProp<TableArrangement>([columns, layoutHeld] { return arrangementOf(*columns, layoutHeld()); });
    // A copy: the selected rows in view order by the visible columns in layout order, or with no row selected the
    // focused cell alone; over `maxCopyRows` keys it is refused and nothing is copied. The rows say how each cell
    // shows; an unloaded cell is copied as empty and counted in the message.
    auto const copy = [this, &config, &element, layoutHeld, keep, slots = config.mountSlots(),
                       scope = ctx.scope](bool headers) {
        static_cast<void>(keep);
        static_cast<void>(slots);
        if (!config.gather) {
            return;
        }
        std::vector<Key> keys;
        std::vector<std::string> columns = runtime().untracked([&] {
            if (ValueList const* const items = tableSelection(element).asList()) {
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
        if (keys.size() > bound || (viewing && *viewing > static_cast<std::int64_t>(bound))) {
            config.message(tableText::copyBound(bound, separator));
            return;
        }
        if (keys.empty() && !viewing) {
            if (!config.focused || !std::ranges::contains(columns, config.focused->column)) {
                return;
            }
            keys.push_back(config.focused->key);
            columns = {config.focused->column};
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
        gather(columns, [this, &config, &element, headers, started, separator = std::move(separator)](
                          std::expected<CopyGathered, std::string> gathered) {
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
                          CopiedText const text =
                              ui::copyText(headers ? gathered->headers : std::vector<std::string>{}, rows);
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
    auto const refilter = [this, ctx, &config, keep, commands = node.event("onFiltersChange")](
                              table::FilterSpec const& spec) {
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
    auto const columnTarget = [&config](std::string const& id) -> std::optional<FilterTarget> {
        return config.filterTarget ? config.filterTarget(id) : std::nullopt;
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
    auto const openColumn = [this, &config, &menu, columnTarget](std::string const& id) {
        runtime().untracked([&] {
            if (config.listing) {
                config.listing({id});
            }
            if (std::optional<FilterTarget> target = columnTarget(id)) {
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
            auto const first = std::ranges::find(visible, group.columns.empty() ? std::string{} : group.columns.front());
            auto const at = static_cast<std::size_t>(first - visible.begin());
            bool together = first != visible.end() && at + group.columns.size() <= visible.size();
            for (std::size_t offset = 0; together && offset < group.columns.size(); ++offset) {
                together = visible.at(at + offset) == group.columns.at(offset);
            }
            if (!together) {
                continue;
            }
            FilterTarget probe;
            probe.group = formed ? std::string{} : group.id;
            probe.columns = group.columns;
            probe.formed = formed;
            out.emplace_back(TableColumnGroup{.label = group.label,
                                              .first = at,
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
        options.onSort = [refused, sort = std::move(options.onSort)](std::size_t index, HeaderClick click) {
            if (!refused()) {
                sort(index, click);
            }
        };
        options.onColumnMove = [refused, move = std::move(options.onColumnMove)](std::size_t column,
                                                                                  std::size_t position) {
            if (!refused()) {
                move(column, position);
            }
        };
        if (options.onColumnResize) {
            options.onColumnResize = [refused, resize = std::move(options.onColumnResize)](
                                         std::size_t column, std::optional<int> units) {
                if (!refused()) {
                    resize(column, units);
                }
            };
        }
        options.onFilter = [refused, filter = std::move(options.onFilter)](std::size_t column) {
            if (!refused()) {
                filter(column);
            }
        };
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
        options.cellMenu = [this, ctx, &config, &node, columns, filterable, copy, refilter, filtersNow, keep,
                            headerOn](Key const& key, std::size_t column) {
            static_cast<void>(keep);
            std::vector<MenuItem> items;
            std::string const id = column < columns->size() ? columns->at(column).id : std::string{};
            std::optional<HeldCell> const held =
                config.heldCell && !id.empty() ? config.heldCell(key, id) : std::optional<HeldCell>{};
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
            items.push_back(command("Filter by this value", filtering([this, id, entry, refilter, filtersNow] {
                                        table::FilterSpec spec = runtime().untracked(filtersNow);
                                        spec.columns.insert_or_assign(id, table::ColumnFilter{.include = {entry}});
                                        static_cast<void>(refilter(spec));
                                    }),
                                    usable));
            items.push_back(command(
                "Exclude this value",
                filtering([this, id, entry, refilter, filtersNow] {
                    table::FilterSpec spec = runtime().untracked(filtersNow);
                    std::vector<table::FilterEntry>& exclude = spec.columns[id].exclude;
                    auto const listed = std::ranges::find_if(exclude, [](table::FilterEntry const& excluded) {
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
                for (table::ColumnGroupInfo const& group : runtime().untracked([&] { return config.declaredGroups(); })) {
                    if (!std::ranges::contains(group.columns, id)) {
                        continue;
                    }
                    items.push_back(command(
                        "Filter by this value in any " + group.label,
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
            auto const scope =
                std::make_shared<RtScope>(*this, ctx.scope, nullptr, ctx.scope->nodes.depth() + 2);
            scope->row = &scope->nodes.make<reactive::Signal<Value>>(runtime(), record);
            scope->rowKey = canonicalJson(valueOfKey(key));
            declare(*scope, node.scope ? &*node.scope : nullptr, {}, {});
            auto bindings = std::make_shared<Bindings>(*ctx.bindings);
            Value const value = [&] {
                Value const* const field = record.field(id);
                return field == nullptr ? Value{} : *field;
            }();
            bindings->push_back(Binding{.name = "row", .read = [record] { return record; }});
            bindings->push_back(Binding{.name = "column", .read = [id] { return Value{id}; }});
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
            std::string const id = column < columns->size() ? columns->at(column).id : std::string{};
            bindings->push_back(Binding{.name = "column", .read = [id] { return Value{id}; }});
            std::vector<MenuItem> own =
                menuItems(parts, LowerCtx{.scope = ctx.scope, .bindings = std::move(bindings), .boundary = ctx.boundary});
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
    auto const applyView = [this, &config, columns, hidden, fittedView, arrangementNow, relayout, resort, refilter, recordView](
                               table::SavedView const& saved, bool shared) {
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
    auto const changeViews = [&config](async::Completion<TableViews::Result> completion,
                                       std::function<void()> done) {
        completion.then(config.viewsScope, [&config, done = std::move(done)](TableViews::Result const& result) {
            if (!result) {
                config.message(result.error());
                return;
            }
            done();
        });
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
                    changeViews(views->change(table::RemoveView{*current}), [recordView] { recordView(std::nullopt, false); });
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
            default:
                break;
        }
    };
    // A menu is built when it opens, and again when an entry is chosen, from the chain and the layout of the moment,
    // so the path the backend reports names the entry it showed.
    auto const entriesOf = [this, &config, &element, columns, sortable, filterable, layoutHeld, groupsNow, groupArgument,
                            filtersNow, viewsInput](std::optional<std::size_t> column) -> std::vector<TableMenuEntry> {
        return runtime().untracked([&]() -> std::vector<TableMenuEntry> {
            table::ColumnLayout const layout = layoutHeld();
            // The groups, each named in its members' menus and in the table's.
            auto const groupsOf = [&](std::string_view id) {
                std::vector<MenuGroup> out;
                for (auto const& [group, formed] : groupsNow()) {
                    if (id.empty() || std::ranges::contains(group.columns, id)) {
                        out.push_back(MenuGroup{
                            .id = groupArgument(group, formed), .label = group.label, .columns = group.columns.size()});
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
            LayoutColumn const& at = columns->at(*column);
            SortChain const chain = sortChainOf(tableSort(element));
            auto const held = std::ranges::find(chain, at.id, &table::SortKey::column);
            std::vector<MenuDimension> dimensions =
                config.dimensions ? config.dimensions(at.id) : std::vector<MenuDimension>{};
            table::ColumnLayoutValue const value = layout.value();
            if (auto const units = value.units.find(at.id); units != value.units.end()) {
                for (MenuDimension& dimension : dimensions) {
                    if (auto const chosen = units->second.find(dimension.dimension); chosen != units->second.end()) {
                        dimension.chosen = chosen->second;
                    }
                }
            }
            return offeredEntries(headerMenu(HeaderMenuInput{
                .column = at,
                .sortable = sortable->at(*column),
                .filterable = filterable->at(*column),
                .sortPlace = held == chain.end() ? std::nullopt
                                                 : std::optional{static_cast<std::size_t>(held - chain.begin())},
                .chainSize = chain.size(),
                .maxSortKeys = config.maxSortKeys(),
                .visibleIndex = static_cast<std::size_t>(std::ranges::find(visible, at.id) - visible.begin()),
                .visibleCount = visible.size(),
                .pinned = layout.pinned(),
                .dimensions = std::move(dimensions),
                .groups = groupsOf(at.id),
                .allowed = config.allowed,
            }));
        });
    };
    // While the header is disabled the table's menu offers copy alone; the mount greys a column's menu itself.
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
    options.onMenu = [&config, &menu, entriesOf, sortBy, arrange, columns, openColumn, openGroup, refilter, copy, headerOn,
                      viewCommand](std::optional<std::size_t> column, std::vector<std::size_t> const& path) {
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
        std::string const id = index < columns->size() ? columns->at(index).id : std::string{};
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
                arrange(MoveColumn{.id = id, .by = -1});
                break;
            case MenuCommand::MoveRight:
                arrange(MoveColumn{.id = id, .by = 1});
                break;
            case MenuCommand::PinColumn:
                arrange(PinColumn{.id = id});
                break;
            case MenuCommand::UnpinColumn:
                arrange(UnpinColumn{.id = id});
                break;
            case MenuCommand::HideColumn:
                arrange(HideColumn{.id = id});
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
                if (!id.empty()) {
                    openColumn(id);
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
                if (cut != std::string::npos && !id.empty()) {
                    std::string const unit = entry->argument.substr(cut + 1);
                    arrange(SetDisplayUnit{.id = id,
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
            [arrange](std::string const& id, bool shown) {
                if (shown) {
                    arrange(ShowColumn{.id = id});
                } else {
                    arrange(HideColumn{.id = id});
                }
            },
        .onMove = [arrange](std::string const& id, int by) { arrange(MoveColumn{.id = id, .by = by}); },
        .onReset = [arrange] { arrange(ResetColumns{}); },
        .onFormGroup =
            [this, arrange, layoutHeld](std::string label, std::vector<std::string> members) {
                std::size_t const before = runtime().untracked([&] { return layoutHeld().groups().size(); });
                arrange(FormGroup{.label = std::move(label), .columns = std::move(members)});
                return runtime().untracked([&] { return layoutHeld().groups().size(); }) > before;
            },
        // A removed group takes its filter with it: the group filter that names the group's columns.
        .onRemoveGroup =
            [this, &config, &menu, arrange, layoutHeld, filtersNow, refilter](std::string const& id) {
                std::vector<std::string> members;
                std::size_t const before = runtime().untracked([&] {
                    table::ColumnLayout const layout = layoutHeld();
                    auto const group = std::ranges::find(layout.groups(), id, &table::ColumnGroupInfo::id);
                    if (group != layout.groups().end()) {
                        members = group->columns;
                    }
                    return layout.groups().size();
                });
                arrange(RemoveGroup{.id = id});
                if (runtime().untracked([&] { return layoutHeld().groups().size(); }) >= before) {
                    return;
                }
                FilterMenuState const& open = runtime().untracked([&]() -> FilterMenuState const& { return menu.state(); });
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
                resort([&](SortChain const& current) {
                    return applySortEdit(current, ids, edit, config.maxSortKeys());
                });
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
    Node list = ui::forEach<Value>(
        [&model] { return std::vector<Value>{Value{model.revision.get()}}; },
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

// NOLINTNEXTLINE(readability-function-cognitive-complexity): the columns, the cells, the row's parts and the handlers.
Node ScreenCore::modelTableOf(DocNode const& node, LowerCtx const& ctx, ElementState& element) {
    ModelTable& model = *element.modelTable;
    TableOptions options;
    commonOf(node, ctx, &element, options.common);
    options.status = slotProp<TableStatus>([this, &element, &model, scope = ctx.scope] {
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
    options.onStatusEntry = [this, &element, &model](std::string const& id) {
        // "Select them" makes the failed rows the selection; "Clear marks" drops their marks.
        if (id == kSelectFailed || id == kClearMarks) {
            tableGesture(element);
            runtime().batch([&] {
                if (id == kSelectFailed) {
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
        if (id == kShowMatching) {
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
        auto const found = model.entryHandlers.find(id);
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
    auto const describedColumn = [description](std::string_view id) -> table::TableColumn const* {
        auto const found = std::ranges::find(description->columns, id,
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
    auto const add = [&](table::TableColumn const& described, DocNode const* part) {
        TableColumn column{.id = described.info.id,
                           .label = described.label.empty() ? described.info.id : described.label,
                           .valign = tableValign,
                           .sortable = described.sortable};
        table::TableColumn display = described;
        CellExprs exprs{.tone = nullptr, .tooltip = nullptr, .path = part != nullptr ? part->path : node.path};
        if (part != nullptr) {
            display.display = displayOf(*part, described.display, [&](ExprPtr const& expr) { return once(ctx, expr); });
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
    for (std::string const& id : reads.ids) {
        if (model.described.contains(id)) {
            source.alwaysFetched.push_back(id);
        }
    }
    std::vector<std::string> ids;
    for (TableColumn const& column : columns) {
        ids.push_back(column.id);
    }
    source.cellReads.resize(parts.size());
    source.ownCell.resize(parts.size(), false);
    for (std::size_t at = 0; at < parts.size(); ++at) {
        for (std::string const& id : partReads.at(at).ids) {
            if (id != ids.at(at) && model.described.contains(id) && !std::ranges::contains(source.alwaysFetched, id)) {
                source.cellReads.at(at).push_back(id);
            }
        }
        source.ownCell.at(at) = parts.at(at) != nullptr && !parts.at(at)->nodes("cell").empty();
    }
    // The companions travel with their column: the query names them beside the columns the mount names.
    model.rows->companions(
        [described = std::make_shared<table::TableDescription const>(*description),
         overrides](std::span<std::string const> named) {
            return table::columnsWithCompanions(*described, named, overrides);
        });
    // A cell's text, by its column's display. Each column's `DisplayUnits` points into `registry`, so the formatters
    // hold it: it lives as long as the cells do.
    auto const textOfCell = [shown, displayUnits, registry](std::size_t column, Value const& record,
                                                            FormatContext const& format) {
        table::TableColumn const& display = shown->at(column);
        DisplayUnits const& units = displayUnits->at(column);
        return formatCell(cellOfRecord(record.field(display.info.id), display, units.units), display, format, units);
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
    bool const cellExprsGiven = std::ranges::any_of(*cellExprs, [](CellExprs const& exprs) {
        return exprs.tone != nullptr || exprs.tooltip != nullptr;
    });
    // The row scope a cell's own tone and tooltip expressions read; none when no column gives one.
    auto const exprScope = [this, ctx, &node, &model](reactive::Signal<Value> const& row) {
        std::string key = runtime().untracked([&] {
            Value const* const id = row.peek().field(model.keyColumn);
            return id == nullptr ? std::string{} : canonicalJson(*id);
        });
        auto const scope = rowScope(node, ctx, row, {}, std::move(key));
        scope->onMissingRowField = &model.onMissing;
        scope->rowKeyNow = [&row, &model] {
            Value const* const id = row.get().field(model.keyColumn);
            return id == nullptr ? std::string{} : canonicalJson(*id);
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
    source.cellView = [this, ctx, &node, &model, parts, roles, textOfCell, accentOf, cellExprs, exprScope](
                          std::size_t column, reactive::Signal<Value> const& row) -> Node {
        DocNode const* const part = column < parts.size() ? parts.at(column) : nullptr;
        std::span<DocNodePtr const> const cell = part != nullptr ? part->nodes("cell") : std::span<DocNodePtr const>{};
        if (!cell.empty()) {
            std::vector<DocNode const*> content;
            for (DocNodePtr const& child : cell) {
                content.push_back(child.get());
            }
            std::string key = runtime().untracked([&] {
                Value const* const id = row.peek().field(model.keyColumn);
                return id == nullptr ? std::string{} : canonicalJson(*id);
            });
            auto const scope = rowScope(node, ctx, row, content, std::move(key));
            scope->onMissingRowField = &model.onMissing;
            // The mount rebinds a pooled cell to other rows by its signal: the row its elements are keyed by is the
            // one the signal holds now.
            scope->rowKeyNow = [&row, &model] {
                Value const* const id = row.get().field(model.keyColumn);
                return id == nullptr ? std::string{} : canonicalJson(*id);
            };
            LowerCtx const rowCtx{.scope = scope, .bindings = ctx.bindings, .boundary = ctx.boundary};
            // The cell owns its scope for as long as its node lives.
            auto const owner =
                std::make_shared<std::pair<Node, std::shared_ptr<RtScope>> const>(lowerAll(cell, rowCtx), scope);
            return accentedCell(Node{owner, owner->first.get()},
                                slotProp<CellAccent>([&row, column, accentOf, scope] {
                                    return accentOf(column, row.get(), scope.get());
                                }));
        }
        CellExprs const& exprs = cellExprs->at(column);
        std::shared_ptr<RtScope const> const cellScope =
            exprs.tone != nullptr || exprs.tooltip != nullptr ? exprScope(row) : nullptr;
        Node text = ui::text(Text{
            .text = slotProp<std::string>([scope = ctx.scope, &row, column, textOfCell] {
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
        source.rowView = [this, ctx, &node, &model, toneExpr, labelExpr, toneReported](
                             reactive::Signal<Value> const& row) {
            std::string key = runtime().untracked([&] {
                Value const* const id = row.peek().field(model.keyColumn);
                return id == nullptr ? std::string{} : canonicalJson(*id);
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
                Value const* const id = row.peek().field(model.keyColumn);
                return id == nullptr ? std::nullopt : std::optional{keyOf(*id)};
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
                table::TableDescription const* const description = model.window->description();
                std::optional<std::string> const locked =
                    description == nullptr ? std::nullopt
                                           : lockText(TableRowRef{.key = *rowKey, .row = record}, *description);
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
    options.selectionMode = mode == "single"     ? SelectionMode::Single
                            : mode == "multiple" ? SelectionMode::Multiple
                                                 : SelectionMode::None;
    options.selection = slotProp<std::vector<Key>>([&element] {
        std::vector<Key> keys;
        if (ValueList const* const items = tableSelection(element).asList()) {
            for (Value const& item : *items) {
                keys.push_back(keyOf(item));
            }
        }
        return keys;
    });
    // The selection the user made: held by the element unless the document binds it, then the command list.
    auto const selectRows = [this, ctx, &element, commands = node.event("onSelectionChange")](
                                std::vector<Key> const& keys) {
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
            std::unordered_set<Key> const reported(keys.begin(), keys.end());
            TableSelection next = *viewing;
            // The rows the backend holds are those the mount named in or near view; a held row outside them is not
            // in the report, and is not read as left out.
            auto const [first, count] = model.rows->shownRange();
            for (std::int64_t at = first; at < first + count; ++at) {
                std::optional<TableRowRef> const row = model.rows->row(at);
                if (!row) {
                    continue;
                }
                if (reported.contains(row->key)) {
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
    options.onSelectRange = [this, ctx, &element, &model, select](std::size_t anchor, std::size_t focus,
                                                                     SelectionChange change) {
        tableGesture(element);
        std::size_t const first = std::min(anchor, focus);
        std::size_t const count = std::max(anchor, focus) - first + 1;
        std::size_t const bound = session.options().maxRangeRows;
        if (count > bound) {
            tableMessage(element, "A range selects at most " +
                                      render::formatCanonicalNumber(std::to_string(bound),
                                                                    ctx.scope->formatting().numeric) +
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
            .then(model.scope,
                  [this, &element, select = std::weak_ptr{select}, change](
                      std::expected<std::vector<table::RowId>, table::TableError> const& found) {
                      if (!found) {
                          tableMessage(element, "The range could not be selected: " + found.error().message);
                          return;
                      }
                      std::vector<Key> keys;
                      if (change == SelectionChange::Extend) {
                          if (ValueList const* const held = tableSelection(element).asList()) {
                              for (Value const& item : *held) {
                                  keys.push_back(keyOf(item));
                              }
                          }
                      }
                      for (table::RowId const& id : *found) {
                          Key const key = std::visit([](auto const& value) { return Key{value}; }, id);
                          if (!std::ranges::contains(keys, key)) {
                              keys.push_back(key);
                          }
                      }
                      if (auto const live = select.lock()) {
                          (*live)(keys);
                      }
                  });
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
                                     listed](std::string const& id) -> std::optional<FilterTarget> {
            auto const found = std::ranges::find_if(*shown, [&id](table::TableColumn const& column) {
                return column.info.id == id;
            });
            if (found == shown->end() || !found->filterable) {
                return std::nullopt;
            }
            auto const at = static_cast<std::size_t>(found - shown->begin());
            if (refused->at(at)) {
                return std::nullopt;
            }
            FilterTarget target;
            target.column = id;
            target.label = labels->at(at);
            target.shown = *found;
            target.units = displayUnits->at(at);
            target.unitList = described->units;
            if (found->info.kind == table::ColumnKind::Measure) {
                for (MenuDimension const& dimension : unitsOf(*found)) {
                    target.unitChoices.insert(target.unitChoices.end(), dimension.units.begin(), dimension.units.end());
                }
            }
            target.listed = listed;
            target.keep = registry;
            return target;
        };
        // A cell a menu opens on: unloaded while its row holds no field for it or its level has not loaded.
        model.config.heldCell = [&model, shown, displayUnits, registry](Key const& key,
                                                                        std::string const& id) -> std::optional<HeldCell> {
            auto const found = std::ranges::find_if(*shown, [&id](table::TableColumn const& column) {
                return column.info.id == id;
            });
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
                    Value const* const field = ref->row.field(id);
                    table::UnloadedLevels const levels = model.rows->unloadedLevels(at);
                    bool const unloaded = field == nullptr || (!found->info.level.empty() &&
                                                               std::ranges::contains(levels.levels, found->info.level));
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
        model.config.dimensions = [shown, unitsOf](std::string const& id) {
            auto const found = std::ranges::find_if(*shown, [&id](table::TableColumn const& column) {
                return column.info.id == id;
            });
            return found == shown->end() || found->info.kind != table::ColumnKind::Measure ? std::vector<MenuDimension>{}
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
                                  std::vector<std::string> const& ids, CopyDone done) {
        std::vector<std::size_t> places;
        std::vector<std::string> named;
        CopyGathered gathered;
        for (std::string const& id : ids) {
            auto const found = std::ranges::find_if(*shown, [&id](table::TableColumn const& column) {
                return column.info.id == id;
            });
            if (found != shown->end()) {
                places.push_back(static_cast<std::size_t>(found - shown->begin()));
                named.push_back(id);
                gathered.headers.push_back(headers->at(places.back()));
            }
        }
        std::optional<TableSelection> const view = model.viewSelection.peek();
        if (!view) {
            done(std::move(gathered));
            return;
        }
        std::unordered_set<Key> const except(view->except.begin(), view->except.end());
        FormatContext const format = scope->formatting();
        model.rows->rowsOf(0, model.rows->total().rows, named)
            .then(model.scope, [shown, displayUnits, registry, places, format, except, done,
                                gathered = std::move(gathered)](
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
                                                : std::optional{formatCell(cell, shown->at(place), format,
                                                                           displayUnits->at(place))});
                    }
                    gathered.rows.push_back(std::move(row));
                }
                done(std::move(gathered));
            });
    };
    model.config.gather = [&model, shown, displayUnits, registry, headers, textOfCell, scope = ctx.scope](
                              std::vector<Key> keys, std::vector<std::string> const& ids, CopyDone done) {
        std::vector<std::size_t> places;
        std::vector<std::string> named;
        CopyGathered gathered;
        for (std::string const& id : ids) {
            auto const found = std::ranges::find_if(*shown, [&id](table::TableColumn const& column) {
                return column.info.id == id;
            });
            if (found != shown->end()) {
                places.push_back(static_cast<std::size_t>(found - shown->begin()));
                named.push_back(id);
                gathered.headers.push_back(headers->at(places.back()));
            }
        }
        FormatContext const format = scope->formatting();
        std::unordered_set<Key> wanted{keys.begin(), keys.end()};
        // The rows held with every column copied; a row held without one of them is fetched with the rest.
        TableRows& rows = *model.rows;
        std::vector<std::int64_t> held;
        for (auto const& [first, count] : model.window->heldRanges()) {
            for (std::int64_t at = first; at < first + count; ++at) {
                held.push_back(at);
            }
        }
        for (std::int64_t const at : held) {
            if (wanted.empty()) {
                break;
            }
            std::optional<TableRowRef> const ref = rows.row(at);
            if (!ref || !wanted.contains(ref->key) ||
                !std::ranges::all_of(named, [&](std::string const& id) { return ref->row.field(id) != nullptr; })) {
                continue;
            }
            std::span<std::string const> const unloaded = rows.unloadedLevels(at).levels;
            CopyRow row{.viewRow = at, .cells = {}};
            for (std::size_t const place : places) {
                std::string const& level = shown->at(place).info.level;
                bool const missing = !level.empty() && std::ranges::contains(unloaded, level);
                row.cells.push_back(missing ? std::nullopt : std::optional{textOfCell(place, ref->row, format)});
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
            .then(model.scope, [shown, displayUnits, registry, places, format, done, gathered = std::move(gathered)](
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
                                                : std::optional{formatCell(cell, shown->at(place), format,
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
    return besideDialogs(options.common, std::move(dialogs), [&] {
        return ui::table(std::move(columns), std::move(source), std::move(options));
    });
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

namespace {

/// @brief A chart's exact value at @p places, as the active locale writes it.
/// @param value  The value.
/// @param places The places, at most 18.
/// @param format The formatting facts.
/// @return The text; the exact JSON when the rounded value does not fit.
[[nodiscard]] std::string chartNumberText(math::Rational const& value, std::uint32_t places,
                                          FormatContext const& format) {
    std::optional<math::Rational> rounded =
        math::checkedRoundToDecimalPlaces(value, math::DecimalPlaces{places}, math::RoundingMode::HalfEven);
    if (!rounded) {
        return textOf(Value{Decimal{value}}, format);
    }
    rounded->decimalPlaces = math::DecimalPlaces{places};
    return textOf(Value{Decimal{*rounded}}, format);
}

/// @brief A reference line's stroke by its document name.
/// @param name `solid`, `dashed`, `dotted`, or empty for solid; the loader refuses any other.
/// @return The stroke.
[[nodiscard]] ChartLineStyle chartLineStyle(std::string_view name) {
    if (name == "dashed") {
        return ChartLineStyle::Dashed;
    }
    return name == "dotted" ? ChartLineStyle::Dotted : ChartLineStyle::Solid;
}

/// @brief Where a point sits along a value axis that a document may have fixed, and on which side it is clipped.
/// @param value The point's value.
/// @param range The axis.
/// @return The value drawn, the axis end for a value outside, and the side it lies beyond.
[[nodiscard]] std::pair<math::Rational, ChartClip> chartClipped(math::Rational const& value, ChartRange const& range) {
    if (value < range.low) {
        return {range.low, ChartClip::Below};
    }
    if (value > range.high) {
        return {range.high, ChartClip::Above};
    }
    return {value, ChartClip::None};
}
}  // namespace

std::vector<detail::ChartRawLine> ScreenCore::chartLines(DocNode const& node, RtScope const& scope,
                                                         Bindings const* bindings, std::optional<std::string>& unit) {
    FormatContext const& format = scope.formatting();
    std::vector<detail::ChartRawLine> lines;
    for (DocNodePtr const& part : node.partList("lines")) {
        ExprPtr const valueExpr = part->prop("value");
        Value const value = evalIn(scope, bindings, valueExpr);
        std::optional<math::Rational> const magnitude =
            value.isNull() ? std::nullopt : chartMagnitude(value, valueExpr->path, unit);
        if (!magnitude) {
            continue;
        }
        detail::ChartRawLine line{.value = *magnitude, .label = std::nullopt};
        if (ExprPtr const label = part->prop("label")) {
            line.label = textOf(evalIn(scope, bindings, label), format);
        }
        if (ExprPtr const tone = part->prop("tone")) {
            line.tone = chartToneOf(evalIn(scope, bindings, tone), tone->path).value_or(Tone::Neutral);
        }
        line.style = detail::chartLineStyle(part->name("style"));
        lines.push_back(std::move(line));
    }
    return lines;
}

ChartRange ScreenCore::chartFixedRange(DocNode const& node, RtScope const& scope, Bindings const* bindings,
                                       std::optional<std::string>& unit, ChartRange const& range) {
    ChartRange fixed = range;
    for (auto const& [key, end] : {std::pair{"yMin", &fixed.low}, std::pair{"yMax", &fixed.high}}) {
        if (ExprPtr const expr = node.prop(key)) {
            Value const value = evalIn(scope, bindings, expr);
            if (!value.isNull()) {
                *end = chartMagnitude(value, expr->path, unit).value_or(*end);
            }
        }
    }
    if (fixed.high > fixed.low) {
        return fixed;
    }
    report(pointerAppend(node.path, "yMin"), "`yMin` is not below `yMax`; the chart takes its range from its values");
    return range;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity): one branch per way a series can be drawn or clipped.
void ScreenCore::chartSeries(ChartData& data, DocNode const& node, RtScope const& scope, Bindings const* bindings,
                             std::vector<std::vector<detail::ChartRawPoint>>& series, bool positioned,
                             ChartRange const& range) {
    auto const positionOf = [positioned](detail::ChartRawPoint const& point) {
        return positioned && point.x ? *point.x
                                     : math::Rational{static_cast<std::int64_t>(point.index), math::DecimalPlaces{0}};
    };
    std::vector<math::Rational> positions;
    for (auto const& points : series) {
        std::ranges::transform(points, std::back_inserter(positions), positionOf);
    }
    // The room one position gives its bars: the least distance between two of their positions, gaps included, so a
    // bar never grows over the label of an empty position between two.
    std::vector<math::Rational> barPositions;
    {
        std::size_t seriesIndex = 0;
        for (DocNodePtr const& part : node.partList("series")) {
            if (part->name("style") == "bars") {
                std::ranges::transform(series.at(seriesIndex), std::back_inserter(barPositions), positionOf);
            }
            ++seriesIndex;
        }
    }
    std::ranges::sort(barPositions);
    std::optional<math::Rational> slot;
    for (std::size_t next = 1; next < barPositions.size(); ++next) {
        auto const distance = math::checkedSub(barPositions.at(next), barPositions.at(next - 1));
        if (distance && distance->numerator > 0 && (!slot || *distance < *slot)) {
            slot = *distance;
        }
    }
    if (!barPositions.empty()) {
        data.barSlot = realOf(slot.value_or(math::Rational{1, math::DecimalPlaces{0}}));
    }
    // A bar is centred on its position and half a slot wide each side, so the axis holds that much beyond the
    // outermost position.
    if (!barPositions.empty() && !positions.empty()) {
        math::Rational const half = math::checkedDiv(slot.value_or(math::Rational{1, math::DecimalPlaces{0}}),
                                                     math::Rational{2, math::DecimalPlaces{0}})
                                        .value_or(math::Rational{0, math::DecimalPlaces{0}});
        auto const [lowest, highest] = std::ranges::minmax(positions);
        auto const below = math::checkedSub(lowest, half);
        auto const above = math::checkedAdd(highest, half);
        if (below && above) {
            positions.push_back(*below);
            positions.push_back(*above);
        }
    }
    if (std::optional<ChartRange> const across = chartRange(positions, 0)) {
        data.xMin = realOf(across->low);
        data.xMax = realOf(across->high);
    }
    std::size_t index = 0;
    for (DocNodePtr const& part : node.partList("series")) {
        ChartSeries drawn;
        if (part->name("style") == "bars") {
            drawn.style = ChartStyle::Bars;
        }
        if (ExprPtr const label = part->prop("label")) {
            drawn.label = textOf(evalIn(scope, bindings, label), scope.formatting());
        }
        drawn.joined = drawn.style == ChartStyle::Line && (!part->flags.contains("joined") || part->flag("joined"));
        for (detail::ChartRawPoint& point : series.at(index)) {
            ChartPoint out{
                .x = realOf(positionOf(point)),
                .y = std::nullopt,
                .clip = ChartClip::None,
                .xLabel = std::move(point.xLabel),
                .tone = point.tone,
                .note = std::move(point.note),
            };
            if (point.y) {
                auto const [shown, clip] = detail::chartClipped(*point.y, range);
                out.y = realOf(shown);
                out.clip = clip;
            }
            drawn.points.push_back(std::move(out));
        }
        data.series.push_back(std::move(drawn));
        ++index;
    }
}

ChartData ScreenCore::chartData(DocNode const& node, RtScope const& scope, Bindings const* bindings) {
    FormatContext const& format = scope.formatting();
    ChartData data;
    data.summary = textOf(evalIn(scope, bindings, node.prop("summary")), format);
    if (ExprPtr const label = node.prop("yLabel")) {
        data.yLabel = textOf(evalIn(scope, bindings, label), format);
    }
    std::optional<std::string> unit;
    detail::ChartAxisKinds kinds;
    std::vector<std::vector<detail::ChartRawPoint>> series;
    for (DocNodePtr const& part : node.partList("series")) {
        series.push_back(chartPoints(*part, scope, bindings, unit, kinds));
    }
    std::vector<detail::ChartRawLine> const lines = chartLines(node, scope, bindings, unit);
    // The values the axis must hold, and the places they are shown at.
    std::vector<math::Rational> values;
    for (auto const& points : series) {
        for (detail::ChartRawPoint const& point : points) {
            if (point.y) {
                values.push_back(*point.y);
            }
        }
    }
    std::ranges::transform(lines, std::back_inserter(values), &detail::ChartRawLine::value);
    // A bar grows from 0, so the axis holds 0; the baseline's side is padded only when something lies beyond it, so
    // that the baseline is the plot's edge when nothing does.
    std::vector<math::Rational> axisValues = values;
    ChartPadding padding;
    bool barsHaveValues = false;
    {
        std::size_t seriesIndex = 0;
        for (DocNodePtr const& part : node.partList("series")) {
            barsHaveValues =
                barsHaveValues || (part->name("style") == "bars" &&
                                   std::ranges::any_of(series.at(seriesIndex), [](detail::ChartRawPoint const& point) {
                                       return point.y.has_value();
                                   }));
            ++seriesIndex;
        }
    }
    if (barsHaveValues) {
        axisValues.emplace_back(0, math::DecimalPlaces{0});
        padding.low = std::ranges::any_of(values, [](math::Rational const& value) { return value.numerator < 0; });
        padding.high = std::ranges::any_of(values, [](math::Rational const& value) { return value.numerator > 0; });
    }
    std::uint32_t widest = 0;
    for (math::Rational const& value : values) {
        widest = std::max(widest, value.decimalPlaces.value);
    }
    ExprPtr const placesExpr = node.prop("yPlaces");
    std::int64_t const* const fixedPlaces = placesExpr ? placesExpr->literal.asInt() : nullptr;
    std::uint32_t const places = fixedPlaces != nullptr ? static_cast<std::uint32_t>(*fixedPlaces) : widest;
    std::optional<ChartRange> const computed = chartRange(axisValues, places, padding);
    if (!computed) {
        report(node.path, "the chart's values overflow its arithmetic; it draws nothing");
        return data;
    }
    ChartRange const range = chartFixedRange(node, scope, bindings, unit, *computed);
    data.yMin = realOf(range.low);
    data.yMax = realOf(range.high);
    // Three or more lines are the scale a reader reads the chart against; fewer leave the values without one.
    std::optional<ChartTicks> const ticks = lines.size() < 3 ? chartTicks(range) : std::nullopt;
    if (ticks) {
        std::uint32_t const tickPlaces = std::max(places, ticks->places);
        for (math::Rational const& tick : ticks->values) {
            data.yTicks.push_back(
                ChartTick{.value = realOf(tick), .text = detail::chartNumberText(tick, tickPlaces, format)});
        }
    }
    for (detail::ChartRawLine const& line : lines) {
        data.lines.push_back(ChartLine{
            .value = realOf(line.value),
            .label = line.label.value_or(detail::chartNumberText(line.value, places, format)),
            .tone = line.tone,
            .style = line.style,
        });
    }
    if (kinds.numbers && kinds.instants) {
        report(pointerAppend(node.path, "series"),
               "a chart's `x` values are all numbers or all dates and instants; the points are spaced by index");
    }
    chartSeries(data, node, scope, bindings, series, kinds.numbers != kinds.instants, range);
    return data;
}

std::optional<Tone> ScreenCore::rowToneOf(Value const& value, std::string const& path, bool& noted) {
    if (value.isNull()) {
        return std::nullopt;
    }
    if (std::optional<Tone> const tone = toneNamed(value)) {
        return tone;
    }
    if (!noted) {
        noted = true;
        report(path, "rowTone is " + canonicalJson(value) + "; a tone is neutral, info, ok, warn or err");
    }
    return std::nullopt;
}

std::vector<RowSpan> ScreenCore::rowSpansOf(Value const& value, std::vector<std::string> const& columns,
                                            std::string const& path, bool& noted) {
    std::vector<RowSpan> spans;
    if (value.isNull()) {
        return spans;
    }
    ValueList const* const items = value.asList();
    bool bad = items == nullptr;
    for (Value const& item : items != nullptr ? *items : ValueList{}) {
        Value const* const column = item.field("column");
        Value const* const rows = item.field("rows");
        std::string const* const columnId = column != nullptr ? column->asString() : nullptr;
        std::int64_t const* const count = rows != nullptr ? rows->asInt() : nullptr;
        auto const found = columnId != nullptr ? std::ranges::find(columns, *columnId) : columns.end();
        if (found == columns.end() || count == nullptr || *count < 1) {
            bad = true;
            continue;
        }
        spans.push_back(RowSpan{
            .column = static_cast<std::size_t>(found - columns.begin()),
            .rows = static_cast<std::size_t>(*count),
        });
    }
    if (bad && !noted) {
        noted = true;
        report(path,
               "rowSpans is " + canonicalJson(value) + "; a span is {column: a column's id, rows: an integer from 1}");
    }
    return spans;
}

void ScreenCore::ownQueriesOf(ExprPtr const& expr, RtScope const& scope, BoundaryRuntime& boundary,
                              std::set<std::string, std::less<>>& seen) {
    if (!expr) {
        return;
    }
    bool const control = expr->kind == ExprKind::Query || expr->kind == ExprKind::Pending ||
                         expr->kind == ExprKind::Error || expr->kind == ExprKind::ErrorKind ||
                         expr->kind == ExprKind::Refusal;
    if (control || expr->kind == ExprKind::Ref) {
        NameRef const* const found = scope.find(expr->name);
        if (found != nullptr && found->category == NameCategory::Query && seen.insert(expr->name).second) {
            boundary.queries.push_back(found->query);
        }
        if (expr->kind == ExprKind::Ref && found != nullptr && found->category == NameCategory::Let &&
            seen.insert("let:" + expr->name).second) {
            ownQueriesOf(found->letExpr, scope, boundary, seen);
        }
    }
    for (ExprPtr const& arg : expr->args) {
        ownQueriesOf(arg, scope, boundary, seen);
    }
    for (auto const& [name, field] : expr->fields) {
        static_cast<void>(name);
        ownQueriesOf(field, scope, boundary, seen);
    }
}

void ScreenCore::ownQueries(DocNode const& node, RtScope const& scope, BoundaryRuntime& boundary,
                            std::set<std::string, std::less<>>& seen) {
    if (node.kind == "boundary") {
        return;
    }
    for (auto const& [key, expr] : node.props) {
        static_cast<void>(key);
        ownQueriesOf(expr, scope, boundary, seen);
    }
    for (auto const& [key, nodes] : node.children) {
        static_cast<void>(key);
        for (DocNodePtr const& child : nodes) {
            ownQueries(*child, scope, boundary, seen);
        }
    }
    for (auto const& [key, parts] : node.parts) {
        static_cast<void>(key);
        for (DocNodePtr const& part : parts) {
            ownQueries(*part, scope, boundary, seen);
        }
    }
}

Node ScreenCore::lowerBoundary(DocNode const& node, LowerCtx const& ctx) {
    auto const boundary = std::make_shared<BoundaryRuntime>();
    std::set<std::string, std::less<>> seen;
    for (DocNodePtr const& child : node.nodes("children")) {
        ownQueries(*child, *ctx.scope, *boundary, seen);
    }
    auto const bindings = std::make_shared<Bindings>(*ctx.bindings);
    bindings->push_back(Binding{
        .name = "boundary",
        .read =
            [boundary] {
                for (QueryRuntime const* const query : boundary->queries) {
                    Value const error = query->error();
                    if (!error.isNull() && query->value().isNull()) {
                        return Value{ValueRecord{{"error", error}}};
                    }
                }
                return Value{ValueRecord{{"error", Value{}}}};
            },
    });
    LowerCtx const errorCtx{.scope = ctx.scope, .bindings = bindings, .boundary = boundary};
    LowerCtx const childCtx{.scope = ctx.scope, .bindings = ctx.bindings, .boundary = boundary};
    auto const group = [this](std::span<DocNodePtr const> nodes, LowerCtx const& where) -> Node {
        return nodes.empty() ? ui::spacer() : lowerAll(nodes, where);
    };
    Switch spec;
    spec.cases.push_back(SwitchCase{.key = Key{std::int64_t{0}}, .node = group(node.nodes("loading"), childCtx)});
    spec.cases.push_back(SwitchCase{.key = Key{std::int64_t{1}}, .node = group(node.nodes("error"), errorCtx)});
    spec.cases.push_back(SwitchCase{.key = Key{std::int64_t{2}}, .node = group(node.nodes("children"), childCtx)});
    spec.selector = slotProp<Key>([boundary] {
        bool loading = false;
        for (QueryRuntime const* const query : boundary->queries) {
            if (!query->settled()) {
                loading = true;
            } else if (query->value().isNull() && !query->error().isNull()) {
                return Key{std::int64_t{1}};
            }
        }
        return Key{std::int64_t{loading ? 0 : 2}};
    });
    commonOf(node, ctx, nullptr, spec.common);
    return ui::switchOf(std::move(spec));
}

Node ScreenCore::lowerCustom(DocNode const& node, LowerCtx const& ctx) {
    std::string const name{node.name("name")};
    // A loaded document's custom node names a declared component, and every component has a fallback.
    Component const& component = _doc->components.at(name);
    std::vector<std::pair<std::string, ExprPtr>> props;
    for (DocNodePtr const& record : node.partList("props")) {
        for (auto const& [key, expr] : record->props) {
            props.emplace_back(key, expr);
        }
    }
    auto const bindings = std::make_shared<Bindings>();
    bindings->push_back(Binding{
        .name = "props",
        .read =
            [scope = ctx.scope, outer = ctx.bindings, props = std::move(props)] {
                ValueRecord record;
                for (auto const& [key, expr] : props) {
                    record.insert_or_assign(key, evalIn(*scope, outer.get(), expr));
                }
                return Value{std::move(record)};
            },
    });
    auto const scope = std::make_shared<RtScope>(*this, nullptr, nullptr, ctx.scope->nodes.depth() + 1);
    scope->formatSource = ctx.scope;
    scope->usePrefix = focusKeyOf(node.path, *ctx.scope) + ">";
    DocNode const& fallback = *component.fallback;
    declare(*scope, nullptr, {&fallback}, {});
    Node lowered = lower(fallback, LowerCtx{.scope = scope, .bindings = bindings, .boundary = nullptr});
    Custom spec{
        .name = name,
        .fallback = std::move(lowered),
        .props = {},
        .minVersion =
            static_cast<int>(std::clamp<std::int64_t>(component.version, 0, std::numeric_limits<int>::max())),
        .onEvent = {},
    };
    // Every declared prop is given, a missing one as its type's empty value: a component lacking one is refused.
    for (auto const& [prop, type] : component.props) {
        ExprPtr expr;
        for (DocNodePtr const& record : node.partList("props")) {
            if (ExprPtr const given = record->prop(prop)) {
                expr = given;
            }
        }
        if (type == "bool") {
            spec.props.push_back(CustomProp{.name = prop, .value = bindProp<bool>(ctx, expr, false, boolOf)});
        } else if (type == "number") {
            spec.props.push_back(CustomProp{
                .name = prop,
                .value = bindProp<double>(
                    ctx, expr, 0.0, [](Value const& value, FormatContext const&) { return componentNumber(value); }),
            });
        } else {
            spec.props.push_back(CustomProp{
                .name = prop,
                .value = bindProp<std::string>(ctx, expr, std::string{},
                                               [type](Value const& value, FormatContext const& format) {
                                                   return componentText(value, type, format);
                                               }),
            });
        }
    }
    spec.onEvent = [this, ctx, site = &node, declared = &component](std::string const& event,
                                                                    std::string const& payload) {
        Commands const* const commands = site->event("on." + event);
        if (commands == nullptr) {
            return;
        }
        auto const type = declared->events.find(event);
        runEvent(commands, ctx, componentPayload(payload, type == declared->events.end() ? nullptr : &type->second));
    };
    return ui::custom(std::move(spec));
}

namespace {

/// @brief One child of an arranged container, as the user's choice places it.
struct ArrangedChild {
    /// @brief The child's id.
    std::string id;
    /// @brief Whether the choice hides it.
    bool hidden = false;
};

/// @brief A container's children in the order a stored `arrange` choice gives, each marked shown or hidden.
///
/// A child the choice does not name keeps its declared place after the named ones, shown, and an entry that names no
/// child, names one twice or is not a `{"id", "hidden"}` record is skipped, so a new document version survives an old
/// choice (spec 5 §12b). A choice that is not a list leaves the declared order. Only `"hidden": true` hides.
/// @param stored   The point's current choice.
/// @param declared The container's children, in document order.
/// @return Every child, in order.
[[nodiscard]] std::vector<ArrangedChild> arrangedChildren(Value const& stored,
                                                          std::span<DocNode const* const> declared) {
    auto const isDeclared = [declared](std::string const& childId) {
        return std::ranges::find(declared, childId, &DocNode::id) != declared.end();
    };
    std::vector<ArrangedChild> out;
    std::set<std::string, std::less<>> placed;
    if (ValueList const* const entries = stored.asList()) {
        for (Value const& entry : *entries) {
            Value const* const named = entry.field("id");
            std::string const* const name = named == nullptr ? nullptr : named->asString();
            if (name == nullptr || !isDeclared(*name) || !placed.insert(*name).second) {
                continue;
            }
            Value const* const hidden = entry.field("hidden");
            bool const* const flag = hidden == nullptr ? nullptr : hidden->asBool();
            out.push_back(ArrangedChild{.id = *name, .hidden = flag != nullptr && *flag});
        }
    }
    for (DocNode const* const child : declared) {
        if (!placed.contains(child->id)) {
            out.push_back(ArrangedChild{.id = child->id});
        }
    }
    return out;
}

/// @brief The ids of the children an `arrange` choice shows, in its order (`arrangedChildren`, without the hidden).
/// @param stored   The point's current choice.
/// @param declared The container's children, in document order.
/// @return The ids to show, in order.
[[nodiscard]] std::vector<Value> arrangedOrder(Value const& stored, std::span<DocNode const* const> declared) {
    std::vector<Value> order;
    for (ArrangedChild const& child : arrangedChildren(stored, declared)) {
        if (!child.hidden) {
            order.emplace_back(child.id);
        }
    }
    return order;
}

/// @brief An arrangement as the choice an `arrange` point stores.
/// @param children Every child, in order.
/// @return `[{"id", "hidden"}, …]`.
[[nodiscard]] Value arrangementValue(std::vector<ArrangedChild> const& children) {
    ValueList entries;
    entries.reserve(children.size());
    for (ArrangedChild const& child : children) {
        entries.emplace_back(ValueRecord{{"id", Value{child.id}}, {"hidden", Value{child.hidden}}});
    }
    return Value{std::move(entries)};
}

/// @brief The element of a document tree with @p elementId.
/// @param node The subtree.
/// @param elementId The element's id.
/// @return The element, or `nullptr` when the subtree has none with that id.
// NOLINTNEXTLINE(misc-no-recursion): one level per level of the document tree.
[[nodiscard]] DocNode const* elementNode(DocNode const& node, std::string_view elementId) {
    if (node.id == elementId) {
        return &node;
    }
    for (auto const* const groups : {&node.children, &node.parts}) {
        for (auto const& [slot, nodes] : *groups) {
            static_cast<void>(slot);
            for (DocNodePtr const& child : nodes) {
                if (DocNode const* const found = elementNode(*child, elementId)) {
                    return found;
                }
            }
        }
    }
    return nullptr;
}
}  // namespace

std::string ScreenCore::arrangedBy(std::string const& elementId) const {
    if (elementId.empty()) {
        return {};
    }
    for (auto const& [name, point] : _doc->customize) {
        if (point.kind == "arrange" && point.of == elementId) {
            return name;
        }
    }
    return {};
}

// A keyed list over the children's ids rather than a stack, so a new choice moves the shown widgets, keeping their
// state and focus, and mounts or unmounts only what it shows or hides. A child is lowered in the container's own
// scope: arranging changes where it is, not what it reads.
Node ScreenCore::lowerArranged(DocNode const& node, LowerCtx const& ctx, ElementState* element,
                               std::string const& point, Axis axis, int gap) {
    std::vector<DocNode const*> declared;
    for (DocNodePtr const& child : node.nodes("children")) {
        declared.push_back(child.get());
    }
    auto order = [this, point, declared] { return arrangedOrder(prefs.get().at(point), declared); };
    auto key = [](Value const& childId) { return keyOf(childId); };
    LowerCtx childCtx = ctx;
    childCtx.listGap = axis == Axis::Vertical ? gap : 0;
    auto view = [this, childCtx, declared](reactive::Signal<Value> const& row) -> Node {
        // The order lists only declared ids, so the lookup always finds the child.
        auto const found = std::ranges::find(declared, textOf(row.peek(), session.formatting()), &DocNode::id);
        return lower(**found, childCtx);
    };
    ForEach spec{
        .model = std::make_shared<TypedForEach<Value> const>(std::move(order), std::move(key), std::move(view)),
        .axis = axis,
        .gap = gap,
    };
    commonOf(node, ctx, element, spec.common);
    return makeNode(std::move(spec));
}

// One row per child of the arranged container, hidden ones included: a checkbox shows or hides it, and two buttons
// move it a place. Each edit stores the whole arrangement, so the stored list always names every child.
Node ScreenCore::lowerArrangeEditor(DocNode const& node, LowerCtx const& ctx, std::string const& point,
                                    CustomizePoint const& spec, std::function<void(Value)> const& choose) {
    // The loader checked that `of` names a column or row of this screen.
    DocNode const* const container = elementNode(*_doc->view, spec.of);
    std::vector<DocNode const*> declared;
    for (DocNodePtr const& child : container->nodes("children")) {
        declared.push_back(child.get());
    }
    auto const current = [this, point, declared] { return arrangedChildren(prefs.get().at(point), declared); };
    auto const place = [current](std::string const& childId) {
        auto const children = current();
        return static_cast<std::size_t>(std::ranges::find(children, childId, &ArrangedChild::id) - children.begin());
    };
    // Applies an edit to the arrangement as it stands, read untracked: the edit is a transition, not a binding.
    auto const edit = [this, current, choose](std::string const& childId, auto const& change) {
        auto children = runtime().untracked(current);
        auto const found = std::ranges::find(children, childId, &ArrangedChild::id);
        change(children, static_cast<std::size_t>(found - children.begin()));
        choose(arrangementValue(children));
    };
    auto rows = [current] {
        std::vector<Value> ids;
        for (ArrangedChild const& child : current()) {
            ids.emplace_back(child.id);
        }
        return ids;
    };
    auto key = [](Value const& childId) { return keyOf(childId); };
    auto view = [this, current, place, edit](reactive::Signal<Value> const& row) -> Node {
        std::string const childId = textOf(row.peek(), session.formatting());
        Checkbox shown{.label = childId};
        shown.checked = slotProp<bool>([current, childId] {
            auto const children = current();
            return !std::ranges::find(children, childId, &ArrangedChild::id)->hidden;
        });
        shown.onToggle = [edit, childId](bool checked) {
            edit(childId, [checked](std::vector<ArrangedChild>& children, std::size_t index) {
                children.at(index).hidden = !checked;
            });
        };
        Button moveUp{.label = std::string{"\xE2\x86\x91"}};
        moveUp.onClick = [edit, childId] {
            // Clamped rather than refused: at the edge the child swaps with itself, which stores the same order.
            edit(childId, [](std::vector<ArrangedChild>& children, std::size_t index) {
                std::swap(children.at(index), children.at(std::max(index, std::size_t{1}) - 1));
            });
        };
        moveUp.common.enabled = slotProp<bool>([place, childId] { return place(childId) > 0; });
        moveUp.common.a11y.name = "Move " + childId + " up";
        moveUp.common.testId = "arrange-up-" + childId;
        Button moveDown{.label = std::string{"\xE2\x86\x93"}};
        moveDown.onClick = [edit, childId] {
            edit(childId, [](std::vector<ArrangedChild>& children, std::size_t index) {
                std::swap(children.at(index), children.at(std::min(index + 1, children.size() - 1)));
            });
        };
        moveDown.common.enabled =
            slotProp<bool>([current, place, childId] { return place(childId) + 1 < current().size(); });
        moveDown.common.a11y.name = "Move " + childId + " down";
        moveDown.common.testId = "arrange-down-" + childId;
        shown.common.testId = "arrange-shown-" + childId;
        return ui::row(Row{
            .children =
                {
                    ui::checkbox(std::move(shown)),
                    ui::button(std::move(moveUp)),
                    ui::button(std::move(moveDown)),
                },
            .gap = 1,
        });
    };
    ForEach editor{
        .model = std::make_shared<TypedForEach<Value> const>(std::move(rows), std::move(key), std::move(view)),
    };
    commonOf(node, ctx, nullptr, editor.common);
    return makeNode(std::move(editor));
}

Node ScreenCore::lowerCustomize(DocNode const& node, LowerCtx const& ctx) {
    std::string const point{node.name("point")};
    // A loaded document's customize node names a declared point, and every point has a choice in `prefs`.
    CustomizePoint const& spec = _doc->customize.at(point);
    auto const choose = [this, point](Value value) {
        session.asUser([&] {
            ValueRecord next = prefs.peek();
            next.insert_or_assign(point, value);
            prefs.set(std::move(next));
        });
        if (host != nullptr) {
            host->preferenceChanged(_doc->id, point, value);
        }
    };
    if (spec.kind == "arrange") {
        return lowerArrangeEditor(node, ctx, point, spec, choose);
    }
    if (spec.kind == "choice") {
        Select select;
        std::vector<SelectOption> options;
        options.reserve(spec.options.size());
        for (std::string const& option : spec.options) {
            options.push_back(SelectOption{.key = Key{option}, .label = option});
        }
        select.options = std::move(options);
        select.selected = slotProp<std::optional<Key>>([this, point] -> std::optional<Key> {
            Value const& chosen = prefs.get().at(point);
            return chosen.isNull() ? std::nullopt : std::optional<Key>{keyOf(chosen)};
        });
        select.onSelect = [choose](Key const& key) { choose(valueOfKey(key)); };
        commonOf(node, ctx, nullptr, select.common);
        return ui::select(std::move(select));
    }
    if (spec.kind == "value" && (spec.type.kind == TypeKind::String || spec.type.isAny())) {
        TextInput input;
        input.value =
            slotProp<std::string>([this, point] { return textOf(prefs.get().at(point), session.formatting()); });
        input.onChange = [choose](std::string text) { choose(Value{std::move(text)}); };
        commonOf(node, ctx, nullptr, input.common);
        return ui::textInput(std::move(input));
    }
    Text placeholder{.text = point, .role = TextRole::Muted};
    commonOf(node, ctx, nullptr, placeholder.common);
    return ui::text(std::move(placeholder));
}
// NOLINTEND(misc-no-recursion)

}  // namespace detail

std::vector<TextSpan> Screen::headSpans(ExprPtr const& expr) const {
    if (expr == nullptr) {
        return {};
    }
    bool dropped = false;
    bool badTone = false;
    std::vector<TextSpan> spans =
        detail::spansOf(detail::evalIn(*_scope, nullptr, expr), _scope->formatting(), dropped, &badTone);
    if (dropped) {
        _core->report(expr->path, detail::kSpansReason);
    }
    if (badTone) {
        _core->report(expr->path, detail::kSpanToneReason);
    }
    return spans;
}

DocNode const* Screen::shownRoot() const {
    DocNode const* node = _core->doc().view.get();
    while (node != nullptr && node->kind == "switch") {
        Value const value = detail::evalIn(*_scope, nullptr, node->prop("on"));
        std::span<DocNodePtr const> shown = node->nodes("default");
        for (DocNodePtr const& part : node->partList("cases")) {
            if (!(detail::evalIn(*_scope, nullptr, part->prop("when")) == value)) {
                continue;
            }
            if (part->scope.has_value() && detail::declaresNames(*part->scope)) {
                return nullptr;
            }
            shown = part->nodes("children");
            break;
        }
        node = shown.size() == 1 ? shown.front().get() : nullptr;
    }
    return node;
}

bool Screen::pageEdge() const {
    DocNode const* const node = shownRoot();
    return node != nullptr && node->flag("pageEdge");
}

bool Screen::headJoined() const {
    DocNode const* const node = shownRoot();
    return node != nullptr && node->flag("headJoined");
}

LeadPlacement Screen::leadPlacement() const {
    ExprPtr const& placement = _core->doc().leadPlacement;
    if (placement == nullptr) {
        return LeadPlacement::Below;
    }
    Value const value = detail::evalIn(*_scope, nullptr, placement);
    if (std::string const* const name = value.asString()) {
        if (*name == "inline") {
            return LeadPlacement::Inline;
        }
        if (*name == "end") {
            return LeadPlacement::End;
        }
        if (*name == "below") {
            return LeadPlacement::Below;
        }
    }
    if (!value.isNull()) {
        _core->report(placement->path, "`leadPlacement` is `below`, `inline` or `end`");
    }
    return LeadPlacement::Below;
}

TextRole Screen::titleRole() const {
    ExprPtr const& role = _core->doc().titleRole;
    if (role == nullptr) {
        return TextRole::Heading;
    }
    Value const value = detail::evalIn(*_scope, nullptr, role);
    if (std::string const* const name = value.asString()) {
        if (*name == "headline") {
            return TextRole::Headline;
        }
        if (*name == "heading") {
            return TextRole::Heading;
        }
    }
    if (!value.isNull()) {
        _core->report(role->path, "`titleRole` is `heading` or `headline`");
    }
    return TextRole::Heading;
}

std::string Screen::headSurface() const {
    Value const value = detail::evalIn(*_scope, nullptr, _core->doc().headSurface);
    std::string const* const name = value.asString();
    return name == nullptr ? std::string{} : *name;
}

std::string Screen::pageSurface() const {
    Value const value = detail::evalIn(*_scope, nullptr, _core->doc().pageSurface);
    std::string const* const name = value.asString();
    return name == nullptr ? std::string{} : *name;
}

std::vector<TextSpan> Screen::subtitleSpans() const { return headSpans(_core->doc().subtitleSpans); }

std::vector<TextSpan> Screen::titleSpans() const { return headSpans(_core->doc().titleSpans); }

std::vector<TextSpan> Screen::eyebrowSpans() const { return headSpans(_core->doc().eyebrowSpans); }

BorrowedModel Screen::lend(std::string_view alias) const {
    detail::NameRef const* const found = _scope->find(alias);
    if (found == nullptr || found->category != detail::NameCategory::Model) {
        return {};
    }
    detail::ModelAlias& model = *found->model;
    return BorrowedModel{
        .channel = detail::channelOf(model, *_core),
        .token = _core->runtime().untracked([&] { return model.token->get(); }),
    };
}

std::expected<std::unique_ptr<Screen>, Refusal> Interpreter::mount(std::shared_ptr<Document const> doc,
                                                                   ValueRecord const& params, IViewBackend& backend,
                                                                   MountOptions const& options) {
    auto const refuse = [&](std::string path, std::string reason) {
        return std::unexpected(Refusal{.document = doc->id, .path = std::move(path), .reason = std::move(reason)});
    };
    if (!doc->view) {
        return refuse("", "the document has no view");
    }
    ValueRecord given;
    for (auto const& [name, type] : doc->params) {
        auto const found = params.find(name);
        Value value = found == params.end() ? Value{} : found->second;
        if (!type.admits(value)) {
            return refuse(pointerAppend("/params", name), value.isNull()
                                                              ? "param `" + name + "` is missing"
                                                              : "param `" + name + "` is not a " + type.name());
        }
        given.insert_or_assign(name, std::move(value));
    }
    for (ModelDecl const& model : doc->scope.models) {
        if (model.instance == InstanceMode::Borrowed && !options.borrowed.contains(model.name)) {
            return refuse(model.path, "alias `" + model.name + "` borrows a model its opener did not lend");
        }
    }
    reactive::Runtime& runtime = _session->runtime();
    std::unique_ptr<Screen> screen{new Screen{}};
    screen->_core = std::make_unique<detail::ScreenCore>(doc, *_session, *_dispatcher, _options, options);
    detail::ScreenCore& core = *screen->_core;
    if (options.keepStateOf != nullptr) {
        core.keptStates = options.keepStateOf->ownState();
    }
    screen->_scope = std::make_shared<detail::RtScope>(core, nullptr, nullptr, options.depth);
    core.rootScope = screen->_scope;
    detail::RtScope& scope = *screen->_scope;
    scope.submitted = &core.submitted;
    scope.showErrors = doc->showErrors;
    for (auto const& [name, value] : given) {
        detail::NameRef ref;
        ref.category = detail::NameCategory::Param;
        ref.param = value;
        scope.names.insert_or_assign(name, std::move(ref));
    }
    std::vector<Commands const*> lists;
    for (auto const& [key, commands] : doc->lifecycle) {
        static_cast<void>(key);
        lists.push_back(&commands);
    }
    for (auto const& [chord, commands] : doc->keys) {
        static_cast<void>(chord);
        lists.push_back(&commands);
    }
    detail::headCommandLists(*doc, lists);
    Node root = runtime.untracked([&] {
        core.declare(scope, &doc->scope, {doc->view.get()}, lists);
        detail::LowerCtx const ctx{
            .scope = screen->_scope,
            .bindings = std::make_shared<detail::Bindings const>(),
            .boundary = nullptr,
        };
        Node lowered = core.lower(*doc->view, ctx);
        screen->_headStatus = core.lowerBadgeParts(doc->status, ctx);
        screen->_headActions = core.lowerActionParts(doc->actions, ctx);
        screen->_headMenu = core.lowerHeadMenu(doc->menu, ctx);
        screen->_headTools = core.lowerHeadTools(doc->tools, ctx);
        if (!doc->keys.empty()) {
            std::vector<KeyBinding> keys;
            for (auto const& [chord, commands] : doc->keys) {
                keys.push_back(KeyBinding{
                    .chord = chord,
                    .onPress = [&core, ctx, list = &commands] { core.runEvent(list, ctx, Value{}); },
                });
            }
            Column column{.children = {std::move(lowered)}};
            column.common.keys = std::move(keys);
            lowered = ui::column(std::move(column));
        }
        return lowered;
    });
    screen->_mounted = std::make_unique<Mounted>(runtime, backend, std::move(root), options.parent, options.depth + 1);
    screen->_core->focusElement = [mounted = screen->_mounted.get()](std::string_view element) {
        return mounted->focus(element);
    };
    screen->_core->clipboard = [&backend](std::string_view text) { return backend.copyText(text); };
    screen->_core->pasteboard = [&backend] { return backend.pasteText(); };
    screen->runLifecycle("onMount");
    return screen;
}

std::expected<std::unique_ptr<AppRoot>, Refusal> Interpreter::mountApp(std::shared_ptr<Document const> doc) {
    if (doc->id != "app") {
        return std::unexpected(
            Refusal{.document = doc->id, .path = "/id", .reason = "the app document's id is `app`"});
    }
    if (_session->appScope() != nullptr) {
        return std::unexpected(
            Refusal{.document = doc->id, .path = {}, .reason = "an app document is already mounted on the session"});
    }
    std::unique_ptr<AppRoot> root{new AppRoot{}};
    root->_core = std::make_unique<detail::ScreenCore>(doc, *_session, *_dispatcher, _options, MountOptions{});
    detail::ScreenCore& core = *root->_core;
    root->_scope = std::make_shared<detail::RtScope>(core, nullptr, nullptr, 0);
    core.rootScope = root->_scope;
    std::vector<Commands const*> lists;
    for (auto const& [key, commands] : doc->lifecycle) {
        static_cast<void>(key);
        lists.push_back(&commands);
    }
    _session->runtime().untracked([&] {
        core.declare(*root->_scope, &doc->scope, {}, lists);
        detail::LowerCtx const ctx{
            .scope = root->_scope,
            .bindings = std::make_shared<detail::Bindings const>(),
            .boundary = nullptr,
        };
        std::vector<detail::BadgeModel> badges = core.lowerBadgeParts(doc->navigation, ctx);
        for (std::size_t index = 0; index < badges.size(); ++index) {
            root->_navigation.push_back(AppRoot::PlacedBadge{
                .screen = std::string{doc->navigation.at(index)->name("screen")},
                .badge = std::move(badges.at(index)),
            });
        }
    });
    root->_navigationBadges = std::make_unique<reactive::Computed<std::vector<NavigationBadge>>>(
        _session->runtime(), [placed = &root->_navigation] {
            std::vector<NavigationBadge> shown;
            for (AppRoot::PlacedBadge const& entry : *placed) {
                if (entry.badge.visible.evaluate()) {
                    shown.push_back(NavigationBadge{
                        .screen = entry.screen,
                        .text = entry.badge.text.evaluate(),
                        .tone = entry.badge.tone.evaluate(),
                    });
                }
            }
            return shown;
        });
    // The app document's date patterns are the application's: every screen of the bundle shows and reads dates so.
    if (!doc->datePattern.empty() || !doc->dateTimePattern.empty()) {
        _session->setDatePatterns(doc->datePattern, doc->dateTimePattern);
    }
    _session->setAppScope(root->_scope.get());
    if (auto const found = doc->lifecycle.find("onMount"); found != doc->lifecycle.end()) {
        root->runCommands(found->second);
    }
    return root;
}

}  // namespace morph::ui
