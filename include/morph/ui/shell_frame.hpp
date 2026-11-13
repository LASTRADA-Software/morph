// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
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
class ShellFrame {
public:
    /// @brief The frame, appended to @p parent.
    /// @param backend The backend its widgets are made with. Borrowed.
    /// @param parent  The container it is appended to; null makes it a backend root.
    /// @param options What it shows and reports.
    ShellFrame(IViewBackend& backend MORPH_LIFETIMEBOUND, ContainerWidget* parent, ShellFrameOptions options)
        : _backend{backend},
          _root{backend.createStack(parent, Axis::Horizontal)},
          _sidebar{backend.createStack(_root.get(), Axis::Horizontal)},
          _sideStart{backend.createSpacer(_sidebar.get())},
          _side{backend.createStack(_sidebar.get(), Axis::Vertical)},
          _sideTop{backend.createSpacer(_side.get())},
          _name{backend.createText(_side.get())},
          _navigator{std::make_unique<Navigator>(backend, _side.get(), std::move(options.navigator))},
          _principal{backend.createText(_side.get())},
          _signOut{backend.createButton(_side.get())},
          _sideEnd{backend.createSpacer(_sidebar.get())},
          _main{backend.createStack(_root.get(), Axis::Vertical)},
          _topBar{backend.createStack(_main.get(), Axis::Vertical)},
          _topAbove{backend.createSpacer(_topBar.get())},
          _topLine{backend.createStack(_topBar.get(), Axis::Horizontal)},
          _topBelow{backend.createSpacer(_topBar.get())},
          _topStart{backend.createSpacer(_topLine.get())},
          _topMain{wrappingRow(backend, _topLine.get())},
          _navMenu{backend.createMenu(_topMain.get(), MenuStyle::Button)},
          _leadBlock{backend.createStack(_topMain.get(), Axis::Vertical)},
          _eyebrow{backend.createStack(_leadBlock.get(), Axis::Vertical)},
          _titleLine{wrappingRow(backend, _leadBlock.get())},
          _title{backend.createText(_titleLine.get())},
          _headInline{backend.createStack(_titleLine.get(), Axis::Horizontal)},
          _headLead{backend.createStack(_leadBlock.get(), Axis::Vertical)},
          _trail{wrappingRow(backend, _topMain.get())},
          _headSlot{backend.createStack(_trail.get(), Axis::Horizontal)},
          _topPrincipal{backend.createText(_trail.get())},
          _trailInset{backend.createStack(_topLine.get(), Axis::Horizontal)},
          _trailInsetStart{backend.createSpacer(_trailInset.get())},
          _trailInsetEnd{backend.createSpacer(_trailInset.get())},
          _topEnd{backend.createSpacer(_topLine.get())},
          _page{backend.createStack(_main.get(), Axis::Vertical)},
          _top{backend.createSpacer(_page.get())},
          _padded{backend.createStack(_page.get(), Axis::Horizontal)},
          _left{backend.createSpacer(_padded.get())},
          _content{backend.createStack(_padded.get(), Axis::Vertical)},
          _right{backend.createSpacer(_padded.get())},
          _chat{backend.createStack(_root.get(), Axis::Horizontal)},
          _chatStart{backend.createSpacer(_chat.get())},
          _chatColumn{backend.createStack(_chat.get(), Axis::Vertical)},
          _chatTop{backend.createSpacer(_chatColumn.get())},
          _chatSlot{backend.createStack(_chatColumn.get(), Axis::Vertical)},
          _chatBottom{backend.createSpacer(_chatColumn.get())},
          _chatEnd{backend.createSpacer(_chat.get())},
          _onSignOut{options.onSignOut} {
        _root->setLayout({.width = Sizing::stretch(), .height = Sizing::stretch()});
        _root->setTestId("shell");

        _sidebar->setSurface("sidebar");
        _sidebar->setLayout({.width = Sizing::fixed(kShellSidebarWidth), .height = Sizing::stretch()});
        _sidebar->setGap(3);
        _sidebar->setTestId("shell_sidebar");
        // Spacers either side and above pad the sidebar's content by its gaps.
        _sideStart->setLayout({.width = Sizing::fixed(0), .height = {}});
        _side->setLayout({.width = Sizing::stretch(), .height = Sizing::stretch()});
        _side->setGap(3);
        _sideTop->setLayout({.width = {}, .height = Sizing::fixed(0)});
        _sideEnd->setLayout({.width = Sizing::fixed(0), .height = {}});
        _name->setRole(TextRole::Heading);
        _name->setText(options.applicationName);
        _name->setTestId("shell_application");

        // The principal and the sign-out button sit at the sidebar's foot: the navigator takes the sidebar's spare
        // height above them. Left on the top line they would compete with the head's actions for its trailing edge.
        _navigator->root().setLayout({.width = {}, .height = Sizing::stretch()});
        _principal->setRole(TextRole::Muted);
        _principal->setTestId("shell_principal");
        _signOut->setLabel("Sign out");
        _signOut->setEmphasis(ButtonEmphasis::Ghost);
        _signOut->setTestId("shell_signOut");
        _signOut->setVisible(static_cast<bool>(options.onSignOut));
        _signOut->setOnClick(std::move(options.onSignOut));

        _main->setLayout({.width = Sizing::stretch(), .height = Sizing::stretch()});
        // One line that wraps only when it is out of room: the title with the head's lead under it, and the head's
        // trail at the trailing edge, which drops to a line of its own where it does not fit beside the lead's
        // minimum. Wrapping is the one choice every backend can draw with the widgets it has; eliding would lose the
        // badges and actions the head exists for, and an overflow menu needs a node and a measuring pass the
        // vocabulary does not have.
        _topBar->setSurface("toolbar");
        _topBar->setTestId("shell_topbar");
        // Zero-sized spacers above and below pad the line by the bar's gap, as they do at the line's two ends by its
        // own gap; the line is centred across the bar.
        _topBar->setGap(2);
        _topBar->setHeadBar(true);
        _topAbove->setLayout({.width = {}, .height = Sizing::fixed(0)});
        _topBelow->setLayout({.width = {}, .height = Sizing::fixed(0)});
        _topLine->setGap(4);
        _topLine->setAlign(Align::Center);
        _topLine->setTestId("shell_topbar_line");
        // Spacers at both ends pad the line's content by its gap: expanded, the page's gutter, so the head's text and
        // the page's content share their edges.
        _topStart->setLayout({.width = Sizing::fixed(0), .height = {}});
        _topEnd->setLayout({.width = Sizing::fixed(0), .height = {}});
        _topMain->setLayout({.width = Sizing::stretch(), .height = {}});
        _topMain->setGap(3);
        _topMain->setAlign(Align::Start);
        _topMain->setJustify(Justify::End);
        _topMain->setTestId("shell_topbar_main");
        // The compact form's navigation: the navigator's entries as one button-style menu, kept current with it.
        _navMenu->setTestId("shell_nav_menu");
        _navMenu->setAccessibleName("Navigation");
        _navMenu->setTooltip("Navigation");
        _navMenu->setTrigger(MenuTrigger{.label = {}, .icon = "nav.menu", .emphasis = ButtonEmphasis::Ghost});
        showNavEntries();
        _navMenu->setOnActivate([this](std::vector<std::size_t> const& path) { chooseNav(path); });
        _navMenu->setVisible(false);
        _navigator->setOnChanged([this] { showNavEntries(); });
        _leadBlock->setLayout({.width = Sizing::stretch(), .height = {}, .minWidth = kShellHeadLeadMinimum});
        _eyebrow->setTestId("shell_head_eyebrow");
        _eyebrow->setVisible(false);
        // The title and the inline lead share a line that wraps inside the lead block: where both do not fit, the
        // lead goes under the title, and the head's trail keeps the title's line until the block's minimum is gone.
        _titleLine->setGap(2);
        _titleLine->setAlign(Align::Center);
        _titleLine->setTestId("shell_title_line");
        _title->setRole(TextRole::Heading);
        _title->setTestId("shell_title");
        _headLead->setLayout({.width = Sizing::stretch(), .height = {}});
        _headLead->setTestId("shell_head_lead");
        _headLead->setVisible(false);
        _headInline->setTestId("shell_head_inline");
        _headInline->setVisible(false);
        // The trail: the head's actions, then the principal, as one wrapping row that stands where the actions did.
        // A wrapping row gives no gap to an item with no width, so a shown slot with nothing mounted in it leaves no
        // space before the principal; the principal wraps under the actions where the two do not fit one line.
        _trail->setGap(3);
        _trail->setAlign(Align::Center);
        _trail->setTestId("shell_head_trail");
        _trail->setVisible(false);
        _headSlot->setTestId("shell_head");
        _headSlot->setVisible(false);
        _topPrincipal->setRole(TextRole::Muted);
        _topPrincipal->setTestId("shell_head_principal");
        _topPrincipal->setVisible(false);
        // Two empty items a gap apart: with the line's gap on either side they add step 3 after the trail. The
        // compact line's end pad is step 1, which is too close for a button that draws a frame.
        _trailInset->setGap(2);
        _trailInset->setTestId("shell_trail_end_inset");
        _trailInset->setVisible(false);
        _trailInsetStart->setLayout({.width = Sizing::fixed(0), .height = {}});
        _trailInsetEnd->setLayout({.width = Sizing::fixed(0), .height = {}});

        // The page is padded by spacing step 4: zero-sized spacers above and either side, each a gap away.
        _page->setSurface("page");
        _page->setLayout({.width = Sizing::stretch(), .height = Sizing::stretch()});
        _page->setGap(4);
        _page->setTestId("shell_page");
        _top->setLayout({.width = {}, .height = Sizing::fixed(0)});
        _padded->setLayout({.width = Sizing::stretch(), .height = Sizing::stretch()});
        _padded->setGap(4);
        _left->setLayout({.width = Sizing::fixed(0), .height = {}});
        _content->setLayout({.width = Sizing::stretch(), .height = Sizing::stretch()});
        _right->setLayout({.width = Sizing::fixed(0), .height = {}});

        // Spacers on every side pad the chat's content by its gaps, as the sidebar's do.
        _chat->setSurface("sidebar");
        _chat->setLayout({.width = Sizing::fixed(kShellChatWidth), .height = Sizing::stretch()});
        _chat->setGap(3);
        _chat->setTestId("shell_chat");
        _chatStart->setLayout({.width = Sizing::fixed(0), .height = {}});
        _chatColumn->setLayout({.width = Sizing::stretch(), .height = Sizing::stretch()});
        _chatColumn->setGap(3);
        _chatTop->setLayout({.width = {}, .height = Sizing::fixed(0)});
        _chatSlot->setLayout({.width = Sizing::stretch(), .height = Sizing::stretch()});
        _chatBottom->setLayout({.width = {}, .height = Sizing::fixed(0)});
        _chatEnd->setLayout({.width = Sizing::fixed(0), .height = {}});
        buildParts();
        showChat();
    }

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
    void setChatShown(bool shown) {
        _chatWanted = shown;
        showChat();
    }

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
    void setHeadShown(bool shown) {
        _headShown = shown;
        _eyebrow->setVisible(shown);
        _headLead->setVisible(shown);
        _headSlot->setVisible(shown);
        showLeadPlace();
        showTrail();
    }

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
    void setLeadInline(bool inlineLead) {
        _leadInline = inlineLead;
        showLeadPlace();
    }

