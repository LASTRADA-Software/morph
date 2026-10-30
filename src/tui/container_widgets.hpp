// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
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
#include <utility>
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
    void setAlign(ui::Align align) override;
    void setJustify(ui::Justify justify) override;
    void setWrap(bool wrap) override;
    void setPadding(ui::Padding padding) override;
    /// From its first call the stack reserves its first column for the selection's bar, inside its leading padding
    /// when that has a column, else as one more column; the bar is `▌` in the list's selected-item style while
    /// selected. The column stays reserved while unselected, so the content does not move with the selection.
    void setSelected(bool selected) override;
    /// Draws no rule: a rule would take a row of its own and double a list's height, and its lines are already apart.
    void setDivided(bool divided) override { static_cast<void>(divided); }
    /// The stack pads its children by its container's `bleedColumns` besides its own padding, and a child that bleeds
    /// takes the stack's full width: the container lays the stack out over its whole span, past its padding.
    void setPassesBleed(bool passes) override;
    [[nodiscard]] bool passesBleed() const noexcept override { return _passesBleed; }
    [[nodiscard]] ui::Axis axis() const noexcept { return _axis; }
    /// The gap in cells along the axis: the spacing step `setGap` was given, mapped by `layout::stepCells`.
    [[nodiscard]] int gap() const noexcept { return _gap; }
    /// Fixed main-axis extents, their gap and each cell's place, for a Table row, whose cells are then placed by
    /// their column rather than by the row's own alignment; an empty list returns the row to the solver and its own
    /// alignment.
    void setColumnLayout(std::vector<int> extents, int gap, std::vector<CellPlace> places);
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    [[nodiscard]] std::vector<::core::tui::Rect> childAreas(::core::tui::Size size) const override;
    void paint(::core::tui::Canvas& canvas) override;

private:
    /// The shown children's places along their lines when the row is @p width cells wide, and each line's height.
    struct WrappedLines {
        std::vector<WidgetBase const*> shown;
        layout::WrapPlan plan;
        std::vector<int> lineHeights;
    };
    [[nodiscard]] WrappedLines wrappedLines(int width) const;
    /// The natural size and the children's places inside the padding.
    [[nodiscard]] ::core::tui::Size innerNaturalSize() const;
    [[nodiscard]] std::vector<::core::tui::Rect> innerAreas(::core::tui::Size size) const;
    /// The columns between the stack's leading edge and its children: the padding, or one for the selection's bar
    /// when the padding has none.
    [[nodiscard]] int leadingColumns() const noexcept {
        return _selectable ? std::max(_paddingColumns, 1) : _paddingColumns;
    }

    ui::Axis _axis;
    int _paddingRows = 0;
    int _paddingColumns = 0;
    /// Whether the stack was told `setSelected`, and what it was told last.
    bool _selectable = false;
    bool _selected = false;
    bool _passesBleed = false;
    int _gap = 0;
    int _gapStep = 0;
    bool _wrap = false;
    /// The width the row was last drawn at; a wrapping row's height is its lines' at this width.
    int _drawnWidth = 0;
    std::vector<int> _extents;
    std::vector<CellPlace> _places;
    int _extentGap = 0;
    ui::Align _align = ui::Align::Stretch;
    ui::Justify _justify = ui::Justify::Start;
};

/// Grid: equal columns, or columns sized by their tracks; cells row-major, a cell spanning several columns. A hidden
/// child leaves no cell: the cells after it move up one place, as a null cell of the view tree leaves none. Across a
/// track, a cell placed at its natural width (`placedAcrossColumn`) sits at the track's start, centre or end.
class GridImpl final : public TuiContainer<ui::GridWidget> {
public:
    explicit GridImpl(Context& context) : TuiContainer{context} { adopt(makeView(*this)); }
    void setColumns(int columns) override;
    void setTracks(std::vector<ui::GridTrack> const& tracks) override;
    void setGap(int gap) override;
    /// Throws std::logic_error when @p child is not one of this grid's children.
    void setSpan(ui::Widget& child, int span) override;
    void setAlign(ui::Align align) override;
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    [[nodiscard]] std::vector<::core::tui::Rect> childAreas(::core::tui::Size size) const override;
    void paint(::core::tui::Canvas& canvas) override;

protected:
    void childForgotten(WidgetBase& child) override;

private:
    [[nodiscard]] int spanOf(WidgetBase const* child) const;
    [[nodiscard]] std::vector<layout::GridCell> cellsOf(std::span<WidgetBase* const> shown) const;
    /// The plan of the shown cells in @p width: equal columns, or the tracks' widths.
    [[nodiscard]] layout::GridPlan planOf(std::span<WidgetBase* const> shown, int width) const;
    /// One stack item per track, a content track's natural width its widest one-track cell's.
    [[nodiscard]] std::vector<layout::Item> trackItems(std::span<WidgetBase* const> shown) const;

