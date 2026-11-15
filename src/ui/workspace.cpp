// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <morph/reactive/runtime.hpp>
#include <morph/reactive/scheduler.hpp>
#include <morph/reactive/scope.hpp>
#include <morph/ui/backend.hpp>
#include <morph/ui/host.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/route.hpp>
#include <morph/ui/shell_frame.hpp>
#include <morph/ui/value.hpp>
#include <morph/ui/view.hpp>
#include <morph/ui/workspace.hpp>
#include <morph/ui/workspace_model.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace morph::ui {

Workspace::Workspace(reactive::Runtime& runtime MORPH_LIFETIMEBOUND, IViewBackend& backend MORPH_LIFETIMEBOUND,
                     DocumentScreenHost& host MORPH_LIFETIMEBOUND, WorkspaceOptions options, ContainerWidget* parent,
                     HeadPlacement headPlacement)
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

Workspace::~Workspace() {
    *_alive = false;
    closeOverlays();
    _model.reset();
}

std::expected<std::size_t, MountError> Workspace::open(std::string_view screenId, ParamMap const& params) {
    auto opened = _model->open(screenId, params);
    if (!opened) {
        return std::unexpected(std::move(opened.error()));
    }
    return indexOf(*opened);
}

std::expected<void, MountError> Workspace::openOverlay(NavigateRequest const& request) {
    if (request.as != "dialog" && request.as != "drawer") {
        return std::unexpected(
            MountError{.screenId = request.screen, .reason = "`as: " + request.as + "` is not a dialog or a drawer"});
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

void Workspace::screenClosed(ScreenHandle const& screen) {
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

std::vector<std::string> Workspace::overlays() const {
    std::vector<std::string> out;
    out.reserve(_overlays.size());
    for (auto const& overlay : _overlays) {
        out.push_back(overlay->screenId);
    }
    return out;
}

void Workspace::setWidthClass(std::string_view widthClass) {
    bool const sheet = widthClass == "compact";
    for (auto const& overlay : _overlays) {
        showSheet(*overlay, sheet);
    }
}

void Workspace::activate(std::size_t index) {
    if (std::optional<WorkspaceTabId> const tab = tabAt(index)) {
        _model->activate(*tab);
    }
}

void Workspace::close(std::size_t index, std::function<void(bool)> done) {
    std::optional<WorkspaceTabId> const tab = tabAt(index);
    if (!tab) {
        if (done) {
            done(false);
        }
        return;
    }
    _model->close(*tab, std::move(done));
}

void Workspace::closeOthers(std::size_t index, std::function<void(bool)> done) {
    std::optional<WorkspaceTabId> const tab = tabAt(index);
    if (!tab) {
        _model->closeAll(std::move(done));
        return;
    }
    _model->closeOthers(*tab, std::move(done));
}

std::optional<std::size_t> Workspace::active() const {
    std::optional<WorkspaceTabId> const tab = _model->active().peek();
    if (!tab) {
        return std::nullopt;
    }
    return indexOf(*tab);
}

ScreenHandle* Workspace::handleOf(std::size_t index) const noexcept {
    std::optional<WorkspaceTabId> const tab = tabAt(index);
    return tab ? _model->handleOf(*tab) : nullptr;
}

ScreenHandle* Workspace::activeHandle() const {
    std::optional<std::size_t> const shown = active();
    return shown ? handleOf(*shown) : nullptr;
}

HeadParts Workspace::headPartsForActive() const {
    std::optional<WorkspaceTabId> const tab = _model->active().peek();
    if (tab) {
        // A host places the lead beside the title and gives it the minimum width it wants, so the lead has none
        // of its own: with one, a lead's natural width would never be less than it.
        Row lead = buildHeadLead(*tab, LeadPlacement::Below);
        lead.common.layout.minWidth.reset();
        return HeadParts{
            .eyebrow = ui::text(buildHeadEyebrow(*tab)),
            .lead = ui::row(std::move(lead)),
            .trail = ui::actionBar(buildHeadTrail(*tab)),
            .inlineLead = ui::row(buildHeadLead(*tab, LeadPlacement::Inline)),
        };
    }
    Text eyebrow{.text = std::string{},
                 .role = TextRole::Muted,
                 .field = {},
                 .common = {}};  // NOLINT(readability-trailing-comma): the last designator is empty braces.
    eyebrow.common.testId = "head_eyebrow";
    eyebrow.common.visible = false;
    Row lead{.children = {},
             .gap = 0,
             .common = {},
             .tone = std::nullopt};  // NOLINT(readability-trailing-comma): the last designator is empty braces.
    lead.common.testId = "head_lead";
    lead.common.visible = false;
    Row trail{.children = {},
              .gap = 0,
              .common = {},
              .tone = std::nullopt};  // NOLINT(readability-trailing-comma): the last designator is empty braces.
    trail.common.testId = "head_trail";
    trail.common.visible = false;
    Row inlineLead{.children = {},
                   .gap = 0,
                   .common = {},
                   .tone = std::nullopt};  // NOLINT(readability-trailing-comma): the last designator is empty braces.
    inlineLead.common.testId = "head_lead_inline";
    inlineLead.common.visible = false;
    return HeadParts{
        .eyebrow = ui::text(std::move(eyebrow)),
        .lead = ui::row(std::move(lead)),
        .trail = ui::row(std::move(trail)),
        .inlineLead = ui::row(std::move(inlineLead)),
    };
}

LeadPlacement Workspace::activeLeadPlacement() const {
    std::optional<WorkspaceTabId> const tab = _model->active().get();
    return tab ? leadPlacementOf(*tab) : LeadPlacement::Below;
}

bool Workspace::activeHeadOneLine() const {
    std::optional<WorkspaceTabId> const tab = _model->active().get();
    if (!tab) {
        return true;
    }
    bool const leadUnderTitle = leadHasContent(*tab) && leadPlacementOf(*tab) == LeadPlacement::Below;
    return !(eyebrowHasContent(*tab) || leadUnderTitle);
}

HeadTrailEnd Workspace::activeHeadTrailEnd() const {
    std::optional<WorkspaceTabId> const tab = _model->active().get();
    if (!tab) {
        return HeadTrailEnd::None;
    }
    if (!headMenuOf(*tab).empty()) {
        return HeadTrailEnd::Unframed;
    }
    ScreenHead const* const head = headOf(*tab);
    if (head == nullptr) {
        return HeadTrailEnd::None;
    }
    std::vector<HeadAction> const actions = head->actions().get();
    if (!actions.empty()) {
        return actions.back().iconOnly || actions.back().emphasis == ButtonEmphasis::Ghost ? HeadTrailEnd::Unframed
                                                                                           : HeadTrailEnd::Framed;
    }
    return headToolsOf(*tab).empty() ? HeadTrailEnd::None : HeadTrailEnd::Framed;
}

bool Workspace::activeHeadJoined() const {
    std::optional<WorkspaceTabId> const tab = _model->active().get();
    if (!tab || activePageInset()) {
        return false;
    }
    ScreenHead const* const head = headOf(*tab);
    reactive::Computed<bool> const* const joined = head == nullptr ? nullptr : head->headJoined();
    return joined != nullptr && joined->get();
}

TextRole Workspace::activeTitleRole() const {
    std::optional<WorkspaceTabId> const tab = _model->active().get();
    ScreenHead const* const head = tab ? headOf(*tab) : nullptr;
    reactive::Computed<TextRole> const* const role = head == nullptr ? nullptr : head->titleRole();
    return role == nullptr ? TextRole::Heading : role->get();
}

std::string Workspace::activeHeadSurface() const {
    std::optional<WorkspaceTabId> const tab = _model->active().get();
    ScreenHead const* const head = tab ? headOf(*tab) : nullptr;
    reactive::Computed<std::string> const* const surface = head == nullptr ? nullptr : head->headSurface();
    return surface == nullptr ? std::string{} : surface->get();
}

std::string Workspace::activePageSurface() const {
    std::optional<WorkspaceTabId> const tab = _model->active().get();
    ScreenHead const* const head = tab ? headOf(*tab) : nullptr;
    reactive::Computed<std::string> const* const surface = head == nullptr ? nullptr : head->pageSurface();
    return surface == nullptr ? std::string{} : surface->get();
}

std::vector<TextSpan> Workspace::activeTitleSpans() const {
    std::optional<WorkspaceTabId> const tab = _model->active().get();
    return tab ? titleSpansOf(headOf(*tab)) : std::vector<TextSpan>{};
}

bool Workspace::activePageInset() const {
    std::optional<WorkspaceTabId> const tab = _model->active().get();
    if (!tab || _model->tabs().get().size() > 1) {
        return true;
    }
    ScreenHead const* const head = headOf(*tab);
    reactive::Computed<bool> const* const edge = head == nullptr ? nullptr : head->pageEdge();
    return edge == nullptr || !edge->get();
}

bool Workspace::activeHeadShown() const {
    std::optional<WorkspaceTabId> const tab = _model->active().get();
    return tab.has_value() && headHasContent(*tab);
}

void Workspace::forceCloseAll() {
    closeOverlays();
    _model->forceCloseAll();
}

std::vector<WorkspaceTab> Workspace::tabs() const {
    std::vector<WorkspaceTab> out;
    for (WorkspaceTabId const tab : _model->order()) {
        ScreenHandle const& handle = *_model->handleOf(tab);
        out.push_back(WorkspaceTab{
            .screenId = std::string{screenOf(tab)},
            .identity = handle.identity().peek(),
            .caption = captionOf(handle),
        });
    }
    return out;
}

Workspace::Page::~Page() {
    head.reset();
    slot.reset();
}

Workspace::Overlay::~Overlay() {
    scope.reset();
    footer.reset();
    handle.reset();
    drawer.reset();
    dialog.reset();
}

void Workspace::Overlay::setOpen(bool open) const {
    if (dialog) {
        dialog->setOpen(open);
    } else {
        drawer->setOpen(open);
    }
}

void Workspace::showSheet(Overlay& overlay, bool sheet) {
    if (overlay.dialog && sheet != overlay.sheet) {
        overlay.sheet = sheet;
        overlay.dialog->setSheet(sheet);
    }
}

void Workspace::showOverlay(Overlay& overlay, ContainerWidget& footer) {
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
                        _diagnostics(Refusal{
                            .document = overlay.screenId,
                            .path = {},
                            .reason = "a dialog or drawer does not show the head menu or tools "
                                      "of " +
                                      overlay.screenId,
                        });
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

Node Workspace::overlayActions(ScreenHead& head) {
    ActionBar bar;
    bar.common.testId = "overlay_actions";
    bar.common.visible = Prop<bool>{[&head] { return !head.actions().get().empty(); }};
    bar.children.push_back(ui::forEach<HeadAction>(
        [&head] { return head.actions().get(); }, [](HeadAction const& action) { return Key{action.id}; },
        [&head](reactive::Signal<HeadAction> const& action) {
            Button button{
                .label = Prop<std::string>{[&action] { return action.get().label; }},
                .emphasis = action.peek().emphasis,
                .onClick = [&head, actionId = action.peek().id] { head.run(actionId); },
                .common = {},  // NOLINT(readability-trailing-comma): the last designator is empty braces.
            };
            button.common.testId = "overlay_action_" + action.peek().id;
            button.common.enabled = Prop<bool>{[&action] { return action.get().enabled; }};
            return ui::button(std::move(button));
        },
        Axis::Horizontal, 2));
    return ui::actionBar(std::move(bar));
}

Workspace::Overlay const* Workspace::overlayOf(std::uint64_t overlayId) const {
    auto const found = std::ranges::find(_overlays, overlayId, [](auto const& open) { return open->id; });
    return found == _overlays.end() ? nullptr : found->get();
}

void Workspace::closeOverlays() {
    while (!_overlays.empty()) {
        std::unique_ptr<Overlay> const last = std::move(_overlays.back());
        _overlays.pop_back();
        last->handle->forceClose();
    }
}

// NOLINTNEXTLINE(misc-no-recursion): menu entries nest, and the walk follows them.
std::string Workspace::shapeOf(std::vector<HeadMenuEntry> const& entries) {
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

HeadMenuEntry const* Workspace::entryAt(std::vector<HeadMenuEntry> const& entries,
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

// NOLINTNEXTLINE(misc-no-recursion): menu entries nest, and the walk follows them.
std::vector<MenuItem> Workspace::headMenuItems(WorkspaceTabId tab, reactive::Signal<ShapedMenu> const& menu,
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

std::optional<WorkspaceTabId> Workspace::tabAt(std::size_t index) const {
    std::vector<WorkspaceTabId> const order = _model->order();
    return index < order.size() ? std::optional<WorkspaceTabId>{order.at(index)} : std::nullopt;
}

std::size_t Workspace::indexOf(WorkspaceTabId tab) const {
    std::vector<WorkspaceTabId> const order = _model->order();
    auto const found = std::ranges::find(order, tab);
    return found == order.end() ? kNone : static_cast<std::size_t>(found - order.begin());
}

std::string_view Workspace::screenOf(WorkspaceTabId tab) const {
    auto const found = _pages.find(tab.value);
    return found == _pages.end() ? std::string_view{} : std::string_view{found->second->screenId};
}

std::string Workspace::captionOf(ScreenHandle const& handle) {
    std::string caption = handle.title().peek();
    if (handle.dirty().peek()) {
        caption += " •";
    }
    return caption;
}

std::expected<std::unique_ptr<ScreenHandle>, MountError> Workspace::mountPage(WorkspaceTabId tab,
                                                                              std::string_view screenId,
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
            return std::unexpected(MountError{
                .screenId = std::string{screenId},
                .reason = "no host screen `" + facts.host + "` is registered",
            });
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

ScreenHead* Workspace::headOf(WorkspaceTabId tab) const {
    static_cast<void>(_model->tabs().get());
    ScreenHandle* const handle = _model->handleOf(tab);
    return handle == nullptr ? nullptr : handle->head();
}

std::vector<HeadMenuEntry> Workspace::headMenuOf(WorkspaceTabId tab) const {
    ScreenHead const* const head = headOf(tab);
    reactive::Computed<std::vector<HeadMenuEntry>> const* const menu = head == nullptr ? nullptr : head->menu();
    return menu == nullptr ? std::vector<HeadMenuEntry>{} : menu->get();
}

std::vector<HeadTool> Workspace::headToolsOf(WorkspaceTabId tab) const {
    ScreenHead const* const head = headOf(tab);
    reactive::Computed<std::vector<HeadTool>> const* const tools = head == nullptr ? nullptr : head->tools();
    return tools == nullptr ? std::vector<HeadTool>{} : tools->get();
}

std::vector<TextSpan> Workspace::eyebrowRunsOf(WorkspaceTabId tab) const {
    ScreenHead const* const head = headOf(tab);
    if (head == nullptr) {
        return {};
    }
    reactive::Computed<std::vector<TextSpan>> const* const spans = head->eyebrowSpans();
    std::vector<TextSpan> runs = spans == nullptr ? std::vector<TextSpan>{} : spans->get();
    if (runs.empty()) {
        reactive::Computed<std::string> const* const plain = head->eyebrow();
        std::string line = plain == nullptr ? std::string{} : plain->get();
        if (!line.empty()) {
            runs.push_back(TextSpan{.text = std::move(line)});
        }
    }
    return runs;
}

Text Workspace::buildHeadEyebrow(WorkspaceTabId tab) const {
    auto const plain = [this, tab] {
        ScreenHead const* const head = headOf(tab);
        reactive::Computed<std::string> const* const eyebrow = head == nullptr ? nullptr : head->eyebrow();
        return eyebrow == nullptr ? std::string{} : eyebrow->get();
    };
    Text text{.text = Prop<std::string>{plain},
              .role = TextRole::Muted,
              .field = {},
              .common = {}};  // NOLINT(readability-trailing-comma): the last designator is empty braces.
    text.spans = Prop<std::vector<TextSpan>>{[this, tab] { return eyebrowRunsOf(tab); }};
    text.common.testId = "head_eyebrow";
    text.common.visible = Prop<bool>{[this, tab] { return eyebrowHasContent(tab); }};
    return text;
}

bool Workspace::leadHasContent(WorkspaceTabId tab) const {
    ScreenHead const* const head = headOf(tab);
    return head != nullptr &&
           (!head->subtitle().get().empty() || !subtitleSpansOf(head).empty() || !head->status().get().empty());
}

std::vector<TextSpan> Workspace::titleSpansOf(ScreenHead const* head) {
    reactive::Computed<std::vector<TextSpan>> const* const spans = head == nullptr ? nullptr : head->titleSpans();
    return spans == nullptr ? std::vector<TextSpan>{} : spans->get();
}

std::vector<TextSpan> Workspace::subtitleSpansOf(ScreenHead const* head) {
    reactive::Computed<std::vector<TextSpan>> const* const spans = head == nullptr ? nullptr : head->subtitleSpans();
    return spans == nullptr ? std::vector<TextSpan>{} : spans->get();
}

bool Workspace::trailHasContent(WorkspaceTabId tab) const {
    ScreenHead const* const head = headOf(tab);
    return head != nullptr &&
           (!headToolsOf(tab).empty() || !head->actions().get().empty() || !headMenuOf(tab).empty() ||
            (leadPlacementOf(tab) == LeadPlacement::End && leadHasContent(tab)));
}

LeadPlacement Workspace::leadPlacementOf(WorkspaceTabId tab) const {
    ScreenHead const* const head = headOf(tab);
    reactive::Computed<LeadPlacement> const* const placement = head == nullptr ? nullptr : head->leadPlacement();
    LeadPlacement const place = placement == nullptr ? LeadPlacement::Below : placement->get();
    return place == LeadPlacement::Inline && _headPlacement == HeadPlacement::InPage ? LeadPlacement::Below : place;
}

Node Workspace::headTool(WorkspaceTabId tab, reactive::Signal<HeadTool> const& tool) const {
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

// NOLINTNEXTLINE(modernize-use-string-view): the declaration returns std::string.
std::string Workspace::idSuffix(LeadPlacement place) {
    if (place == LeadPlacement::Inline) {
        return "_inline";
    }
    return place == LeadPlacement::End ? "_end" : "";
}

Row Workspace::buildHeadLead(WorkspaceTabId tab, LeadPlacement place) const {
    std::string const suffix = idSuffix(place);
    auto const subtitle = [this, tab] {
        ScreenHead const* const head = headOf(tab);
        return head == nullptr ? std::string{} : head->subtitle().get();
    };
    auto const status = [this, tab] {
        ScreenHead const* const head = headOf(tab);
        return head == nullptr ? std::vector<HeadBadge>{} : head->status().get();
    };
    Row row{.children = {},
            .gap = 1,
            .common = {},
            .tone = std::nullopt};  // NOLINT(readability-trailing-comma): the last designator is empty braces.
    row.wrap = true;
    row.align = Align::Center;
    row.common.testId = "head_lead" + suffix;
    if (place == LeadPlacement::Below) {
        row.common.layout.width = Sizing::stretch();
        row.common.layout.minWidth = kHeadLeadMinimum;
    } else if (place == LeadPlacement::Inline) {
        row.common.layout.width = Sizing::stretch();
    }
    row.common.visible =
        Prop<bool>{[this, tab, place] { return leadHasContent(tab) && leadPlacementOf(tab) == place; }};
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
    Text text{.text = Prop<std::string>{subtitle},
              .role = TextRole::Muted,
              .field = {},
              .common = {}};  // NOLINT(readability-trailing-comma): the last designator is empty braces.
    text.spans = Prop<std::vector<TextSpan>>{spans};
    text.common.testId = "head_subtitle" + suffix;
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
        [suffix](reactive::Signal<PlacedBadge> const& placed) {
            Badge badge{
                .tone = Prop<Tone>{[&placed] { return placed.get().badge.tone; }},
                .text = Prop<std::string>{[&placed] { return placed.get().badge.text; }},
                .icon = placed.peek().badge.icon,
                .common = {},
            };
            badge.common.testId = "head_status" + suffix + "_" + std::to_string(placed.peek().index);
            return ui::badge(std::move(badge));
        },
        Axis::Horizontal, 1, {}, true));
    return row;
}

ActionBar Workspace::buildHeadTrail(WorkspaceTabId tab) const {
    auto const actions = [this, tab] {
        ScreenHead const* const head = headOf(tab);
        return head == nullptr ? std::vector<HeadAction>{} : head->actions().get();
    };
    ActionBar row;
    // Wrapping, so a part with nothing in it (no tools, no menu) takes no gap: a non-wrapping row would leave one
    // after the last action, short of the edge by a step.
    row.wrap = true;
    row.common.testId = "head_trail";
    row.common.visible = Prop<bool>{[this, tab] { return trailHasContent(tab); }};
    row.align = Align::Center;
    row.children.push_back(ui::forEach<HeadTool>(
        [this, tab] { return headToolsOf(tab); }, [](HeadTool const& tool) { return Key{tool.id}; },
        [this, tab](reactive::Signal<HeadTool> const& tool) { return headTool(tab, tool); }, Axis::Horizontal, 1));
    row.children.push_back(ui::row(buildHeadLead(tab, LeadPlacement::End)));
    row.children.push_back(ui::forEach<HeadAction>(
        actions, [](HeadAction const& action) { return Key{action.id}; },
        [this, tab](reactive::Signal<HeadAction> const& action) {
            Button button{
                .label = Prop<std::string>{[&action] {
                    HeadAction const& current = action.get();
                    return current.iconOnly ? std::string{} : current.label;
                }},
                .emphasis = action.peek().emphasis,
                .onClick =
                    [this, tab, actionId = action.peek().id] {
                        ScreenHandle* const handle = _model->handleOf(tab);
                        if (ScreenHead* const head = handle == nullptr ? nullptr : handle->head()) {
                            head->run(actionId);
                        }
                    },
                .common = {},  // NOLINT(readability-trailing-comma): the last designator is empty braces.
                .icon = action.peek().icon,
            };
            button.common.testId = "head_action_" + action.peek().id;
            // An icon-only button is named and titled by its label, which it does not draw.
            auto const labelWhenIconOnly = [&action] {
                HeadAction const& current = action.get();
                return current.iconOnly ? current.label : std::string{};
            };
            button.common.a11y.name = Prop<std::string>{labelWhenIconOnly};
            button.common.tooltip = Prop<std::string>{labelWhenIconOnly};
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
            Menu spec{
                .items = headMenuItems(tab, menu, menu.peek().entries, {}),
                .common = {},  // NOLINT(readability-trailing-comma): the last designator is empty braces.
                .style = MenuStyle::Button,
                .label = {},
                .icon = "more",
                .emphasis = ButtonEmphasis::Normal,
            };
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

Node Workspace::headRow(WorkspaceTabId tab) const {
    Row row{.children = {},
            .gap = 2,
            .common = {},
            .tone = std::nullopt};  // NOLINT(readability-trailing-comma): the last designator is empty braces.
    row.children.push_back(ui::row(buildHeadLead(tab, LeadPlacement::Below)));
    row.children.push_back(ui::actionBar(buildHeadTrail(tab)));
    row.common.testId = "head";
    row.common.layout.width = Sizing::stretch();
    row.common.visible = Prop<bool>{[this, tab] { return leadHasContent(tab) || trailHasContent(tab); }};
    row.align = Align::Start;
    row.justify = Justify::End;
    row.wrap = true;
    Column block{.children = {}, .gap = 1, .common = {}};
    block.children.push_back(ui::text(buildHeadEyebrow(tab)));
    block.children.push_back(ui::row(std::move(row)));
    block.common.testId = "head_block";
    block.common.layout.width = Sizing::stretch();
    block.common.visible = Prop<bool>{[this, tab] { return headHasContent(tab); }};
    return ui::column(std::move(block));
}

void Workspace::showTabs() {
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
            _shownOrder.insert(_shownOrder.begin() + static_cast<std::ptrdiff_t>(std::min(index, _shownOrder.size())),
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
}  // namespace morph::ui
