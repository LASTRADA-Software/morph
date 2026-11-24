// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <morph/export.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../attributes.hpp"
#include "backend.hpp"
#include "navigator.hpp"
#include "shell_parts.hpp"
#include "shell_parts_view.hpp"

/// @file
/// @brief `morph::ui::ShellFrame`: the frame a client shows its screens in — a sidebar with the navigator, a top bar,
///        and the page the workspace's tabs sit on. Built from backend widgets, so every frontend shows it.
///
/// Specified in `docs/spec/ui/shell_frame.md`.

namespace morph::ui {

/// @brief The frame's width for the sidebar, in units.
inline constexpr int kShellSidebarWidth = 28;

/// @brief The chat column's width, in units: enough for a message to read as prose beside the page.
inline constexpr int kShellChatWidth = 32;
static_assert(kShellSidebarWidth + 80 <= kExpandedWidthUnits,
              "the page beside the sidebar must not be narrower than a compact window");

/// @brief The narrowest the top bar's title and head lead are given before the head's trail wraps to a line of its
///        own, in units; the same bound as the in-page head's.
inline constexpr int kShellHeadLeadMinimum = 32;

/// @brief What ends the line of a head's trail, which decides how far a compact top bar's trailing end stands from the
///        window's edge: a button that draws no frame pads itself, so a frame leaves it to the button; anything else
///        reaches the edge of its box.
enum class HeadTrailEnd : std::uint8_t {
    None,      ///< Nothing is in the trail: the lead's text or badges end the line.
    Unframed,  ///< A ghost or icon-only action, or the head menu's trigger, which carries its own padding.
    Framed,    ///< A framed action, or a head tool such as a select.
};

/// @brief What a `ShellFrame` shows and reports.
struct ShellFrameOptions {
    /// @brief The application's name, at the head of the sidebar.
    std::string applicationName;
    /// @brief The navigator's entries and handlers.
    NavigatorOptions navigator;
    /// @brief Called when the person asks to sign out; empty hides the sign-out button.
    std::function<void()> onSignOut;
};

/// @brief The frame around a client's screens.
///
/// A row filling the window: the sidebar (surface `sidebar`, `kShellSidebarWidth` units wide), holding the
/// application's name and a `Navigator`, and the main column holding the top bar (surface `toolbar`) above the page
/// (surface `page`), where the client places the workspace. The top bar is one line that wraps only when it is out of
/// room: the shown screen's title with the active head's lead (subtitle and badges) under it, and the head's trail
/// (tools and actions, then the principal while it sits in the head) at the trailing edge, dropping to a line of its
/// own where it does not fit. The principal and a sign-out button sit at the sidebar's foot. Signed out, the sidebar
/// and the top bar are hidden and the page is all there is. Compact (`setCompact(true)`), the sidebar is hidden and
/// the top bar starts with a navigation menu over the navigator's entries, ending with the principal's sign-out entry,
/// so the page has nearly the whole window. Neither copyable nor movable; owned by the owner thread of its backend.
class MORPH_API ShellFrame {
public:
    /// @brief The frame, appended to @p parent.
    /// @param backend The backend its widgets are made with. Borrowed.
    /// @param parent  The container it is appended to; null makes it a backend root.
    /// @param options What it shows and reports.
    ShellFrame(IViewBackend& backend MORPH_LIFETIMEBOUND, ContainerWidget* parent, ShellFrameOptions options);

    ShellFrame(ShellFrame const&) = delete;
    ShellFrame& operator=(ShellFrame const&) = delete;
    ShellFrame(ShellFrame&&) = delete;
    ShellFrame& operator=(ShellFrame&&) = delete;
    ~ShellFrame() = default;

    /// @brief The container the client places the workspace in.
    /// @return It.
    [[nodiscard]] ContainerWidget& page() noexcept { return *_content; }

    /// @brief The container a client mounts a chat panel into, at the window's trailing edge.
    /// @return It.
    [[nodiscard]] ContainerWidget& chatSlot() noexcept { return *_chatSlot; }

    /// @brief Says whether there is a chat to show. It stays hidden signed out and while compact whatever this says: a
    ///        compact window has no width to spare for a column beside the page.
    /// @param shown Whether there is a chat.
    void setChatShown(bool shown);

    /// @brief The navigator in the sidebar.
    /// @return It.
    [[nodiscard]] Navigator& navigator() noexcept { return *_navigator; }

