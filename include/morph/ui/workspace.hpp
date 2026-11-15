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
#include <morph/export.hpp>
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
#include "shell_frame.hpp"
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

/// @brief The parts of the active screen's head, for a host that lays them out itself.
struct HeadParts {
    /// @brief The eyebrow, muted, the line a host draws above the title; hidden while the head has none.
    Node eyebrow;
    /// @brief The subtitle, muted, then the badges, on one wrapping line: beside one another while they fit, and
    /// wrapped
    ///        under one another where they do not.
    Node lead;
    /// @brief The tools, the actions and the head's own menu, in one row. While the lead's placement is `end`, the
    ///        lead sits in it between the tools and the actions.
    Node trail;
    /// @brief The lead as the host draws it on the title's line, content-sized; shown only while the lead's
    ///        placement is `inline`. The host mounts it right after the title.
    Node inlineLead;
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
class MORPH_API Workspace {
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
              ContainerWidget* parent = nullptr, HeadPlacement headPlacement = HeadPlacement::InPage);

    /// @brief Closes every overlay without asking, newest first, then unmounts every tab's screen the same way, then
    ///        the pages: a callee goes before the opener its call answers.
    ~Workspace();

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
    std::expected<std::size_t, MountError> open(std::string_view screenId, ParamMap const& params);

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
    std::expected<void, MountError> openOverlay(NavigateRequest const& request);

    /// @brief A screen the workspace shows closed itself, by `return` or `close: self`: its overlay goes, or its tab
    ///        does. What a host's `DocumentScreenHost::onScreenClosed` forwards here.
    /// @param screen The closed screen's handle; one the workspace does not show does nothing.
    void screenClosed(ScreenHandle const& screen);

    /// @brief The screens shown over the tabs, oldest first.
    /// @return Each overlay's screen id.
    [[nodiscard]] std::vector<std::string> overlays() const;

    /// @brief Tells the workspace the window's width class changed, as the backend's `setOnWidthClass` listener
    ///        reports it: each open dialog overlay is a sheet while it is `compact`.
    /// @param widthClass The class.
    void setWidthClass(std::string_view widthClass);

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
    void activate(std::size_t index);

    /// @brief Shows the next tab in most-recently-used order (Ctrl+Tab). Repeated calls walk the order as it was at
    ///        the first of them; any other activation starts a new walk.
    void next() { _model->next(); }

    /// @brief Shows the previous tab in most-recently-used order (Ctrl+Shift+Tab).
    void previous() { _model->previous(); }

    /// @brief Closes a tab through its screen's close protocol.
    /// @param index The tab.
    /// @param done  Called once with whether it closed; may be empty.
    void close(std::size_t index, std::function<void(bool)> done);

    /// @brief Closes every tab but one, one at a time, stopping at the first veto.
    /// @param index The tab kept; past the last one closes every tab.
    /// @param done  Called once with whether every other tab closed; may be empty.
    void closeOthers(std::size_t index, std::function<void(bool)> done);

    /// @brief Closes every tab, one at a time, stopping at the first veto.
    /// @param done Called once with whether every tab closed; may be empty.
    void closeAll(std::function<void(bool)> done) { _model->closeAll(std::move(done)); }

    /// @brief Closes every tab without unsaved changes, one at a time, stopping at the first veto.
    /// @param done Called once with whether each of them closed; may be empty.
    void closeSaved(std::function<void(bool)> done) { _model->closeSaved(std::move(done)); }

    /// @brief How many open tabs hold unsaved changes.
    /// @return The count.
    [[nodiscard]] std::size_t dirtyTabs() const { return _model->dirtyTabs(); }

    /// @brief Closes the tabs that hold unsaved changes without asking: what a change of scope does once the person
    ///        agreed to discard the work.
    void closeDirtyTabs() { _model->forceCloseDirtyTabs(); }

    /// @brief The active tab's index.
    /// @return It, or `std::nullopt` while no tab is open.
    [[nodiscard]] std::optional<std::size_t> active() const;

    /// @brief The screen handle of a tab: what a client remounts for development reload and inspects.
    /// @param index The tab's index.
    /// @return Its handle, valid while the tab is open; null past the end.
    [[nodiscard]] ScreenHandle* handleOf(std::size_t index) const noexcept;

