// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../attributes.hpp"
#include "../reactive/runtime.hpp"
#include "../reactive/scheduler.hpp"
#include "../reactive/scope.hpp"
#include "../reactive/signal.hpp"
#include "host.hpp"
#include "route.hpp"
#include "value.hpp"

/// @file
/// @brief The workspace's tab logic with no widgets (spec 6 §7, §15): what an application's own tab strip binds to,
///        and what `ui::Workspace` draws as a tab bar.
///
/// Specified in `docs/spec/ui/workspace.md`, "Model".

namespace morph::ui {

/// @brief What the shell needs to know of one screen id: the catalog's `singleton` and `reuseEmpty`.
struct WorkspaceScreen {
    /// @brief The screen has at most one tab.
    bool singleton = false;
    /// @brief A tab of the screen with no identity and no unsaved changes is replaced, not stacked, when the screen
    ///        is opened with an identity.
    bool reuseEmpty = false;
    /// @brief The host screen the catalog entry names instead of a document; empty for a document screen.
    std::string host;
    /// @brief Whether the entry can be opened now; `resume` skips a recorded screen that cannot.
    bool available = true;
};

/// @brief How a workspace starts (spec 6 §7, `app-start`).
struct WorkspaceStart {
    /// @brief `resume` reopens the recorded tabs; anything else is the screen id to open.
    std::string start = "resume";
    /// @brief The screen opened when `resume` opens nothing; empty opens nothing.
    std::string fallback;
    /// @brief The record `resume` reads: the routes of the person's tabs when they were last written, in order.
    std::vector<std::string> recorded;
};

/// @brief How a workspace treats screens.
struct WorkspaceOptions {
    /// @brief The catalog facts of a screen id; empty treats every screen as plain.
    std::function<WorkspaceScreen(std::string_view)> screens;
    /// @brief The host screens a catalog entry's `host` names. Borrowed; null mounts none.
    ScreenHostRegistry const* hosts = nullptr;
    /// @brief Receives the record: the routes of the open tabs, in order, each time the tabs change. A tab with no
    ///        route (a param with no route form) is left out. Empty writes no record.
    std::function<void(std::vector<std::string> const&)> onRecord;
    /// @brief When given, a burst of changes writes the record once, `recordDelay` after the last of them. Borrowed.
    reactive::Scheduler* scheduler = nullptr;
    /// @brief The quiet period before the record is written, with a scheduler.
    std::chrono::milliseconds recordDelay{500};
    /// @brief Called after the open tabs, their titles, unsaved state or identities, or the active tab changed;
    ///        empty calls nothing.
    std::function<void()> onChanged;
};

/// @brief A tab's identity in a `WorkspaceModel`: issued when it opens, never reused.
struct WorkspaceTabId {
    /// @brief The id's number; 0 is never issued.
    std::uint64_t value = 0;

    /// @brief Ordered by issue.
    /// @return The ordering of the two values.
    auto operator<=>(WorkspaceTabId const&) const = default;
};

/// @brief One open tab, as an application's tab strip binds to it.
struct WorkspaceTabState {
    /// @brief The tab.
    WorkspaceTabId id;
    /// @brief The screen id.
    std::string screenId;
    /// @brief The screen's identity; `null` when it has none.
    Value identity;
    /// @brief The screen's title.
    std::string title;
    /// @brief Whether the screen has unsaved changes.
    bool dirty = false;