    /// @brief The container a client mounts the active head's eyebrow into: at the top of the lead block, above the
    ///        title, shown only while `setHeadShown(true)`.
    /// @return It.
    [[nodiscard]] ContainerWidget& headEyebrowSlot() noexcept { return *_eyebrow; }

    /// @brief The container a client mounts the active head's lead into: under the title, in the top bar's first
    ///        line, shown only while `setHeadShown(true)`.
    /// @return It.
    [[nodiscard]] ContainerWidget& headLeadSlot() noexcept { return *_headLead; }

    /// @brief The container a client mounts the active head's inline lead into: on the title's line, right after the
    ///        title, shown only while `setLeadInline(true)` and `setHeadShown(true)`.
    /// @return It.
    [[nodiscard]] ContainerWidget& headInlineSlot() noexcept { return *_headInline; }

    /// @brief The container a client mounts the active head's trail into: at the trailing edge of the top bar's
    ///        line, followed by the principal while it sits in the head, as one wrapping row that drops under the
    ///        title where it does not fit beside it. Shown only while `setHeadShown(true)`.
    /// @return It.
    [[nodiscard]] ContainerWidget& headSlot() noexcept { return *_headSlot; }

    /// @brief Shows or hides the three head slots. The frame cannot tell whether what is mounted there shows anything,
    ///        so the client says; a new frame starts with them hidden.
    /// @param shown Whether the slots are shown.
    void setHeadShown(bool shown);

    /// @brief Sets the role the top bar's title is drawn in: `Heading`, or `Headline` for a record's identity. A
    ///        tab's caption, the window's title and an overlay's title keep their own.
    /// @param role The role. A new frame draws `Heading`.
    void setTitleRole(TextRole role) { _title->setRole(role); }

    /// @brief Shows the head's lead on the title's line (`headInlineSlot()`), or hides that slot. The slot is a
    ///        content-sized item of a wrapping line inside the lead block, which always stretches from
    ///        `kShellHeadLeadMinimum` units: the lead follows the title while both fit the block, and wraps to a line
    ///        under the title when they do not, before the head's trail wraps. The spare width lies after the lead,
    ///        before the trail.
    /// @param inlineLead Whether the lead sits inline. A new frame has it under the title.
    void setLeadInline(bool inlineLead);

    /// @brief Aligns the top line's items across it while the head is a single line (no eyebrow, nothing under the
    ///        title), so a title alone is level with the trail's buttons; a taller head aligns the items' tops, so
    ///        the trail's top is the eyebrow's.
    /// @param oneLine Whether the head is one line. A new frame aligns tops.
    void setHeadOneLine(bool oneLine);

    /// @brief Tells the frame that the shown page continues the head's block, so the top bar is drawn on the
    ///        `toolbarJoined` surface, whatever surface the screen asks for.
    /// @param joined Whether the page continues the head. A new frame is not joined.
    void setHeadJoined(bool joined);

    /// @brief Sets the surface the shown screen asks the top bar to be drawn on.
    /// @param surface The surface's name; empty for none, which is `toolbar`. A name no surface answers to draws as
    ///        none does.
    void setHeadSurface(std::string_view surface);

    /// @brief Sets the surface the shown screen asks the page area to be drawn on.
    /// @param surface The surface's name; empty for none, which is `page`. A name no surface answers to draws as
    ///        `page` does.
    void setPageSurface(std::string_view surface) { _page->setSurface(surface.empty() ? "page" : surface); }

    /// @brief Moves the principal from the sidebar's foot to the end of the head's trail, centred on its actions.
    /// Compact, the head draws
    ///        none and the navigation menu's sign-out entry stays described by the principal.
    /// @param inHead Whether the principal sits in the top bar. A new frame has it in the sidebar.
    void setPrincipalInHead(bool inHead);

    /// @brief Shows the shown screen's title in the top bar.
    /// @param title The title; empty shows none.
    void setTitle(std::string_view title);

    /// @brief Insets the page's content by the page's step, or lets it fill the page to its edges. A hidden spacer
    ///        takes no gap, so hiding the three inset spacers leaves the content at the page's own edges: for a
    ///        screen that insets itself. A new frame insets.
    /// @param inset Whether the content is inset.
    void setPageInset(bool inset);

    /// @brief Shows the shown screen's title as runs in the top bar, in place of the plain title: an identifier in the
    ///        monospace face beside plain words. The title's role gives every run its size and weight.
    /// @param spans The runs; empty draws `setTitle`'s text.
    void setTitleSpans(std::vector<TextSpan> const& spans);