    /// @brief The active tab's screen handle.
    /// @return It, or null while no tab is open.
    [[nodiscard]] ScreenHandle* activeHandle() const;

    /// @brief The active tab's head as two parts, the same reactive content `headRow` builds for the in-page
    ///        rendering, for a host that mounts them elsewhere (a shell's own top bar, say). Neither is null: with no
    ///        active tab each is an invisible, empty container rather than a null `Node`, since a caller mounting them
    ///        may not expect to handle one.
    /// @return The lead and the trail, reactive to the active tab changing and to its head's own content changing.
    [[nodiscard]] HeadParts headPartsForActive() const;

    /// @brief Where the active tab's head puts its lead. A tracked read.
    /// @return The place; `Below` with no active tab. With the head in the page (`HeadPlacement::InPage`) the tab
    ///         bar carries the title, so `Inline` reads as `Below` there.
    [[nodiscard]] LeadPlacement activeLeadPlacement() const;

    /// @brief Whether the active tab's head is a single line: it has no eyebrow, and its lead is not drawn under the
    ///        title (it has none, or it sits inline or at the end). A tracked read.
    /// @return Whether the head is one line; `true` with no active tab.
    [[nodiscard]] bool activeHeadOneLine() const;

    /// @brief What ends the active tab's head trail, so a compact top bar can stand the line's end the right distance
    ///        from the window's edge. A tracked read.
    /// @return The last element of the trail, in its order (tools, actions, then the head menu): a head menu, whose
    ///         trigger is an icon button, or a last action that is icon-only or ghost is `Unframed`; any other last
    ///         action is `Framed`; with no action and no menu, a head tool is `Framed`; an empty trail, and no active
    ///         tab, is `None`.
    [[nodiscard]] HeadTrailEnd activeHeadTrailEnd() const;

    /// @brief Whether the active tab's page continues the head's block: the screen's shown root says so
    ///        (`ScreenHead::headJoined`) and the page is not inset, which two tabs or more force. A tracked read.
    /// @return Whether the head is joined to the page.
    [[nodiscard]] bool activeHeadJoined() const;

    /// @brief The text role the active screen asks for its head's title. A tracked read.
    /// @return `Heading`, or `Headline` where the screen asks for it; `Heading` with no active tab.
    [[nodiscard]] TextRole activeTitleRole() const;

    /// @brief The surface the active screen asks the shell to draw its top bar on. A tracked read.
    /// @return The surface's name; empty with no active tab and while the screen names none.
    [[nodiscard]] std::string activeHeadSurface() const;

    /// @brief The surface the active screen asks the shell to draw the page area on. A tracked read.
    /// @return The surface's name; empty with no active tab and while the screen names none.
    [[nodiscard]] std::string activePageSurface() const;

    /// @brief The active tab's title as runs, which a shell that knows them draws in place of the tab's caption. A
    ///        tracked read.
    /// @return The runs; empty with no active tab, or while the active screen gives none.
    [[nodiscard]] std::vector<TextSpan> activeTitleSpans() const;

    /// @brief Whether the shell insets the page around the active tab's screen. A tracked read. The page is inset with
    ///        no active tab and while the workspace's tab strip shows (two tabs or more), since the strip is a child
    ///        of the page and would otherwise touch the window's edges; with one tab it is inset unless the screen's
    ///        shown root lays itself at the page's edges (`ScreenHead::pageEdge`).
    /// @return Whether the page is inset.
    [[nodiscard]] bool activePageInset() const;

    /// @brief Whether the active tab's head shows anything: an eyebrow, a subtitle, a badge, a tool, an action or a
    ///        menu entry. A tracked read, so a
    ///        shell that shows the head on a line of its own follows a tab switch and a head's own changes; the head
    ///        row's own visibility is the same rule.
    /// @return Whether there is an active tab whose head has content.
    [[nodiscard]] bool activeHeadShown() const;