    /// @brief Aligns the top line's items across it while the head is a single line (no eyebrow, nothing under the
    ///        title), so a title alone is level with the trail's buttons; a taller head aligns the items' tops, so
    ///        the trail's top is the eyebrow's.
    /// @param oneLine Whether the head is one line. A new frame aligns tops.
    void setHeadOneLine(bool oneLine) {
        _headTall = !oneLine;
        _topMain->setAlign(oneLine ? Align::Center : Align::Start);
        showBarInset();
        showTrail();
    }

    /// @brief Tells the frame that the shown page continues the head's block, so the top bar is drawn on the
    ///        `toolbarJoined` surface, whatever surface the screen asks for.
    /// @param joined Whether the page continues the head. A new frame is not joined.
    void setHeadJoined(bool joined) {
        _headJoined = joined;
        showTopBarSurface();
    }

    /// @brief Sets the surface the shown screen asks the top bar to be drawn on.
    /// @param surface The surface's name; empty for none, which is `toolbar`. A name no surface answers to draws as
    ///        none does.
    void setHeadSurface(std::string_view surface) {
        _headSurface = std::string{surface};
        showTopBarSurface();
    }

    /// @brief Sets the surface the shown screen asks the page area to be drawn on.
    /// @param surface The surface's name; empty for none, which is `page`. A name no surface answers to draws as
    ///        `page` does.
    void setPageSurface(std::string_view surface) { _page->setSurface(surface.empty() ? "page" : surface); }

    /// @brief Moves the principal from the sidebar's foot to the end of the head's trail, centred on its actions.
    /// Compact, the head draws
    ///        none and the navigation menu's sign-out entry stays described by the principal.
    /// @param inHead Whether the principal sits in the top bar. A new frame has it in the sidebar.
    void setPrincipalInHead(bool inHead) {
        _principalInHead = inHead;
        showChrome();
    }

