// SPDX-License-Identifier: Apache-2.0

#include "tui/widget.hpp"

#include <algorithm>
#include <core/tui/KeyCode.hpp>
#include <core/tui/Modifier.hpp>
#include <core/tui/Screen.hpp>
#include <core/tui/Unicode.hpp>
#include <iterator>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <variant>

#include "tui/layout.hpp"

namespace morph::tui::detail {

using ::core::tui::EventResult;

namespace {

/// The plain view: painting and events go to the owner.
class View final : public Hosted<::core::tui::Component> {
public:
    using Hosted::Hosted;
    void render(::core::tui::Canvas& canvas) override {
        if (owner().container() == nullptr) {
            forgetDrawnBounds(*this);
        }
        owner().paint(canvas);
    }
    /// Hands the event to the owner. A handler may destroy the owner meanwhile, and this view with it, so nothing is
    /// touched after the call.
    [[nodiscard]] EventResult onEvent(::core::tui::InputEvent const& event) override {
        return owner().dispatch(event);
    }
};

}  // namespace

std::unique_ptr<::core::tui::Component> makeView(WidgetBase& owner) { return std::make_unique<View>(owner); }

WidgetBase::WidgetBase(Context& context) noexcept : _context{&context} {}

WidgetBase::~WidgetBase() {
    if (_view != nullptr) {
        if (_context->screen->focusedComponent() == _view.get()) {
            _context->screen->setFocus(nullptr);
        }
        _context->owners.erase(_view.get());
    }
    _context->forget(*this);
    if (_container != nullptr) {
        _container->forget(*this);
    }
}

WidgetBase& WidgetBase::of(ui::Widget& widget) {
    auto* const base = dynamic_cast<WidgetBase*>(&widget);
    if (base == nullptr) {
        throw std::logic_error{"morph::tui: a widget another backend created"};
    }
    return *base;
}

WidgetBase const& WidgetBase::of(ui::Widget const& widget) {
    auto const* const base = dynamic_cast<WidgetBase const*>(&widget);
    if (base == nullptr) {
        throw std::logic_error{"morph::tui: a widget another backend created"};
    }
    return *base;
}

void WidgetBase::applyVisible(bool visible) {
    _userVisible = visible;
    syncVisible();
}

void WidgetBase::setStructuralVisible(bool visible) {
    _structuralVisible = visible;
    syncVisible();
}

void WidgetBase::syncVisible() {
    if (_view != nullptr) {
        _view->setVisible(shown());
    }
    refresh();
}

void WidgetBase::applyEnabled(bool enabled) {
    _enabled = enabled;
    refresh();
}

bool WidgetBase::actionable() const {
    if (!shown() || !_enabled) {
        return false;
    }
    for (ContainerBase const* outer = _container; outer != nullptr; outer = outer->container()) {
        if (!outer->shown() || !outer->enabled() || !outer->letsChildrenAct()) {
            return false;
        }
    }
    return true;
}

void WidgetBase::applyLayout(ui::LayoutHints const& hints) {
    _layout = hints;
    refresh();
}

void WidgetBase::applyDragKey(std::optional<ui::Key> const& key) { _dragKey = key; }

void WidgetBase::applyDropHandler(std::function<bool(ui::Key const&)> accepts, std::function<void(ui::Key)> onDrop) {
    _accepts = std::move(accepts);
    _onDrop = std::move(onDrop);
}

bool WidgetBase::accepts(ui::Key const& key) const {
    if (!isDropTarget()) {
        return false;
    }
    auto const predicate = _accepts;
    return !predicate || predicate(key);
}

void WidgetBase::drop(ui::Key key) const {
    auto const handler = _onDrop;
    if (handler) {
        handler(std::move(key));
    }
}

void WidgetBase::paint(::core::tui::Canvas& /*canvas*/) {}

EventResult WidgetBase::key(::core::tui::KeyEvent const& /*key*/) { return EventResult::Ignored; }

void WidgetBase::click(::core::tui::Point /*cell*/) { activate(); }

bool WidgetBase::wheel(int /*delta*/) { return false; }

EventResult WidgetBase::dispatch(::core::tui::InputEvent const& event) {
    if (auto const* mouse = std::get_if<::core::tui::MouseEvent>(&event)) {
        return pointer(*mouse);
    }
    if (auto const* pressed = std::get_if<::core::tui::KeyEvent>(&event); pressed != nullptr && actionable()) {
        return key(*pressed);
    }
    return EventResult::Ignored;
}

EventResult WidgetBase::pointer(::core::tui::MouseEvent const& mouse) {
    using Type = ::core::tui::MouseEvent::Type;
    if (mouse.type == Type::ScrollUp || mouse.type == Type::ScrollDown) {
        bool const handled = actionable() && wheel(mouse.type == Type::ScrollUp ? -1 : 1);
        return handled ? EventResult::Handled : EventResult::Ignored;
    }
    if (mouse.type == Type::Press) {
        return press(mouse);
    }
    if (mouse.type == Type::Release) {
        return release(mouse);
    }
    return _context->pressed == this ? EventResult::Handled : EventResult::Ignored;
}

// Any press ends an earlier gesture whose release never arrived, whether or not this widget takes the new one.
// Handling the press makes this view the screen's pointer capture target, so the moves and the release that follow
// come here wherever the pointer goes.
EventResult WidgetBase::press(::core::tui::MouseEvent const& mouse) {
    _context->pressed = nullptr;
    if (mouse.button != 0 || !actionable() || !(wantsFocus() || _dragKey)) {
        return EventResult::Ignored;
    }
    _context->pressed = this;
    if (wantsFocus()) {
        _context->screen->setFocus(_view.get());
    }
    return EventResult::Handled;
}

// A release clicks only inside the view and only while the widget is still actionable: its container may have been
// disabled or hidden since the press.
EventResult WidgetBase::release(::core::tui::MouseEvent const& mouse) {
    if (_context->pressed != this) {
        return EventResult::Ignored;
    }
    _context->pressed = nullptr;
    ::core::tui::Point const cell{.x = mouse.x - 1, .y = mouse.y - 1};
    auto const bounds = _view->screenBounds();
    bool const inside = cell.x >= 0 && cell.y >= 0 && cell.x < bounds.width && cell.y < bounds.height;
    if (inside && actionable()) {
        click(cell);
    }
    return EventResult::Handled;
}

void WidgetBase::refresh() const { _context->screen->invalidate(); }

ContainerBase::~ContainerBase() {
    for (auto* child : _children) {
        child->setContainer(nullptr);
    }
}

ContainerBase& ContainerBase::of(ui::ContainerWidget& container) {
    auto* const base = dynamic_cast<ContainerBase*>(&container);
    if (base == nullptr) {
        throw std::logic_error{"morph::tui: a container another backend created"};
    }
    return *base;
}

ContainerBase const& ContainerBase::of(ui::ContainerWidget const& container) {
    auto const* const base = dynamic_cast<ContainerBase const*>(&container);
    if (base == nullptr) {
        throw std::logic_error{"morph::tui: a container another backend created"};
    }
    return *base;
}

void ContainerBase::attach(WidgetBase& child) {
    _children.push_back(&child);
    child.setContainer(this);
    host().addChild(child.view(), ::core::tui::LayoutParams{.area = {}, .visible = child.shown()});
    childAttached(child);
    refresh();
}

void ContainerBase::forget(WidgetBase& child) {
    std::erase(_children, &child);
    childForgotten(child);
    refresh();
}

void ContainerBase::move(WidgetBase& child, std::size_t index) {
    auto const found = std::ranges::find(_children, &child);
    if (found == _children.end()) {
        return;
    }
    _children.erase(found);
    auto const position = static_cast<std::ptrdiff_t>(std::min(index, _children.size()));
    _children.insert(std::next(_children.begin(), position), &child);
    refresh();
}

std::vector<WidgetBase*> ContainerBase::shownChildren() const {
    std::vector<WidgetBase*> shown;
    std::ranges::copy_if(_children, std::back_inserter(shown), [](WidgetBase const* child) { return child->shown(); });
    return shown;
}

void ContainerBase::childAttached(WidgetBase& /*child*/) {}

void ContainerBase::childForgotten(WidgetBase& /*child*/) {}

::core::tui::Size stackNaturalSize(std::span<WidgetBase* const> children, ui::Axis axis, int gap) {
    bool const vertical = axis == ui::Axis::Vertical;
    int along = 0;
    int across = 0;
    int count = 0;
    for (auto const* child : children) {
        if (!child->shown()) {
            continue;
        }
        auto const natural = child->naturalSize();
        along += vertical ? natural.height : natural.width;
        across = std::max(across, vertical ? natural.width : natural.height);
        ++count;
    }
    if (count > 1) {
        along += gap * (count - 1);
    }
    return vertical ? ::core::tui::Size{.width = across, .height = along}
                    : ::core::tui::Size{.width = along, .height = across};
}

namespace {

/// What every child of one `arrangeStack` call shares.
struct Placement {
    ::core::tui::Rect area{};  ///< The stack's area, parent-relative.
    bool vertical = true;      ///< Whether the main axis is vertical.
};

/// @p size along the main axis.
int mainExtent(::core::tui::Size size, bool vertical) { return vertical ? size.height : size.width; }

/// Gives @p child the slot at @p offset, @p extent long, along the main axis; across, its sizing decides.
void place(WidgetBase& child, Placement const& placement, int offset, int extent) {
    auto const& area = placement.area;
    if (placement.vertical) {
        int const across = layout::crossExtent(child.layout().width, area.width);
        child.view().setArea({.x = area.x, .y = area.y + offset, .width = across, .height = extent});
    } else {
        int const across = layout::crossExtent(child.layout().height, area.height);
        child.view().setArea({.x = area.x + offset, .y = area.y, .width = extent, .height = across});
    }
}

/// The solver's mode: the shown children past the skipped ones share the area; the others get none of it.
void arrangeSolved(std::span<WidgetBase* const> children, Placement const& placement, StackSpec const& spec) {
    std::vector<WidgetBase*> placed;
    placed.reserve(children.size());
    std::size_t skipped = 0;
    for (auto* child : children) {
        if (!child->shown() || skipped < spec.skip) {
            skipped += child->shown() ? 1U : 0U;
            child->view().setArea({});
            continue;
        }
        placed.push_back(child);
    }

    std::vector<layout::Item> items;
    items.reserve(placed.size());
    for (auto const* child : placed) {
        auto sizing = placement.vertical ? child->layout().height : child->layout().width;
        if (sizing.kind == ui::Sizing::Kind::Content && child->expandsByDefault()) {
            sizing = ui::Sizing::stretch(1);
        }
        items.push_back(
            layout::Item{.sizing = sizing, .natural = mainExtent(child->naturalSize(), placement.vertical)});
    }
    auto const length = placement.vertical ? placement.area.height : placement.area.width;
    auto const extents = layout::distribute(items, layout::Track{.length = length, .gap = spec.gap});

    int offset = 0;
    for (auto const [child, extent] : std::views::zip(placed, extents)) {
        place(*child, placement, offset, extent);
        offset += extent + spec.gap;
    }
}

/// The extents' mode: each child takes the next extent, a hidden one keeping its slot empty.
void arrangeColumns(std::span<WidgetBase* const> children, Placement const& placement, StackSpec const& spec) {
    auto given = spec.extents.begin();
    std::size_t skipped = 0;
    int offset = 0;
    for (auto* child : children) {
        int extent = 0;
        if (given != spec.extents.end()) {
            extent = std::max(*given, 0);
            ++given;
        } else {
            extent = mainExtent(child->naturalSize(), placement.vertical);
        }
        if (child->shown() && skipped < spec.skip) {
            ++skipped;
            child->view().setArea({});
            continue;
        }
        if (child->shown()) {
            place(*child, placement, offset, extent);
        } else {
            child->view().setArea({});
        }
        offset += extent + spec.gap;
    }
}

}  // namespace

void arrangeStack(std::span<WidgetBase* const> children, ::core::tui::Rect area, StackSpec const& spec) {
    Placement const placement{.area = area, .vertical = spec.axis == ui::Axis::Vertical};
    if (spec.extents.empty()) {
        arrangeSolved(children, placement, spec);
    } else {
        arrangeColumns(children, placement, spec);
    }
}

std::vector<std::string_view> splitLines(std::string_view text) {
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    for (;;) {
        auto const end = text.find('\n', start);
        lines.push_back(text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        if (end == std::string_view::npos) {
            return lines;
        }
        start = end + 1;
    }
}

int displayWidth(std::string_view text) { return ::core::tui::stringWidth(text); }

bool isActivation(::core::tui::KeyEvent const& key) noexcept {
    return ::core::tui::withoutLockKeys(key.modifiers) == ::core::tui::Modifier::None &&
           (key.key == ::core::tui::KeyCode::Enter || key.codepoint == U' ');
}

// A subtree whose root is not actionable has no actionable widget in it, so the walk does not descend into it.
void collectFocusable(WidgetBase& root, std::vector<::core::tui::Component*>& out) {
    std::vector<WidgetBase*> pending{&root};
    while (!pending.empty()) {
        auto* const widget = pending.back();
        pending.pop_back();
        if (!widget->actionable()) {
            continue;
        }
        if (widget->wantsFocus()) {
            out.push_back(&widget->view());
        }
        if (auto const* const container = dynamic_cast<ContainerBase const*>(widget)) {
            auto const children = container->children();
            pending.insert(pending.end(), children.rbegin(), children.rend());
        }
    }
}

void forgetDrawnBounds(::core::tui::Component& top) {
    auto const below = top.children();
    std::vector<::core::tui::Component*> pending{below.begin(), below.end()};
    while (!pending.empty()) {
        auto* const component = pending.back();
        pending.pop_back();
        component->setScreenBounds({});
        auto const children = component->children();
        pending.insert(pending.end(), children.begin(), children.end());
    }
}

bool isWithin(::core::tui::Component const* node, ::core::tui::Component const& root) noexcept {
    for (; node != nullptr; node = node->parent()) {
        if (node == &root) {
            return true;
        }
    }
    return false;
}

void attach(Context& context, ui::ContainerWidget* parent, WidgetBase& widget) {
    if (parent == nullptr) {
        context.roots.push_back(&widget);
        context.screen->root().addChild(widget.view(),
                                        ::core::tui::LayoutParams{.area = context.screen->viewportArea()});
        return;
    }
    ContainerBase::of(*parent).attach(widget);
}

void fit(Context& context) {
    auto const area = context.screen->viewportArea();
    for (auto* root : context.roots) {
        root->view().setArea(area);
    }
}

// core::tui's own focusNext walks its child lists, which keep attach order (see ContainerBase); Tab follows
// morph's order instead.
void moveFocus(Context& context, Direction direction) {
    std::vector<::core::tui::Component*> order;
    for (auto* root : context.roots) {
        collectFocusable(*root, order);
    }
    if (order.empty()) {
        return;
    }
    auto const current = std::ranges::find(order, context.screen->focusedComponent());
    auto target = order.begin();
    if (direction == Direction::Forward) {
        if (current != order.end() && std::next(current) != order.end()) {
            target = std::next(current);
        }
    } else if (current == order.end() || current == order.begin()) {
        target = std::prev(order.end());
    } else {
        target = std::prev(current);
    }
    context.screen->setFocus(*target);
}

}  // namespace morph::tui::detail
