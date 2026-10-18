// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../reactive/runtime.hpp"
#include "../reactive/scheduler.hpp"
#include "../reactive/scope.hpp"
#include "backend.hpp"
#include "host.hpp"
#include "route.hpp"
#include "value.hpp"

/// @file
/// @brief The workspace shell (spec 6 §7, §15): tabs over screens, mounted through a `DocumentScreenHost`.
///
/// Specified in `docs/spec/ui/workspace.md`.

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

/// @brief How a `Workspace` treats screens.
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
    /// @brief Called after the open tabs, their captions or the active tab changed; empty calls nothing.
    std::function<void()> onChanged;
};

/// @brief One open tab, as an application's tab menu or a test reads it.
struct WorkspaceTab {
    /// @brief The screen id.
    std::string screenId;
    /// @brief The screen's identity; `null` when it has none.
    Value identity;
    /// @brief The caption the bar shows: the title, with ` •` while there are unsaved changes.
    std::string caption;
};

/// @brief Tabs over screens: one screen instance per tab, mounted into the tab's page through
///        `DocumentScreenHost::mountInto`.
///
/// Tabs keep the order they were opened in. Opening a screen whose identity an open tab already has focuses that
/// tab; a singleton screen has one tab. Closing runs each screen's close protocol (spec 6 §3), one tab at a time,
/// and a walk over several tabs stops at the first veto. The shell takes Ctrl+Tab, Ctrl+Shift+Tab and Ctrl+W, which
/// documents may not declare. Neither copyable nor movable; owned by the owner thread of its runtime.
class Workspace {
public:
    /// @brief The shell, its tab bar appended to @p parent.
    /// @param runtime The runtime the screens' reactive nodes belong to. Borrowed.
    /// @param backend The backend the bar and the pages are made with. Borrowed.
    /// @param host    What mounts the screens. Borrowed; it outlives the shell.
    /// @param options The screens' catalog facts.
    /// @param parent  The container the shell is appended to; null makes it a backend root.
    Workspace(reactive::Runtime& runtime MORPH_LIFETIMEBOUND, IViewBackend& backend MORPH_LIFETIMEBOUND,
              DocumentScreenHost& host MORPH_LIFETIMEBOUND, WorkspaceOptions options = {},
              ContainerWidget* parent = nullptr)
        : _runtime{&runtime},
          _backend{&backend},
          _host{&host},
          _options{std::move(options)},
          _bar{backend.createTabs(parent)} {
        // The bar and each page fill what they are given, so a screen whose root stretches fills the page.
        _bar->setLayout({.width = Sizing::stretch(), .height = Sizing::stretch()});
        _bar->setTabs({});
        _bar->setSelected(kNone);
        _bar->setOnSelect([this](std::size_t index) {
            if (index < _tabs.size()) {
                activateTab(_tabs.at(index)->id, false);
            }
        });
        _bar->setKeys({"Ctrl+Tab", "Ctrl+Shift+Tab", "Ctrl+W"}, [this](std::string const& chord) {
            if (chord == "Ctrl+Tab") {
                next();
            } else if (chord == "Ctrl+Shift+Tab") {
                previous();
            } else if (std::optional<std::size_t> const shown = active()) {
                close(*shown, {});
            }
        });
    }

    /// @brief Unmounts every screen without asking, newest first.
    ~Workspace() {
        while (!_tabs.empty()) {
            _tabs.pop_back();
        }
    }

    Workspace(Workspace const&) = delete;
    Workspace& operator=(Workspace const&) = delete;
    Workspace(Workspace&&) = delete;
    Workspace& operator=(Workspace&&) = delete;

    /// @brief The tab bar: the shell's root widget, which the host places.
    /// @return It.
    [[nodiscard]] TabsWidget& root() noexcept { return *_bar; }