    /// @brief Shows the shown screen's title in the top bar.
    /// @param title The title; empty shows none.
    void setTitle(std::string_view title) {
        _titleText = std::string{title};
        showTitle();
    }

    /// @brief Insets the page's content by the page's step, or lets it fill the page to its edges. A hidden spacer
    ///        takes no gap, so hiding the three inset spacers leaves the content at the page's own edges: for a
    ///        screen that insets itself. A new frame insets.
    /// @param inset Whether the content is inset.
    void setPageInset(bool inset) {
        _top->setVisible(inset);
        _left->setVisible(inset);
        _right->setVisible(inset);
    }

    /// @brief Shows the shown screen's title as runs in the top bar, in place of the plain title: an identifier in the
    ///        monospace face beside plain words. The title's role gives every run its size and weight.
    /// @param spans The runs; empty draws `setTitle`'s text.
    void setTitleSpans(std::vector<TextSpan> const& spans) {
        _titleSpans = spans;
        showTitle();
    }

    /// @brief Shows who is signed in, at the sidebar's foot and as the compact menu's sign-out entry's description.
    /// @param principal The principal's name; empty shows none.
    void setPrincipal(std::string_view principal) {
        _principal->setText(principal);
        _topPrincipal->setText(principal);
        _principalName = std::string{principal};
        showNavEntries();
        showTrail();
    }

    /// @brief Shows or hides the sidebar and the top bar: signed out, the page is all there is.
    /// @param signedIn Whether someone is signed in.
    void showSignedIn(bool signedIn) {
        _signedIn = signedIn;
        showChrome();
    }

    /// @brief Takes the frame's compact form, or its full one again. Compact, the sidebar, with the principal and the
    ///        sign-out button at its foot, is hidden, and the top bar starts with the navigation menu
    ///        (`shell_nav_menu`, the framework icon `nav.menu`, named "Navigation"), which lists the navigator's
    ///        entries (`Navigator::menuEntries`) and opens the one chosen, as the navigator does; with a sign-out
    ///        handler it ends with a separator and a "Sign out" entry described by the principal, which signs out.
    ///        A new frame is not compact.
    /// @param compact Whether the frame is compact.
    void setCompact(bool compact) {
        _compact = compact;
        // The compact top bar is tighter: a touch-sized menu square beside the title leaves no room for the full
        // steps, so the bar pads its line by step 1 above and below (more for a head that is not one plain line, see
        // showBarInset), and the line pads its ends by step 1 and spaces its items by step 2.
        showBarInset();
        _topLine->setGap(compact ? 1 : 4);
        _topMain->setGap(compact ? 2 : 3);
        showChrome();
    }

    /// @brief Tells the frame what ends the head's trail. Compact, the line's ends are padded by step 1, which is the
    ///        whole inset a button that pads itself needs; anything else that ends the line (text, a badge, a select
    ///        or a button that draws a frame) stands step 4 from the edge, the page's gutter, so the frame adds step 3
    ///        after the trail. A trail that ends in a framed button or a tool also takes the larger vertical padding
    ///        of the bar. The expanded line's own end pad is that gutter already.
    /// @param end What ends the trail. A new frame has `None`.
    void setTrailEnd(HeadTrailEnd end) {
        _trailEnd = end;
        showBarInset();
        showTrail();
    }

    /// @brief Whether the frame has its compact form.
    /// @return It.
    [[nodiscard]] bool compact() const noexcept { return _compact; }

    /// @brief Installs what the frame draws of the application's declared parts ([`ShellPartsView`]): the scope
    ///        switcher at the sidebar's top, and in the top bar the search with its results panel, the inbox button
    ///        and the person's menu. Compact, the switcher, the module rows and the person's menu are entries of the
    ///        navigation menu, and the search is a button that opens a full-window overlay. A frame with no parts
    ///        draws none of them.
    /// @param view What to draw.
    void setParts(ShellPartsView view) {
        if (view == _parts) {
            return;
        }
        std::string const previousSignature = hitSignature();
        _parts = std::move(view);
        if (hitSignature() != previousSignature) {
            _highlight = hitCount() == 0 ? std::nullopt : std::optional<std::size_t>{0};
        }
        showParts();
    }

    /// @brief Sets what the frame reports of the parts.
    /// @param handlers The handlers; an empty one does nothing.
    void setPartsHandlers(ShellPartsHandlers handlers) { _handlers = std::move(handlers); }

    /// @brief Moves keyboard focus to the search field, as Ctrl K does; compact, opens the search overlay.
    void focusSearch() {
        if (!_parts.search.shown) {
            return;
        }
        if (_compact) {
            _overlay->setOpen(true);
            _overlayInput->focus();
        } else {
            _search->focus();
        }
    }

    /// @brief Puts @p text in the search field, as typing it would, without reporting it: a client that restores a
    ///        search, or a test, drives the panel with it.
    /// @param text The text; empty clears the field and closes the panel.
    void setSearchText(std::string_view text) {
        _searchText = std::string{text};
        _searchDismissed = false;
        _search->setText(text);
        _overlayInput->setText(text);
        showSearch();
    }

    /// @brief The texts the frame wrote for the navigation menu, so the client can name the module rows with the same
    ///        words.
    /// @return The current labels.
    [[nodiscard]] ShellLabels const& labels() const noexcept { return _parts.labels; }

    /// @brief Asks a yes-or-no question in a dialog over the frame and calls @p done with the answer, once. Escape and
    ///        the cancel button answer no.
    /// @param message The question.
    /// @param yes     The confirming button's caption.
    /// @param no      The cancelling button's caption.
    /// @param done    Called with whether the person confirmed.
    void confirm(std::string_view message, std::string_view yes, std::string_view no, std::function<void(bool)> done) {
        if (_confirm == nullptr) {
            _confirm = _backend.createDialog(_main.get());
            _confirm->setTestId("shell_confirm");
            _confirm->setOnDismiss([this] { answerConfirm(false); });
            _confirmYes = _backend.createButton(&_confirm->footer());
            _confirmYes->setTestId("shell_confirm_yes");
            _confirmYes->setEmphasis(ButtonEmphasis::Danger);
            _confirmYes->setOnClick([this] { answerConfirm(true); });
            _confirmNo = _backend.createButton(&_confirm->footer());
            _confirmNo->setTestId("shell_confirm_no");
            _confirmNo->setOnClick([this] { answerConfirm(false); });
        }
        _confirm->setTitle(message);
        _confirmYes->setLabel(yes);
        _confirmNo->setLabel(no);
        _confirmDone = std::move(done);
        _confirm->setOpen(true);
    }