    int _columns = 1;
    std::vector<ui::GridTrack> _tracks;
    int _columnGap = 0;
    int _rowGap = 0;
    ui::Align _align = ui::Align::Stretch;
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
    void setHeader(ui::Widget& header) override;
    void setPadding(int padding) override;
    void setCollapsible(bool collapsible) override;
    void setCollapsed(bool collapsed) override;
    void setOnToggle(std::function<void(bool)> onToggle) override;
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    [[nodiscard]] bool wantsFocus() const override { return _collapsible; }
    [[nodiscard]] bool focusesOnPress() const override { return false; }
    [[nodiscard]] std::string probeText() const override { return _title; }
    [[nodiscard]] bool letsChildrenAct() const override { return !_collapsed; }
    [[nodiscard]] bool letsChildAct(WidgetBase const& child) const override {
        return !_collapsed || &child == _header;
    }
    [[nodiscard]] std::vector<::core::tui::Rect> childAreas(::core::tui::Size size) const override;
    [[nodiscard]] int bleedColumns() const override { return _paddingColumns; }
    void paint(::core::tui::Canvas& canvas) override;
    [[nodiscard]] ::core::tui::EventResult key(::core::tui::KeyEvent const& key) override;
    void activate() override;
    void click(::core::tui::Point cell) override;

protected:
    void childAttached(WidgetBase& child) override;
    void childForgotten(WidgetBase& child) override;

private:
    [[nodiscard]] std::string heading() const;
    // The children other than the header, in stacking order.
    [[nodiscard]] std::vector<WidgetBase*> body() const;
    void syncChildren();

    std::string _title;
    // Placed in-flow above the body, since the terminal has no floating/area concept to draw it beside the title.
    WidgetBase* _header = nullptr;
    int _paddingRows = 0;
    int _paddingColumns = 0;
    bool _collapsible = false;
    bool _collapsed = false;
    std::function<void(bool)> _onToggle;
};

/// What a Scroll is, and a scrollable Slot while it scrolls: a viewport onto its content, which is laid
/// out at its natural extent along the scroll's axis (at least the viewport's) and at the viewport's across it, and
/// drawn moved back by the scroll position.
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
class ScrollViewport {
public:
    explicit ScrollViewport(ui::Axis axis) noexcept : _axis{axis} {}
    /// The axis it scrolls along.
    [[nodiscard]] ui::Axis axis() const noexcept { return _axis; }
    /// Where @p owner's children go in a view of @p size, moved back by the scroll position.
    [[nodiscard]] std::vector<::core::tui::Rect> childAreas(ContainerBase const& owner, ::core::tui::Size size) const;
    /// Whether @p owner takes the focus itself: while nothing inside it can.
    [[nodiscard]] static bool wantsFocus(ContainerBase const& owner);
    /// Lays @p owner's children out for the frame @p canvas is, following the focus.
    void paint(ContainerBase const& owner, ::core::tui::Canvas& canvas);
    /// Moves the view on a scrolling key; whether it moved.
    [[nodiscard]] ::core::tui::EventResult key(ContainerBase const& owner, ::core::tui::KeyEvent const& key);
    /// Moves the view a cell per notch; whether it moved.
    [[nodiscard]] bool wheel(ContainerBase const& owner, int delta);

private:
    /// Where the children go at scroll position 0, for a viewport of @p viewport.
    [[nodiscard]] std::vector<::core::tui::Rect> contentAreas(ContainerBase const& owner,
                                                              ::core::tui::Size viewport) const;
    /// Where the widget owning the focused view lies in the content at scroll position 0, as far as the containers
    /// on the way to it say; nothing when the focus is not inside.
    [[nodiscard]] std::optional<::core::tui::Rect> focusedArea(ContainerBase const& owner,
                                                               ::core::tui::Size viewport) const;
    /// Moves the view to the focused widget inside when it, its place in the content or the view's size changed
    /// since the scroll last followed it.
    void followFocus(ContainerBase const& owner, ::core::tui::Size viewport);
    /// Moves the view by @p cells along the axis, within the content; whether it moved.
    bool scrollBy(ContainerBase const& owner, int cells);

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

class ScrollImpl final : public TuiContainer<ui::ScrollWidget> {
public:
    ScrollImpl(Context& context, ui::Axis axis) : TuiContainer{context}, _scroller{axis} { adopt(makeView(*this)); }
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    [[nodiscard]] std::vector<::core::tui::Rect> childAreas(::core::tui::Size size) const override;
    [[nodiscard]] bool wantsFocus() const override;
    void paint(::core::tui::Canvas& canvas) override;
    [[nodiscard]] ::core::tui::EventResult key(::core::tui::KeyEvent const& key) override;
    [[nodiscard]] bool wheel(int delta) override;

private:
    ScrollViewport _scroller;
};

/// The slot a Switch case or a Tabs page mounts into: its children stacked vertically, no gap. A scrollable one whose
/// shown children all keep their content height is a vertical `ScrollViewport` over them: it follows the focus, takes
/// the arrows, PageUp and PageDown, Home and End and the wheel, and takes the focus itself while nothing inside can. A
/// child that stretches down fills the slot, which then does not scroll, unless the child's `minHeight` is more than
/// the slot's height: the slot then scrolls the child at that height.
class SlotImpl final : public TuiContainer<ui::SlotWidget> {
public:
    explicit SlotImpl(Context& context) : TuiContainer{context} { adopt(makeView(*this)); }
    void setScrollable(bool scrollable) override;
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    [[nodiscard]] std::vector<::core::tui::Rect> childAreas(::core::tui::Size size) const override;
    [[nodiscard]] bool wantsFocus() const override;
    void paint(::core::tui::Canvas& canvas) override;
    [[nodiscard]] ::core::tui::EventResult key(::core::tui::KeyEvent const& key) override;
    [[nodiscard]] bool wheel(int delta) override;

private:
    // Whether the slot scrolls when it is @p height tall: scrollable, and no shown child that stretches down fits.
    [[nodiscard]] bool scrolls(int height) const;
    // The slot's height as of the last frame.
    [[nodiscard]] int height() const;

