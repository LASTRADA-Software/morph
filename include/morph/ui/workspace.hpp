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
/// @brief The workspace shell (spec 6 §7, §15): tabs over screens, and overlays over the tabs, mounted through a
///        `DocumentScreenHost`.
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

/// @brief Where a workspace draws the active screen's head row.
enum class HeadPlacement : std::uint8_t {
    /// @brief Above each page, inside the workspace.
    InPage,
    /// @brief Nowhere: the host mounts `headPartsForActive()` where it chooses, and a page holds only its screen.
    External,
};

/// @brief The narrowest a head's lead is given before the trail wraps to a line of its own, in units.
inline constexpr int kHeadLeadMinimum = 32;

/// @brief The two parts of the active screen's head, for a host that lays them out itself.
struct HeadParts {
    /// @brief The subtitle, muted, then the badges, on one wrapping line: beside one another while they fit, and
    /// wrapped
    ///        under one another where they do not.
    Node lead;
    /// @brief The tools, the actions and the head's own menu, in one row.
    Node trail;
};

/// @brief Tabs over screens: a tab bar over a `WorkspaceModel`, one screen instance per tab, mounted into the tab's
///        page through `DocumentScreenHost::mountInto`, with the screen's head row above it or, when the placement is
///        `External`, mounted by the host instead.
///
/// The model holds the tab logic: tabs keep the order they were opened in, opening a screen whose identity an open
/// tab already has focuses that tab, a singleton screen has one tab, and closing runs each screen's close protocol
/// (spec 6 §3), one tab at a time, a walk over several tabs stopping at the first veto. The bar shows the model's tabs
/// and takes Ctrl+Tab, Ctrl+Shift+Tab and Ctrl+W, which documents may not declare. A screen a document navigates to as
/// a dialog or a drawer is shown over the tabs (`openOverlay`). Neither copyable nor movable; owned by the owner
/// thread of its runtime.
class Workspace {
public:
    /// @brief The shell, its tab bar appended to @p parent.
    /// @param runtime The runtime the screens' reactive nodes belong to. Borrowed.
    /// @param backend The backend the bar and the pages are made with. Borrowed.
    /// @param host    What mounts the screens. Borrowed; it outlives the shell.
    /// @param options The screens' catalog facts.
    /// @param parent  The container the shell is appended to; null makes it a backend root.
    /// @param headPlacement Where the active screen's head row is drawn. `External` leaves the host to mount
    ///        `headPartsForActive()` itself, so a shell with its own top bar shows the head once.
    Workspace(reactive::Runtime& runtime MORPH_LIFETIMEBOUND, IViewBackend& backend MORPH_LIFETIMEBOUND,
              DocumentScreenHost& host MORPH_LIFETIMEBOUND, WorkspaceOptions options = {},
              ContainerWidget* parent = nullptr, HeadPlacement headPlacement = HeadPlacement::InPage)
        : _runtime{&runtime},
          _backend{&backend},
          _host{&host},
          _parent{parent},
          _bar{backend.createTabs(parent)},
          _hosts{options.hosts},
          _screens{options.screens},
          _onChanged{std::move(options.onChanged)},
          _diagnostics{options.diagnostics},
          _headPlacement{headPlacement} {
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

    /// @brief Closes every overlay without asking, newest first, then unmounts every tab's screen the same way, then
    ///        the pages: a callee goes before the opener its call answers.
    ~Workspace() {
        *_alive = false;
        closeOverlays();
        _model.reset();
    }

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

    /// @brief Opens a screen as `open` does, and says whether it mounted one (`WorkspaceModel::openTab`): what a host
    ///        needs to settle the call of a navigation that only focused a tab.
    /// @param screenId The screen id.
    /// @param params   Its params.
    /// @return The tab and whether a screen was mounted for it, or why the screen could not be mounted.
    std::expected<WorkspaceOpened, MountError> openTab(std::string_view screenId, ParamMap const& params) {
        return _model->openTab(screenId, params);
    }

    /// @brief Shows a document screen over the tabs, as a dialog (`as: dialog`) or a drawer on the end side
    ///        (`as: drawer`), mounted with `DocumentScreenHost::mountInto`, so the mount adopts the navigation's call.
    ///
    /// It always mounts a new instance: it never focuses an open tab, so a call is never left unclaimed. The
    /// overlay is titled by the screen's title, and the screen's head actions are its footer's buttons; its badges,
    /// menu and tools are not shown, and a head with a menu or tools is reported once. A dialog is a sheet while the
    /// width class is `compact`, and follows `setWidthClass` while it is open. A user's dismissal opens it again at
    /// once and asks the screen to close (`requestClose`); it goes only when the screen closed. A screen that closes
    /// itself goes through `screenClosed`. An overlay adds no tab, no record entry and no route.
    /// @param request The navigation: a screen, its params, and `as` either `dialog` or `drawer`.
    /// @return Nothing, or why nothing was shown: an `as` that is not an overlay, a host screen, or a refused mount.
    std::expected<void, MountError> openOverlay(NavigateRequest const& request) {
        if (request.as != "dialog" && request.as != "drawer") {
            return std::unexpected(MountError{.screenId = request.screen,
                                              .reason = "`as: " + request.as + "` is not a dialog or a drawer"});
        }
        if (_screens && !_screens(request.screen).host.empty()) {
            return std::unexpected(
                MountError{.screenId = request.screen, .reason = "a host screen opens as a tab, not an overlay"});
        }
        ValueRecord const* const record = request.params.asRecord();
        ParamMap const params = record == nullptr ? ParamMap{} : *record;
        auto overlay = std::make_unique<Overlay>();
        overlay->id = ++_lastOverlay;
        overlay->screenId = request.screen;
        ContainerWidget* content = nullptr;
        ContainerWidget* footer = nullptr;
        if (request.as == "drawer") {
            overlay->drawer = _backend->createDrawer(_parent, Side::End);
            content = overlay->drawer.get();
            footer = &overlay->drawer->footer();
        } else {
            overlay->dialog = _backend->createDialog(_parent);
            content = overlay->dialog.get();
            footer = &overlay->dialog->footer();
        }
        auto handle = _host->mountInto(request.screen, params, *content);
        if (!handle) {
            return std::unexpected(std::move(handle.error()));
        }
        overlay->handle = std::move(*handle);
        showOverlay(*overlay, *footer);
        _overlays.push_back(std::move(overlay));
        return {};
    }

    /// @brief A screen the workspace shows closed itself, by `return` or `close: self`: its overlay goes, or its tab
    ///        does. What a host's `DocumentScreenHost::onScreenClosed` forwards here.
    /// @param screen The closed screen's handle; one the workspace does not show does nothing.
    void screenClosed(ScreenHandle const& screen) {
        auto const overlay = std::ranges::find_if(
            _overlays, [&screen](std::unique_ptr<Overlay> const& open) { return open->handle.get() == &screen; });
        if (overlay != _overlays.end()) {
            _overlays.erase(overlay);
            return;
        }
        for (WorkspaceTabId const tab : _model->order()) {
            if (_model->handleOf(tab) == &screen) {
                // The screen is closed, so its close answers at once and the tab goes.
                _model->close(tab, {});
                return;
            }
        }
    }

    /// @brief The screens shown over the tabs, oldest first.
    /// @return Each overlay's screen id.
    [[nodiscard]] std::vector<std::string> overlays() const {
        std::vector<std::string> out;
        out.reserve(_overlays.size());
        for (auto const& overlay : _overlays) {
            out.push_back(overlay->screenId);
        }
        return out;
    }

    /// @brief Tells the workspace the window's width class changed, as the backend's `setOnWidthClass` listener
    ///        reports it: each open dialog overlay is a sheet while it is `compact`.
    /// @param widthClass The class.
    void setWidthClass(std::string_view widthClass) {
        bool const sheet = widthClass == "compact";
        for (auto const& overlay : _overlays) {
            showSheet(*overlay, sheet);
        }
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

    /// @brief The active tab's head as two parts, the same reactive content `headRow` builds for the in-page
    ///        rendering, for a host that mounts them elsewhere (a shell's own top bar, say). Neither is null: with no
    ///        active tab each is an invisible, empty container rather than a null `Node`, since a caller mounting them
    ///        may not expect to handle one.
    /// @return The lead and the trail, reactive to the active tab changing and to its head's own content changing.
    [[nodiscard]] HeadParts headPartsForActive() const {
        std::optional<WorkspaceTabId> const tab = _model->active().peek();
        if (tab) {
            // A host places the lead beside the title and gives it the minimum width it wants, so the lead has none
            // of its own: with one, a lead's natural width would never be less than it.
            Row lead = buildHeadLead(*tab);
            lead.common.layout.minWidth.reset();
            return HeadParts{.lead = ui::row(std::move(lead)), .trail = ui::row(buildHeadTrail(*tab))};
        }
        Row lead{.children = {}, .gap = 0, .common = {}, .tone = std::nullopt};
        lead.common.testId = "head_lead";
        lead.common.visible = false;
        Row trail{.children = {}, .gap = 0, .common = {}, .tone = std::nullopt};
        trail.common.testId = "head_trail";
        trail.common.visible = false;
        return HeadParts{.lead = ui::row(std::move(lead)), .trail = ui::row(std::move(trail))};
    }

    /// @brief Whether the active tab's head shows anything: a subtitle, a badge, an action or a menu entry. A tracked
    ///        read, so a
    ///        shell that shows the head on a line of its own follows a tab switch and a head's own changes; the head
    ///        row's own visibility is the same rule.
    /// @return Whether there is an active tab whose head has content.
    [[nodiscard]] bool activeHeadShown() const {
        std::optional<WorkspaceTabId> const tab = _model->active().get();
        return tab.has_value() && headHasContent(*tab);
    }

    /// @brief Closes every overlay, then every tab, without asking, newest first: what signing out does, since no
    ///        close guard may keep a screen of the principal who is leaving. A callee goes before the opener its call
    ///        answers, so the opener's `onCancel` runs in a live scope; then every call no mount adopted is cancelled.
    void forceCloseAll() {
        closeOverlays();
        _model->forceCloseAll();
    }

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

    // A screen shown over the tabs: its dialog or drawer, its handle, the footer of its head's actions and the scope
    // of the effects that follow it, destroyed in the reverse order, so the handle goes before its container.
    struct Overlay {
        std::uint64_t id = 0;
        std::string screenId;
        std::unique_ptr<DialogWidget> dialog;
        std::unique_ptr<DrawerWidget> drawer;
        std::unique_ptr<ScreenHandle> handle;
        std::optional<Mounted> footer;
        std::unique_ptr<reactive::Scope> scope;
        bool sheet = false;
        bool reported = false;

        Overlay() = default;
        Overlay(Overlay const&) = delete;
        Overlay& operator=(Overlay const&) = delete;
        Overlay(Overlay&&) = delete;
        Overlay& operator=(Overlay&&) = delete;
        ~Overlay() {
            scope.reset();
            footer.reset();
            handle.reset();
            drawer.reset();
            dialog.reset();
        }

        void setOpen(bool open) const {
            if (dialog) {
                dialog->setOpen(open);
            } else {
                drawer->setOpen(open);
            }
        }
    };

    // Draws a dialog overlay as a sheet, or as a centred dialog again; a drawer has no sheet.
    static void showSheet(Overlay& overlay, bool sheet) {
        if (overlay.dialog && sheet != overlay.sheet) {
            overlay.sheet = sheet;
            overlay.dialog->setSheet(sheet);
        }
    }

    // Titles a mounted overlay, puts its head's actions in its footer, opens it and makes a dismissal ask its screen.
    void showOverlay(Overlay& overlay, ContainerWidget& footer) {
        ScreenHandle const* const handle = overlay.handle.get();
        ScreenHead* const head = overlay.handle->head();
        overlay.scope = std::make_unique<reactive::Scope>(*_runtime);
        overlay.scope->effect([&overlay, handle] {
            std::string const title = handle->title().get();
            if (overlay.dialog) {
                overlay.dialog->setTitle(title);
            } else {
                overlay.drawer->setTitle(title);
            }
        });
        if (head != nullptr) {
            overlay.footer.emplace(*_runtime, *_backend, overlayActions(*head), &footer);
            overlay.scope->effect([this, &overlay, head] {
                bool const menu = head->menu() != nullptr && !head->menu()->get().empty();
                bool const tools = head->tools() != nullptr && !head->tools()->get().empty();
                if ((menu || tools) && !overlay.reported) {
                    overlay.reported = true;
                    _runtime->untracked([this, &overlay] {
                        if (_diagnostics) {
                            _diagnostics(Refusal{.document = overlay.screenId,
                                                 .path = {},
                                                 .reason = "a dialog or drawer does not show the head menu or tools "
                                                           "of " +
                                                           overlay.screenId});
                        }
                    });
                }
            });
        }
        showSheet(overlay, _backend->widthClass() == "compact");
        std::uint64_t const overlayId = overlay.id;
        auto const dismissed = [this, overlayId] {
            Overlay const* const shown = overlayOf(overlayId);
            if (shown == nullptr) {
                return;
            }
            // The dismissal closed the widget. It is opened again at once, so the screen stays reachable while its
            // close protocol runs, and goes only when the screen closed: a dirty screen without a guard stays.
            shown->setOpen(true);
            shown->handle->requestClose([this, alive = std::weak_ptr<bool>{_alive}, overlayId](bool closed) {
                if (auto const live = alive.lock(); live != nullptr && *live && closed) {
                    removeOverlay(overlayId);
                }
            });
        };
        if (overlay.dialog) {
            overlay.dialog->setOnDismiss(dismissed);
        } else {
            overlay.drawer->setOnDismiss(dismissed);
        }
        overlay.setOpen(true);
    }

    // The footer of an overlay: a button per visible head action, each running the action by its id.
    [[nodiscard]] static Node overlayActions(ScreenHead& head) {
        Row row{.children = {}, .gap = 2, .common = {}, .tone = std::nullopt};
        row.common.testId = "overlay_actions";
        row.common.visible = Prop<bool>{[&head] { return !head.actions().get().empty(); }};
        row.children.push_back(ui::forEach<HeadAction>(
            [&head] { return head.actions().get(); }, [](HeadAction const& action) { return Key{action.id}; },
            [&head](reactive::Signal<HeadAction> const& action) {
                Button button{.label = Prop<std::string>{[&action] { return action.get().label; }},
                              .emphasis = action.peek().emphasis,
                              .onClick = [&head, actionId = action.peek().id] { head.run(actionId); },
                              .common = {}};
                button.common.testId = "overlay_action_" + action.peek().id;
                button.common.enabled = Prop<bool>{[&action] { return action.get().enabled; }};
                return ui::button(std::move(button));
            },
            Axis::Horizontal, 2));
        return ui::row(std::move(row));
    }

    [[nodiscard]] Overlay const* overlayOf(std::uint64_t overlayId) const {
        auto const found = std::ranges::find(_overlays, overlayId, [](auto const& open) { return open->id; });
        return found == _overlays.end() ? nullptr : found->get();
    }

    void removeOverlay(std::uint64_t overlayId) {
        std::erase_if(_overlays, [overlayId](std::unique_ptr<Overlay> const& open) { return open->id == overlayId; });
    }

    // Closes every overlay without asking, newest first; each one's close answers its call with a cancel.
    void closeOverlays() {
        while (!_overlays.empty()) {
            std::unique_ptr<Overlay> const last = std::move(_overlays.back());
            _overlays.pop_back();
            last->handle->forceClose();
        }
    }

    // A badge of the head row, by its place among the visible badges.
    struct PlacedBadge {
        std::size_t index = 0;
        HeadBadge badge;
    };

    // The head menu as the row draws it: its visible entries, keyed by their shape (the ids, separators and
    // submenus), so the menu is drawn again when an entry is shown or hidden and only rebinds when a label changes.
    struct ShapedMenu {
        std::string shape;
        std::vector<HeadMenuEntry> entries;

        bool operator==(ShapedMenu const&) const = default;
    };

    // NOLINTNEXTLINE(misc-no-recursion): a submenu is a menu one level down.
    [[nodiscard]] static std::string shapeOf(std::vector<HeadMenuEntry> const& entries) {
        std::string shape;
        for (HeadMenuEntry const& entry : entries) {
            if (entry.separator) {
                shape += '|';
            } else if (!entry.items.empty()) {
                shape += '(' + shapeOf(entry.items) + ')';
            } else {
                shape += entry.id + ';';
            }
        }
        return shape;
    }

    // The entry at @p path through @p entries; null past the end, which a binding reads only in the moment before
    // the menu is drawn again for a new shape.
    [[nodiscard]] static HeadMenuEntry const* entryAt(std::vector<HeadMenuEntry> const& entries,
                                                      std::vector<std::size_t> const& path) {
        std::vector<HeadMenuEntry> const* level = &entries;
        HeadMenuEntry const* entry = nullptr;
        for (std::size_t const index : path) {
            if (index >= level->size()) {
                return nullptr;
            }
            entry = &level->at(index);
            level = &entry->items;
        }
        return entry;
    }

    // The menu items of one level of the head menu: each bound to its entry by its place, which the shape keeps, and
    // each chosen by its id, so the screen refuses an entry hidden since by name rather than running its neighbour.
    // NOLINTNEXTLINE(misc-no-recursion): a submenu is a menu one level down.
    [[nodiscard]] std::vector<MenuItem> headMenuItems(WorkspaceTabId tab, reactive::Signal<ShapedMenu> const& menu,
                                                      std::vector<HeadMenuEntry> const& level,
                                                      std::vector<std::size_t> const& outer) const {
        std::vector<MenuItem> items;
        for (std::size_t index = 0; index < level.size(); ++index) {
            HeadMenuEntry const& entry = level.at(index);
            MenuItem item;
            if (entry.separator) {
                item.separator = true;
                items.push_back(std::move(item));
                continue;
            }
            std::vector<std::size_t> path = outer;
            path.push_back(index);
            auto const read = [&menu, path](auto field, auto const& fallback) {
                return [&menu, path, field, fallback] {
                    HeadMenuEntry const* const shown = entryAt(menu.get().entries, path);
                    return shown == nullptr ? fallback : std::invoke(field, *shown);
                };
            };
            item.label = Prop<std::string>{read(&HeadMenuEntry::label, std::string{})};
            item.description = Prop<std::string>{read(&HeadMenuEntry::description, std::string{})};
            item.enabled = Prop<bool>{read(&HeadMenuEntry::enabled, false)};
            item.checked = Prop<std::optional<bool>>{read(&HeadMenuEntry::checked, std::optional<bool>{})};
            item.icon = entry.icon;
            item.keys = entry.keys;
            if (entry.items.empty()) {
                item.onSelect = [this, tab, entryId = entry.id] {
                    ScreenHandle* const handle = _model->handleOf(tab);
                    if (ScreenHead* const head = handle == nullptr ? nullptr : handle->head()) {
                        head->choose(entryId);
                    }
                };
            } else {
                item.items = headMenuItems(tab, menu, entry.items, path);
            }
            items.push_back(std::move(item));
        }
        return items;
    }

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
        // A screen whose root keeps its content height scrolls inside the page instead of being cut at the window's
        // edge; one whose root stretches fills the page and lays itself out.
        page->slot->setScrollable(true);
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
        if (_headPlacement == HeadPlacement::InPage && (*handle)->head() != nullptr) {
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

    // The visible entries of a tab's head menu; a tracked read.
    [[nodiscard]] std::vector<HeadMenuEntry> headMenuOf(WorkspaceTabId tab) const {
        ScreenHead const* const head = headOf(tab);
        reactive::Computed<std::vector<HeadMenuEntry>> const* const menu = head == nullptr ? nullptr : head->menu();
        return menu == nullptr ? std::vector<HeadMenuEntry>{} : menu->get();
    }

    // The visible tools of a tab's head; a tracked read.
    [[nodiscard]] std::vector<HeadTool> headToolsOf(WorkspaceTabId tab) const {
        ScreenHead const* const head = headOf(tab);
        reactive::Computed<std::vector<HeadTool>> const* const tools = head == nullptr ? nullptr : head->tools();
        return tools == nullptr ? std::vector<HeadTool>{} : tools->get();
    }

    // Whether a tab's head has a subtitle, a badge, a tool, an action or a menu entry; a tracked read.
    [[nodiscard]] bool headHasContent(WorkspaceTabId tab) const { return leadHasContent(tab) || trailHasContent(tab); }

    // Whether the head has a subtitle or a badge.
    [[nodiscard]] bool leadHasContent(WorkspaceTabId tab) const {
        ScreenHead const* const head = headOf(tab);
        return head != nullptr &&
               (!head->subtitle().get().empty() || !subtitleSpansOf(head).empty() || !head->status().get().empty());
    }

    // The head's subtitle runs; a tracked read, empty for a head that has none.
    [[nodiscard]] static std::vector<TextSpan> subtitleSpansOf(ScreenHead const* head) {
        reactive::Computed<std::vector<TextSpan>> const* const spans =
            head == nullptr ? nullptr : head->subtitleSpans();
        return spans == nullptr ? std::vector<TextSpan>{} : spans->get();
    }

    // Whether the head has a tool, an action or a menu entry.
    [[nodiscard]] bool trailHasContent(WorkspaceTabId tab) const {
        ScreenHead const* const head = headOf(tab);
        return head != nullptr &&
               (!headToolsOf(tab).empty() || !head->actions().get().empty() || !headMenuOf(tab).empty());
    }

    // A tool of the head row: a search field or a select bound to the tool's value, its label the accessible name and
    // no caption, reporting each edit to the head, which runs the document's commands.
    [[nodiscard]] Node headTool(WorkspaceTabId tab, reactive::Signal<HeadTool> const& tool) const {
        std::string const toolId = tool.peek().id;
        auto const report = [this, tab, toolId](Value const& value) {
            ScreenHandle* const handle = _model->handleOf(tab);
            if (ScreenHead* const head = handle == nullptr ? nullptr : handle->head()) {
                head->edit(toolId, value);
            }
        };
        Common common;
        common.testId = "head_tool_" + toolId;
        common.enabled = Prop<bool>{[&tool] { return tool.get().enabled; }};
        common.a11y.name = Prop<std::string>{[&tool] { return tool.get().label; }};
        if (int const width = tool.peek().width; width > 0) {
            common.layout.width = Sizing::fixed(width);
        }
        if (tool.peek().kind == HeadToolKind::Select) {
            Select select;
            select.options = Prop<std::vector<SelectOption>>{[&tool] { return tool.get().options; }};
            select.selected = Prop<std::optional<Key>>{[&tool] { return tool.get().selected; }};
            select.onSelect = [report](Key const& key) { report(detail::valueOfKey(key)); };
            select.label = Prop<std::string>{[&tool] { return tool.get().label; }};
            select.common = std::move(common);
            return ui::select(std::move(select));
        }
        TextInput input;
        input.value = Prop<std::string>{[&tool] { return tool.get().value; }};
        input.placeholder = Prop<std::string>{[&tool] { return tool.get().placeholder; }};
        input.label = Prop<std::string>{[&tool] { return tool.get().label; }};
        input.onChange = [report](std::string text) { report(Value{std::move(text)}); };
        // A submit of text the tool does not show yet reports it as an edit first, so `onSubmit` reads what was
        // submitted.
        input.onSubmit = [this, tab, toolId, &tool](std::string const& text) {
            ScreenHandle* const handle = _model->handleOf(tab);
            ScreenHead* const head = handle == nullptr ? nullptr : handle->head();
            if (head == nullptr) {
                return;
            }
            if (text != tool.peek().value) {
                head->edit(toolId, Value{text});
            }
            head->submit(toolId);
        };
        input.common = std::move(common);
        return ui::textInput(std::move(input));
    }

    // The head's lead, built but not yet wrapped into an immutable Node: the subtitle (muted) and the badges on one
    // wrapping line, so they sit side by side while they fit and wrap under one another where they do not; the badges'
    // holder wraps too. It stretches from `kHeadLeadMinimum` units, so a trail beside it leaves it at least that much
    // before the trail wraps to a line of its own.
    [[nodiscard]] Row buildHeadLead(WorkspaceTabId tab) const {
        auto const subtitle = [this, tab] {
            ScreenHead const* const head = headOf(tab);
            return head == nullptr ? std::string{} : head->subtitle().get();
        };
        auto const status = [this, tab] {
            ScreenHead const* const head = headOf(tab);
            return head == nullptr ? std::vector<HeadBadge>{} : head->status().get();
        };
        Row row{.children = {}, .gap = 1, .common = {}, .tone = std::nullopt};
        row.wrap = true;
        row.align = Align::Center;
        row.common.testId = "head_lead";
        row.common.layout.width = Sizing::stretch();
        row.common.layout.minWidth = kHeadLeadMinimum;
        row.common.visible = Prop<bool>{[this, tab] { return leadHasContent(tab); }};
        // The runs when the screen gives them, else its plain subtitle as one run, so the text is bound to one thing.
        auto const spans = [this, tab, subtitle] {
            std::vector<TextSpan> runs = subtitleSpansOf(headOf(tab));
            if (runs.empty()) {
                std::string plain = subtitle();
                if (!plain.empty()) {
                    runs.push_back(TextSpan{.text = std::move(plain)});
                }
            }
            return runs;
        };
        Text text{.text = Prop<std::string>{subtitle}, .role = TextRole::Muted, .field = {}, .common = {}};
        text.spans = Prop<std::vector<TextSpan>>{spans};
        text.common.testId = "head_subtitle";
        text.common.visible = Prop<bool>{[spans] { return !spans().empty(); }};
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
            Axis::Horizontal, 1, {}, true));
        return row;
    }

    // The head's trail: the tools, the actions as buttons and the head's own menu behind one icon button, in one row.
    // Hidden while the head has none of them.
    [[nodiscard]] Row buildHeadTrail(WorkspaceTabId tab) const {
        auto const actions = [this, tab] {
            ScreenHead const* const head = headOf(tab);
            return head == nullptr ? std::vector<HeadAction>{} : head->actions().get();
        };
        Row row{.children = {}, .gap = 1, .common = {}, .tone = std::nullopt};
        row.common.testId = "head_trail";
        row.common.visible = Prop<bool>{[this, tab] { return trailHasContent(tab); }};
        row.align = Align::Center;
        row.children.push_back(ui::forEach<HeadTool>(
            [this, tab] { return headToolsOf(tab); }, [](HeadTool const& tool) { return Key{tool.id}; },
            [this, tab](reactive::Signal<HeadTool> const& tool) { return headTool(tab, tool); }, Axis::Horizontal, 1));
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
                              .common = {},
                              .icon = action.peek().icon};
                button.common.testId = "head_action_" + action.peek().id;
                button.common.enabled = Prop<bool>{[&action] { return action.get().enabled; }};
                return ui::button(std::move(button));
            },
            Axis::Horizontal, 1));
        row.children.push_back(ui::forEach<ShapedMenu>(
            [this, tab] {
                std::vector<HeadMenuEntry> entries = headMenuOf(tab);
                if (entries.empty()) {
                    return std::vector<ShapedMenu>{};
                }
                std::string shape = shapeOf(entries);
                return std::vector<ShapedMenu>{ShapedMenu{.shape = std::move(shape), .entries = std::move(entries)}};
            },
            [](ShapedMenu const& menu) { return Key{menu.shape}; },
            [this, tab](reactive::Signal<ShapedMenu> const& menu) {
                Menu spec{.items = headMenuItems(tab, menu, menu.peek().entries, {}),
                          .common = {},
                          .style = MenuStyle::Button,
                          .label = {},
                          .icon = "more",
                          .emphasis = ButtonEmphasis::Normal};
                // The trigger draws the icon alone; its name is the head's `menuLabel`.
                Prop<std::string> const name{[this, tab] {
                    ScreenHead const* const head = headOf(tab);
                    reactive::Computed<std::string> const* const label = head == nullptr ? nullptr : head->menuLabel();
                    return label == nullptr ? std::string{"More actions"} : label->get();
                }};
                spec.common.a11y.name = name;
                spec.common.tooltip = name;
                spec.common.testId = "head_menu";
                return ui::menu(std::move(spec));
            },
            Axis::Horizontal, 1));
        return row;
    }

    // The head row above a tab's screen: the lead and the trail on one wrapping line, the trail at the trailing edge,
    // dropping to a line of its own where it does not fit beside the lead's minimum. Hidden while the head has no
    // content, so a screen without a head looks as it would without the row.
    [[nodiscard]] Node headRow(WorkspaceTabId tab) const {
        Row row{.children = {}, .gap = 2, .common = {}, .tone = std::nullopt};
        row.children.push_back(ui::row(buildHeadLead(tab)));
        row.children.push_back(ui::row(buildHeadTrail(tab)));
        row.common.testId = "head";
        row.common.layout.width = Sizing::stretch();
        row.common.visible = Prop<bool>{[this, tab] { return headHasContent(tab); }};
        row.align = Align::Start;
        row.justify = Justify::End;
        row.wrap = true;
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
        // One tab needs no strip to choose it by; the chords still work with the strip hidden.
        _bar->setBarShown(captions.size() > 1);
        if (_onChanged) {
            _onChanged();
        }
    }

    reactive::Runtime* _runtime;
    IViewBackend* _backend;
    DocumentScreenHost* _host;
    ContainerWidget* _parent;
    std::unique_ptr<TabsWidget> _bar;
    ScreenHostRegistry const* _hosts = nullptr;
    std::function<WorkspaceScreen(std::string_view)> _screens;
    std::function<void()> _onChanged;
    std::function<void(Refusal const&)> _diagnostics;
    HeadPlacement _headPlacement = HeadPlacement::InPage;
    std::shared_ptr<bool> _alive = std::make_shared<bool>(true);
    std::map<std::uint64_t, std::unique_ptr<Page>> _pages;
    // The pages in the bar's order.
    std::vector<WorkspaceTabId> _shownOrder;
    std::unique_ptr<WorkspaceModel> _model;
    // The overlays, oldest first; declared after the model, so they go before the tabs they were opened from.
    std::vector<std::unique_ptr<Overlay>> _overlays;
    std::uint64_t _lastOverlay = 0;
};

}  // namespace morph::ui