    /// @brief Shows a passive, transient message over the frame's main content area. Made the first time it is
    ///        asked for; a later call replaces its text and tone and restarts its dismiss interval, whether or not
    ///        it is still showing.
    /// @param text The message, UTF-8.
    /// @param tone The tone, mapped to the backend's theme.
    void showToast(std::string_view text, Tone tone) {
        if (_toast == nullptr) {
            _toast = _backend.createToast(_main.get());
        }
        _toast->show(text, tone);
    }

private:
    // A row that wraps, made wrapping before it gets its children: a stack's wrap is set once, before it has any.
    [[nodiscard]] static std::unique_ptr<StackWidget> wrappingRow(IViewBackend& backend, ContainerWidget* parent) {
        auto row = backend.createStack(parent, Axis::Horizontal);
        row->setWrap(true);
        return row;
    }

    // The title: the runs while there are any, else the plain text. Each call replaces what the text shows whole, so
    // clearing the runs brings the plain text back.
    void showTitle() {
        if (_titleSpans.empty()) {
            _title->setText(_titleText);
        } else {
            _title->setSpans(_titleSpans);
        }
    }

    // The words of a connection state.
    [[nodiscard]] std::string connectionText(ConnectionState state) const {
        ShellLabels const& labels = _parts.labels;
        return state == ConnectionState::Connected      ? labels.connected
               : state == ConnectionState::Reconnecting ? labels.reconnecting
                                                        : labels.offline;
    }

    // The foot's words: the connection state, then the version, joined by a dot.
    [[nodiscard]] std::string footText() const {
        std::string text;
        if (_parts.connection) {
            text = connectionText(*_parts.connection);
        }
        if (_parts.brand.shown && !_parts.brand.version.empty()) {
            text += (text.empty() ? "" : " · ") + _parts.brand.version;
        }
        return text;
    }

    // The navigation menu: the scope switcher, the navigator's entries (the modules as submenus when they are
    // grouped), the person's menu, and a sign-out entry where no person's menu holds one. `_navTargets` says what
    // each top-level entry is, so a chosen path is mapped by what the frame built and not by a position.
    void showNavEntries() {
        std::vector<MenuEntry> entries;
        _navTargets.clear();
        auto const add = [&](MenuEntry entry, NavTarget::Kind kind, std::size_t offset) {
            entries.push_back(std::move(entry));
            _navTargets.push_back(NavTarget{.kind = kind, .offset = offset});
        };
        if (_parts.context.shown) {
            MenuEntry submenu = contextMenuEntry();
            add(std::move(submenu), NavTarget::Kind::Context, 0);
            add(MenuEntry{.separator = true}, NavTarget::Kind::Separator, 0);
        }
        std::vector<MenuEntry> navigation = _navigator->menuEntries();
        _navEntryCount = navigation.size();
        std::size_t const firstNavigation = entries.size();
        for (std::size_t index = 0; index < navigation.size(); ++index) {
            add(std::move(navigation.at(index)), NavTarget::Kind::Navigator, firstNavigation);
        }
        if (_parts.help.shown) {
            add(MenuEntry{.separator = true}, NavTarget::Kind::Separator, 0);
            add(MenuEntry{.label = _parts.labels.help}, NavTarget::Kind::Help, 0);
        }
        if (_parts.principal.shown) {
            add(MenuEntry{.separator = true}, NavTarget::Kind::Separator, 0);
            MenuEntry person = principalMenuEntry();
            add(std::move(person), NavTarget::Kind::Principal, 0);
        } else if (_onSignOut) {
            add(MenuEntry{.separator = true}, NavTarget::Kind::Separator, 0);
            add(MenuEntry{.label = _parts.labels.signOut, .description = _principalName}, NavTarget::Kind::SignOut, 0);
        }
        // The sidebar's foot is not on screen, so its text ends the menu as a heading. A separator target, because a
        // heading is inert on choose just as a separator is.
        if (std::string const foot = footText(); !foot.empty()) {
            add(MenuEntry{.separator = true}, NavTarget::Kind::Separator, 0);
            MenuEntry heading;
            heading.label = foot;
            heading.heading = true;
            heading.enabled = false;
            add(std::move(heading), NavTarget::Kind::Separator, 0);
        }
        _navMenu->setItems(entries);
    }

    // A chosen path of the navigation menu.
    void chooseNav(std::vector<std::size_t> const& path) {
        if (path.empty() || path.front() >= _navTargets.size()) {
            return;
        }
        NavTarget const target = _navTargets.at(path.front());
        switch (target.kind) {
            case NavTarget::Kind::Navigator: {
                std::vector<std::size_t> inner = path;
                inner.front() -= target.offset;
                _navigator->choose(inner);
                return;
            }
            case NavTarget::Kind::SignOut:
                if (_onSignOut && path.size() == 1) {
                    _onSignOut();
                }
                return;
            case NavTarget::Kind::Help:
                if (path.size() == 1 && _handlers.onHelp) {
                    _handlers.onHelp();
                }
                return;
            case NavTarget::Kind::Context:
                if (path.size() == 2 && path.at(1) < _contextMap.size() && _contextMap.at(path.at(1)) &&
                    _handlers.onContext) {
                    _handlers.onContext(*_contextMap.at(path.at(1)));
                }
                return;
            case NavTarget::Kind::Principal:
                if (path.size() == 2) {
                    choosePrincipal(path.at(1));
                }
                return;
            case NavTarget::Kind::Separator:
            default:
                return;
        }
    }

    // The chrome that shows for the sign-in state and the form.
    void showChrome() {
        _sidebar->setVisible(_signedIn && !_compact);
        _topBar->setVisible(_signedIn);
        _navMenu->setVisible(_compact);
        showParts();
        showChat();
        showTrail();
    }

    void showChat() { _chat->setVisible(_chatWanted && _signedIn && !_compact); }

    // What the trail holds shows: the principal while it sits in the head, the frame is expanded and there is a name,
    // the slot while the head shows, and the row while either does. Every call that changes one of those ends here,
    // so no stale visibility outlives it.
    void showTrail() {
        bool const principalShown =
            _principalInHead && !_compact && !_principalName.empty() && !_parts.principal.shown;
        _topPrincipal->setVisible(principalShown);
        _trail->setVisible((_headShown || principalShown) && !barParts());
        _trailInset->setVisible(_compact && _trailEnd != HeadTrailEnd::Unframed);
    }

    // The bar's padding above and below its line. Expanded it is step 2, or the theme's `head.paddingY` where a
    // backend reads one (`setHeadBar`). Compact it is step 1 while the head is one
    // line of plain text and icons, which fits the bar to one touch target; a head with an eyebrow or a lead under
    // its title, or one that ends in a framed button, is content the card's own padding frames, so it takes step 3.
    void showBarInset() {
        bool const plain = !_headTall && _trailEnd != HeadTrailEnd::Framed;
        int const compactStep = plain ? 1 : 3;
        int const step = _compact ? compactStep : 2;
        _topBar->setGap(step);
        // Only the expanded bar's gap is the theme's `head.paddingY`; a compact bar fits the touch target.
        _topBar->setHeadBar(!_compact);
    }