    bool _scrollable = false;
    ScrollViewport _scroller{ui::Axis::Vertical};
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
///
/// A dialog drawn as a sheet fills the viewport; its top row draws `[dismissLabel]` before the title, and a press on
/// it dismisses as Esc does. Its footer, when it holds anything, takes the box's bottom rows.
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
    /// The line under the title: drawn muted on the first row inside the frame, then a rule row, both above the
    /// content, which loses those two rows.
    void setDescription(std::string_view description);
    /// The description as runs, drawn on the same row; the accessible description is their joined characters.
    void setDescriptionSpans(std::vector<ui::TextSpan> const& spans);
    void setOnDismiss(ui::Action onDismiss) { _onDismiss = std::move(onDismiss); }
    void setSheet(bool sheet);
    void setDismissLabel(std::string_view label);
    /// The footer, which a sheet lays out at the bottom of its box.
    void setFooter(WidgetBase const* footer) noexcept { _footer = footer; }
    /// Closes the overlay and calls `onDismiss`, as Esc does.
    void dismiss();
    [[nodiscard]] std::string const& title() const noexcept { return _title; }
    [[nodiscard]] std::string const& description() const noexcept { return _description; }
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
    /// The cells of a sheet's dismiss control in the box's top row: its first column and its width.
    [[nodiscard]] std::pair<int, int> dismissCells() const;
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
    std::string _description;
    std::vector<ui::TextSpan> _descriptionRuns;  ///< The description's runs; empty for a plain description.
    ui::Action _onDismiss;
    std::string _dismissLabel = "Close";
    WidgetBase const* _footer = nullptr;
    bool _sheet = false;
    bool _open = false;
    bool _framed = false;  ///< Whether the box showed when last seen.
    WidgetBase* _restore = nullptr;
};

/// Dialog: an `OverlayCore` centred on the screen. Closed until opened.
/// An overlay's footer: its children in one row against the right edge, one cell apart. Empty, it takes no room. It
/// never scrolls.
class FooterImpl final : public TuiContainer<ui::SlotWidget> {
public:
    explicit FooterImpl(Context& context) : TuiContainer{context} { adopt(makeView(*this)); }
    void setScrollable(bool scrollable) override { static_cast<void>(scrollable); }
    [[nodiscard]] ::core::tui::Size naturalSize() const override;
    [[nodiscard]] std::vector<::core::tui::Rect> childAreas(::core::tui::Size size) const override;
    void paint(::core::tui::Canvas& canvas) override;
};

/// The footer of a dialog or drawer, made the first time it is asked for, and kept after the overlay's content:
/// content mounted again when the overlay reopens comes after it among the children, so each ask moves it last.
class OverlayFooter {
public:
    [[nodiscard]] ui::ContainerWidget& of(ContainerBase& overlay);

private:
    std::unique_ptr<FooterImpl> _footer;
};

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
    void setDescription(std::string_view description) override { _overlay.setDescription(description); }
    void setDescriptionSpans(std::vector<ui::TextSpan> const& spans) override { _overlay.setDescriptionSpans(spans); }
    [[nodiscard]] std::string accessibleDescription() const override { return _overlay.description(); }
    void setOnDismiss(ui::Action onDismiss) override { _overlay.setOnDismiss(std::move(onDismiss)); }
    void setSheet(bool sheet) override { _overlay.setSheet(sheet); }
    void setDismissLabel(std::string_view label) override { _overlay.setDismissLabel(label); }
    [[nodiscard]] ui::ContainerWidget& footer() override {
        ui::ContainerWidget& footer = _footer.of(*this);
        _overlay.setFooter(dynamic_cast<WidgetBase const*>(&footer));
        return footer;
    }
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
    OverlayFooter _footer;
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
    void setDescription(std::string_view description) override { _overlay.setDescription(description); }
    void setDescriptionSpans(std::vector<ui::TextSpan> const& spans) override { _overlay.setDescriptionSpans(spans); }
    [[nodiscard]] std::string accessibleDescription() const override { return _overlay.description(); }
    void setOnDismiss(ui::Action onDismiss) override { _overlay.setOnDismiss(std::move(onDismiss)); }
    [[nodiscard]] ui::ContainerWidget& footer() override { return _footer.of(*this); }
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
    OverlayFooter _footer;
};

}  // namespace morph::tui::detail
