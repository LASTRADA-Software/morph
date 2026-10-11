// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <core/tui/Canvas.hpp>
#include <core/tui/Component.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/Rect.hpp>
#include <cstddef>
#include <functional>
#include <memory>
#include <morph/ui/backend.hpp>
#include <morph/ui/view.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "tui/context.hpp"
#include "tui/layout.hpp"
#include "tui/widget.hpp"

namespace morph::tui::detail {

/// Column or Row (and ForEach, and a Table row): children along one axis, with a gap.
class StackImpl final : public TuiContainer<ui::StackWidget> {
public:
    StackImpl(Context& context, ui::Axis axis) : TuiContainer{context}, _axis{axis} { adopt(makeView(*this)); }
    void setGap(int gap) override;
    [[nodiscard]] ui::Axis axis() const noexcept { return _axis; }
    [[nodiscard]] int gap() const noexcept { return _gap; }
    /// Fixed main-axis extents and their gap, for a Table row; an empty list returns the row to the solver.
    void setColumnLayout(std::vector<int> extents, int gap);
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    [[nodiscard]] std::vector<::core::tui::Rect> childAreas(::core::tui::Size size) const override;
    void paint(::core::tui::Canvas& canvas) override;

private:
    ui::Axis _axis;
    int _gap = 0;
    std::vector<int> _extents;
    int _extentGap = 0;
};

/// The slot a Switch case or a Tabs page mounts into: its children stacked vertically, no gap.
class SlotImpl final : public TuiContainer<ui::SlotWidget> {
public:
    explicit SlotImpl(Context& context) : TuiContainer{context} { adopt(makeView(*this)); }
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    [[nodiscard]] std::vector<::core::tui::Rect> childAreas(::core::tui::Size size) const override;
    void paint(::core::tui::Canvas& canvas) override;
};

/// Grid: equal columns, cells row-major, a cell spanning several columns. A hidden child leaves no cell: the cells
/// after it move up one place, as a null cell of the view tree leaves none.
class GridImpl final : public TuiContainer<ui::GridWidget> {
public:
    explicit GridImpl(Context& context) : TuiContainer{context} { adopt(makeView(*this)); }
    void setColumns(int columns) override;
    void setGap(int gap) override;
    /// Throws std::logic_error when @p child is not one of this grid's children.
    void setSpan(ui::Widget& child, int span) override;
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    [[nodiscard]] std::vector<::core::tui::Rect> childAreas(::core::tui::Size size) const override;
    void paint(::core::tui::Canvas& canvas) override;

protected:
    void childForgotten(WidgetBase& child) override;

private:
    [[nodiscard]] int spanOf(WidgetBase const* child) const;
    [[nodiscard]] std::vector<layout::GridCell> cellsOf(std::span<WidgetBase* const> shown) const;

    int _columns = 1;
    int _gap = 0;
    std::unordered_map<WidgetBase const*, int> _spans;
};

/// Panel: a box with a title around its children, inset by padding; a collapsible panel folds to its title line.
///
/// Collapsing hides the children, so they neither draw nor take input; the panel itself, focusable while
/// collapsible, toggles on Enter or Space while it holds the focus, and on a click on its title line, which also
/// focuses it. A press elsewhere on the panel that no child takes leaves the focus where it was. Collapsing a panel
/// that holds the focus inside moves the focus to the panel, or clears it when the panel cannot take it.
class PanelImpl final : public TuiContainer<ui::PanelWidget> {
public:
    explicit PanelImpl(Context& context) : TuiContainer{context} { adopt(makeView(*this)); }
    void setTitle(std::string_view title) override;
    void setPadding(int padding) override;
    void setCollapsible(bool collapsible) override;
    void setCollapsed(bool collapsed) override;
    void setOnToggle(std::function<void(bool)> onToggle) override;
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    [[nodiscard]] bool wantsFocus() const override { return _collapsible; }
    [[nodiscard]] bool focusesOnPress() const override { return false; }
    [[nodiscard]] std::string probeText() const override { return _title; }
    [[nodiscard]] bool letsChildrenAct() const override { return !_collapsed; }
    [[nodiscard]] std::vector<::core::tui::Rect> childAreas(::core::tui::Size size) const override;
    void paint(::core::tui::Canvas& canvas) override;
    [[nodiscard]] ::core::tui::EventResult key(::core::tui::KeyEvent const& key) override;
    void activate() override;
    void click(::core::tui::Point cell) override;

protected:
    void childAttached(WidgetBase& child) override;

private:
    [[nodiscard]] std::string heading() const;
    void syncChildren();

