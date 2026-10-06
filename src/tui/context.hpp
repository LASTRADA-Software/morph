// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <core/tui/Component.hpp>
#include <core/tui/Screen.hpp>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace morph::tui::detail {

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
    std::vector<::core::tui::Component*> openDialogs;                       ///< Open dialog frames, innermost last.
    std::size_t activeBusy = 0;                                             ///< Busy widgets that are spinning.
    std::size_t animationFrame = 0;                                         ///< The spinner frame to draw.

    /// The widget owning @p view, or null for a component no widget registered (an overlay, the root).
    [[nodiscard]] WidgetBase* ownerOf(::core::tui::Component const* view) const;
    /// Drops every reference to a widget being destroyed.
    void forget(WidgetBase& widget);
};

}  // namespace morph::tui::detail
