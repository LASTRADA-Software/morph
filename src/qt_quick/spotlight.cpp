// SPDX-License-Identifier: Apache-2.0

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
void collectTabStops(QQuickItem& item, std::vector<QQuickItem*>& out) {
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

// Scrolls the nearest flickable around @p target until the target's box is inside its viewport.
void scrollIntoView(QQuickItem& target) {
    for (QQuickItem* outer = target.parentItem(); outer != nullptr; outer = outer->parentItem()) {
        if (!outer->inherits("QQuickFlickable")) {
            continue;
        }
        auto* const content = outer->property("contentItem").value<QQuickItem*>();
        if (content == nullptr) {
            return;
        }
        QRectF const box = target.mapRectToItem(content, QRectF{0, 0, target.width(), target.height()});
        qreal const top = outer->property("contentY").toReal();
        qreal const left = outer->property("contentX").toReal();
        if (box.top() < top) {
            outer->setProperty("contentY", box.top());
        } else if (box.bottom() > top + outer->height()) {
            outer->setProperty("contentY", box.bottom() - outer->height());
        }
        if (box.left() < left) {
            outer->setProperty("contentX", box.left());
        } else if (box.right() > left + outer->width()) {
            outer->setProperty("contentX", box.right() - outer->width());
        }
        return;
    }
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
      })} {}

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
            QQuickItem* const at = candidate->item();
            if (at == nullptr || candidate == this || (card != nullptr && card->isAncestorOf(at))) {
                continue;
            }
            // Its effective visibility: a hidden container around it, or a tab page not selected, hides it too.
            _watching.push_back(QObject::connect(at, &QQuickItem::visibleChanged, _filter.get(), [this] {
                resolve();
                track();
            }));
            if (found == nullptr && at->isVisible()) {
                found = candidate;
            }
        }
    }
    bool const was = _ringed != nullptr;
    _ringed = found;
    write("ringed", found != nullptr);
    _resolving = false;
    if ((found != nullptr) != was && _onChanged) {
        auto const handler = _onChanged;
        handler(found != nullptr);
    }
}

void SpotlightImpl::track() {
    if (_ringed == nullptr || _ringed->item() == nullptr) {
        return;
    }
    QQuickItem& target = *_ringed->item();
    scrollIntoView(target);
    write("cutout", target.mapRectToScene(QRectF{0, 0, target.width(), target.height()}));
}

std::vector<QQuickItem*> SpotlightImpl::tabChain() const {
    std::vector<QQuickItem*> chain;
    if (auto* const content = qobject_cast<QQuickItem*>(part("content"))) {
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
    auto const at = std::ranges::find_if(chain, [focused](QQuickItem const* item) {
        return focused != nullptr && (item == focused || item->isAncestorOf(focused));
    });
    std::size_t const count = chain.size();
    std::size_t next = forward ? 0 : count - 1;
    if (at != chain.end()) {
        auto const index = static_cast<std::size_t>(std::distance(chain.begin(), at));
        next = forward ? (index + 1) % count : (index + count - 1) % count;
    }
    chain.at(next)->forceActiveFocus(forward ? Qt::TabFocusReason : Qt::BacktabFocusReason);
}

bool SpotlightImpl::filterKey(QKeyEvent const& event) {
    auto* const content = qobject_cast<QQuickItem*>(part("content"));
    QQuickItem* const focused = _window != nullptr ? _window->activeFocusItem() : nullptr;
    if (content == nullptr || focused == nullptr) {
        return false;
    }
    bool const inside = content->isAncestorOf(focused);
    int const key = event.key();
    if (key == Qt::Key_Tab || key == Qt::Key_Backtab) {
        QQuickItem const* const target = _ringed != nullptr ? _ringed->item() : nullptr;
        bool const onTarget = target != nullptr && (target == focused || target->isAncestorOf(focused));
        if (!inside && !onTarget) {
            return false;
        }
        cycleFocus(key == Qt::Key_Tab && !event.modifiers().testFlag(Qt::ShiftModifier));
        return true;
    }
    if (!inside || key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Space) {
        return false;
    }
    for (QQuickItem* at = focused; at != nullptr && at != content->parentItem(); at = at->parentItem()) {
        if (at->metaObject()->indexOfMethod("takeChord(int,int)") < 0) {
            continue;
        }
        bool taken = false;
        QMetaObject::invokeMethod(at, "takeChord", Qt::DirectConnection, qReturnArg(taken), key,
                                  static_cast<int>(event.modifiers().toInt()));
        if (taken) {
            return true;
        }
    }
    return false;
}

}  // namespace morph::qt_quick::detail