    // Whether the inline lead shows: see setLeadInline.
    void showLeadPlace() { _headInline->setVisible(_headShown && _leadInline); }

    // The top bar's surface: the joined one while the page continues the head, else the one the screen asks for, else
    // the plain toolbar.
    void showTopBarSurface() {
        if (_headJoined) {
            _topBar->setSurface("toolbarJoined");
        } else {
            _topBar->setSurface(_headSurface.empty() ? "toolbar" : _headSurface);
        }
    }

    // What a top-level entry of the navigation menu is.
    struct NavTarget {
        enum class Kind : std::uint8_t { Context, Navigator, Principal, SignOut, Help, Separator };
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
        void clear() {
            rows.clear();
            texts.clear();
        }
        // Takes the widgets out of sight and out of reach of a lookup by id, until they are destroyed.
        void hide() {
            for (HitRow& row : rows) {
                row.button->setVisible(false);
                row.button->setTestId({});
            }
            for (auto& text : texts) {
                text->setVisible(false);
                text->setTestId({});
            }
        }
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

    [[nodiscard]] bool barParts() const noexcept {
        return _parts.search.shown || _parts.inbox.shown || _parts.principal.shown || _parts.help.shown;
    }

    // The widgets the parts are drawn with, all hidden until `setParts` says otherwise.
    void buildParts() {
        // The product mark stands where the application's name does, above the switcher.
        _brand = _backend.createStack(_side.get(), Axis::Horizontal);
        _side->moveChild(*_brand, 2);
        _brand->setGap(2);
        _brand->setAlign(Align::Center);
        _brand->setTestId("shell_brand");
        _brand->setVisible(false);
        makeBrandMark({});
        _brandTitle = _backend.createText(_brand.get());
        _brandTitle->setRole(TextRole::Heading);
        _brandTitle->setTestId("shell_brand_title");
        _context = _backend.createMenu(_side.get(), MenuStyle::Button);
        _side->moveChild(*_context, 3);
        _context->setTestId("shell_context");
        _context->setVisible(false);
        _context->setOnActivate([this](std::vector<std::size_t> const& path) {
            if (path.size() == 1 && path.front() < _contextMap.size() && _contextMap.at(path.front()) &&
                _handlers.onContext) {
                _handlers.onContext(*_contextMap.at(path.front()));
            }
        });

        // The top bar's tools sit in the wrapping row after the navigation menu: the search, the inbox, the person.
        _tools = _backend.createStack(_topMain.get(), Axis::Horizontal);
        _topMain->moveChild(*_tools, 1);
        _tools->setGap(3);
        _tools->setAlign(Align::Center);
        _tools->setTestId("shell_topbar_tools");
        _tools->setVisible(false);
        _searchBox = _backend.createStack(_tools.get(), Axis::Vertical);
        _searchBox->setLayout(
            {.width = Sizing::stretch(), .height = {}, .minWidth = std::nullopt, .maxWidth = kShellSearchWidth});
        _searchBox->setTestId("shell_search_box");
        _search = _backend.createTextInput(_searchBox.get(), TextInputMode::SingleLine);
        _search->setTestId("shell_search");
        _search->setOnChange([this](std::string const& text) { searchEdited(text, false); });
        _search->setOnSubmit([this](std::string const&) { openHighlighted(); });
        _search->setKeys({"Down", "Up", "Escape", "Shift+Enter", "Alt+Enter"},
                         [this](std::string const& chord) { searchChord(chord); });
        _popover = _backend.createPopover(_searchBox.get());
        _popover->setTestId("shell_search_popover");
        _popover->setWidth(kShellSearchWidth);
        _popover->setOnDismiss([this] {
            _searchDismissed = true;
            showSearch();
        });
        _resultsBody = _backend.createStack(_popover.get(), Axis::Vertical);
        _resultsBody->setTestId("shell_search_results");
        _resultsBody->setGap(1);
        _searchOpenButton = _backend.createButton(_tools.get());
        _searchOpenButton->setTestId("shell_search_open");
        _searchOpenButton->setLabel("⌕");
        _searchOpenButton->setEmphasis(ButtonEmphasis::Ghost);
        _searchOpenButton->setVisible(false);
        _searchOpenButton->setOnClick([this] { focusSearch(); });
        _inboxGroup = _backend.createStack(_tools.get(), Axis::Horizontal);
        _inboxGroup->setGap(1);
        _inboxGroup->setAlign(Align::Center);
        _inboxGroup->setTestId("shell_inbox_group");
        _inboxGroup->setVisible(false);
        _inbox = _backend.createButton(_inboxGroup.get());
        _inbox->setTestId("shell_inbox");
        _inbox->setOnClick([this] {
            if (_handlers.onInbox) {
                _handlers.onInbox();
            }
        });
        _inboxCount = _backend.createBadge(_inboxGroup.get());
        _inboxCount->setTestId("shell_inbox_count");
        _inboxCount->setVisible(false);
        _inboxError = _backend.createText(_inboxGroup.get());
        _inboxError->setTestId("shell_inbox_error");
        _inboxError->setText("!");
        _inboxError->setVisible(false);
        _help = _backend.createButton(_tools.get());
        _help->setTestId("shell_help");
        _help->setLabel("?");
        _help->setVisible(false);
        _help->setOnClick([this] {
            if (_handlers.onHelp) {
                _handlers.onHelp();
            }
        });
        _principalMenu = _backend.createMenu(_tools.get(), MenuStyle::Button);
        _principalMenu->setTestId("shell_principal_menu");
        _principalMenu->setVisible(false);
        _principalMenu->setOnActivate([this](std::vector<std::size_t> const& path) {
            if (path.size() == 1) {
                choosePrincipal(path.front());
            }
        });

        // The compact search: a sheet over the window with its own field and the same results.
        _overlay = _backend.createDialog(_main.get());
        _overlay->setTestId("shell_search_overlay");
        _overlay->setSheet(true);
        _overlay->setOnDismiss([this] { _overlay->setOpen(false); });
        _overlayInput = _backend.createTextInput(_overlay.get(), TextInputMode::SingleLine);
        _overlayInput->setTestId("shell_search_overlay_input");
        _overlayInput->setOnChange([this](std::string const& text) { searchEdited(text, true); });
        _overlayInput->setOnSubmit([this](std::string const&) { openHighlighted(); });
        _overlayInput->setKeys({"Down", "Up", "Escape", "Shift+Enter", "Alt+Enter"},
                               [this](std::string const& chord) { searchChord(chord); });
        _overlayBody = _backend.createStack(_overlay.get(), Axis::Vertical);
        _overlayBody->setTestId("shell_search_overlay_results");
        _overlayBody->setGap(1);

        // The sidebar's foot: the connection state, then the application's version at the far end.
        _footer = _backend.createStack(_side.get(), Axis::Horizontal);
        _footer->setGap(2);
        _footer->setAlign(Align::Center);
        _footer->setTestId("shell_footer");
        _footer->setVisible(false);
        _footerDot = _backend.createText(_footer.get());
        _footerDot->setText("●");
        _footerDot->setTestId("shell_connection_dot");
        _footerState = _backend.createText(_footer.get());
        _footerState->setRole(TextRole::Muted);
        _footerState->setTestId("shell_connection");
        _footerState->setLayout({.width = Sizing::stretch(), .height = {}});
        _footerVersion = _backend.createText(_footer.get());
        _footerVersion->setRole(TextRole::Muted);
        _footerVersion->setTestId("shell_version");

        _root->setKeys({"Ctrl+K"}, [this](std::string const&) { focusSearch(); });
    }

