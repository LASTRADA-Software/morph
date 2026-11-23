// SPDX-License-Identifier: Apache-2.0

#include <QCoreApplication>
#include <QDebug>
#include <QEvent>
#include <QKeyEvent>
#include <QMetaObject>
#include <QObject>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRectF>
#include <QVariant>
#include <algorithm>
#include <cstddef>
#include <functional>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "qt_quick/keys.hpp"
#include "qt_quick/kinds.hpp"

namespace morph::qt_quick::detail {

namespace {

// Depth first: the items Tab stops at, shown and enabled.
// NOLINTNEXTLINE(misc-no-recursion): an item tree, walked down.
void collectTabStops(QQuickItem const& item, std::vector<QQuickItem*>& out) {
    for (QQuickItem* const child : item.childItems()) {
        if (!child->isVisible() || !child->isEnabled()) {
            continue;
        }
        if (child->activeFocusOnTab()) {
            out.push_back(child);
        }
        collectTabStops(*child, out);
    }
}

// Scrolls the nearest flickable around @p target until the target's box is inside its viewport. A box taller (or
// wider) than the viewport shows its top (left) edge: scrolling to its far edge would leave the near one out of view.
void scrollIntoView(QQuickItem const& target) {
    for (QQuickItem* outer = target.parentItem(); outer != nullptr; outer = outer->parentItem()) {
        if (!outer->inherits("QQuickFlickable")) {
            continue;
        }
        QQuickItem const* const content = outer->property("contentItem").value<QQuickItem*>();
        if (content == nullptr) {
            return;
        }
        QRectF const box = target.mapRectToItem(content, QRectF{0, 0, target.width(), target.height()});
        qreal const top = outer->property("contentY").toReal();
        qreal const left = outer->property("contentX").toReal();
        if (box.top() < top || box.height() > outer->height()) {
            outer->setProperty("contentY", box.top());
        } else if (box.bottom() > top + outer->height()) {
            outer->setProperty("contentY", box.bottom() - outer->height());
        }
        if (box.left() < left || box.width() > outer->width()) {
            outer->setProperty("contentX", box.left());
        } else if (box.right() > left + outer->width()) {
            outer->setProperty("contentX", box.right() - outer->width());
        }
        return;
    }
}

// Whether @p item is a MorphWidget, which declares `chords`.
bool declaresChords(QQuickItem const& item) { return item.metaObject()->indexOfProperty("chords") >= 0; }

// Depth first: every item under @p item.
// NOLINTNEXTLINE(misc-no-recursion): an item tree, walked down.
void collectItems(QQuickItem const& item, std::vector<QQuickItem*>& out) {
    for (QQuickItem* const child : item.childItems()) {
        out.push_back(child);
        collectItems(*child, out);
    }
}

// Offers @p event to @p focused and then to each ancestor, as the window would, and says whether one took it. The
// event is delivered as a copy that starts accepted, which is how an item's `keyPressEvent` is entered: it ignores the
// copy when the key means nothing to it.
bool takenByControl(QQuickItem* focused, QKeyEvent const& event) {
    for (QQuickItem* at = focused; at != nullptr; at = at->parentItem()) {
        QKeyEvent offered{event.type(), event.key(),          event.modifiers(),
                          event.text(), event.isAutoRepeat(), static_cast<ushort>(event.count())};
        QCoreApplication::sendEvent(at, &offered);
        if (offered.isAccepted()) {
            return true;
        }
    }
    return false;
}

}  // namespace

// The window's key filter while the spotlight is open: keys reach the spotlight before the focused item.
class SpotlightImpl::Filter final : public QObject {
public:
    explicit Filter(SpotlightImpl& owner) : _owner{&owner} {}

    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::KeyPress) {
            return _owner->filterKey(
                *static_cast<QKeyEvent*>(event));  // NOLINT(cppcoreguidelines-pro-type-static-cast-downcast)
        }
        return QObject::eventFilter(watched, event);
    }

private:
    SpotlightImpl* _owner;
};

SpotlightImpl::SpotlightImpl(Context& context, ui::ContainerWidget* parent)
    : ContainerItemWidget{context, QStringLiteral("MorphSpotlight"), parent},
      _filter{std::make_unique<Filter>(*this)},
      _listener{context.widgets().listen([this] {
          resolve();
          track();
      })} {}  // NOLINT(readability-trailing-comma)

SpotlightImpl::~SpotlightImpl() {
    context().widgets().unlisten(_listener);
    for (QMetaObject::Connection const& connection : _watching) {
        QObject::disconnect(connection);
    }
    QObject::disconnect(_frames);
    if (_window != nullptr) {
        _window->removeEventFilter(_filter.get());
    }
}

void SpotlightImpl::setOpen(bool open) {
    if (open == _open) {
        return;
    }
    _open = open;
    write("open", open);
    if (open) {
        _window = item() != nullptr ? item()->window() : nullptr;
        if (_window != nullptr) {
            _restore = _window->activeFocusItem();
            _window->installEventFilter(_filter.get());
            _frames = QObject::connect(_window, &QQuickWindow::afterAnimating, _filter.get(), [this] { track(); });
        }
        _scrollPending = true;
        resolve();
        track();
        focusCard();
        return;
    }
    if (_window != nullptr) {
        _window->removeEventFilter(_filter.get());
    }
    QObject::disconnect(_frames);
    _window.clear();
    if (QQuickItem* const restore = _restore) {
        restore->forceActiveFocus(Qt::OtherFocusReason);
    }
    _restore.clear();
}

void SpotlightImpl::setTarget(std::optional<std::string> testId) {
    _target = testId ? std::optional<QString>{toQString(*testId)} : std::nullopt;
    resolve();
    track();
}