    std::string _title;
    int _padding = 0;
    bool _collapsible = false;
    bool _collapsed = false;
    std::function<void(bool)> _onToggle;
};

/// Scroll: a viewport onto its content, which is laid out at its natural extent along the scroll's axis (at least
/// the viewport's) and at the viewport's across it, and drawn moved back by the scroll position.
///
/// A widget scrolled wholly out of view is given no area, so it neither draws nor takes a click; one scrolled partly
/// out of view draws the part that shows (see `WidgetBase::render`). Whenever the focus moves to a widget inside, or
/// that widget's place in the content or the view's size changes (new content, a resized terminal), the scroll
/// brings it into view, its top edge first when it is taller than the view. The wheel and the keys may move the view
/// away from it until then. The wheel moves the view one
/// cell, and is left to whatever lies around the scroll when the view cannot move that way.
///
/// While nothing inside can take the focus (read-only content), the scroll takes it itself, so the keyboard reaches
/// it. Up and Down (Left and Right across a horizontal scroll) move the view one cell, PageUp and PageDown one view
/// less a cell, Home and End to either end; they work too when they bubble up from a focused widget inside that
/// leaves them, and like the wheel they are left to the scroll's surroundings when the view cannot move.
class ScrollImpl final : public TuiContainer<ui::ScrollWidget> {
public:
    ScrollImpl(Context& context, ui::Axis axis) : TuiContainer{context}, _axis{axis} { adopt(makeView(*this)); }
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    [[nodiscard]] std::vector<::core::tui::Rect> childAreas(::core::tui::Size size) const override;
    [[nodiscard]] bool wantsFocus() const override;
    void paint(::core::tui::Canvas& canvas) override;
    [[nodiscard]] ::core::tui::EventResult key(::core::tui::KeyEvent const& key) override;
    [[nodiscard]] bool wheel(int delta) override;

private:
    /// Where the children go at scroll position 0, for a viewport of @p viewport.
    [[nodiscard]] std::vector<::core::tui::Rect> contentAreas(::core::tui::Size viewport) const;
    /// Where the widget owning the focused view lies in the content at scroll position 0, as far as the containers
    /// on the way to it say; nothing when the focus is not inside.
    [[nodiscard]] std::optional<::core::tui::Rect> focusedArea(::core::tui::Size viewport) const;
    /// Moves the view to the focused widget inside when it, its place in the content or the view's size changed
    /// since the scroll last followed it.
    void followFocus(::core::tui::Size viewport);
    /// Moves the view by @p cells along the axis, within the content; whether it moved.
    bool scrollBy(int cells);

    ui::Axis _axis;
    int _offset = 0;                    ///< The scroll position: content cells before the view.
    int _extent = 0;                    ///< The content's extent along the axis, as of the last frame.
    int _viewport = 0;                  ///< The view's extent along the axis, as of the last frame.
    ::core::tui::Size _viewportSize{};  ///< The view's size, as of the last frame.

    /// What the scroll last followed.
    struct Followed {
        /// The focused view, compared by address only: it may be gone by the next frame.
        ::core::tui::Component const* view = nullptr;
        ::core::tui::Rect area{};      ///< Where its widget lay in the content.
        ::core::tui::Size viewport{};  ///< The view's size then.

        bool operator==(Followed const&) const = default;
    };
    Followed _followed;
};

/// The overlay of a dialog or a drawer: a box with a border and a title over everything else, holding the children's
/// views; the widget's place in the tree is an empty anchor. A dialog's box is centred, a drawer's stands against
/// its side the screen's whole height.
///
/// The box (`frame()`, the container's `host()`) shows while the widget is open and the user could see it: the
/// widget and every container around it shown, and no dialog around it closed. One opened inside a closed one
/// therefore shows once that one opens, and then on top of it. The box on top is modal (`activeDialog`): Tab cycles
/// inside it, and neither the pointer nor a key reaches anything behind it.
///
/// Opening moves the focus inside, to the first focusable widget of the box on top, or to the box itself when it
/// holds none. Closing hands the focus back to the widget that had it when the box opened, when the focus was inside
/// or nowhere by then (the mount destroys the content before closing it, and the focused content clears the focus as
/// it goes), and that widget is still there and can take it; else to the first focusable widget of the box now on
/// top, or of the roots, and nowhere only when nothing can take it. A box that vanishes while the widget stays open
/// (it or a container around it hidden) hands the focus back the same way, and takes it again when it shows. Esc
/// inside the open, actionable box closes it and then calls `onDismiss`; no setter calls it.
class OverlayCore {
public:
    /// @param self The dialog or drawer this is the overlay of.
    /// @param side The drawer's side; none for a dialog.
    OverlayCore(ContainerBase& self, std::optional<ui::Side> side);
    ~OverlayCore();
    OverlayCore(OverlayCore const&) = delete;
    OverlayCore& operator=(OverlayCore const&) = delete;
    OverlayCore(OverlayCore&&) = delete;
    OverlayCore& operator=(OverlayCore&&) = delete;