    // The brand's round mark. A badge takes its icon once, so a mark with another icon is made again, at the row's
    // start.
    void makeBrandMark(std::string const& icon) {
        _brandMark = _backend.createBadge(_brand.get());
        _brand->moveChild(*_brandMark, 0);
        _brandMark->setTestId("shell_brand_mark");
        _brandMark->setTone(Tone::Ok);
        _brandMark->setShape(ControlShape::Round);
        if (!icon.empty()) {
            _brandMark->setIcon(icon);
        }
        _brandIcon = icon;
    }

    // The product mark and the foot.
    void showBrand() {
        ShellBrandView const& brand = _parts.brand;
        _name->setVisible(!brand.shown);
        _brand->setVisible(brand.shown);
        if (brand.shown) {
            if (brand.icon != _brandIcon) {
                makeBrandMark(brand.icon);
            }
            // Without an icon the mark holds the title's first letter.
            _brandMark->setText(brand.icon.empty() && !brand.title.empty() ? detail::firstLetter(brand.title)
                                                                           : std::string{});
            _brandTitle->setText(brand.title);
        }
        bool const connection = _parts.connection.has_value();
        _footer->setVisible(connection || (brand.shown && !brand.version.empty()));
        _footerDot->setVisible(connection);
        _footerState->setVisible(connection);
        if (connection) {
            _footerState->setText(connectionText(*_parts.connection));
            _footerDot->setRole(*_parts.connection == ConnectionState::Connected      ? TextRole::Success
                                : *_parts.connection == ConnectionState::Reconnecting ? TextRole::Muted
                                                                                      : TextRole::Error);
        }
        _footerVersion->setVisible(brand.shown && !brand.version.empty());
        _footerVersion->setText(brand.version);
    }

    // Everything the parts draw, from the view and the frame's form.
    void showParts() {
        bool const bar = barParts();
        ShellLabels const& labels = _parts.labels;
        // The switcher, at the sidebar's top.
        _context->setVisible(_parts.context.shown);
        if (_parts.context.shown) {
            _context->setItems(contextEntries());
            ShellOptionView const* const chosen =
                _parts.context.chosen && *_parts.context.chosen < _parts.context.options.size()
                    ? &_parts.context.options.at(*_parts.context.chosen)
                    : nullptr;
            std::string const label = chosen == nullptr ? std::string{} : chosen->label;
            std::string const detail = chosen == nullptr ? std::string{} : chosen->detail;
            _context->setTrigger(MenuTrigger{.label = label.empty() ? labels.scope : label,
                                             .icon = {},
                                             .emphasis = ButtonEmphasis::Normal,
                                             .detail = detail,
                                             .avatar = {},
                                             .caret = MenuCaret::UpDown});
            _context->setEnabled(_parts.context.enabled);
            _context->setAccessibleName(labels.scope + ": " + label + (detail.empty() ? "" : ", " + detail));
        }
        // The top bar.
        _tools->setVisible(bar);
        _leadBlock->setVisible(!bar);
        _signOut->setLabel(labels.signOut);
        _searchBox->setVisible(_parts.search.shown && !_compact);
        _searchOpenButton->setVisible(_parts.search.shown && _compact);
        _searchOpenButton->setAccessibleName(labels.search);
        _search->setAccessibleName(labels.search);
        _search->setPlaceholder(_parts.search.hint.empty() ? labels.search + " (Ctrl K)"
                                                           : _parts.search.hint + " (Ctrl K)");
        _overlay->setTitle(labels.search);
        _overlay->setDismissLabel(labels.close);
        _overlayInput->setAccessibleName(labels.search);
        _resultsBody->setAccessibleName(labels.searchResults);
        _overlayBody->setAccessibleName(labels.searchResults);
        _overlayInput->setPlaceholder(_parts.search.hint.empty() ? labels.search : _parts.search.hint);
        showSearch();
        // The inbox.
        _inboxGroup->setVisible(_parts.inbox.shown);
        _inbox->setLabel(_compact ? std::string{"✉"} : _parts.inbox.title);
        _inbox->setEmphasis(_compact ? ButtonEmphasis::Ghost : ButtonEmphasis::Normal);
        std::string inboxName = _parts.inbox.title;
        bool const counted = _parts.inbox.count && *_parts.inbox.count > 0;
        if (counted) {
            inboxName += ", " + shellCountText(*_parts.inbox.count) + " " + labels.waiting;
        }
        _inbox->setAccessibleName(inboxName);
        _inboxCount->setVisible(counted);
        if (counted) {
            _inboxCount->setText(shellCountText(*_parts.inbox.count));
            _inboxCount->setTone(Tone::Err);
        }
        _inboxError->setVisible(_parts.inbox.failed);
        _inboxError->setAccessibleName(_parts.inbox.title + ": " + labels.countFailed);
        // The person's menu.
        ShellPrincipalView const& person = _parts.principal;
        _principalMenu->setVisible(person.shown && !_compact);
        if (person.shown) {
            _principalMenu->setItems(principalEntries());
            _principalMenu->setTrigger(MenuTrigger{.label = person.name,
                                                   .icon = {},
                                                   .emphasis = ButtonEmphasis::Ghost,
                                                   .detail = person.subtitle,
                                                   .avatar = person.initials,
                                                   .caret = MenuCaret::Down});
            _principalMenu->setAccessibleName(labels.signedInAs + " " + person.name);
        }
        // The help button.
        _help->setVisible(_parts.help.shown && !_compact);
        _help->setAccessibleName(labels.help);
        showBrand();
        _principal->setVisible(!_principalInHead && !person.shown);
        _signOut->setVisible(static_cast<bool>(_onSignOut) && !person.shown);
        _navigator->setSearchShown(!_parts.search.shown);
        showNavEntries();
        showTrail();
    }