    /// @brief Memberwise equality.
    /// @return Whether every member matches.
    bool operator==(WorkspaceTabState const&) const = default;
};

/// @brief A `WorkspaceModel`'s options: the workspace's, and how a tab's page is made and unmade.
struct WorkspaceModelOptions : WorkspaceOptions {
    /// @brief Makes a tab's page and mounts the screen in it, a document screen or a host screen alike; the model owns
    ///        the handle it answers. Required.
    std::function<std::expected<std::unique_ptr<ScreenHandle>, MountError>(
        WorkspaceTabId tab, std::string_view screenId, ParamMap const& params)>
        mountPage;
    /// @brief Called after a tab's handle is destroyed, so the application destroys its page; may be empty.
    std::function<void(WorkspaceTabId tab)> unmountPage;
};

/// @brief The workspace's tabs with no widgets: which screens are open, in which order, which is active, and how
///        they open, close and are recorded (spec 6 §7, §15).
///
/// Opening a screen whose identity an open tab already has focuses that tab, a singleton screen has one tab, and a
/// `reuseEmpty` screen opened with an identity replaces its tab with no identity and no unsaved changes. Closing runs
/// each screen's close protocol (spec 6 §3), one tab at a time, and a walk over several tabs stops at the first veto.
/// `next` and `previous` walk the most recently used order. The page each tab shows is the application's:
/// `mountPage` makes it and mounts the screen there, `unmountPage` is told when it goes. Neither copyable nor movable;
/// owned by the owner thread of its runtime.
class WorkspaceModel {
public:
    /// @brief An empty workspace.
    /// @param runtime The runtime the screens' reactive nodes belong to. Borrowed.
    /// @param host    Answers identities and routes. Borrowed; it outlives the model.
    /// @param options The screens' catalog facts, the record, and the page callbacks.
    /// @throws std::invalid_argument when `mountPage` is empty.
    WorkspaceModel(reactive::Runtime& runtime MORPH_LIFETIMEBOUND, DocumentScreenHost& host MORPH_LIFETIMEBOUND,
                   WorkspaceModelOptions options)
        : _runtime{&runtime},
          _host{&host},
          _options{std::move(options)},
          _changes{runtime, 0},
          _activeTab{runtime, std::nullopt},
          _tabStates{runtime, [this] { return tabStates(); }},
          _activeState{runtime, [this] { return _activeTab.get(); }} {
        if (!_options.mountPage) {
            throw std::invalid_argument{"morph::ui::WorkspaceModel: mountPage is empty; it makes each tab's page"};
        }
    }

    /// @brief Unmounts every screen without asking, newest first.
    ~WorkspaceModel() {
        while (!_tabs.empty()) {
            unmountLast();
        }
    }

    WorkspaceModel(WorkspaceModel const&) = delete;
    WorkspaceModel& operator=(WorkspaceModel const&) = delete;
    WorkspaceModel(WorkspaceModel&&) = delete;
    WorkspaceModel& operator=(WorkspaceModel&&) = delete;

    /// @brief Opens a screen, or focuses the tab that already shows it; the tab opened or focused is the active one.
    /// @param screenId The screen id.
    /// @param params   Its params.
    /// @return The tab, or why the screen could not be mounted; a refused mount, or a `mountPage` that throws, opens
    ///         nothing.
    std::expected<WorkspaceTabId, MountError> open(std::string_view screenId, ParamMap const& params) {
        WorkspaceScreen const facts = factsOf(screenId);
        std::optional<std::string> const identity = _host->identityOf(screenId, params);
        for (auto const& tab : _tabs) {
            if (tab->screenId == screenId && (facts.singleton || (identity && identityText(*tab) == *identity))) {
                activateTab(tab->id, false);
                return tab->id;
            }
        }
        std::optional<WorkspaceTabId> replaced;
        if (facts.reuseEmpty && identity) {
            for (auto const& tab : _tabs) {
                if (tab->screenId == screenId && tab->handle->identity().peek().isNull() &&
                    !tab->handle->dirty().peek()) {
                    replaced = tab->id;
                    break;
                }
            }
        }
        auto opened = mountTab(screenId, params);
        if (!opened) {
            return std::unexpected(std::move(opened.error()));
        }
        WorkspaceTabId const tabId = *opened;
        if (replaced) {
            moveTab(tabId, indexOf(*replaced));
            removeTab(*replaced, true);
        }
        activateTab(tabId, false);
        return tabId;
    }

    /// @brief Opens the start screens: with `resume`, every recorded route whose screen is known and available, in
    ///        order, and the fallback when none of them opened; otherwise the `start` screen. The first tab opened is
    ///        the active one.
    /// @param how The start screen, the fallback and the record.
    /// @return The number of tabs opened.
    std::size_t start(WorkspaceStart const& how) {
        std::size_t opened = 0;
        std::optional<WorkspaceTabId> first;
        auto const openOne = [&](std::string_view screenId, ParamMap const& params) {
            if (auto const tab = open(screenId, params)) {
                ++opened;
                if (!first) {
                    first = *tab;
                }
            }
        };
        if (how.start == "resume") {
            for (std::string const& route : how.recorded) {
                std::optional<Route> const read = readRoute(route);
                if (read && factsOf(read->screen).available) {
                    openOne(read->screen, read->params);
                }
            }
            if (opened == 0 && !how.fallback.empty()) {
                openOne(how.fallback, {});
            }
        } else {
            openOne(how.start, {});
        }
        if (first && indexOf(*first) != kNone) {
            activateTab(*first, false);
        }
        return opened;
    }

    /// @brief Shows a tab and makes it the most recently used.
    /// @param tab The tab.
    /// @throws std::out_of_range for a tab the model does not hold.
    void activate(WorkspaceTabId tab) { activateTab(held(tab), false); }

