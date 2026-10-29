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
/// (surface `page`), where the client places the workspace. The top bar has two lines: the shown screen's title, the
/// principal and a sign-out button, and below them, while `setHeadShown(true)`, a line that scrolls sideways holding
/// the slot for the active tab's head row. Signed out, the sidebar and the top bar are hidden and
/// the page is all there is. Compact (`setCompact(true)`), the sidebar and the principal are hidden and the top bar's
/// first line starts with a navigation menu over the navigator's entries, so the page has nearly the whole window.
/// Neither copyable nor movable; owned by the owner thread of its backend.
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
          _sideEnd{backend.createSpacer(_sidebar.get())},
          _main{backend.createStack(_root.get(), Axis::Vertical)},
          _topBar{backend.createStack(_main.get(), Axis::Vertical)},
          _topLine{backend.createStack(_topBar.get(), Axis::Horizontal)},
          _topStart{backend.createSpacer(_topLine.get())},
          _navMenu{backend.createMenu(_topLine.get(), MenuStyle::Button)},
          _title{backend.createText(_topLine.get())},
          _principal{backend.createText(_topLine.get())},
          _signOut{backend.createButton(_topLine.get())},
          _topEnd{backend.createSpacer(_topLine.get())},
          _headLine{backend.createStack(_topBar.get(), Axis::Horizontal)},
          _headStart{backend.createSpacer(_headLine.get())},
          _headScroll{backend.createScroll(_headLine.get(), Axis::Horizontal)},
          _headSlot{backend.createStack(_headScroll.get(), Axis::Horizontal)},
          _headEnd{backend.createSpacer(_headLine.get())},
          _page{backend.createStack(_main.get(), Axis::Vertical)},
          _top{backend.createSpacer(_page.get())},
          _padded{backend.createStack(_page.get(), Axis::Horizontal)},
          _left{backend.createSpacer(_padded.get())},
          _content{backend.createStack(_padded.get(), Axis::Vertical)},
          _right{backend.createSpacer(_padded.get())} {
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

        _main->setLayout({.width = Sizing::stretch(), .height = Sizing::stretch()});
        // Two lines: the title has the first to itself beside two small widgets, so it does not wrap, and a head
        // row of any width gets the second, which scrolls sideways when even that is too narrow. Wrapping is the
        // one choice every backend can draw with the widgets it has; eliding would lose the badges and actions the
        // head exists for, and an overflow menu needs a node and a measuring pass the vocabulary does not have.
        _topBar->setSurface("toolbar");
        _topBar->setGap(1);
        _topBar->setTestId("shell_topbar");
        _topLine->setGap(3);
        _topLine->setTestId("shell_topbar_main");
        // Spacers at both ends pad each line by its gap.
        _topStart->setLayout({.width = Sizing::fixed(0), .height = {}});
        _topEnd->setLayout({.width = Sizing::fixed(0), .height = {}});
        // Centred across the line, so the principal's text sits level with the sign-out button's label.
        _topLine->setAlign(Align::Center);
        // The compact form's navigation: the navigator's entries as one button-style menu, kept current with it.
        _navMenu->setTestId("shell_nav_menu");
        _navMenu->setAccessibleName("Navigation");
        _navMenu->setTooltip("Navigation");
        _navMenu->setTrigger(MenuTrigger{.label = {}, .icon = "nav.menu", .emphasis = ButtonEmphasis::Ghost});
        _navMenu->setItems(_navigator->menuEntries());
        _navMenu->setOnActivate([this](std::vector<std::size_t> const& path) { _navigator->choose(path); });
        _navMenu->setVisible(false);
        _navigator->setOnChanged([this] { _navMenu->setItems(_navigator->menuEntries()); });
        _title->setRole(TextRole::Heading);
        _title->setLayout({.width = Sizing::stretch(), .height = {}});
        _title->setTestId("shell_title");
        _headLine->setGap(3);
        _headLine->setTestId("shell_head_line");
        _headLine->setVisible(false);
        _headStart->setLayout({.width = Sizing::fixed(0), .height = {}});
        _headEnd->setLayout({.width = Sizing::fixed(0), .height = {}});
        _headScroll->setLayout({.width = Sizing::stretch(), .height = {}});
        _headScroll->setTestId("shell_head_scroll");
        // It stretches, so a mounted row that stretches fills the line and its spacer pushes the actions to the
        // trailing edge; a row wider than the line makes the scroll's content wider, and the line scrolls.
        _headSlot->setLayout({.width = Sizing::stretch(), .height = {}});
        _headSlot->setTestId("shell_head");
        _principal->setRole(TextRole::Muted);
        _principal->setTestId("shell_principal");
        _signOut->setLabel("Sign out");
        _signOut->setTestId("shell_signOut");
        _signOut->setVisible(static_cast<bool>(options.onSignOut));
        _signOut->setOnClick(std::move(options.onSignOut));

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

    /// @brief The container a client mounts the active screen's head row into: the top bar's second line, below the
    ///        title, shown only while `setHeadShown(true)`.
    /// @return It.
    [[nodiscard]] ContainerWidget& headSlot() noexcept { return *_headSlot; }

    /// @brief Shows or hides the top bar's second line, which holds `headSlot()`. The frame cannot tell whether what
    ///        is mounted there shows anything, so the client says; a new frame starts with it hidden.
    /// @param shown Whether the line is shown.
    void setHeadShown(bool shown) { _headLine->setVisible(shown); }

    /// @brief Shows the shown screen's title in the top bar.
    /// @param title The title; empty shows none.
    void setTitle(std::string_view title) { _title->setText(title); }

    /// @brief Shows who is signed in, in the top bar.
    /// @param principal The principal's name; empty shows none.
    void setPrincipal(std::string_view principal) { _principal->setText(principal); }

    /// @brief Shows or hides the sidebar and the top bar: signed out, the page is all there is.
    /// @param signedIn Whether someone is signed in.
    void showSignedIn(bool signedIn) {
        _signedIn = signedIn;
        showChrome();
    }

    /// @brief Takes the frame's compact form, or its full one again. Compact, the sidebar and the principal are
    ///        hidden, and the top bar's first line starts with the navigation menu (`shell_nav_menu`, the framework
    ///        icon `nav.menu`, named "Navigation"), which lists the navigator's entries (`Navigator::menuEntries`) and
    ///        opens the one chosen, as the navigator does. A new frame is not compact.
    /// @param compact Whether the frame is compact.
    void setCompact(bool compact) {
        _compact = compact;
        showChrome();
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
    // The chrome that shows for the sign-in state and the form.
    void showChrome() {
        _sidebar->setVisible(_signedIn && !_compact);
        _topBar->setVisible(_signedIn);
        _principal->setVisible(!_compact);
        _navMenu->setVisible(_compact);
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
    std::unique_ptr<SpacerWidget> _sideEnd;
    std::unique_ptr<StackWidget> _main;
    // Lazily made, the first time showToast is called.
    std::unique_ptr<ToastWidget> _toast;
    std::unique_ptr<StackWidget> _topBar;
    std::unique_ptr<StackWidget> _topLine;
    std::unique_ptr<SpacerWidget> _topStart;
    std::unique_ptr<MenuWidget> _navMenu;
    std::unique_ptr<TextWidget> _title;
    std::unique_ptr<TextWidget> _principal;
    std::unique_ptr<ButtonWidget> _signOut;
    std::unique_ptr<SpacerWidget> _topEnd;
    std::unique_ptr<StackWidget> _headLine;
    std::unique_ptr<SpacerWidget> _headStart;
    std::unique_ptr<ScrollWidget> _headScroll;
    std::unique_ptr<StackWidget> _headSlot;
    std::unique_ptr<SpacerWidget> _headEnd;
    std::unique_ptr<StackWidget> _page;
    std::unique_ptr<SpacerWidget> _top;
    std::unique_ptr<StackWidget> _padded;
    std::unique_ptr<SpacerWidget> _left;
    std::unique_ptr<StackWidget> _content;
    std::unique_ptr<SpacerWidget> _right;
    bool _signedIn = true;
    bool _compact = false;
};

}  // namespace morph::ui