    /// @brief Shows who is signed in, at the sidebar's foot and as the compact menu's sign-out entry's description.
    /// @param principal The principal's name; empty shows none.
    void setPrincipal(std::string_view principal);

    /// @brief Shows or hides the sidebar and the top bar: signed out, the page is all there is.
    /// @param signedIn Whether someone is signed in.
    void showSignedIn(bool signedIn);

    /// @brief Takes the frame's compact form, or its full one again. Compact, the sidebar, with the principal and the
    ///        sign-out button at its foot, is hidden, and the top bar starts with the navigation menu
    ///        (`shell_nav_menu`, the framework icon `nav.menu`, named "Navigation"), which lists the navigator's
    ///        entries (`Navigator::menuEntries`) and opens the one chosen, as the navigator does; with a sign-out
    ///        handler it ends with a separator and a "Sign out" entry described by the principal, which signs out.
    ///        A new frame is not compact.
    /// @param compact Whether the frame is compact.
    void setCompact(bool compact);

    /// @brief Tells the frame what ends the head's trail. Compact, the line's ends are padded by step 1, which is the
    ///        whole inset a button that pads itself needs; anything else that ends the line (text, a badge, a select
    ///        or a button that draws a frame) stands step 4 from the edge, the page's gutter, so the frame adds step 3
    ///        after the trail. A trail that ends in a framed button or a tool also takes the larger vertical padding
    ///        of the bar. The expanded line's own end pad is that gutter already.
    /// @param end What ends the trail. A new frame has `None`.
    void setTrailEnd(HeadTrailEnd end);

    /// @brief Whether the frame has its compact form.
    /// @return It.
    [[nodiscard]] bool compact() const noexcept { return _compact; }

    /// @brief Whether the chat column is in view: there is a chat, someone is signed in, and the frame is not compact.
    /// @return It.
    [[nodiscard]] bool chatVisible() const noexcept { return _chatWanted && _signedIn && !_compact; }

    /// @brief Installs what the frame draws of the application's declared parts ([`ShellPartsView`]): the scope
    ///        switcher at the sidebar's top, and in the top bar the search with its results panel, the inbox button
    ///        and the person's menu. Compact, the switcher, the module rows and the person's menu are entries of the
    ///        navigation menu, and the search is a button that opens a full-window overlay. A frame with no parts
    ///        draws none of them.
    /// @param view What to draw.
    void setParts(ShellPartsView view);

    /// @brief Sets what the frame reports of the parts.
    /// @param handlers The handlers; an empty one does nothing.
    void setPartsHandlers(ShellPartsHandlers handlers) { _handlers = std::move(handlers); }

    /// @brief Moves keyboard focus to the search field, as Ctrl K does; compact, opens the search overlay.
    void focusSearch();

    /// @brief Puts @p text in the search field, as typing it would, without reporting it: a client that restores a
    ///        search, or a test, drives the panel with it.
    /// @param text The text; empty clears the field and closes the panel.
    void setSearchText(std::string_view text);

    /// @brief The texts the frame wrote for the navigation menu, so the client can name the module rows with the same
    ///        words.
    /// @return The current labels.
    [[nodiscard]] ShellLabels const& labels() const noexcept { return _parts.labels; }

    /// @brief Asks a yes-or-no question in a dialog over the frame and calls @p done with the answer, once. Escape and
    ///        the cancel button answer no.
    /// @param message The question.
    /// @param yes     The confirming button's caption.
    /// @param noLabel The cancelling button's caption.
    /// @param done    Called with whether the person confirmed.
    void confirm(std::string_view message, std::string_view yes, std::string_view noLabel,
                 std::function<void(bool)> done);

    /// @brief Shows a passive, transient message over the frame's main content area. Made the first time it is
    ///        asked for; a later call replaces its text and tone and restarts its dismiss interval, whether or not
    ///        it is still showing.
    /// @param text The message, UTF-8.
    /// @param tone The tone, mapped to the backend's theme.
    void showToast(std::string_view text, Tone tone);

    /// @brief Installs the Tours button (`shell_tours`): a button-style menu after the help button and before the
    ///        person's menu, listing each group's heading and its tours (the title, the summary as the entry's
    ///        description). Shown while the view holds a tour; it counts as a part of the top bar, so the top bar's
    ///        tools show for it alone. Compact, it hides and a submenu with the same entries joins the navigation menu
    ///        after the help entry.
    /// @param view What to draw.
    void setTours(ShellToursView view);