    /// @brief Shows the next tab in most-recently-used order (Ctrl+Tab). Repeated calls walk the order as it was at
    ///        the first of them; any other activation starts a new walk.
    void next() { walk(1); }

    /// @brief Shows the previous tab in most-recently-used order (Ctrl+Shift+Tab).
    void previous() { walk(-1); }

    /// @brief Closes a tab through its screen's close protocol.
    /// @param tab  The tab.
    /// @param done Called once with whether it closed; may be empty.
    /// @throws std::out_of_range for a tab the model does not hold.
    void close(WorkspaceTabId tab, std::function<void(bool)> done) { closeEach({held(tab)}, std::move(done)); }

    /// @brief Closes every tab but one, one at a time, stopping at the first veto.
    /// @param tab  The tab kept.
    /// @param done Called once with whether every other tab closed; may be empty.
    /// @throws std::out_of_range for a tab the model does not hold.
    void closeOthers(WorkspaceTabId tab, std::function<void(bool)> done) {
        WorkspaceTabId const kept = held(tab);
        std::vector<WorkspaceTabId> ids;
        ids.reserve(_tabs.size());
        for (auto const& open : _tabs) {
            if (open->id != kept) {
                ids.push_back(open->id);
            }
        }
        closeEach(std::move(ids), std::move(done));
    }

    /// @brief Closes every tab, one at a time, stopping at the first veto.
    /// @param done Called once with whether every tab closed; may be empty.
    void closeAll(std::function<void(bool)> done) { closeEach(order(), std::move(done)); }

    /// @brief Closes every tab without unsaved changes, one at a time, stopping at the first veto.
    /// @param done Called once with whether each of them closed; may be empty.
    void closeSaved(std::function<void(bool)> done) {
        std::vector<WorkspaceTabId> ids;
        ids.reserve(_tabs.size());
        for (auto const& tab : _tabs) {
            if (!tab->handle->dirty().peek()) {
                ids.push_back(tab->id);
            }
        }
        closeEach(std::move(ids), std::move(done));
    }

    /// @brief Closes every tab without asking, newest first: what signing out does, since no close guard may keep a
    ///        screen of the principal who is leaving.
    void forceCloseAll() {
        while (!_tabs.empty()) {
            removeTab(_tabs.back()->id, true);
        }
    }

    /// @brief Writes the record now, cancelling a pending debounced write: what sign-out does before the session is
    ///        cleared.
    void flushRecord() {
        _recordTimer.cancel();
        if (_options.onRecord) {
            _options.onRecord(record());
        }
    }

    /// @brief The record as it would be written now: the routes of the open tabs, in order.
    /// @return The routes.
    [[nodiscard]] std::vector<std::string> record() const {
        std::vector<std::string> routes;
        routes.reserve(_tabs.size());
        for (auto const& tab : _tabs) {
            if (!tab->route.empty()) {
                routes.push_back(tab->route);
            }
        }
        return routes;
    }

    /// @brief The open tabs, in order, each with its screen, identity, title and unsaved state. A tab's values follow
    ///        its handle.
    /// @return The tabs, equality-gated: an Effect over it runs once per change.
    [[nodiscard]] reactive::Computed<std::vector<WorkspaceTabState>> const& tabs() const { return _tabStates; }

    /// @brief The active tab.
    /// @return It, or `std::nullopt` while no tab is open.
    [[nodiscard]] reactive::Computed<std::optional<WorkspaceTabId>> const& active() const { return _activeState; }

    /// @brief The open tabs' ids, in order, read without tracking: the order `tabs()` lists them in.
    /// @return The ids.
    [[nodiscard]] std::vector<WorkspaceTabId> order() const {
        std::vector<WorkspaceTabId> ids;
        ids.reserve(_tabs.size());
        for (auto const& tab : _tabs) {
            ids.push_back(tab->id);
        }
        return ids;
    }

    /// @brief A tab's screen handle: what a client remounts for development reload and inspects.
    /// @param tab The tab.
    /// @return Its handle, valid while the tab is open; null for a tab the model does not hold.
    [[nodiscard]] ScreenHandle* handleOf(WorkspaceTabId tab) const noexcept {
        std::size_t const where = indexOf(tab);
        return where == kNone ? nullptr : _tabs.at(where)->handle.get();
    }

private:
    static constexpr std::size_t kNone = static_cast<std::size_t>(-1);

    // One tab: the effect over its handle, then its handle, destroyed in that order.
    struct Tab {
        WorkspaceTabId id;
        std::string screenId;
        // The route the record lists; empty for an instance with none.
        std::string route;
        std::unique_ptr<ScreenHandle> handle;
        std::unique_ptr<reactive::Scope> scope;

