// SPDX-License-Identifier: Apache-2.0

#include "tui/drag.hpp"

#include <core/tui/Box.hpp>
#include <core/tui/Canvas.hpp>
#include <core/tui/Component.hpp>
#include <core/tui/KeyCode.hpp>
#include <core/tui/Screen.hpp>
#include <core/tui/Theme.hpp>
#include <cstdlib>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "tui/widget.hpp"

namespace morph::tui::detail {

namespace {

/// Cells the pointer travels from the press before the press becomes a drag.
constexpr int kDragThreshold = 1;
/// Columns between the pointer and the label.
constexpr int kLabelOffset = 2;

std::string keyText(ui::Key const& key) {
    return std::visit(
        [](auto const& value) -> std::string {
            if constexpr (std::is_same_v<std::decay_t<decltype(value)>, std::string>) {
                return value;
            } else {
                return std::to_string(value);
            }
        },
        key);
}

}  // namespace

// The label and the outline are drawn, but are not there for hit-testing: each empties the screen bounds core::tui
// gives it for the frame as it renders. Hit-testing finds overlays first, so otherwise a wheel or a press during the
// drag would land on them instead of on the widget under them, a drop would look for its target in them, and
// core::tui's hover state would keep a pointer to a component the backend owns.

/// The label that follows the pointer: the source's first line of text, or its key.
class DragController::Label final : public ::core::tui::Component {
public:
    void setText(std::string text) { _text = std::move(text); }
    void render(::core::tui::Canvas& canvas) override {
        setScreenBounds({});
        canvas.putString(0, 0, _text, canvas.theme().listItemSelected);
    }
    [[nodiscard]] ::core::tui::Size preferredSize() const override {
        return {.width = displayWidth(_text), .height = 1};
    }

private:
    std::string _text;
};

/// The outline around the drop target, an overlay so that the target's children cannot paint over it. It spans the
/// viewport and draws the box where the target was drawn in this frame, read as it renders, after the tree: the
/// outline stays on the target when the target moves during the drag.
class DragController::Outline final : public ::core::tui::Component {
public:
    explicit Outline(DragController const& drag) : _drag{&drag} {}
    void render(::core::tui::Canvas& canvas) override {
        auto const origin = screenBounds().position();
        setScreenBounds({});
        auto const* const target = _drag->_target;
        if (target == nullptr || !reachable(*target)) {
            return;
        }
        auto const bounds = target->view().screenBounds();
        if (!bounds.empty()) {
            canvas.drawBox(bounds.offset(-origin.x, -origin.y), ::core::tui::BorderStyle::Double,
                           canvas.theme().textAccent);
        }
    }
    /// From the screen's corner to the viewport's far one; core::tui clips an overlay to the viewport.
    [[nodiscard]] ::core::tui::Size preferredSize() const override {
        auto const area = _drag->_context->screen->viewportArea();
        return {.width = area.x + area.width, .height = area.y + area.height};
    }

private:
    DragController const* _drag;
};

DragController::DragController(Context& context)
    : _context{&context}, _label{std::make_unique<Label>()}, _outline{std::make_unique<Outline>(*this)} {}

DragController::~DragController() { cancel(); }

void DragController::press(WidgetBase& source, ui::Key key, ::core::tui::Point point) {
    cancel();
    _source = &source;
    _key = std::move(key);
    _pressedAt = point;
}

void DragController::move(::core::tui::Point point) {
    if (_source == nullptr) {
        return;
    }
    if (!reachable(*_source)) {
        cancel();
        return;
    }
    if (!_dragging) {
        if (std::abs(point.x - _pressedAt.x) + std::abs(point.y - _pressedAt.y) < kDragThreshold) {
            return;
        }
        start();
    }
    _context->screen->showOverlay(*_label, {.x = point.x + kLabelOffset, .y = point.y});
    auto* const target = findTarget(point);
    if (_source != nullptr) {
        setTarget(target);
    }
}

bool DragController::release(::core::tui::Point point) {
    if (_source == nullptr) {
        return false;
    }
    bool const dragged = _dragging;
    if (!dragged || !_key || !reachable(*_source)) {
        cancel();
        return dragged;
    }
    auto key = *_key;
    auto* const target = findTarget(point);
    bool const live = _source != nullptr;
    cancel();
    // Last, after the overlays are gone: the handler may destroy the target, the source or anything else.
    if (live && target != nullptr) {
        target->drop(std::move(key));
    }
    return true;
}

void DragController::cancel() {
    auto& screen = *_context->screen;
    screen.hideOverlay(*_label);
    screen.hideOverlay(*_outline);
    _source = nullptr;
    _target = nullptr;
    _key.reset();
    _dragging = false;
}

void DragController::keyChanged(WidgetBase const& widget, std::optional<ui::Key> const& key) {
    if (&widget != _source) {
        return;
    }
    if (key) {
        _key = key;
        return;
    }
    _context->endPress();
}

void DragController::forget(WidgetBase const& widget) {
    if (&widget == _source) {
        cancel();
    } else if (&widget == _target) {
        setTarget(nullptr);
    }
}

bool DragController::reachable(WidgetBase const& widget) { return widget.actionable() && !widget.blockedByDialog(); }

// The candidates are gathered before any predicate runs and are watched while they run: a predicate may destroy any
// of them, the source included, and `Context::forget` then sets the entry to null. The key is a copy, as ending the
// gesture resets `_key`.
WidgetBase* DragController::findTarget(::core::tui::Point point) {
    if (_source == nullptr || !_key) {
        return nullptr;
    }
    auto const key = *_key;
    std::vector<WidgetBase*> candidates;
    for (auto const* node = _context->screen->componentAt(point.y, point.x); node != nullptr; node = node->parent()) {
        auto* const widget = _context->ownerOf(node);
        if (widget != nullptr && widget != _source) {
            candidates.push_back(widget);
        }
    }
    for (auto& candidate : candidates) {
        _context->watches.push_back(&candidate);
    }
    WidgetBase* found = nullptr;
    for (auto const& candidate : candidates) {
        if (candidate == nullptr || !candidate->isDropTarget() || !reachable(*candidate)) {
            continue;
        }
        bool const accepted = candidate->accepts(key);
        if (accepted && candidate != nullptr) {
            found = candidate;
            break;
        }
    }
    for (auto& candidate : candidates) {
        std::erase(_context->watches, &candidate);
    }
    return _source != nullptr ? found : nullptr;
}

void DragController::setTarget(WidgetBase* target) {
    if (target != _target) {
        _target = target;
        _context->screen->invalidate();
    }
}

// The outline is shown first, so the label, shown after it, is drawn over it.
void DragController::start() {
    _dragging = true;
    _label->setText("[" + labelText() + "]");
    _context->screen->showOverlay(*_outline, {});
}

// Depth first, the source before what it holds: a card that is a column of a title and a detail shows its title. Each
// text is held while its first line is read, as `splitLines` returns views into it.
std::string DragController::labelText() const {
    std::vector<WidgetBase const*> pending{_source};
    while (!pending.empty()) {
        auto const* const widget = pending.back();
        pending.pop_back();
        if (widget != _source && !widget->shown()) {
            continue;
        }
        auto const text = widget->probeText();
        if (auto const line = splitLines(text).front(); !line.empty()) {
            return std::string{line};
        }
        if (auto const* const container = dynamic_cast<ContainerBase const*>(widget)) {
            auto const children = container->children();
            pending.insert(pending.end(), children.rbegin(), children.rend());
        }
    }
    return _key ? keyText(*_key) : std::string{};
}

// The capture is the source's view while it drags; released here, the moves and the release that follow are
// hit-tested and belong to no press.
bool escapeEndsDrag(Context& context, ::core::tui::InputEvent const& event) {
    auto const* const key = std::get_if<::core::tui::KeyEvent>(&event);
    if (key == nullptr || key->codepoint != 0 || key->key != ::core::tui::KeyCode::Escape ||
        !context.drag->dragging()) {
        return false;
    }
    auto& screen = *context.screen;
    if (auto const* const source = context.drag->source();
        source != nullptr && screen.pointerCapture() == &source->view()) {
        screen.releasePointer();
    }
    context.endPress();
    return true;
}

}  // namespace morph::tui::detail