    /// @brief Sets what choosing a tour, from the button or the compact submenu, calls.
    /// @param onTour The handler, given the tour's id; empty does nothing.
    void setOnTour(std::function<void(std::string const&)> onTour) { _onTour = std::move(onTour); }

    /// @brief The spotlight a guided tour is shown in: made the first time it is asked for (`shell_spotlight`), as the
    ///        last child of the main column, where it takes no room and covers the whole window, and kept as long as
    ///        the frame.
    /// @return It.
    [[nodiscard]] SpotlightWidget& spotlight();

private:
    // A row that wraps, made wrapping before it gets its children: a stack's wrap is set once, before it has any.
    [[nodiscard]] static std::unique_ptr<StackWidget> wrappingRow(IViewBackend& backend, ContainerWidget* parent);

    // The title: the runs while there are any, else the plain text. Each call replaces what the text shows whole, so
    // clearing the runs brings the plain text back.
    void showTitle();

    // The words of a connection state.
    [[nodiscard]] std::string connectionText(ConnectionState state) const;

    // The foot's words: the connection state, then the version, joined by a dot.
    [[nodiscard]] std::string footText() const;

    // The navigation menu: the scope switcher, the navigator's entries (the modules as submenus when they are
    // grouped), the person's menu, and a sign-out entry where no person's menu holds one. `_navTargets` says what
    // each top-level entry is, so a chosen path is mapped by what the frame built and not by a position.
    void showNavEntries();

    // A chosen path of the navigation menu.
    void chooseNav(std::vector<std::size_t> const& path);

    // The chrome that shows for the sign-in state and the form.
    void showChrome();

    void showChat() { _chat->setVisible(chatVisible()); }

    // What the trail holds shows: the principal while it sits in the head, the frame is expanded and there is a name,
    // the slot while the head shows, and the row while either does. Every call that changes one of those ends here,
    // so no stale visibility outlives it.
    void showTrail();

    // The bar's padding above and below its line. Expanded it is step 2, or the theme's `head.paddingY` where a
    // backend reads one (`setHeadBar`). Compact it is step 1 while the head is one
    // line of plain text and icons, which fits the bar to one touch target; a head with an eyebrow or a lead under
    // its title, or one that ends in a framed button, is content the card's own padding frames, so it takes step 3.
    void showBarInset();

    // Whether the inline lead shows: see setLeadInline.
    void showLeadPlace() { _headInline->setVisible(_headShown && _leadInline); }

    // The top bar's surface: the joined one while the page continues the head, else the one the screen asks for, else
    // the plain toolbar.
    void showTopBarSurface();

    // What a top-level entry of the navigation menu is.
    struct NavTarget {
        enum class Kind : std::uint8_t { Context, Navigator, Principal, SignOut, Help, Tours, Separator };
        Kind kind = Kind::Separator;
        // For a navigator entry, the index of the first one among the menu's entries.
        std::size_t offset = 0;
    };

    // What a person's menu entry does.
    struct PrincipalChoice {
        enum class Kind : std::uint8_t { None, Locale, Format, SignOut };
        Kind kind = Kind::None;
        std::size_t index = 0;
    };

    // One hit's row in a results column: the button holds a muted kind, the title and a muted detail.
    struct HitRow {
        std::unique_ptr<ButtonWidget> button;
        std::unique_ptr<StackWidget> content;
        std::unique_ptr<TextWidget> kind;
        std::unique_ptr<TextWidget> title;
        std::unique_ptr<TextWidget> detail;
    };

    // The widgets of one results column, a panel under the field or the compact overlay's body.
    struct ResultsColumn {
        std::vector<std::unique_ptr<TextWidget>> texts;
        std::vector<HitRow> rows;
        void clear();
        // Takes the widgets out of sight and out of reach of a lookup by id, until they are destroyed.
        void hide();
    };

    // Counts the click handlers of hit rows that are running, for the lifetime of the rows they belong to.
    class DispatchScope {
    public:
        explicit DispatchScope(int& depth) : _depth{depth} { ++_depth; }
        DispatchScope(DispatchScope const&) = delete;
        DispatchScope& operator=(DispatchScope const&) = delete;
        ~DispatchScope() { --_depth; }

    private:
        int& _depth;
    };

    // The groups as heading entries and their tours; fills `_tourMap`.
    [[nodiscard]] std::vector<MenuEntry> tourEntries();

    void chooseTour(std::size_t entry);