        Tab() = default;
        Tab(Tab const&) = delete;
        Tab& operator=(Tab const&) = delete;
        Tab(Tab&&) = delete;
        Tab& operator=(Tab&&) = delete;
        ~Tab() {
            scope.reset();
            handle.reset();
        }
    };

    struct Walk {
        std::vector<WorkspaceTabId> order;
        std::size_t position = 0;
    };

    [[nodiscard]] static std::string identityText(Tab const& tab) {
        return identityKey(tab.handle->identity().peek()).value_or(std::string{});
    }

    [[nodiscard]] std::vector<WorkspaceTabState> tabStates() const {
        static_cast<void>(_changes.get());
        std::vector<WorkspaceTabState> states;
        states.reserve(_tabs.size());
        for (auto const& tab : _tabs) {
            states.push_back(WorkspaceTabState{.id = tab->id,
                                               .screenId = tab->screenId,
                                               .identity = tab->handle->identity().peek(),
                                               .title = tab->handle->title().peek(),
                                               .dirty = tab->handle->dirty().peek()});
        }
        return states;
    }

    [[nodiscard]] std::size_t indexOf(WorkspaceTabId tab) const noexcept {
        auto const found = std::ranges::find_if(_tabs, [tab](auto const& open) { return open->id == tab; });
        return found == _tabs.end() ? kNone : static_cast<std::size_t>(found - _tabs.begin());
    }

    // A tab the caller names must be one the model holds.
    [[nodiscard]] WorkspaceTabId held(WorkspaceTabId tab) const {
        if (indexOf(tab) == kNone) {
            throw std::out_of_range{"morph::ui::WorkspaceModel: tab " + std::to_string(tab.value) +
                                    " is not open (never issued, or closed)"};
        }
        return tab;
    }

    [[nodiscard]] WorkspaceScreen factsOf(std::string_view screenId) const {
        return _options.screens ? _options.screens(screenId) : WorkspaceScreen{};
    }

    // A recorded route read back: a host screen's route is its id, a document screen's is read against its params.
    [[nodiscard]] std::optional<Route> readRoute(std::string const& route) const {
        if (!factsOf(route).host.empty()) {
            return Route{.screen = route, .params = {}};
        }
        auto read = _host->parseRoute(route);
        if (!read) {
            return std::nullopt;
        }
        return std::move(*read);
    }

    // Asks for a new page at the end and mounts the screen in it; a refusal, or a throw, opens nothing.
    std::expected<WorkspaceTabId, MountError> mountTab(std::string_view screenId, ParamMap const& params) {
        WorkspaceTabId const tabId{++_lastId};
        std::expected<std::unique_ptr<ScreenHandle>, MountError> handle;
        try {
            handle = _options.mountPage(tabId, screenId, params);
        } catch (std::exception const& failure) {
            handle = std::unexpected(MountError{.screenId = std::string{screenId}, .reason = failure.what()});
        } catch (...) {
            handle = std::unexpected(MountError{.screenId = std::string{screenId}, .reason = "mountPage threw"});
        }
        if (handle && *handle == nullptr) {
            handle = std::unexpected(
                MountError{.screenId = std::string{screenId}, .reason = "mountPage answered no handle"});
        }
        if (!handle) {
            return std::unexpected(std::move(handle.error()));
        }
        auto tab = std::make_unique<Tab>();
        tab->id = tabId;
        tab->screenId = std::string{screenId};
        if (!factsOf(screenId).host.empty()) {
            tab->route = std::string{screenId};
        } else if (auto route = _host->routeOf(screenId, params)) {
            tab->route = std::move(*route);
        }
        tab->handle = std::move(*handle);
        tab->scope = std::make_unique<reactive::Scope>(*_runtime);
        ScreenHandle const* const screen = tab->handle.get();
        tab->scope->effect([this, screen] {
            static_cast<void>(screen->title().get());
            static_cast<void>(screen->dirty().get());
            static_cast<void>(screen->identity().get());
            _runtime->untracked([this] { changed(); });
        });
        _tabs.push_back(std::move(tab));
        changed();
        recordChanged();
        return tabId;
    }

