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
#include <utility>
#include <vector>

#include "tui/context.hpp"

namespace morph::tui::detail {

class ContainerBase;

/// The morph side of one TUI widget: the Common props, the container it sits in, and the core::tui view it owns.
///
/// The view is registered in the Context while the widget lives. The destructor clears the screen's focus from it
/// and unregisters it before the view itself is destroyed, so core::tui never keeps a pointer to a freed view.
///
/// Every handler is called through a local copy, and nothing of the widget is touched after the call returns: a
/// handler may destroy its own widget.
class WidgetBase {
public:
    explicit WidgetBase(Context& context) noexcept;
    /// @brief Clears the screen's focus from the view, unregisters it, and leaves the roots and its container.
    virtual ~WidgetBase();
    WidgetBase(WidgetBase const&) = delete;
    WidgetBase& operator=(WidgetBase const&) = delete;
    WidgetBase(WidgetBase&&) = delete;
    WidgetBase& operator=(WidgetBase&&) = delete;

    /// The TUI widget behind @p widget; throws std::logic_error for a widget another backend made.
    [[nodiscard]] static WidgetBase& of(ui::Widget& widget);
    /// The TUI widget behind @p widget; throws std::logic_error for a widget another backend made.
    [[nodiscard]] static WidgetBase const& of(ui::Widget const& widget);

    [[nodiscard]] ::core::tui::Component& view() const noexcept { return *_view; }
    [[nodiscard]] Context& context() const noexcept { return *_context; }
    [[nodiscard]] ContainerBase* container() const noexcept { return _container; }
    void setContainer(ContainerBase* container) noexcept { _container = container; }
    /// This widget as the ui interface it implements.
    [[nodiscard]] virtual ui::Widget const& asWidget() const noexcept = 0;

    void applyVisible(bool visible);
    /// Shown or hidden by its container (a collapsed panel), independently of `visible`.
    void setStructuralVisible(bool visible);
    [[nodiscard]] bool userVisible() const noexcept { return _userVisible; }
    [[nodiscard]] bool shown() const noexcept { return _userVisible && _structuralVisible; }
    void applyEnabled(bool enabled);
    [[nodiscard]] bool enabled() const noexcept { return _enabled; }
    /// Whether the user can reach this widget: it and every container around it are shown and enabled, and each of
    /// those containers lets its children act (a collapsed panel and a closed dialog do not). Every input path asks
    /// this, because core::tui sends keys to the focused view however its ancestors changed since it took focus.
    [[nodiscard]] bool actionable() const;
    void applyLayout(ui::LayoutHints const& hints);
    [[nodiscard]] ui::LayoutHints const& layout() const noexcept { return _layout; }
    void applyDragKey(std::optional<ui::Key> const& key);
    [[nodiscard]] std::optional<ui::Key> const& dragKey() const noexcept { return _dragKey; }
    void applyDropHandler(std::function<bool(ui::Key const&)> accepts, std::function<void(ui::Key)> onDrop);
    [[nodiscard]] bool isDropTarget() const noexcept { return static_cast<bool>(_onDrop); }
    /// Whether a drop of @p key lands here: a drop target whose `accepts` is empty or holds.
    [[nodiscard]] bool accepts(ui::Key const& key) const;
    /// Calls the drop handler with @p key.
    void drop(ui::Key key) const;

    /// The size this widget asks for, in cells.
    [[nodiscard]] virtual ::core::tui::Size naturalSize() const = 0;
    /// Whether keyboard focus may land here (and, while actionable, does).
    [[nodiscard]] virtual bool wantsFocus() const { return false; }
    /// Whether a Content main-axis size means Stretch(1) in a stack (a Spacer).
    [[nodiscard]] virtual bool expandsByDefault() const { return false; }
    /// The text a conformance probe reads.
    [[nodiscard]] virtual std::string probeText() const { return {}; }
    /// Draws the widget into its own area; a container also places its children here.
    virtual void paint(::core::tui::Canvas& canvas);
    /// Handles a key while actionable and focused (or bubbled to).
    [[nodiscard]] virtual ::core::tui::EventResult key(::core::tui::KeyEvent const& key);
    /// The primary action: what Enter, Space and a click do.
    virtual void activate() {}
    /// A click on @p cell (0-based, relative to this widget); runs `activate()` unless overridden.
    virtual void click(::core::tui::Point cell);
    /// A wheel notch, -1 up and +1 down; true when handled.
    [[nodiscard]] virtual bool wheel(int delta);