    /// @brief Closes every overlay, then every tab, without asking, newest first: what signing out does, since no
    ///        close guard may keep a screen of the principal who is leaving. A callee goes before the opener its call
    ///        answers, so the opener's `onCancel` runs in a live scope; then every call no mount adopted is cancelled.
    void forceCloseAll();

    /// @brief The open tabs, in order.
    /// @return Each tab's screen, identity and caption.
    [[nodiscard]] std::vector<WorkspaceTab> tabs() const;

private:
    static constexpr std::size_t kNone = static_cast<std::size_t>(-1);

    // A tab's page: the slot of the bar it shows in, the head row above its screen, and the screen's id. The model
    // destroys the screen's handle before it lets the page go.
    struct MORPH_API Page {
        std::string screenId;
        std::unique_ptr<SlotWidget> slot;
        // The head row reads the handle through the model, which answers null once the handle is gone.
        std::optional<Mounted> head;

        Page() = default;
        Page(Page const&) = delete;
        Page& operator=(Page const&) = delete;
        Page(Page&&) = delete;
        Page& operator=(Page&&) = delete;
        ~Page();
    };

    // A screen shown over the tabs: its dialog or drawer, its handle, the footer of its head's actions and the scope
    // of the effects that follow it, destroyed in the reverse order, so the handle goes before its container.
    struct MORPH_API Overlay {
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
        ~Overlay();

        void setOpen(bool open) const;
    };

    // Draws a dialog overlay as a sheet, or as a centred dialog again; a drawer has no sheet.
    static void showSheet(Overlay& overlay, bool sheet);

    // Titles a mounted overlay, puts its head's actions in its footer, opens it and makes a dismissal ask its screen.
    void showOverlay(Overlay& overlay, ContainerWidget& footer);

    // The footer of an overlay: a button per visible head action, each running the action by its id. It is an action
    // bar, so its buttons are the theme's action gap apart, as the same actions are in a head.
    [[nodiscard]] static Node overlayActions(ScreenHead& head);

    [[nodiscard]] Overlay const* overlayOf(std::uint64_t overlayId) const;

    void removeOverlay(std::uint64_t overlayId) {
        std::erase_if(_overlays, [overlayId](std::unique_ptr<Overlay> const& open) { return open->id == overlayId; });
    }

    // Closes every overlay without asking, newest first; each one's close answers its call with a cancel.
    void closeOverlays();

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
    [[nodiscard]] static std::string shapeOf(std::vector<HeadMenuEntry> const& entries);

    // The entry at @p path through @p entries; null past the end, which a binding reads only in the moment before
    // the menu is drawn again for a new shape.
    [[nodiscard]] static HeadMenuEntry const* entryAt(std::vector<HeadMenuEntry> const& entries,
                                                      std::vector<std::size_t> const& path);

    // The menu items of one level of the head menu: each bound to its entry by its place, which the shape keeps, and
    // each chosen by its id, so the screen refuses an entry hidden since by name rather than running its neighbour.
    // NOLINTNEXTLINE(misc-no-recursion): a submenu is a menu one level down.
    [[nodiscard]] std::vector<MenuItem> headMenuItems(WorkspaceTabId tab, reactive::Signal<ShapedMenu> const& menu,
                                                      std::vector<HeadMenuEntry> const& level,
                                                      std::vector<std::size_t> const& outer) const;

    [[nodiscard]] std::optional<WorkspaceTabId> tabAt(std::size_t index) const;

    [[nodiscard]] std::size_t indexOf(WorkspaceTabId tab) const;

    [[nodiscard]] std::string_view screenOf(WorkspaceTabId tab) const;

    [[nodiscard]] static std::string captionOf(ScreenHandle const& handle);

    // The model's mountPage: a page slot at the end of the bar, the screen mounted into it, a host screen from the
    // registry and a document screen from the document host, then the head row above it. The page goes with a
    // refused mount.
    std::expected<std::unique_ptr<ScreenHandle>, MountError> mountPage(WorkspaceTabId tab, std::string_view screenId,
                                                                       ParamMap const& params);

    // The head of a tab's screen, while the tab is open. A tracked read of the model's tabs, so a binding made while
    // the model is still opening the tab reads the head once it holds it.
    [[nodiscard]] ScreenHead* headOf(WorkspaceTabId tab) const;