    // The open tabs changed: the record is written now, or once the burst is over.
    void recordChanged() {
        if (!_options.onRecord) {
            return;
        }
        if (_options.scheduler == nullptr) {
            _options.onRecord(record());
            return;
        }
        std::weak_ptr<bool> const alive = _alive;
        _recordTimer = _options.scheduler->after(_options.recordDelay, [this, alive] {
            if (!alive.expired()) {
                flushRecord();
            }
        });
    }

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a tab, then the index it moves to.
    void moveTab(WorkspaceTabId tabId, std::size_t target) {
        std::size_t const from = indexOf(tabId);
        if (from == kNone || from == target) {
            return;
        }
        std::unique_ptr<Tab> tab = std::move(_tabs.at(from));
        _tabs.erase(_tabs.begin() + static_cast<std::ptrdiff_t>(from));
        _tabs.insert(_tabs.begin() + static_cast<std::ptrdiff_t>(target), std::move(tab));
        changed();
    }

    // Destroys the last tab's handle, then tells the application its page can go.
    void unmountLast() {
        WorkspaceTabId const tabId = _tabs.back()->id;
        _tabs.pop_back();
        if (_options.unmountPage) {
            _options.unmountPage(tabId);
        }
    }

    void removeTab(WorkspaceTabId tabId, bool force) {
        std::size_t const where = indexOf(tabId);
        if (where == kNone) {
            return;
        }
        if (force) {
            _tabs.at(where)->handle->forceClose();
        }
        _tabs.erase(_tabs.begin() + static_cast<std::ptrdiff_t>(where));
        if (_options.unmountPage) {
            _options.unmountPage(tabId);
        }
        std::erase(_recent, tabId);
        _walk.reset();
        recordChanged();
        if (_active == tabId) {
            setActive(std::nullopt);
            if (!_recent.empty()) {
                activateTab(_recent.front(), false);
                return;
            }
        }
        changed();
    }

    void activateTab(WorkspaceTabId tabId, bool walking) {
        if (!walking) {
            _walk.reset();
        }
        setActive(tabId);
        std::erase(_recent, tabId);
        _recent.insert(_recent.begin(), tabId);
        changed();
    }

    void walk(int step) {
        if (_tabs.size() < 2 || !_active) {
            return;
        }
        if (!_walk) {
            _walk = Walk{.order = _recent, .position = 0};
        }
        auto const count = static_cast<std::ptrdiff_t>(_walk->order.size());
        std::ptrdiff_t const next = (((static_cast<std::ptrdiff_t>(_walk->position) + step) % count) + count) % count;
        _walk->position = static_cast<std::size_t>(next);
        // A walk keeps its own order; the tab it shows moves to the front of the recent list.
        activateTab(_walk->order.at(_walk->position), true);
    }

    // Closes the tabs one at a time, stopping at the first that stays open.
    void closeEach(std::vector<WorkspaceTabId> ids, std::function<void(bool)> done) {
        std::erase_if(ids, [this](WorkspaceTabId tabId) { return indexOf(tabId) == kNone; });
        if (ids.empty()) {
            if (done) {
                done(true);
            }
            return;
        }
        WorkspaceTabId const tabId = ids.front();
        ids.erase(ids.begin());
        std::weak_ptr<bool> const alive = _alive;
        _tabs.at(indexOf(tabId))
            ->handle->requestClose(
                [this, alive, tabId, rest = std::move(ids), done = std::move(done)](bool closed) mutable {
                    if (alive.expired()) {
                        return;
                    }
                    if (!closed) {
                        if (done) {
                            done(false);
                        }
                        return;
                    }
                    removeTab(tabId, false);
                    closeEach(std::move(rest), std::move(done));
                });
    }

    void setActive(std::optional<WorkspaceTabId> tab) {
        _active = tab;
        _runtime->batch([this, tab] { _activeTab.set(tab); });
    }

    // Something a tab strip shows changed: `tabs()` follows, and the application is told.
    void changed() {
        _runtime->batch([this] { _changes.set(_changes.peek() + 1); });
        if (_options.onChanged) {
            _options.onChanged();
        }
    }

    reactive::Runtime* _runtime;
    DocumentScreenHost* _host;
    WorkspaceModelOptions _options;
    std::vector<std::unique_ptr<Tab>> _tabs;
    std::vector<WorkspaceTabId> _recent;
    std::optional<WorkspaceTabId> _active;
    std::optional<Walk> _walk;
    std::uint64_t _lastId = 0;
    reactive::TimerHandle _recordTimer;
    std::shared_ptr<bool> _alive = std::make_shared<bool>(true);
    reactive::Signal<std::uint64_t> _changes;
    reactive::Signal<std::optional<WorkspaceTabId>> _activeTab;
    reactive::Computed<std::vector<WorkspaceTabState>> _tabStates;
    reactive::Computed<std::optional<WorkspaceTabId>> _activeState;
};

}  // namespace morph::ui