    void setOpen(bool open);
    void setTitle(std::string_view title);
    void setOnDismiss(ui::Action onDismiss) { _onDismiss = std::move(onDismiss); }
    /// Closes the overlay and calls `onDismiss`, as Esc does.
    void dismiss();
    [[nodiscard]] std::string const& title() const noexcept { return _title; }
    [[nodiscard]] ::core::tui::Component& frame() const noexcept { return *_frame; }
    [[nodiscard]] bool isOpen() const noexcept { return _open; }
    [[nodiscard]] std::vector<::core::tui::Rect> childAreas(::core::tui::Size size) const;
    /// Where the box shows, in screen cells.
    [[nodiscard]] ::core::tui::Point framePosition() const;
    static void followFrames(Context& context);

private:
    class Frame;
    [[nodiscard]] bool showsFrame() const;
    [[nodiscard]] ::core::tui::Size frameSize() const;
    void paintFrame(::core::tui::Canvas& canvas);
    [[nodiscard]] ::core::tui::EventResult frameEvent(::core::tui::InputEvent const& event);
    [[nodiscard]] bool holdsFocus() const;
    void raiseNested();
    void returnFocusTo(WidgetBase* widget);
    void forgetRestore() noexcept;
    void markFramed();
    void regainFocus();
    void releaseFocus();
    void openFrame();
    void closeFrame();

    ContainerBase* _self;
    std::optional<ui::Side> _side;
    std::unique_ptr<::core::tui::Component> _frame;  ///< A Frame.
    std::string _title;
    ui::Action _onDismiss;
    bool _open = false;
    bool _framed = false;  ///< Whether the box showed when last seen.
    WidgetBase* _restore = nullptr;
};

/// Dialog: an `OverlayCore` centred on the screen. Closed until opened.
class DialogImpl final : public TuiContainer<ui::DialogWidget> {
public:
    explicit DialogImpl(Context& context) : TuiContainer{context}, _overlay{*this, std::nullopt} {
        adopt(makeView(*this));
    }
    ~DialogImpl() override { _overlay.setOpen(false); }
    DialogImpl(DialogImpl const&) = delete;
    DialogImpl& operator=(DialogImpl const&) = delete;
    DialogImpl(DialogImpl&&) = delete;
    DialogImpl& operator=(DialogImpl&&) = delete;

    void setOpen(bool open) override { _overlay.setOpen(open); }
    void setTitle(std::string_view title) override { _overlay.setTitle(title); }
    void setOnDismiss(ui::Action onDismiss) override { _overlay.setOnDismiss(std::move(onDismiss)); }
    [[nodiscard]] ::core::tui::Size naturalSize() const override { return {.width = 0, .height = 0}; }
    [[nodiscard]] std::string probeText() const override { return _overlay.title(); }
    [[nodiscard]] ::core::tui::Component& host() override { return _overlay.frame(); }
    [[nodiscard]] bool letsChildrenAct() const override { return _overlay.isOpen(); }
    [[nodiscard]] std::vector<::core::tui::Rect> childAreas(::core::tui::Size size) const override {
        return _overlay.childAreas(size);
    }
    [[nodiscard]] OverlayCore* overlay() override { return &_overlay; }
    [[nodiscard]] bool isOpen() const noexcept { return _overlay.isOpen(); }

private:
    OverlayCore _overlay;
};

/// Drawer: an `OverlayCore` against the side fixed at creation. Closed until opened.
class DrawerImpl final : public TuiContainer<ui::DrawerWidget> {
public:
    DrawerImpl(Context& context, ui::Side side) : TuiContainer{context}, _overlay{*this, side} {
        adopt(makeView(*this));
    }
    ~DrawerImpl() override { _overlay.setOpen(false); }
    DrawerImpl(DrawerImpl const&) = delete;
    DrawerImpl& operator=(DrawerImpl const&) = delete;
    DrawerImpl(DrawerImpl&&) = delete;
    DrawerImpl& operator=(DrawerImpl&&) = delete;

    void setOpen(bool open) override { _overlay.setOpen(open); }
    void setTitle(std::string_view title) override { _overlay.setTitle(title); }
    void setOnDismiss(ui::Action onDismiss) override { _overlay.setOnDismiss(std::move(onDismiss)); }
    [[nodiscard]] ::core::tui::Size naturalSize() const override { return {.width = 0, .height = 0}; }
    [[nodiscard]] std::string probeText() const override { return _overlay.title(); }
    [[nodiscard]] ::core::tui::Component& host() override { return _overlay.frame(); }
    [[nodiscard]] bool letsChildrenAct() const override { return _overlay.isOpen(); }
    [[nodiscard]] std::vector<::core::tui::Rect> childAreas(::core::tui::Size size) const override {
        return _overlay.childAreas(size);
    }
    [[nodiscard]] OverlayCore* overlay() override { return &_overlay; }
    [[nodiscard]] bool isOpen() const noexcept { return _overlay.isOpen(); }

private:
    OverlayCore _overlay;
};

}  // namespace morph::tui::detail
