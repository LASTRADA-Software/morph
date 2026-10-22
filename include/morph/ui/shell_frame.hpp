// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

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
/// application's name and a `Navigator`, and the main column holding the top bar (surface `toolbar`: the shown
/// screen's title, a slot for its active tab's head row, the principal and a sign-out button) above the page
/// (surface `page`), where the client places the workspace. Signed out, the sidebar and the top bar are hidden and
/// the page is all there is. Neither copyable nor movable; owned by the owner thread of its backend.
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
          _topBar{backend.createStack(_main.get(), Axis::Horizontal)},
          _topStart{backend.createSpacer(_topBar.get())},
          _title{backend.createText(_topBar.get())},
          _headSlot{backend.createStack(_topBar.get(), Axis::Horizontal)},
          _principal{backend.createText(_topBar.get())},
          _signOut{backend.createButton(_topBar.get())},
          _topEnd{backend.createSpacer(_topBar.get())},
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
        _topBar->setSurface("toolbar");
        _topBar->setGap(3);
        // Spacers at both ends pad the bar by its gap.
        _topStart->setLayout({.width = Sizing::fixed(0), .height = {}});
        _topEnd->setLayout({.width = Sizing::fixed(0), .height = {}});
        _topBar->setTestId("shell_topbar");
        // Centred across the bar, so the principal's text sits level with the sign-out button's label.
        _topBar->setAlign(Align::Center);
        _title->setRole(TextRole::Heading);
        _title->setLayout({.width = Sizing::stretch(), .height = {}});
        _title->setTestId("shell_title");
        // Its own stretch, independent of whether the row mounted inside it is currently visible: the mounted
        // content's own visibility (hidden while a screen has no head) must not collapse the slot's width, since
        // MorphStack.qml reads the *slot's* widthKind to decide whether the bar as a whole stretches or packs.
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

    /// @brief The container in the top bar a client mounts the active screen's head row into, beside the title.
    /// @return It.
    [[nodiscard]] ContainerWidget& headSlot() noexcept { return *_headSlot; }

    /// @brief Shows the shown screen's title in the top bar.
    /// @param title The title; empty shows none.
    void setTitle(std::string_view title) { _title->setText(title); }

    /// @brief Shows who is signed in, in the top bar.
    /// @param principal The principal's name; empty shows none.
    void setPrincipal(std::string_view principal) { _principal->setText(principal); }

    /// @brief Shows or hides the sidebar and the top bar: signed out, the page is all there is.
    /// @param signedIn Whether someone is signed in.
    void showSignedIn(bool signedIn) {
        _sidebar->setVisible(signedIn);
        _topBar->setVisible(signedIn);
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
    std::unique_ptr<SpacerWidget> _topStart;
    std::unique_ptr<TextWidget> _title;
    std::unique_ptr<StackWidget> _headSlot;
    std::unique_ptr<TextWidget> _principal;
    std::unique_ptr<ButtonWidget> _signOut;
    std::unique_ptr<SpacerWidget> _topEnd;
    std::unique_ptr<StackWidget> _page;
    std::unique_ptr<SpacerWidget> _top;
    std::unique_ptr<StackWidget> _padded;
    std::unique_ptr<SpacerWidget> _left;
    std::unique_ptr<StackWidget> _content;
    std::unique_ptr<SpacerWidget> _right;
};

}  // namespace morph::ui
