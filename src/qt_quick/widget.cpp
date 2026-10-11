// SPDX-License-Identifier: Apache-2.0

#include "qt_quick/widget.hpp"

#include <QAccessible>
#include <QMetaObject>
#include <QStringList>
#include <QVariantList>
#include <algorithm>
#include <array>
#include <iterator>
#include <utility>

#include "qt_quick/keys.hpp"

namespace morph::qt_quick::detail {

QStringList toStringList(std::vector<std::string> const& texts) {
    QStringList list;
    list.reserve(static_cast<qsizetype>(texts.size()));
    for (std::string const& text : texts) {
        list.append(toQString(text));
    }
    return list;
}

ItemBase* baseOf(ui::Widget const& widget) noexcept {
    return dynamic_cast<ItemBase*>(const_cast<ui::Widget*>(&widget));  // NOLINT(cppcoreguidelines-pro-type-const-cast)
}

ContainerCore* containerOf(ui::ContainerWidget* widget) noexcept { return dynamic_cast<ContainerCore*>(widget); }

ItemBase::ItemBase(Context& context, QString const& type, ui::ContainerWidget* parent, QVariantMap initial)
    : ItemBase{context, context.make(type, std::move(initial)), parent} {}

ItemBase::ItemBase(Context& context, Context::Made made, ui::ContainerWidget* parent)
    : _context{&context}, _relay{made.relay} {
    _item = made.item;
    ContainerCore* const container = parent != nullptr ? containerOf(parent) : &context.roots();
    if (container != nullptr) {
        _container = container;
        container->adopt(*this);
    }
}

ItemBase::~ItemBase() {
    _relay->kill();
    if (_container != nullptr) {
        _container->release(*this);
    }
    if (QQuickItem* const item = _item) {
        item->setVisible(false);
        item->setParentItem(nullptr);
        item->deleteLater();
    }
}

void ItemBase::detachedFrom(ContainerCore const& container) noexcept {
    if (_container == &container) {
        _container = nullptr;
    }
}

QVariant ItemBase::read(char const* name) const { return _item != nullptr ? _item->property(name) : QVariant{}; }

void ItemBase::write(char const* name, QVariant const& value) const {
    if (_item == nullptr) {
        return;
    }
    if (_item->property(name) != value) {
        _item->setProperty(name, value);
    }
}

QObject* ItemBase::part(char const* name) const {
    return _item != nullptr ? _item->property(name).value<QObject*>() : nullptr;
}

void ItemBase::showVisible(bool visible) {
    _visible = visible;
    if (_item != nullptr) {
        _item->setVisible(visible);
    }
}

void ItemBase::showEnabled(bool enabled) {
    _enabled = enabled;
    if (_item != nullptr) {
        _item->setEnabled(enabled);
    }
}

void ItemBase::showLayout(ui::LayoutHints const& hints) const {
    write("widthKind", static_cast<int>(hints.width.kind));
    write("widthAmount", hints.width.amount);
    write("heightKind", static_cast<int>(hints.height.kind));
    write("heightAmount", hints.height.amount);
}

void ItemBase::showDragKey(std::optional<ui::Key> const& key) const {
    write("dragKey", key ? encodeKey(*key) : QString{});
}

void ItemBase::takeDrops(std::function<bool(ui::Key const&)> accepts, std::function<void(ui::Key)> onDrop) const {
    write("dropEnabled", static_cast<bool>(onDrop));
    _relay->answer(QStringLiteral("accepts"), [accepts](QVariant const& payload) {
        std::optional<ui::Key> const key = decodeKey(payload.toString());
        return key.has_value() && accepts && accepts(*key);
    });
    _relay->on(QStringLiteral("drop"), [accepts, onDrop = std::move(onDrop)](QVariant const& payload) {
        std::optional<ui::Key> const key = decodeKey(payload.toString());
        if (key && accepts && accepts(*key) && onDrop) {
            onDrop(*key);
        }
    });
}

void ItemBase::showAccessibleName(std::string_view name) const { write("a11yName", toQString(name)); }

namespace {

struct RoleName {
    std::string_view name;
    QAccessible::Role role;
};

constexpr std::array<RoleName, 16> kRoles = {{{"button", QAccessible::Button},
                                              {"checkbox", QAccessible::CheckBox},
                                              {"dialog", QAccessible::Dialog},
                                              {"heading", QAccessible::Heading},
                                              {"link", QAccessible::Link},
                                              {"list", QAccessible::List},
                                              {"listitem", QAccessible::ListItem},
                                              {"menu", QAccessible::PopupMenu},
                                              {"progressbar", QAccessible::ProgressBar},
                                              {"radio", QAccessible::RadioButton},
                                              {"region", QAccessible::Section},
                                              {"slider", QAccessible::Slider},
                                              {"status", QAccessible::StatusBar},
                                              {"tab", QAccessible::PageTab},
                                              {"table", QAccessible::Table},
                                              {"textbox", QAccessible::EditableText}}};

}  // namespace

void ItemBase::showAccessibleRole(std::string_view role) const {
    auto const found = std::ranges::find(kRoles, role, &RoleName::name);
    write("a11yRole", found != kRoles.end() ? static_cast<int>(found->role) : 0);
}

void ItemBase::showTestId(std::string_view testId) const {
    if (_item != nullptr) {
        _item->setObjectName(toQString(testId));
    }
}

void ItemBase::showTooltip(std::string_view text) const { write("tooltip", toQString(text)); }

void ItemBase::showSurface(std::string_view surface) const { write("surface", toQString(surface)); }

void ItemBase::takeChords(std::vector<std::string> const& chords, std::function<void(std::string)> onChord) const {
    write("chords", toStringList(chords));
    _relay->answer(QStringLiteral("chord"), [this, chords, onChord = std::move(onChord)](QVariant const& payload) {
        QVariantList const pressed = payload.toList();
        if (pressed.size() != 2 || !reachable()) {
            return false;
        }
        int const key = pressed.at(0).toInt();
        auto const modifiers = Qt::KeyboardModifiers::fromInt(pressed.at(1).toUInt());
        auto const found = std::ranges::find_if(
            chords, [&](std::string const& chord) { return chordMatches(chord, key, modifiers); });
        if (found == chords.end()) {
            return false;
        }
        if (onChord) {
            onChord(*found);
        }
        return true;
    });
}

QQuickItem* ItemBase::focusTarget() const {
    auto* const target = qobject_cast<QQuickItem*>(part("focusTarget"));
    return target != nullptr ? target : _item.data();
}

void ItemBase::takeFocus() const {
    // A widget the user cannot reach takes no focus: it would take keys a hidden or disabled widget must not.
    if (!reachable()) {
        return;
    }
    if (QQuickItem* const target = focusTarget()) {
        target->forceActiveFocus(Qt::OtherFocusReason);
    }
}

bool ItemBase::reachable() const {
    return _item != nullptr && _item->window() != nullptr && _item->isVisible() && _item->isEnabled();
}

ContainerCore::~ContainerCore() {
    for (ItemBase* const child : _children) {
        child->detachedFrom(*this);
    }
}

void ContainerCore::adopt(ItemBase& child) {
    QQuickItem* const content = contentFor(child);
    if (QQuickItem* const item = child.item(); item != nullptr && content != nullptr) {
        item->setParentItem(content);
    }
    _children.push_back(&child);
    adopted(child);
}

void ContainerCore::release(ItemBase& child) noexcept {
    auto const found = std::ranges::find(_children, &child);
    if (found == _children.end()) {
        return;
    }
    releasing(child);
    _children.erase(found);
}

void ContainerCore::move(ItemBase& child, std::size_t index) {
    auto const found = std::ranges::find(_children, &child);
    if (found == _children.end()) {
        return;
    }
    auto const from = static_cast<std::size_t>(std::distance(_children.begin(), found));
    _children.erase(found);
    // `index` counts the children without `child`, as the contract's "the child now at index" does.
    std::size_t const to = std::min(index, _children.size());
    _children.insert(std::next(_children.begin(), static_cast<std::ptrdiff_t>(to)), &child);
    if (to == from) {
        return;
    }
    reorder(child, from, to);
    ++_restacks;
    moved();
}

void ContainerCore::reorder(ItemBase& child, std::size_t from, std::size_t to) {
    static_cast<void>(from);
    QQuickItem* const item = child.item();
    if (item == nullptr) {
        return;
    }
    if (to + 1 < _children.size()) {
        if (QQuickItem* const next = _children.at(to + 1)->item()) {
            item->stackBefore(next);
        }
    } else if (to > 0) {
        if (QQuickItem* const previous = _children.at(to - 1)->item()) {
            item->stackAfter(previous);
        }
    }
}

}  // namespace morph::qt_quick::detail