    /// @brief Opens a screen, or focuses the tab that already shows it.
    ///
    /// The tab whose screen and identity match is focused; a singleton screen's tab is focused; a `reuseEmpty`
    /// screen opened with an identity replaces its tab that has no identity and no unsaved changes; otherwise a new
    /// tab is opened. The tab opened or focused is the active one.
    /// @param screenId The screen id.
    /// @param params   Its params.
    /// @return The tab's index, or why the screen could not be mounted; a refused mount opens nothing.
    std::expected<std::size_t, MountError> open(std::string_view screenId, ParamMap const& params) {
        WorkspaceScreen const facts = _options.screens ? _options.screens(screenId) : WorkspaceScreen{};
        std::optional<std::string> const identity = _host->identityOf(screenId, params);
        for (std::size_t index = 0; index < _tabs.size(); ++index) {
            Tab const& tab = *_tabs.at(index);
            if (tab.screenId != screenId) {
                continue;
            }
            if (facts.singleton || (identity && identityText(tab) == *identity)) {
                activateTab(tab.id, false);
                return index;
            }
        }
        std::optional<std::uint64_t> replaced;
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
        std::uint64_t const tabId = *opened;
        if (replaced) {
            std::size_t const where = indexOf(*replaced);
            moveTab(tabId, where);
            removeTab(*replaced, true);
        }
        activateTab(tabId, false);
        return indexOf(tabId);
    }