    [[nodiscard]] bool barParts() const noexcept {
        return _parts.search.shown || _parts.inbox.shown || _parts.principal.shown || _parts.help.shown ||
               _toursView.shown();
    }

    // The widgets the parts are drawn with, all hidden until `setParts` says otherwise.
    void buildParts();

    // The brand's round mark. A badge takes its icon once, so a mark with another icon is made again, at the row's
    // start.
    void makeBrandMark(std::string const& icon);

    // The product mark and the foot.
    void showBrand();

    // Everything the parts draw, from the view and the frame's form.
    void showParts();

    // The options as radio entries, a heading entry where the group changes; `_contextMap` maps an entry to its
    // option, with no value for a heading.
    [[nodiscard]] std::vector<MenuEntry> contextEntries();

    // The switcher as a submenu of the navigation menu.
    [[nodiscard]] MenuEntry contextMenuEntry();

    // The person's menu: the name (the detail beneath), the roles under a heading, the languages and the formats under
    // theirs, and sign-out. `_principalMap` maps an entry to what choosing it does.
    [[nodiscard]] std::vector<MenuEntry> principalEntries();

    // The person's menu as a submenu of the navigation menu.
    [[nodiscard]] MenuEntry principalMenuEntry();

    void answerConfirm(bool yes);

    void choosePrincipal(std::size_t entry);

    // The hits as one flat list, in the order they are drawn.
    [[nodiscard]] std::size_t hitCount() const;

    [[nodiscard]] std::string hitSignature() const;