    // The options as radio entries, a heading entry where the group changes; `_contextMap` maps an entry to its
    // option, with no value for a heading.
    [[nodiscard]] std::vector<MenuEntry> contextEntries() {
        std::vector<MenuEntry> entries;
        _contextMap.clear();
        std::string group;
        for (std::size_t index = 0; index < _parts.context.options.size(); ++index) {
            ShellOptionView const& option = _parts.context.options.at(index);
            if (!option.group.empty() && option.group != group) {
                MenuEntry heading;
                heading.label = option.group;
                heading.heading = true;
                heading.enabled = false;
                entries.push_back(std::move(heading));
                _contextMap.push_back(std::nullopt);
            }
            group = option.group;
            MenuEntry entry;
            entry.label = option.label;
            entry.description = option.detail;
            entry.checked = _parts.context.chosen == index;
            entries.push_back(std::move(entry));
            _contextMap.push_back(index);
        }
        return entries;
    }

    // The switcher as a submenu of the navigation menu.
    [[nodiscard]] MenuEntry contextMenuEntry() {
        MenuEntry submenu;
        std::string label = _parts.labels.scope;
        if (_parts.context.chosen && *_parts.context.chosen < _parts.context.options.size()) {
            label = _parts.context.options.at(*_parts.context.chosen).label;
        }
        submenu.label = label;
        submenu.items = contextEntries();
        submenu.enabled = _parts.context.enabled;
        return submenu;
    }

    // The person's menu: the name (the detail beneath), the roles under a heading, the languages and the formats under
    // theirs, and sign-out. `_principalMap` maps an entry to what choosing it does.
    [[nodiscard]] std::vector<MenuEntry> principalEntries() {
        std::vector<MenuEntry> entries;
        _principalMap.clear();
        ShellPrincipalView const& person = _parts.principal;
        ShellLabels const& labels = _parts.labels;
        auto const add = [&](MenuEntry entry, PrincipalChoice choice) {
            entries.push_back(std::move(entry));
            _principalMap.push_back(choice);
        };
        auto const heading = [&](std::string const& text) {
            MenuEntry entry;
            entry.label = text;
            entry.heading = true;
            entry.enabled = false;
            add(std::move(entry), PrincipalChoice{});
        };
        auto const separator = [&] { add(MenuEntry{.separator = true}, PrincipalChoice{}); };
        MenuEntry name;
        name.label = person.name;
        name.description = person.detail;
        name.enabled = false;
        add(std::move(name), PrincipalChoice{});
        if (!person.roles.empty()) {
            separator();
            heading(labels.roles);
            for (ShellRoleView const& role : person.roles) {
                MenuEntry entry;
                entry.label = role.label;
                entry.description = role.detail;
                entry.enabled = false;
                add(std::move(entry), PrincipalChoice{});
            }
        }
        if (!person.locales.empty()) {
            separator();
            heading(labels.language);
            for (std::size_t index = 0; index < person.locales.size(); ++index) {
                MenuEntry entry;
                entry.label = person.locales.at(index).label;
                entry.checked = person.chosenLocale == index;
                add(std::move(entry), PrincipalChoice{.kind = PrincipalChoice::Kind::Locale, .index = index});
            }
        }
        if (!person.formats.empty()) {
            separator();
            heading(labels.formats);
            for (std::size_t index = 0; index < person.formats.size(); ++index) {
                MenuEntry entry;
                entry.label = person.formats.at(index).label;
                entry.checked = person.chosenFormat == index;
                add(std::move(entry), PrincipalChoice{.kind = PrincipalChoice::Kind::Format, .index = index});
            }
        }
        if (_onSignOut) {
            separator();
            add(MenuEntry{.label = labels.signOut},
                PrincipalChoice{.kind = PrincipalChoice::Kind::SignOut, .index = 0});
        }
        return entries;
    }

    // The person's menu as a submenu of the navigation menu.
    [[nodiscard]] MenuEntry principalMenuEntry() {
        MenuEntry submenu;
        submenu.label = _parts.principal.name;
        submenu.description = _parts.principal.subtitle;
        submenu.items = principalEntries();
        return submenu;
    }

    void answerConfirm(bool yes) {
        _confirm->setOpen(false);
        if (auto done = std::exchange(_confirmDone, {})) {
            done(yes);
        }
    }

    void choosePrincipal(std::size_t entry) {
        if (entry >= _principalMap.size()) {
            return;
        }
        PrincipalChoice const choice = _principalMap.at(entry);
        if (choice.kind == PrincipalChoice::Kind::Locale && _handlers.onLocale) {
            _handlers.onLocale(choice.index);
        } else if (choice.kind == PrincipalChoice::Kind::Format && _handlers.onFormat) {
            _handlers.onFormat(choice.index);
        } else if (choice.kind == PrincipalChoice::Kind::SignOut && _onSignOut) {
            _onSignOut();
        }
    }

    // The hits as one flat list, in the order they are drawn.
    [[nodiscard]] std::size_t hitCount() const {
        std::size_t total = 0;
        for (ShellSourceView const& source : _parts.search.sources) {
            total += source.hits.size();
        }
        return total;
    }

    [[nodiscard]] std::string hitSignature() const {
        std::string out;
        for (ShellSourceView const& source : _parts.search.sources) {
            out += source.title + "\x1f";
            for (ShellHitView const& hit : source.hits) {
                out += hit.kind + "\x1e" + hit.title + "\x1e" + hit.detail + "\x1d";
            }
        }
        return out;
    }

