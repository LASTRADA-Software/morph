// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <chrono>
#include <core/tui/Component.hpp>
#include <core/tui/Screen.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace morph::tui::detail {

class ContainerBase;
class DragController;
class WidgetBase;

/// Which way keyboard focus moves.
enum class Direction : std::uint8_t { Forward, Backward };

/// What every widget of one backend shares: the screen, which widget owns which view, the roots, the open dialogs
/// and the spinner state.
struct Context {
    explicit Context(::core::tui::Screen& target);
    ~Context();
    Context(Context const&) = delete;
    Context& operator=(Context const&) = delete;
    Context(Context&&) = delete;
    Context& operator=(Context&&) = delete;

    ::core::tui::Screen* screen;                                            ///< Where every view renders.
    std::unordered_map<::core::tui::Component const*, WidgetBase*> owners;  ///< Each registered view's widget.
    std::vector<WidgetBase*> roots;                                         ///< Widgets created with no parent.
    /// Open dialogs, in the order their frames (each one's `ContainerBase::host()`) are shown: the last one shown is
    /// drawn on top.
    std::vector<ContainerBase*> openDialogs;
    std::vector<WidgetBase*> busy;   ///< Busy widgets that are active.
    std::size_t activeBusy = 0;      ///< How many of `busy` the user could see: while any, the spinner animates.
    std::size_t animationFrame = 0;  ///< The spinner frame to draw.
    /// The widget whose press is waiting for its release, or null. Its view holds the screen's pointer capture while
    /// it is set: every press a widget sees, and the first frame or motion after the capture ended otherwise, end it
    /// (see `endPress`), so a press whose release never arrived clicks nothing later.
    WidgetBase* pressed = nullptr;
    /// Widgets that code is waiting on — code running a handler (see `focusWidget`), an open dialog that will hand
    /// the focus back — each through a pointer `forget` sets to null when that widget is destroyed.
    std::vector<WidgetBase**> watches;
    /// While a dialog that is opening moves the focus inside, the widget the focus is leaving, or null: a dialog that
    /// the handler run by that leaving opens hands the focus back there too. Points into the opening call, where it
    /// is watched.
    WidgetBase** focusLeaving = nullptr;
    /// Widgets that have a popup open (a dropdown's list), which close it once the user cannot reach them (see
    /// `closeUnreachablePopups`).
    std::vector<WidgetBase*> popups;
    /// The clock every widget reads the time from (a table telling a double click from two clicks); a test may
    /// replace it.
    std::function<std::chrono::steady_clock::time_point()> now = [] { return std::chrono::steady_clock::now(); };
    /// The one drag gesture; last, so it is destroyed first and hides its overlays while the screen is still there.
    std::unique_ptr<DragController> drag;

    /// The widget owning @p view, or null for a component no widget registered (an overlay, the root).
    [[nodiscard]] WidgetBase* ownerOf(::core::tui::Component const* view) const;
    /// Drops every reference to a widget being destroyed.
    void forget(WidgetBase& widget);
    /// Ends the press waiting for its release, if any, and the drag it started: nothing of that gesture happens
    /// afterwards. A new press calls it, as does a press whose pointer capture ended without its release.
    void endPress();
};

}  // namespace morph::tui::detail