    // The visible entries of a tab's head menu; a tracked read.
    [[nodiscard]] std::vector<HeadMenuEntry> headMenuOf(WorkspaceTabId tab) const;

    // The visible tools of a tab's head; a tracked read.
    [[nodiscard]] std::vector<HeadTool> headToolsOf(WorkspaceTabId tab) const;

    // Whether a tab's head has a subtitle, a badge, a tool, an action or a menu entry; a tracked read.
    [[nodiscard]] bool headHasContent(WorkspaceTabId tab) const {
        return eyebrowHasContent(tab) || leadHasContent(tab) || trailHasContent(tab);
    }

    // Whether the head has an eyebrow.
    [[nodiscard]] bool eyebrowHasContent(WorkspaceTabId tab) const { return !eyebrowRunsOf(tab).empty(); }

    // The head's eyebrow runs when the screen gives them, else its plain eyebrow as one run; a tracked read.
    [[nodiscard]] std::vector<TextSpan> eyebrowRunsOf(WorkspaceTabId tab) const;

    // The head's eyebrow as a muted text, hidden while there is none.
    [[nodiscard]] Text buildHeadEyebrow(WorkspaceTabId tab) const;

    // Whether the head has a subtitle or a badge.
    [[nodiscard]] bool leadHasContent(WorkspaceTabId tab) const;

    // The head's title runs; a tracked read, empty for a head that has none.
    [[nodiscard]] static std::vector<TextSpan> titleSpansOf(ScreenHead const* head);

    // The head's subtitle runs; a tracked read, empty for a head that has none.
    [[nodiscard]] static std::vector<TextSpan> subtitleSpansOf(ScreenHead const* head);

    // Whether the head has a tool, an action or a menu entry.
    // An `end` lead counts: it sits in the trail.
    [[nodiscard]] bool trailHasContent(WorkspaceTabId tab) const;

    // Where a tab's head puts its lead; a tracked read. In the page there is no title line to follow, so `Inline`
    // draws as `Below`.
    [[nodiscard]] LeadPlacement leadPlacementOf(WorkspaceTabId tab) const;

    // A tool of the head row: a search field or a select bound to the tool's value, its label the accessible name and
    // no caption, reporting each edit to the head, which runs the document's commands.
    [[nodiscard]] Node headTool(WorkspaceTabId tab, reactive::Signal<HeadTool> const& tool) const;

    // The test-id suffix of the lead built for @p place: none for the copy under the title.
    [[nodiscard]] static std::string idSuffix(LeadPlacement place);

    // The head's lead, built but not yet wrapped into an immutable Node: the subtitle (muted) and the badges on one
    // wrapping line, so they sit side by side while they fit and wrap under one another where they do not; the badges'
    // holder wraps too. It stretches from `kHeadLeadMinimum` units, so a trail beside it leaves it at least that much
    // before the trail wraps to a line of its own.
    //
    // It is built once per place and shown only while the head's placement is that place. The `Below` copy stretches
    // from its minimum. The `Inline` copy stretches with none, so it fills what the title's line gives it and its
    // subtitle wraps there, while the frame's slot, still content-sized, holds the spare width. The `End` copy is
    // content-sized, in the trail.
    [[nodiscard]] Row buildHeadLead(WorkspaceTabId tab, LeadPlacement place) const;

    // The head's trail: the tools, the actions as buttons and the head's own menu behind one icon button, in one row.
    // It is a row of actions: its parts are the theme's action gap apart, and it wraps, so a part with nothing in it
    // takes none. Hidden while the head has none of them.
    [[nodiscard]] ActionBar buildHeadTrail(WorkspaceTabId tab) const;

    // The head above a tab's screen: the eyebrow, then the row of the lead and the trail on one wrapping line, the
    // trail at the trailing edge, dropping to a line of its own where it does not fit beside the lead's minimum.
    // Hidden while the head has no content, so a screen without a head looks as it would without it.
    [[nodiscard]] Node headRow(WorkspaceTabId tab) const;

    // The model changed: the bar shows its tabs in order, with their captions, and the active tab's page.
    void showTabs();

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
