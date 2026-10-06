// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <core/tui/InputEvent.hpp>
#include <core/tui/Rect.hpp>
#include <memory>
#include <morph/ui/view.hpp>
#include <optional>
#include <string>

#include "tui/context.hpp"

namespace morph::tui::detail {

class WidgetBase;

/// The one drag gesture a backend can have at a time.
///
/// A press on a widget with a drag key arms it; the first motion a cell away starts the drag and shows a label beside
/// the pointer. The drop target is the first widget, from the one under the pointer up through its parents, that the
/// user can reach and that accepts the key; it is outlined. The release asks again and drops the key there. Pointer
/// capture (core::tui) sends the whole gesture to the source, so it holds while the source moves in the tree, and the
/// gesture ends with the capture (see `Context::endPress`).
///
/// A source the user can no longer reach ends the gesture with no drop, and a target the user cannot reach is never
/// one. Every `accepts` and the drop handler run through copies, and nothing they may destroy is touched afterwards.
class DragController {
public:
    explicit DragController(Context& context);
    /// @brief Hides the label and the outline, which the screen would otherwise keep showing.
    ~DragController();
    DragController(DragController const&) = delete;
    DragController& operator=(DragController const&) = delete;
    DragController(DragController&&) = delete;
    DragController& operator=(DragController&&) = delete;

    /// A press on @p source, carrying @p key, at viewport cell @p point; ends any earlier gesture.
    void press(WidgetBase& source, ui::Key key, ::core::tui::Point point);
    /// The pointer moved to viewport cell @p point with the button held. It may run `accepts` predicates.
    void move(::core::tui::Point point);
    /// The button was released at viewport cell @p point; true when the gesture was a drag, so the release is not a
    /// click. The drop handler runs last and may destroy anything, the source included.
    [[nodiscard]] bool release(::core::tui::Point point);
    /// Ends the gesture, if any, with no drop.
    void cancel();
    /// The drag key of @p widget changed to @p key: a source that lost its key ends the gesture with its press, and
    /// one that carries another one drops that.
    void keyChanged(WidgetBase const& widget, std::optional<ui::Key> const& key);
    /// A widget is being destroyed: a source ends the gesture; a target stops being one.
    void forget(WidgetBase const& widget);

    [[nodiscard]] WidgetBase const* source() const noexcept { return _source; }
    [[nodiscard]] WidgetBase const* target() const noexcept { return _target; }
    [[nodiscard]] bool dragging() const noexcept { return _dragging; }

private:
    class Label;
    class Outline;

    /// Whether @p widget can take part in a drag now: actionable and not behind an open dialog.
    [[nodiscard]] static bool reachable(WidgetBase const& widget);
    /// The drop target under @p point, or null. Runs `accepts` predicates; one may end the gesture, and then this
    /// answers null.
    [[nodiscard]] WidgetBase* findTarget(::core::tui::Point point);
    void setTarget(WidgetBase* target);
    void start();
    /// What the label shows: the first line of the first text in the source or a shown widget inside it, in
    /// `children()` order, or else its key.
    [[nodiscard]] std::string labelText() const;

    Context* _context;
    WidgetBase* _source = nullptr;
    std::optional<ui::Key> _key;
    ::core::tui::Point _pressedAt{};
    bool _dragging = false;
    WidgetBase* _target = nullptr;
    std::unique_ptr<Label> _label;
    std::unique_ptr<Outline> _outline;
};

/// Ends the drag in progress, as a native drag ends, when @p event is Esc: no drop, and the press, its pointer
/// capture, the label and the outline are gone. Every component of the backend that takes keys asks this first, so
/// Esc ends a drag wherever the focus is, as long as some view of this backend has it; true when it did.
[[nodiscard]] bool escapeEndsDrag(Context& context, ::core::tui::InputEvent const& event);

}  // namespace morph::tui::detail