    /// Routes one event of this widget's view: mouse to `pointer()`, keys to `key()` while actionable.
    [[nodiscard]] ::core::tui::EventResult dispatch(::core::tui::InputEvent const& event);
    /// Press, motion, release and wheel on this widget's view.
    [[nodiscard]] ::core::tui::EventResult pointer(::core::tui::MouseEvent const& mouse);
    [[nodiscard]] bool focusable() const { return wantsFocus() && actionable(); }
    [[nodiscard]] bool hasFocus() const noexcept { return _view != nullptr && _view->focused(); }
    /// Asks the screen for a full repaint on the next draw.
    void refresh() const;

protected:
    /// Takes ownership of the widget's view and registers it; returns it typed.
    template <class ViewType>
    ViewType& adopt(std::unique_ptr<ViewType> view) {
        ViewType& adopted = *view;
        _view = std::move(view);
        _context->owners.insert_or_assign(_view.get(), this);
        return adopted;
    }

private:
    void syncVisible();
    [[nodiscard]] ::core::tui::EventResult press(::core::tui::MouseEvent const& mouse);
    [[nodiscard]] ::core::tui::EventResult release(::core::tui::MouseEvent const& mouse);

    Context* _context;
    ContainerBase* _container = nullptr;
    bool _userVisible = true;
    bool _structuralVisible = true;
    bool _enabled = true;
    ui::LayoutHints _layout{};
    std::optional<ui::Key> _dragKey;
    std::function<bool(ui::Key const&)> _accepts;
    std::function<void(ui::Key)> _onDrop;
    std::unique_ptr<::core::tui::Component> _view;
};

/// A widget whose children's views sit in a host component it provides (its own view, unless overridden).
///
/// The order of `children()` is the order of layout and of Tab. The child views stay in core::tui's tree in the
/// order they were attached: core::tui can reorder a child only by removing and re-adding it, and a removal ends
/// the screen's pointer capture on it, which would cut a press or a drag short. That order decides only which of
/// two overlapping siblings is on top, and the children of a stack never overlap.
class ContainerBase : public WidgetBase {
public:
    explicit ContainerBase(Context& context) noexcept : WidgetBase{context} {}
    /// @brief Leaves the children that outlive this container with no container.
    ~ContainerBase() override;
    ContainerBase(ContainerBase const&) = delete;
    ContainerBase& operator=(ContainerBase const&) = delete;
    ContainerBase(ContainerBase&&) = delete;
    ContainerBase& operator=(ContainerBase&&) = delete;

    /// The TUI container behind @p container; throws std::logic_error for one another backend made.
    [[nodiscard]] static ContainerBase& of(ui::ContainerWidget& container);
    /// The TUI container behind @p container; throws std::logic_error for one another backend made.
    [[nodiscard]] static ContainerBase const& of(ui::ContainerWidget const& container);

    void attach(WidgetBase& child);
    void forget(WidgetBase& child);
    /// Moves @p child to position @p index of `children()` (past the end: last); its view is not touched.
    void move(WidgetBase& child, std::size_t index);
    [[nodiscard]] std::span<WidgetBase* const> children() const noexcept { return _children; }
    [[nodiscard]] std::vector<WidgetBase*> shownChildren() const;
    [[nodiscard]] virtual ::core::tui::Component& host() { return view(); }
    /// Whether the children may take input while this container is actionable itself; false while it holds them
    /// out of reach (a collapsed panel, a closed dialog).
    [[nodiscard]] virtual bool letsChildrenAct() const { return true; }

protected:
    virtual void childAttached(WidgetBase& child);
    virtual void childForgotten(WidgetBase& child);

private:
    std::vector<WidgetBase*> _children;
};

/// A core::tui component that renders and reacts for its owner, and is visible only while its ancestors are.
template <class Base>
class Hosted : public Base {
public:
    explicit Hosted(WidgetBase& owner) : _owner{&owner} {}

