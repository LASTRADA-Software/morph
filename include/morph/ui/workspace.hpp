// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
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
#include "mount.hpp"
#include "route.hpp"
#include "value.hpp"
#include "view.hpp"
#include "workspace_model.hpp"

/// @file
/// @brief The workspace shell (spec 6 §7, §15): tabs over screens, mounted through a `DocumentScreenHost`.
///
/// Specified in `docs/spec/ui/workspace.md`.

namespace morph::ui {

/// @brief One open tab, as an application's tab menu or a test reads it.
struct WorkspaceTab {
    /// @brief The screen id.
    std::string screenId;
    /// @brief The screen's identity; `null` when it has none.
    Value identity;
    /// @brief The caption the bar shows: the title, with ` •` while there are unsaved changes.
    std::string caption;
};

/// @brief Tabs over screens: a tab bar over a `WorkspaceModel`, one screen instance per tab, mounted into the tab's
///        page through `DocumentScreenHost::mountInto`, with the screen's head row above it.
///
/// The model holds the tab logic: tabs keep the order they were opened in, opening a screen whose identity an open
/// tab already has focuses that tab, a singleton screen has one tab, and closing runs each screen's close protocol
/// (spec 6 §3), one tab at a time, a walk over several tabs stopping at the first veto. The bar shows the model's tabs
/// and takes Ctrl+Tab, Ctrl+Shift+Tab and Ctrl+W, which documents may not declare. Neither copyable nor movable;
/// owned by the owner thread of its runtime.
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
          _bar{backend.createTabs(parent)},
          _hosts{options.hosts},
          _screens{options.screens},
          _onChanged{std::move(options.onChanged)} {
        // The bar and each page fill what they are given, so a screen whose root stretches fills the page.
        _bar->setLayout({.width = Sizing::stretch(), .height = Sizing::stretch()});
        _bar->setTabs({});
        _bar->setSelected(kNone);
        _bar->setOnSelect([this](std::size_t index) {
            std::vector<WorkspaceTabId> const order = _model->order();
            if (index < order.size()) {
                _model->activate(order.at(index));
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
        WorkspaceModelOptions modelOptions{std::move(options), {}, {}};
        modelOptions.onChanged = [this] { showTabs(); };
        modelOptions.mountPage = [this](WorkspaceTabId tab, std::string_view screenId, ParamMap const& params) {
            return mountPage(tab, screenId, params);
        };
        modelOptions.unmountPage = [this](WorkspaceTabId tab) {
            std::erase(_shownOrder, tab);
            _pages.erase(tab.value);
        };
        _model = std::make_unique<WorkspaceModel>(runtime, host, std::move(modelOptions));
    }

    /// @brief Unmounts every screen without asking, newest first, then the pages.
    ~Workspace() { _model.reset(); }

    Workspace(Workspace const&) = delete;
    Workspace& operator=(Workspace const&) = delete;
    Workspace(Workspace&&) = delete;
    Workspace& operator=(Workspace&&) = delete;

    /// @brief The tab bar: the shell's root widget, which the host places.
    /// @return It.
    [[nodiscard]] TabsWidget& root() noexcept { return *_bar; }

    /// @brief The tab logic the bar shows, for an application that reads or drives it by tab id.
    /// @return It, valid as long as the shell.
    [[nodiscard]] WorkspaceModel& model() noexcept { return *_model; }

    /// @brief Opens a screen, or focuses the tab that already shows it (`WorkspaceModel::open`).
    /// @param screenId The screen id.
    /// @param params   Its params.
    /// @return The tab's index, or why the screen could not be mounted; a refused mount opens nothing.
    std::expected<std::size_t, MountError> open(std::string_view screenId, ParamMap const& params) {
        auto opened = _model->open(screenId, params);
        if (!opened) {
            return std::unexpected(std::move(opened.error()));
        }
        return indexOf(*opened);
    }

    /// @brief Opens the start screens (`WorkspaceModel::start`).
    /// @param how The start screen, the fallback and the record.
    /// @return The number of tabs opened.
    std::size_t start(WorkspaceStart const& how) { return _model->start(how); }

    /// @brief Writes the record now, cancelling a pending debounced write: what sign-out does before the session is
    ///        cleared.
    void flushRecord() { _model->flushRecord(); }

    /// @brief The record as it would be written now: the routes of the open tabs, in order.
    /// @return The routes.
    [[nodiscard]] std::vector<std::string> record() const { return _model->record(); }

    /// @brief Shows a tab and makes it the most recently used.
    /// @param index The tab; past the last one does nothing.
    void activate(std::size_t index) {
        if (std::optional<WorkspaceTabId> const tab = tabAt(index)) {
            _model->activate(*tab);
        }
    }

    /// @brief Shows the next tab in most-recently-used order (Ctrl+Tab). Repeated calls walk the order as it was at
    ///        the first of them; any other activation starts a new walk.
    void next() { _model->next(); }

    /// @brief Shows the previous tab in most-recently-used order (Ctrl+Shift+Tab).
    void previous() { _model->previous(); }

    /// @brief Closes a tab through its screen's close protocol.
    /// @param index The tab.
    /// @param done  Called once with whether it closed; may be empty.
    void close(std::size_t index, std::function<void(bool)> done) {
        std::optional<WorkspaceTabId> const tab = tabAt(index);
        if (!tab) {
            if (done) {
                done(false);
            }
            return;
        }
        _model->close(*tab, std::move(done));
    }

    /// @brief Closes every tab but one, one at a time, stopping at the first veto.
    /// @param index The tab kept; past the last one closes every tab.
    /// @param done  Called once with whether every other tab closed; may be empty.
    void closeOthers(std::size_t index, std::function<void(bool)> done) {
        std::optional<WorkspaceTabId> const tab = tabAt(index);
        if (!tab) {
            _model->closeAll(std::move(done));
            return;
        }
        _model->closeOthers(*tab, std::move(done));
    }

    /// @brief Closes every tab, one at a time, stopping at the first veto.
    /// @param done Called once with whether every tab closed; may be empty.
    void closeAll(std::function<void(bool)> done) { _model->closeAll(std::move(done)); }

    /// @brief Closes every tab without unsaved changes, one at a time, stopping at the first veto.
    /// @param done Called once with whether each of them closed; may be empty.
    void closeSaved(std::function<void(bool)> done) { _model->closeSaved(std::move(done)); }

    /// @brief The active tab's index.
    /// @return It, or `std::nullopt` while no tab is open.
    [[nodiscard]] std::optional<std::size_t> active() const {
        std::optional<WorkspaceTabId> const tab = _model->active().peek();
        if (!tab) {
            return std::nullopt;
        }
        return indexOf(*tab);
    }

    /// @brief The screen handle of a tab: what a client remounts for development reload and inspects.
    /// @param index The tab's index.
    /// @return Its handle, valid while the tab is open; null past the end.
    [[nodiscard]] ScreenHandle* handleOf(std::size_t index) const noexcept {
        std::optional<WorkspaceTabId> const tab = tabAt(index);
        return tab ? _model->handleOf(*tab) : nullptr;
    }

    /// @brief The active tab's screen handle.
    /// @return It, or null while no tab is open.
    [[nodiscard]] ScreenHandle* activeHandle() const {
        std::optional<std::size_t> const shown = active();
        return shown ? handleOf(*shown) : nullptr;
    }

    /// @brief Closes every tab without asking, newest first: what signing out does, since no close guard may keep a
    ///        screen of the principal who is leaving.
    void forceCloseAll() { _model->forceCloseAll(); }

    /// @brief The open tabs, in order.
    /// @return Each tab's screen, identity and caption.
    [[nodiscard]] std::vector<WorkspaceTab> tabs() const {
        std::vector<WorkspaceTab> out;
        for (WorkspaceTabId const tab : _model->order()) {
            ScreenHandle const& handle = *_model->handleOf(tab);
            out.push_back(WorkspaceTab{.screenId = std::string{screenOf(tab)},
                                       .identity = handle.identity().peek(),
                                       .caption = captionOf(handle)});
        }
        return out;
    }

private:
    static constexpr std::size_t kNone = static_cast<std::size_t>(-1);

    // A tab's page: the slot of the bar it shows in, the head row above its screen, and the screen's id. The model
    // destroys the screen's handle before it lets the page go.
    struct Page {
        std::string screenId;
        std::unique_ptr<SlotWidget> slot;
        // The head row reads the handle through the model, which answers null once the handle is gone.
        std::optional<Mounted> head;

        Page() = default;
        Page(Page const&) = delete;
        Page& operator=(Page const&) = delete;
        Page(Page&&) = delete;
        Page& operator=(Page&&) = delete;
        ~Page() {
            head.reset();
            slot.reset();
        }
    };

    // A badge of the head row, by its place among the visible badges.
    struct PlacedBadge {
        std::size_t index = 0;
        HeadBadge badge;
    };

    [[nodiscard]] std::optional<WorkspaceTabId> tabAt(std::size_t index) const {
        std::vector<WorkspaceTabId> const order = _model->order();
        return index < order.size() ? std::optional<WorkspaceTabId>{order.at(index)} : std::nullopt;
    }

    [[nodiscard]] std::size_t indexOf(WorkspaceTabId tab) const {
        std::vector<WorkspaceTabId> const order = _model->order();
        auto const found = std::ranges::find(order, tab);
        return found == order.end() ? kNone : static_cast<std::size_t>(found - order.begin());
    }

    [[nodiscard]] std::string_view screenOf(WorkspaceTabId tab) const {
        auto const found = _pages.find(tab.value);
        return found == _pages.end() ? std::string_view{} : std::string_view{found->second->screenId};
    }

    [[nodiscard]] static std::string captionOf(ScreenHandle const& handle) {
        std::string caption = handle.title().peek();
        if (handle.dirty().peek()) {
            caption += " •";
        }
        return caption;
    }

    // The model's mountPage: a page slot at the end of the bar, the screen mounted into it, a host screen from the
    // registry and a document screen from the document host, then the head row above it. The page goes with a
    // refused mount.
    std::expected<std::unique_ptr<ScreenHandle>, MountError> mountPage(WorkspaceTabId tab, std::string_view screenId,
                                                                       ParamMap const& params) {
        auto page = std::make_unique<Page>();
        page->screenId = std::string{screenId};
        page->slot = _backend->createSlot(_bar.get());
        page->slot->setLayout({.width = Sizing::stretch(), .height = Sizing::stretch()});
        page->slot->setVisible(false);
        std::expected<std::unique_ptr<ScreenHandle>, MountError> handle;
        WorkspaceScreen const facts = _screens ? _screens(screenId) : WorkspaceScreen{};
        if (facts.host.empty()) {
            handle = _host->mountInto(screenId, params, *page->slot);
        } else {
            HostScreenFactory const* const factory = _hosts == nullptr ? nullptr : _hosts->find(facts.host);
            if (factory == nullptr) {
                return std::unexpected(MountError{.screenId = std::string{screenId},
                                                  .reason = "no host screen `" + facts.host + "` is registered"});
            }
            handle = (*factory)(params, *page->slot);
        }
        if (!handle) {
            return handle;
        }
        if ((*handle)->head() != nullptr) {
            // Mounted after the screen, since it reads the handle, and moved above it.
            page->head.emplace(*_runtime, *_backend, headRow(tab), page->slot.get());
            page->slot->moveChild(page->head->root(), 0);
        }
        _pages.insert_or_assign(tab.value, std::move(page));
        _shownOrder.push_back(tab);
        return handle;
    }

    // The head of a tab's screen, while the tab is open. A tracked read of the model's tabs, so a binding made while
    // the model is still opening the tab reads the head once it holds it.
    [[nodiscard]] ScreenHead* headOf(WorkspaceTabId tab) const {
        static_cast<void>(_model->tabs().get());
        ScreenHandle* const handle = _model->handleOf(tab);
        return handle == nullptr ? nullptr : handle->head();
    }

    // The head row above a tab's screen: the subtitle (muted), the badges, a spacer and the actions as buttons. It is
    // hidden while the head has none of them, so a screen without a head looks as it would without the row.
    [[nodiscard]] Node headRow(WorkspaceTabId tab) const {
        auto const subtitle = [this, tab] {
            ScreenHead const* const head = headOf(tab);
            return head == nullptr ? std::string{} : head->subtitle().get();
        };
        auto const status = [this, tab] {
            ScreenHead const* const head = headOf(tab);
            return head == nullptr ? std::vector<HeadBadge>{} : head->status().get();
        };
        auto const actions = [this, tab] {
            ScreenHead const* const head = headOf(tab);
            return head == nullptr ? std::vector<HeadAction>{} : head->actions().get();
        };
        Row row{.children = {}, .gap = 2, .common = {}, .tone = std::nullopt};
        row.common.testId = "head";
        row.common.visible = Prop<bool>{
            [subtitle, status, actions] { return !subtitle().empty() || !status().empty() || !actions().empty(); }};
        Text text{.text = Prop<std::string>{subtitle}, .role = TextRole::Muted, .field = {}, .common = {}};
        text.common.testId = "head_subtitle";
        text.common.visible = Prop<bool>{[subtitle] { return !subtitle().empty(); }};
        row.children.push_back(ui::text(std::move(text)));
        row.children.push_back(ui::forEach<PlacedBadge>(
            [status] {
                std::vector<PlacedBadge> placed;
                for (HeadBadge const& badge : status()) {
                    placed.push_back(PlacedBadge{.index = placed.size(), .badge = badge});
                }
                return placed;
            },
            [](PlacedBadge const& placed) { return Key{static_cast<std::int64_t>(placed.index)}; },
            [](reactive::Signal<PlacedBadge> const& placed) {
                Badge badge{.tone = Prop<Tone>{[&placed] { return placed.get().badge.tone; }},
                            .text = Prop<std::string>{[&placed] { return placed.get().badge.text; }},
                            .icon = placed.peek().badge.icon,
                            .common = {}};
                badge.common.testId = "head_status_" + std::to_string(placed.peek().index);
                return ui::badge(std::move(badge));
            },
            Axis::Horizontal, 1));
        row.children.push_back(ui::spacer());
        row.children.push_back(ui::forEach<HeadAction>(
            actions, [](HeadAction const& action) { return Key{action.id}; },
            [this, tab](reactive::Signal<HeadAction> const& action) {
                Button button{.label = Prop<std::string>{[&action] { return action.get().label; }},
                              .emphasis = action.peek().emphasis,
                              .onClick =
                                  [this, tab, actionId = action.peek().id] {
                                      ScreenHandle* const handle = _model->handleOf(tab);
                                      if (ScreenHead* const head = handle == nullptr ? nullptr : handle->head()) {
                                          head->run(actionId);
                                      }
                                  },
                              .common = {}};
                button.common.testId = "head_action_" + action.peek().id;
                button.common.enabled = Prop<bool>{[&action] { return action.get().enabled; }};
                return ui::button(std::move(button));
            },
            Axis::Horizontal, 1));
        return ui::row(std::move(row));
    }

    // The model changed: the bar shows its tabs in order, with their captions, and the active tab's page.
    void showTabs() {
        if (_model == nullptr) {
            return;
        }
        std::vector<WorkspaceTabId> const order = _model->order();
        std::optional<WorkspaceTabId> const shown = _model->active().peek();
        std::vector<std::string> captions;
        captions.reserve(order.size());
        for (std::size_t index = 0; index < order.size(); ++index) {
            auto const found = _pages.find(order.at(index).value);
            if (found == _pages.end()) {
                continue;
            }
            Page& page = *found->second;
            // A tab the model moved (a reuseEmpty replacement) moves its page in the bar too; `_shownOrder` is the
            // bar's own order of the pages.
            if (index >= _shownOrder.size() || _shownOrder.at(index) != order.at(index)) {
                std::erase(_shownOrder, order.at(index));
                _shownOrder.insert(
                    _shownOrder.begin() + static_cast<std::ptrdiff_t>(std::min(index, _shownOrder.size())),
                    order.at(index));
                _bar->moveChild(*page.slot, index);
            }
            page.slot->setVisible(shown == order.at(index));
            captions.push_back(captionOf(*_model->handleOf(order.at(index))));
        }
        _bar->setTabs(captions);
        _bar->setSelected(shown ? indexOf(*shown) : kNone);
        if (_onChanged) {
            _onChanged();
        }
    }

    reactive::Runtime* _runtime;
    IViewBackend* _backend;
    DocumentScreenHost* _host;
    std::unique_ptr<TabsWidget> _bar;
    ScreenHostRegistry const* _hosts = nullptr;
    std::function<WorkspaceScreen(std::string_view)> _screens;
    std::function<void()> _onChanged;
    std::map<std::uint64_t, std::unique_ptr<Page>> _pages;
    // The pages in the bar's order.
    std::vector<WorkspaceTabId> _shownOrder;
    std::unique_ptr<WorkspaceModel> _model;
};

}  // namespace morph::ui