    // The source and the place in it of the hit at @p flat.
    [[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>> hitAt(std::size_t flat) const;

    void searchEdited(std::string const& text, bool fromOverlay);

    void clearSearch();

    void openHighlighted();

    void searchChord(std::string const& chord);

    // The panel and the overlay's body from the view.
    //
    // The rows are rebuilt only when what they draw changed, so a row that holds focus keeps it while an unrelated
    // part of the view changes. Rows replaced from inside a hit row's own click handler are kept, hidden, until the
    // next call outside any handler: the handler runs from the widget being replaced.
    void showSearch();

    // Everything the results draw, as one text: equal texts draw the same widgets.
    [[nodiscard]] std::string resultsKey(bool panel, bool overlay) const;

    // Drops a column's widgets, or keeps them out of sight while a hit row's handler is running.
    void retire(ResultsColumn& column);

    void renderResults(StackWidget& into, ResultsColumn& column);

    void addHitRow(StackWidget& into, ResultsColumn& column, ShellHitView const& hit, std::size_t source,
                   std::size_t place, bool highlighted);

    // Declared parent before child, so destruction takes the children first.
    IViewBackend& _backend;
    std::unique_ptr<StackWidget> _root;
    std::unique_ptr<StackWidget> _sidebar;
    std::unique_ptr<SpacerWidget> _sideStart;
    std::unique_ptr<StackWidget> _side;
    std::unique_ptr<SpacerWidget> _sideTop;
    std::unique_ptr<TextWidget> _name;
    std::unique_ptr<Navigator> _navigator;
    std::unique_ptr<TextWidget> _principal;
    std::unique_ptr<ButtonWidget> _signOut;
    std::unique_ptr<SpacerWidget> _sideEnd;
    std::unique_ptr<StackWidget> _main;
    // Lazily made, the first time showToast is called.
    std::unique_ptr<ToastWidget> _toast;
    // Made the first time `spotlight` is called, a child of the main column.
    std::unique_ptr<SpotlightWidget> _spotlight;
    std::unique_ptr<StackWidget> _topBar;
    std::unique_ptr<SpacerWidget> _topAbove;
    std::unique_ptr<StackWidget> _topLine;
    std::unique_ptr<SpacerWidget> _topBelow;
    std::unique_ptr<SpacerWidget> _topStart;
    std::unique_ptr<StackWidget> _topMain;
    std::unique_ptr<MenuWidget> _navMenu;
    std::unique_ptr<StackWidget> _leadBlock;
    // Declared between the lead block and the title: members are made in declaration order, so the slot is the first
    // child of the block.
    std::unique_ptr<StackWidget> _eyebrow;
    // The title and the inline lead, a wrapping line of the lead block: declared after the eyebrow and before the
    // lead slot, so it sits between them.
    std::unique_ptr<StackWidget> _titleLine;
    std::unique_ptr<TextWidget> _title;
    std::unique_ptr<StackWidget> _headInline;
    std::unique_ptr<StackWidget> _headLead;
    // The head's actions and the principal, a wrapping row: declared before both so it is their parent.
    std::unique_ptr<StackWidget> _trail;
    std::unique_ptr<StackWidget> _headSlot;
    std::unique_ptr<TextWidget> _topPrincipal;
    std::unique_ptr<StackWidget> _trailInset;
    std::unique_ptr<SpacerWidget> _trailInsetStart;
    std::unique_ptr<SpacerWidget> _trailInsetEnd;
    std::unique_ptr<SpacerWidget> _topEnd;
    std::unique_ptr<StackWidget> _page;
    std::unique_ptr<SpacerWidget> _top;
    std::unique_ptr<StackWidget> _padded;
    std::unique_ptr<SpacerWidget> _left;
    std::unique_ptr<StackWidget> _content;
    std::unique_ptr<SpacerWidget> _right;
    std::unique_ptr<StackWidget> _chat;
    std::unique_ptr<SpacerWidget> _chatStart;
    std::unique_ptr<StackWidget> _chatColumn;
    std::unique_ptr<SpacerWidget> _chatTop;
    std::unique_ptr<StackWidget> _chatSlot;
    std::unique_ptr<SpacerWidget> _chatBottom;
    std::unique_ptr<SpacerWidget> _chatEnd;
    std::unique_ptr<StackWidget> _brand;
    std::unique_ptr<BadgeWidget> _brandMark;
    std::unique_ptr<TextWidget> _brandTitle;
    std::unique_ptr<StackWidget> _footer;
    std::unique_ptr<TextWidget> _footerDot;
    std::unique_ptr<TextWidget> _footerState;
    std::unique_ptr<TextWidget> _footerVersion;
    std::unique_ptr<ButtonWidget> _help;
    std::unique_ptr<MenuWidget> _tours;
    std::unique_ptr<MenuWidget> _context;
    std::unique_ptr<StackWidget> _tools;
    std::unique_ptr<StackWidget> _searchBox;
    std::unique_ptr<TextInputWidget> _search;
    std::unique_ptr<PopoverWidget> _popover;
    std::unique_ptr<StackWidget> _resultsBody;
    std::unique_ptr<ButtonWidget> _searchOpenButton;
    std::unique_ptr<StackWidget> _inboxGroup;
    std::unique_ptr<ButtonWidget> _inbox;
    std::unique_ptr<BadgeWidget> _inboxCount;
    std::unique_ptr<TextWidget> _inboxError;
    std::unique_ptr<MenuWidget> _principalMenu;
    std::unique_ptr<DialogWidget> _overlay;
    std::unique_ptr<TextInputWidget> _overlayInput;
    std::unique_ptr<StackWidget> _overlayBody;
    std::unique_ptr<DialogWidget> _confirm;
    std::unique_ptr<ButtonWidget> _confirmYes;
    std::unique_ptr<ButtonWidget> _confirmNo;
    std::function<void(bool)> _confirmDone;
    ResultsColumn _panelResults;
    ResultsColumn _overlayResults;
    // Rows replaced while one of their own handlers ran; destroyed at the next `showSearch` outside a handler.
    std::vector<ResultsColumn> _retired;
    std::string _resultsKey;
    int _dispatching = 0;
    ShellPartsView _parts;
    ShellPartsHandlers _handlers;
    ShellToursView _toursView;
    std::function<void(std::string const&)> _onTour;
    std::vector<NavTarget> _navTargets;
    std::vector<std::optional<std::size_t>> _contextMap;
    std::vector<PrincipalChoice> _principalMap;
    // Maps a Tours menu entry to its tour's id, with no value for a heading.
    std::vector<std::optional<std::string>> _tourMap;
    std::string _searchText;
    std::optional<std::size_t> _highlight;
    bool _searchDismissed = false;
    bool _popoverOpen = false;
    std::function<void()> _onSignOut;
    std::string _principalName;
    std::string _brandIcon;
    std::string _titleText;
    std::vector<TextSpan> _titleSpans;
    std::string _headSurface;
    std::size_t _navEntryCount = 0;
    bool _signedIn = true;
    bool _chatWanted = false;
    bool _compact = false;
    bool _headShown = false;
    bool _leadInline = false;
    bool _headJoined = false;
    bool _principalInHead = false;
    HeadTrailEnd _trailEnd = HeadTrailEnd::None;
    bool _headTall = false;
};

}  // namespace morph::ui