void SpotlightImpl::resolve() {
    if (_resolving) {
        return;
    }
    _resolving = true;
    for (QMetaObject::Connection const& connection : _watching) {
        QObject::disconnect(connection);
    }
    _watching.clear();
    ItemBase* found = nullptr;
    auto const* const card = qobject_cast<QQuickItem*>(part("card"));
    if (_target) {
        for (ItemBase* const candidate : context().widgets().carrying(*_target)) {
            QQuickItem const* const hit = candidate->item();
            if (hit == nullptr || candidate == this || (card != nullptr && card->isAncestorOf(hit))) {
                continue;
            }
            // Its effective visibility: a hidden container around it, or a tab page not selected, hides it too.
            _watching.push_back(QObject::connect(hit, &QQuickItem::visibleChanged, _filter.get(), [this] {
                resolve();
                track();
            }));
            if (found == nullptr && hit->isVisible()) {
                found = candidate;
            }
        }
    }
    bool const was = _ringed != nullptr;
    if (found != _ringed) {
        _scrollPending = true;
    }
    _ringed = found;
    write("ringed", found != nullptr);
    _resolving = false;
    if ((found != nullptr) != was && _onChanged) {
        auto const handler = _onChanged;
        handler(found != nullptr);
    }
}

void SpotlightImpl::track() {
    if (!_open || _ringed == nullptr || _ringed->item() == nullptr) {
        return;
    }
    QQuickItem const& target = *_ringed->item();
    // Once per target, when it has a box: a frame-by-frame scroll would fight the person scrolling the page.
    if (_scrollPending && target.height() > 0 && target.width() > 0) {
        _scrollPending = false;
        scrollIntoView(target);
    }
    write("cutout", target.mapRectToScene(QRectF{0, 0, target.width(), target.height()}));
}

std::vector<QQuickItem*> SpotlightImpl::tabChain() const {
    std::vector<QQuickItem*> chain;
    if (auto const* const content = qobject_cast<QQuickItem*>(part("content"))) {
        collectTabStops(*content, chain);
    }
    if (_targetFocusable && _ringed != nullptr && _ringed->focusTarget() != nullptr) {
        chain.push_back(_ringed->focusTarget());
    }
    return chain;
}

void SpotlightImpl::focusCard() {
    std::vector<QQuickItem*> const chain = tabChain();
    if (!chain.empty()) {
        chain.front()->forceActiveFocus(Qt::TabFocusReason);
    } else if (auto* const content = qobject_cast<QQuickItem*>(part("content"))) {
        content->forceActiveFocus(Qt::OtherFocusReason);
    }
}

void SpotlightImpl::cycleFocus(bool forward) {
    std::vector<QQuickItem*> const chain = tabChain();
    if (chain.empty() || _window == nullptr) {
        return;
    }
    QQuickItem const* const focused = _window->activeFocusItem();
    auto const inChain = std::ranges::find_if(chain, [focused](QQuickItem const* item) {
        return focused != nullptr && (item == focused || item->isAncestorOf(focused));
    });
    std::size_t const count = chain.size();
    std::size_t next = forward ? 0 : count - 1;
    if (inChain != chain.end()) {
        auto const index = static_cast<std::size_t>(std::distance(chain.begin(), inChain));
        next = forward ? (index + 1) % count : (index + count - 1) % count;
    }
    chain.at(next)->forceActiveFocus(forward ? Qt::TabFocusReason : Qt::BacktabFocusReason);
}

bool SpotlightImpl::filterKey(QKeyEvent const& event) {
    auto const* const content = qobject_cast<QQuickItem*>(part("content"));
    if (content == nullptr) {
        return false;
    }
    int const key = event.key();
    if (key == Qt::Key_Tab || key == Qt::Key_Backtab) {
        // The trap holds wherever focus is: a card button that hid, or a click on the page, leaves it elsewhere.
        cycleFocus(key == Qt::Key_Tab && !event.modifiers().testFlag(Qt::ShiftModifier));
        return true;
    }
    // Enter and Space belong to the focused control, whichever it is.
    if (key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Space) {
        return false;
    }
    // Focus outside the card is on the ringed target or on a popup it opened: that control takes the key first, so
    // Escape closes an open menu, and the card's chords run only for a key it left alone.
    QQuickItem* const focused = _window != nullptr ? _window->activeFocusItem() : nullptr;
    if (focused != nullptr && focused != content && !content->isAncestorOf(focused) &&
        takenByControl(focused, event)) {
        return true;
    }
    // The card's widgets, the one holding focus and its ancestors first, then the rest, then the spotlight itself:
    // its chords reach the card whatever has focus.
    std::vector<QQuickItem*> candidates;
    for (QQuickItem* at = focused; at != nullptr && at != content->parentItem(); at = at->parentItem()) {
        if (content->isAncestorOf(at)) {
            candidates.push_back(at);
        }
    }
    std::vector<QQuickItem*> everything;
    collectItems(*content, everything);
    candidates.insert(candidates.end(), everything.begin(), everything.end());
    candidates.push_back(item());
    std::vector<QQuickItem*> seen;
    for (QQuickItem* const widget : candidates) {
        if (widget == nullptr || std::ranges::find(seen, widget) != seen.end() || !declaresChords(*widget)) {
            continue;
        }
        seen.push_back(widget);
        if (widget->metaObject()->indexOfMethod("takeChord(int,int)") < 0) {
            qCritical("morph::qt_quick: a MorphWidget in a spotlight has no takeChord(int,int): its chords never run");
            continue;
        }
        bool taken = false;
        QMetaObject::invokeMethod(widget, "takeChord", Qt::DirectConnection, qReturnArg(taken), key,
                                  static_cast<int>(event.modifiers().toInt()));
        if (taken) {
            return true;
        }
    }
    return false;
}

}  // namespace morph::qt_quick::detail