    // The source and the place in it of the hit at @p flat.
    [[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>> hitAt(std::size_t flat) const {
        for (std::size_t source = 0; source < _parts.search.sources.size(); ++source) {
            std::size_t const size = _parts.search.sources.at(source).hits.size();
            if (flat < size) {
                return std::pair{_parts.search.sources.at(source).source, flat};
            }
            flat -= size;
        }
        return std::nullopt;
    }

    void searchEdited(std::string const& text, bool fromOverlay) {
        _searchText = text;
        _searchDismissed = false;
        // The two fields are one search: the one not typed in follows.
        (fromOverlay ? _search : _overlayInput)->setText(text);
        if (_handlers.onSearchText) {
            _handlers.onSearchText(text);
        }
        showSearch();
    }

    void clearSearch() {
        _searchDismissed = false;
        _searchText.clear();
        _search->setText({});
        _overlayInput->setText({});
        if (_handlers.onSearchText) {
            _handlers.onSearchText({});
        }
        _overlay->setOpen(false);
        showSearch();
    }

    void openHighlighted() {
        if (!_highlight) {
            return;
        }
        if (auto const place = hitAt(*_highlight)) {
            if (_handlers.onSearchOpen) {
                _handlers.onSearchOpen(place->first, place->second);
            }
            clearSearch();
        }
    }

    void searchChord(std::string const& chord) {
        std::size_t const total = hitCount();
        if (chord == "Down" || chord == "Up") {
            if (total != 0) {
                std::size_t const at = _highlight.value_or(chord == "Down" ? total - 1 : 0);
                _highlight = chord == "Down" ? (at + 1) % total : (at + total - 1) % total;
                showSearch();
            }
        } else if (chord == "Shift+Enter" || chord == "Alt+Enter") {
            if (total != 0 && _handlers.onSearchOpenAll) {
                _handlers.onSearchOpenAll();
                clearSearch();
            }
        } else if (chord == "Escape") {
            if (_popoverOpen) {
                _searchDismissed = true;
                showSearch();
            } else if (!_searchText.empty()) {
                clearSearch();
            } else {
                if (_compact) {
                    _overlay->setOpen(false);
                }
                _content->focus();
            }
        }
    }

    // The panel and the overlay's body from the view.
    //
    // The rows are rebuilt only when what they draw changed, so a row that holds focus keeps it while an unrelated
    // part of the view changes. Rows replaced from inside a hit row's own click handler are kept, hidden, until the
    // next call outside any handler: the handler runs from the widget being replaced.
    void showSearch() {
        if (_dispatching == 0) {
            _retired.clear();
        }
        bool const wanted = _parts.search.shown && !_searchText.empty();
        bool const open = wanted && !_searchDismissed && !_compact;
        bool const overlay = wanted && _compact;
        _popoverOpen = open;
        _popover->setOpen(open);
        std::string key = resultsKey(open, overlay);
        if (key == _resultsKey) {
            return;
        }
        _resultsKey = std::move(key);
        retire(_panelResults);
        retire(_overlayResults);
        if (open) {
            renderResults(*_resultsBody, _panelResults);
        }
        if (overlay) {
            renderResults(*_overlayBody, _overlayResults);
        }
    }

    // Everything the results draw, as one text: equal texts draw the same widgets.
    [[nodiscard]] std::string resultsKey(bool panel, bool overlay) const {
        std::string key = std::string{panel ? "1" : "0"} + (overlay ? "1" : "0");
        if (!panel && !overlay) {
            return key;
        }
        ShellLabels const& labels = _parts.labels;
        key += labels.searching + "\x1f" + labels.searchEmpty + "\x1f" + labels.searchFailed + "\x1f" + _searchText +
               "\x1f" + (_highlight ? std::to_string(*_highlight) : std::string{"-"}) + "\x1f";
        for (ShellSourceView const& source : _parts.search.sources) {
            key += std::to_string(source.source) + "\x1e" + source.title + "\x1e" + (source.failed ? "F" : "-") +
                   (source.pending ? "P" : "-");
            for (ShellHitView const& hit : source.hits) {
                key += "\x1e" + hit.kind + "\x1d" + hit.title + "\x1d" + hit.detail;
            }
            key += "\x1f";
        }
        return key;
    }

    // Drops a column's widgets, or keeps them out of sight while a hit row's handler is running.
    void retire(ResultsColumn& column) {
        if (_dispatching == 0) {
            column.clear();
            return;
        }
        column.hide();
        _retired.push_back(std::move(column));
        column = ResultsColumn{};
    }

    void renderResults(StackWidget& into, ResultsColumn& column) {
        ShellLabels const& labels = _parts.labels;
        auto const text = [&](std::string const& value, TextRole role, std::string const& id) {
            auto widget = _backend.createText(&into);
            widget->setText(value);
            widget->setRole(role);
            widget->setTestId(id);
            column.texts.push_back(std::move(widget));
        };
        bool pending = false;
        bool failed = false;
        for (ShellSourceView const& source : _parts.search.sources) {
            pending = pending || source.pending;
            failed = failed || source.failed;
        }
        if (pending) {
            text(labels.searching, TextRole::Muted, "shell_search_busy");
        }
        std::size_t flat = 0;
        for (std::size_t index = 0; index < _parts.search.sources.size(); ++index) {
            ShellSourceView const& source = _parts.search.sources.at(index);
            if (source.hits.empty() && !source.failed) {
                continue;
            }
            text(source.title, TextRole::Overline, "shell_search_source_" + std::to_string(source.source));
            if (source.failed) {
                text(source.title + " " + labels.searchFailed, TextRole::Muted,
                     "shell_search_failed_" + std::to_string(source.source));
            }
            for (std::size_t hit = 0; hit < source.hits.size(); ++hit, ++flat) {
                addHitRow(into, column, source.hits.at(hit), source.source, hit, _highlight == flat);
            }
        }
        if (!pending && !failed && hitCount() == 0) {
            text(labels.searchEmpty + " “" + _searchText + "”", TextRole::Muted, "shell_search_empty");
        }
    }

    void addHitRow(StackWidget& into, ResultsColumn& column, ShellHitView const& hit, std::size_t source,
                   std::size_t place, bool highlighted) {
        HitRow row;
        row.button = _backend.createButton(&into);
        row.button->setTestId("shell_search_hit_" + std::to_string(source) + "_" + std::to_string(place));
        row.button->setStyle(ButtonStyle::Item);
        row.button->setLayout({.width = Sizing::stretch(), .height = {}});
        row.button->setSelected(highlighted);
        std::string name = hit.kind;
        name += (name.empty() ? "" : ", ") + hit.title;
        name += hit.detail.empty() ? "" : ", " + hit.detail;
        row.button->setAccessibleName(name);
        row.button->setOnClick([this, source, place] {
            DispatchScope const running{_dispatching};
            if (_handlers.onSearchOpen) {
                _handlers.onSearchOpen(source, place);
            }
            clearSearch();
        });
        row.content = _backend.createStack(&row.button->content(), Axis::Horizontal);
        row.content->setGap(2);
        row.content->setAlign(Align::Center);
        row.content->setLayout({.width = Sizing::stretch(), .height = {}});
        if (!hit.kind.empty()) {
            row.kind = _backend.createText(row.content.get());
            row.kind->setText(hit.kind);
            row.kind->setRole(TextRole::Muted);
        }
        row.title = _backend.createText(row.content.get());
        row.title->setText(hit.title);
        if (!hit.detail.empty()) {
            row.detail = _backend.createText(row.content.get());
            row.detail->setText(hit.detail);
            row.detail->setRole(TextRole::Muted);
        }
        column.rows.push_back(std::move(row));
    }

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
    std::vector<NavTarget> _navTargets;
    std::vector<std::optional<std::size_t>> _contextMap;
    std::vector<PrincipalChoice> _principalMap;
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