    /// @brief Opens the start screens: with `resume`, every recorded route whose screen is known and available, in
    ///        order, and the fallback when none of them opened; otherwise the `start` screen. The first tab opened is
    ///        the active one.
    /// @param how The start screen, the fallback and the record.
    /// @return The number of tabs opened.
    std::size_t start(WorkspaceStart const& how) {
        std::size_t opened = 0;
        std::optional<std::size_t> first;
        auto const openOne = [&](std::string_view screenId, ParamMap const& params) {
            if (auto const index = open(screenId, params)) {
                ++opened;
                if (!first) {
                    first = *index;
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
        if (first) {
            activate(*first);
        }
        return opened;
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

    /// @brief Shows a tab and makes it the most recently used.
    /// @param index The tab; past the last one does nothing.
    void activate(std::size_t index) {
        if (index < _tabs.size()) {
            activateTab(_tabs.at(index)->id, false);
        }
    }

    /// @brief Shows the next tab in most-recently-used order (Ctrl+Tab). Repeated calls walk the order as it was at
    ///        the first of them; any other activation starts a new walk.
    void next() { walk(1); }

    /// @brief Shows the previous tab in most-recently-used order (Ctrl+Shift+Tab).
    void previous() { walk(-1); }

    /// @brief Closes a tab through its screen's close protocol.
    /// @param index The tab.
    /// @param done  Called once with whether it closed; may be empty.
    void close(std::size_t index, std::function<void(bool)> done) {
        if (index >= _tabs.size()) {
            if (done) {
                done(false);
            }
            return;
        }
        closeEach({_tabs.at(index)->id}, std::move(done));
    }

    /// @brief Closes every tab but one, one at a time, stopping at the first veto.
    /// @param index The tab kept.
    /// @param done  Called once with whether every other tab closed; may be empty.
    void closeOthers(std::size_t index, std::function<void(bool)> done) {
        std::vector<std::uint64_t> ids;
        ids.reserve(_tabs.size());
        for (std::size_t where = 0; where < _tabs.size(); ++where) {
            if (where != index) {
                ids.push_back(_tabs.at(where)->id);
            }
        }
        closeEach(std::move(ids), std::move(done));
    }

    /// @brief Closes every tab, one at a time, stopping at the first veto.
    /// @param done Called once with whether every tab closed; may be empty.
    void closeAll(std::function<void(bool)> done) {
        std::vector<std::uint64_t> ids;
        ids.reserve(_tabs.size());
        for (auto const& tab : _tabs) {
            ids.push_back(tab->id);
        }
        closeEach(std::move(ids), std::move(done));
    }

    /// @brief Closes every tab without unsaved changes, one at a time, stopping at the first veto.
    /// @param done Called once with whether each of them closed; may be empty.
    void closeSaved(std::function<void(bool)> done) {
        std::vector<std::uint64_t> ids;
        ids.reserve(_tabs.size());
        for (auto const& tab : _tabs) {
            if (!tab->handle->dirty().peek()) {
                ids.push_back(tab->id);
            }
        }
        closeEach(std::move(ids), std::move(done));
    }

    /// @brief The active tab's index.
    /// @return It, or `std::nullopt` while no tab is open.
    [[nodiscard]] std::optional<std::size_t> active() const {
        if (!_active) {
            return std::nullopt;
        }
        return indexOf(*_active);
    }

    /// @brief The screen handle of a tab: what a client remounts for development reload and inspects.
    /// @param index The tab's index.
    /// @return Its handle, valid while the tab is open; null past the end.
    [[nodiscard]] ScreenHandle* handleOf(std::size_t index) const noexcept {
        return index < _tabs.size() ? _tabs.at(index)->handle.get() : nullptr;
    }

    /// @brief The active tab's screen handle.
    /// @return It, or null while no tab is open.
    [[nodiscard]] ScreenHandle* activeHandle() const noexcept {
        std::optional<std::size_t> const shown = active();
        return shown ? handleOf(*shown) : nullptr;
    }

    /// @brief Closes every tab without asking, newest first: what signing out does, since no close guard may keep a
    ///        screen of the principal who is leaving.
    void forceCloseAll() {
        while (!_tabs.empty()) {
            removeTab(_tabs.back()->id, true);
        }
    }

    /// @brief The open tabs, in order.
    /// @return Each tab's screen, identity and caption.
    [[nodiscard]] std::vector<WorkspaceTab> tabs() const {
        std::vector<WorkspaceTab> out;
        out.reserve(_tabs.size());
        for (auto const& tab : _tabs) {
            out.push_back(WorkspaceTab{
                .screenId = tab->screenId, .identity = tab->handle->identity().peek(), .caption = captionOf(*tab)});
        }
        return out;
    }

private:
    static constexpr std::size_t kNone = static_cast<std::size_t>(-1);

    // One tab: its reactive scope, its screen and its page, destroyed in that order.
    struct Tab {
        std::uint64_t id = 0;
        std::string screenId;
        // The route the record lists; empty for an instance with none.
        std::string route;
        std::unique_ptr<SlotWidget> page;
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
            page.reset();
        }
    };

    [[nodiscard]] static std::string identityText(Tab const& tab) {
        Value const identity = tab.handle->identity().peek();
        return identity.isNull() ? std::string{} : detail::canonicalJson(identity);
    }

    [[nodiscard]] static std::string captionOf(Tab const& tab) {
        std::string caption = tab.handle->title().peek();
        if (tab.handle->dirty().peek()) {
            caption += " •";
        }
        return caption;
    }

    [[nodiscard]] std::size_t indexOf(std::uint64_t tabId) const {
        auto const found = std::ranges::find_if(_tabs, [tabId](auto const& tab) { return tab->id == tabId; });
        return found == _tabs.end() ? kNone : static_cast<std::size_t>(found - _tabs.begin());
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

    // Mounts a screen into a new page at the end; the page goes with a refused mount. A host screen comes from the
    // registry, a document screen from the document host.
    std::expected<std::uint64_t, MountError> mountTab(std::string_view screenId, ParamMap const& params) {
        WorkspaceScreen const facts = factsOf(screenId);
        auto tab = std::make_unique<Tab>();
        tab->id = ++_lastId;
        tab->screenId = std::string{screenId};
        tab->page = _backend->createSlot(_bar.get());
        tab->page->setLayout({.width = Sizing::stretch(), .height = Sizing::stretch()});
        tab->page->setVisible(false);
        std::expected<std::unique_ptr<ScreenHandle>, MountError> handle;
        if (facts.host.empty()) {
            handle = _host->mountInto(screenId, params, *tab->page);
            if (auto route = _host->routeOf(screenId, params)) {
                tab->route = std::move(*route);
            }
        } else {
            HostScreenFactory const* const factory =
                _options.hosts == nullptr ? nullptr : _options.hosts->find(facts.host);
            if (factory == nullptr) {
                return std::unexpected(MountError{.screenId = std::string{screenId},
                                                  .reason = "no host screen `" + facts.host + "` is registered"});
            }
            handle = (*factory)(params, *tab->page);
            tab->route = std::string{screenId};
        }
        if (!handle) {
            return std::unexpected(std::move(handle.error()));
        }
        tab->handle = std::move(*handle);
        tab->scope = std::make_unique<reactive::Scope>(*_runtime);
        ScreenHandle const* const screen = tab->handle.get();
        tab->scope->effect([this, screen] {
            static_cast<void>(screen->title().get());
            static_cast<void>(screen->dirty().get());
            _runtime->untracked([this] { showCaptions(); });
        });
        std::uint64_t const tabId = tab->id;
        _tabs.push_back(std::move(tab));
        showCaptions();
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

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a tab id, then the index it moves to.
    void moveTab(std::uint64_t tabId, std::size_t target) {
        std::size_t const from = indexOf(tabId);
        if (from == kNone || from == target) {
            return;
        }
        std::unique_ptr<Tab> tab = std::move(_tabs.at(from));
        _tabs.erase(_tabs.begin() + static_cast<std::ptrdiff_t>(from));
        _bar->moveChild(*tab->page, target);
        _tabs.insert(_tabs.begin() + static_cast<std::ptrdiff_t>(target), std::move(tab));
        showCaptions();
    }

    void removeTab(std::uint64_t tabId, bool force) {
        std::size_t const where = indexOf(tabId);
        if (where == kNone) {
            return;
        }
        if (force) {
            _tabs.at(where)->handle->forceClose();
        }
        _tabs.erase(_tabs.begin() + static_cast<std::ptrdiff_t>(where));
        std::erase(_recent, tabId);
        _walk.reset();
        recordChanged();
        if (_active == tabId) {
            _active.reset();
            if (!_recent.empty()) {
                activateTab(_recent.front(), false);
                return;
            }
        }
        showCaptions();
    }

    void activateTab(std::uint64_t tabId, bool walking) {
        if (!walking) {
            _walk.reset();
        }
        _active = tabId;
        std::erase(_recent, tabId);
        _recent.insert(_recent.begin(), tabId);
        for (auto const& tab : _tabs) {
            tab->page->setVisible(tab->id == tabId);
        }
        showCaptions();
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
        std::uint64_t const tabId = _walk->order.at(_walk->position);
        // A walk keeps its own order; activating moves the tab to the front of the recent list only when it ends.
        _active = tabId;
        for (auto const& tab : _tabs) {
            tab->page->setVisible(tab->id == tabId);
        }
        std::erase(_recent, tabId);
        _recent.insert(_recent.begin(), tabId);
        showCaptions();
    }

    // Closes the tabs one at a time, stopping at the first that stays open.
    void closeEach(std::vector<std::uint64_t> ids, std::function<void(bool)> done) {
        std::erase_if(ids, [this](std::uint64_t tabId) { return indexOf(tabId) == kNone; });
        if (ids.empty()) {
            if (done) {
                done(true);
            }
            return;
        }
        std::uint64_t const tabId = ids.front();
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

    void showCaptions() {
        std::vector<std::string> captions;
        captions.reserve(_tabs.size());
        for (auto const& tab : _tabs) {
            captions.push_back(captionOf(*tab));
        }
        _bar->setTabs(captions);
        std::optional<std::size_t> const shown = active();
        _bar->setSelected(shown.value_or(kNone));
        if (_options.onChanged) {
            _options.onChanged();
        }
    }

    struct Walk {
        std::vector<std::uint64_t> order;
        std::size_t position = 0;
    };

    reactive::Runtime* _runtime;
    IViewBackend* _backend;
    DocumentScreenHost* _host;
    WorkspaceOptions _options;
    std::unique_ptr<TabsWidget> _bar;
    std::vector<std::unique_ptr<Tab>> _tabs;
    std::vector<std::uint64_t> _recent;
    std::optional<std::uint64_t> _active;
    std::optional<Walk> _walk;
    std::uint64_t _lastId = 0;
    reactive::TimerHandle _recordTimer;
    std::shared_ptr<bool> _alive = std::make_shared<bool>(true);
};

}  // namespace morph::ui