    [[nodiscard]] bool visible() const noexcept override {
        return Base::visible() && (this->parent() == nullptr || this->parent()->visible());
    }
    [[nodiscard]] bool focusable() const override { return _owner->focusable(); }
    [[nodiscard]] ::core::tui::Size preferredSize() const override { return _owner->naturalSize(); }
    [[nodiscard]] WidgetBase& owner() const noexcept { return *_owner; }

private:
    WidgetBase* _owner;
};

/// The plain view most widgets use: painting and events go to @p owner. Its class lives in widget.cpp, where it is
/// the only instantiation of `Hosted` over a plain Component: a header-defined one would instantiate `Hosted` in
/// every translation unit that includes this file, most of which never use its virtual members.
[[nodiscard]] std::unique_ptr<::core::tui::Component> makeView(WidgetBase& owner);

/// Implements the Common setters of ui interface @p Interface over WidgetBase.
template <class Interface>
class TuiWidget : public Interface, public WidgetBase {
public:
    explicit TuiWidget(Context& context) noexcept : WidgetBase{context} {}
    void setVisible(bool visible) override { applyVisible(visible); }
    void setEnabled(bool enabled) override { applyEnabled(enabled); }
    void setLayout(ui::LayoutHints const& hints) override { applyLayout(hints); }
    void setDragKey(std::optional<ui::Key> const& key) override { applyDragKey(key); }
    void setDropHandler(std::function<bool(ui::Key const&)> accepts, std::function<void(ui::Key)> onDrop) override {
        applyDropHandler(std::move(accepts), std::move(onDrop));
    }
    [[nodiscard]] ui::Widget const& asWidget() const noexcept override { return *this; }
};

/// Implements the Common setters and `moveChild` of container interface @p Interface over ContainerBase.
template <class Interface>
class TuiContainer : public Interface, public ContainerBase {
public:
    explicit TuiContainer(Context& context) noexcept : ContainerBase{context} {}
    void setVisible(bool visible) override { applyVisible(visible); }
    void setEnabled(bool enabled) override { applyEnabled(enabled); }
    void setLayout(ui::LayoutHints const& hints) override { applyLayout(hints); }
    void setDragKey(std::optional<ui::Key> const& key) override { applyDragKey(key); }
    void setDropHandler(std::function<bool(ui::Key const&)> accepts, std::function<void(ui::Key)> onDrop) override {
        applyDropHandler(std::move(accepts), std::move(onDrop));
    }
    void moveChild(ui::Widget& child, std::size_t index) override { move(WidgetBase::of(child), index); }
    [[nodiscard]] ui::Widget const& asWidget() const noexcept override { return *this; }
};

/// How `arrangeStack` places children.
struct StackSpec {
    ui::Axis axis = ui::Axis::Vertical;  ///< The main axis.
    int gap = 0;                         ///< Cells between neighbours.
    std::size_t skip = 0;                ///< Leading shown children given no area: scrolled out of view.
    std::span<int const> extents;        ///< When non-empty, the main-axis extents to use, in order (table columns).
};

/// The natural size of @p children stacked along @p axis with @p gap between them; hidden children count nothing.
[[nodiscard]] ::core::tui::Size stackNaturalSize(std::span<WidgetBase* const> children, ui::Axis axis, int gap);
/// Places @p children inside @p area (parent-relative). Hidden and skipped children get an empty area, so they
/// neither render nor take a click (see `forgetDrawnBounds`).
///
/// With no extents the layout solver sizes the shown children, and a hidden one takes no space. With extents every
/// child takes the next one, in order, so a hidden child keeps its slot and the children after it stay in their
/// columns; a child past the last extent takes its natural extent. A skipped child takes no space in either mode;
/// with extents it still uses up its extent, so each later child keeps its own column's.
void arrangeStack(std::span<WidgetBase* const> children, ::core::tui::Rect area, StackSpec const& spec);
/// @p text split at '\n'.
[[nodiscard]] std::vector<std::string_view> splitLines(std::string_view text);
/// The display width of @p text in cells.
[[nodiscard]] int displayWidth(std::string_view text);
/// Enter, or Space, with no modifier.
[[nodiscard]] bool isActivation(::core::tui::KeyEvent const& key) noexcept;
/// Appends the view of every focusable widget at or below @p root, depth first in `children()` order: Tab order.
void collectFocusable(WidgetBase& root, std::vector<::core::tui::Component*>& out);
/// Empties the screen bounds of every component below @p top, which a drawn tree's top component (a root widget's
/// view) calls as it renders, before its descendants render.
///
/// core::tui leaves a component's screen bounds as they were when it is not drawn: one whose area is empty or lies
/// outside its parent's. Hit-testing reads those bounds, so a widget squeezed or scrolled out of a frame would still
/// take clicks at the place it last had. Emptied first, the bounds after a frame are those of what it drew.
void forgetDrawnBounds(::core::tui::Component& top);
/// Whether @p node is @p root or one of its descendants.
[[nodiscard]] bool isWithin(::core::tui::Component const* node, ::core::tui::Component const& root) noexcept;
/// Puts @p widget under @p parent, or at the screen's root when @p parent is null.
void attach(Context& context, ui::ContainerWidget* parent, WidgetBase& widget);
/// Sizes every root widget to the viewport.
void fit(Context& context);
/// Moves keyboard focus to the next or previous focusable widget of the roots, in `collectFocusable` order,
/// wrapping around at either end.
void moveFocus(Context& context, Direction direction);

}  // namespace morph::tui::detail
