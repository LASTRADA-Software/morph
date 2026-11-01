// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../attributes.hpp"
#include "backend.hpp"
#include "navigator.hpp"

/// @file
/// @brief `morph::ui::ShellFrame`: the frame a client shows its screens in — a sidebar with the navigator, a top bar,
///        and the page the workspace's tabs sit on. Built from backend widgets, so every frontend shows it.
///
/// Specified in `docs/spec/ui/shell_frame.md`.

namespace morph::ui {

/// @brief The frame's width for the sidebar, in units.
inline constexpr int kShellSidebarWidth = 28;
static_assert(kShellSidebarWidth + 80 <= kExpandedWidthUnits,
              "the page beside the sidebar must not be narrower than a compact window");

/// @brief The narrowest the top bar's title and head lead are given before the head's trail wraps to a line of its
///        own, in units; the same bound as the in-page head's.
inline constexpr int kShellHeadLeadMinimum = 32;

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
        _topLine->setGap(3);
        _topLine->setAlign(Align::Center);
        _topLine->setTestId("shell_topbar_line");
        // Spacers at both ends pad the line's content by its gap, left and right.
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
        _navMenu->setOnActivate([this](std::vector<std::size_t> const& path) {
            if (_onSignOut && path.size() == 1 && path.front() == _navEntryCount + 1) {
                _onSignOut();
                return;
            }
            _navigator->choose(path);
        });
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
    }

    ShellFrame(ShellFrame const&) = delete;
    ShellFrame& operator=(ShellFrame const&) = delete;
    ShellFrame(ShellFrame&&) = delete;
    ShellFrame& operator=(ShellFrame&&) = delete;
    ~ShellFrame() = default;

    /// @brief The container the client places the workspace in.
    /// @return It.
    [[nodiscard]] ContainerWidget& page() noexcept { return *_content; }

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
    ///        monospace face beside plain words. The heading role gives every run its size and weight.
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
        _topLine->setGap(compact ? 1 : 3);
        _topMain->setGap(compact ? 2 : 3);
        showChrome();
    }

    /// @brief Tells the frame whether the head's last action draws a frame. Compact, the bar's end pad is step 1,
    ///        which is the whole inset a button without a frame needs; a button that draws one stands step 4 from the
    ///        edge, so the bar adds step 3 after the trail. The expanded bar's step-3 end pad already matches.
    /// @param endsFramed Whether the trail ends in a framed button. A new frame has no inset.
    void setTrailEndInset(bool endsFramed) {
        _trailEndFramed = endsFramed;
        showBarInset();
        showTrail();
    }

    /// @brief Whether the frame has its compact form.
    /// @return It.
    [[nodiscard]] bool compact() const noexcept { return _compact; }

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

    // The navigation menu lists the navigator's entries, and, with a sign-out handler, a separator and a sign-out
    // entry described by the principal.
    void showNavEntries() {
        std::vector<MenuEntry> entries = _navigator->menuEntries();
        _navEntryCount = entries.size();
        if (_onSignOut) {
            entries.push_back(MenuEntry{.separator = true});
            entries.push_back(MenuEntry{.label = "Sign out", .description = _principalName});
        }
        _navMenu->setItems(entries);
    }

    // The chrome that shows for the sign-in state and the form.
    void showChrome() {
        _sidebar->setVisible(_signedIn && !_compact);
        _topBar->setVisible(_signedIn);
        _navMenu->setVisible(_compact);
        _principal->setVisible(!_principalInHead);
        showTrail();
    }

    // What the trail holds shows: the principal while it sits in the head, the frame is expanded and there is a name,
    // the slot while the head shows, and the row while either does. Every call that changes one of those ends here,
    // so no stale visibility outlives it.
    void showTrail() {
        bool const principalShown = _principalInHead && !_compact && !_principalName.empty();
        _topPrincipal->setVisible(principalShown);
        _trail->setVisible(_headShown || principalShown);
        _trailInset->setVisible(_compact && _trailEndFramed);
    }

    // The bar's padding above and below its line. Expanded it is step 2, or the theme's `head.paddingY` where a
    // backend reads one (`setHeadBar`). Compact it is step 1 while the head is one
    // line of plain text and icons, which fits the bar to one touch target; a head with an eyebrow or a lead under
    // its title, or one that ends in a framed button, is content the card's own padding frames, so it takes step 3.
    void showBarInset() {
        bool const plain = !_headTall && !_trailEndFramed;
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
    std::function<void()> _onSignOut;
    std::string _principalName;
    std::string _titleText;
    std::vector<TextSpan> _titleSpans;
    std::string _headSurface;
    std::size_t _navEntryCount = 0;
    bool _signedIn = true;
    bool _compact = false;
    bool _headShown = false;
    bool _leadInline = false;
    bool _headJoined = false;
    bool _principalInHead = false;
    bool _trailEndFramed = false;
    bool _headTall = false;
};

}  // namespace morph::ui
