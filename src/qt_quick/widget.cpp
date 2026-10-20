// SPDX-License-Identifier: Apache-2.0

#include "qt_quick/widget.hpp"

#include <QAccessible>
#include <QMetaObject>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlProperty>
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

namespace {

// Every live widget, in creation order, for tests and tools that find a widget by its test id.
std::vector<ItemBase*>& registry() {
    static std::vector<ItemBase*> widgets;
    return widgets;
}

}  // namespace

std::vector<ItemBase*> const& liveWidgets() noexcept { return registry(); }

ItemBase* findWidget(QString const& testId) noexcept {
    auto const found = std::ranges::find_if(registry(), [&testId](ItemBase const* widget) {
        return widget->item() != nullptr && widget->item()->objectName() == testId;
    });
    return found != registry().end() ? *found : nullptr;
}

ItemBase::ItemBase(Context& context, Context::Made made, ui::ContainerWidget* parent)
    : _context{&context}, _item{made.item}, _relay{made.relay} {
    registry().push_back(this);
    ContainerCore* const container = parent != nullptr ? containerOf(parent) : &context.roots();
    if (container != nullptr) {
        _container = container;
        container->adopt(*this);
    }
}

ItemBase::~ItemBase() {
    _relay->kill();
    std::erase(registry(), this);
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

QVariant ItemBase::read(char const* name) const {
    if (_item == nullptr) {
        return {};
    }
    return _item->property(name);
}

void ItemBase::write(char const* name, QVariant const& value) const {
    if (_item == nullptr) {
        return;
    }
    // A name the item does not declare would become a dynamic property no binding reads: refused and reported.
    if (_item->metaObject()->indexOfProperty(name) < 0) {
        _context->undeclared(*_item, name);
        return;
    }
    if (_item->property(name) != value) {
        _item->setProperty(name, value);
    }
}

void ItemBase::offer(char const* name, QVariant const& value) const {
    if (_item != nullptr && _item->metaObject()->indexOfProperty(name) >= 0) {
        write(name, value);
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
    // A bound or an alignment given must reach the item, and one it cannot take is reported; an absent one is
    // offered, so a component that declares neither is not reported for a node that has none.
    if (hints.alignSelf) {
        write("alignSelf", static_cast<int>(*hints.alignSelf));
    } else {
        offer("alignSelf", -1);
    }
    for (auto const& [name, bound] :
         {std::pair{"minWidthUnits", hints.minWidth}, std::pair{"maxWidthUnits", hints.maxWidth},
          std::pair{"minHeightUnits", hints.minHeight}, std::pair{"maxHeightUnits", hints.maxHeight}}) {
        if (bound) {
            write(name, *bound);
        } else {
            offer(name, -1);
        }
    }
}

void ItemBase::showDragKey(std::optional<ui::Key> const& key) const {
    QString text;
    if (key) {
        text = encodeKey(*key);
    }
    write("dragKey", text);
}

void ItemBase::takeDrops(std::function<bool(ui::Key const&)> accepts, std::function<void(ui::Key)> onDrop) const {
    write("dropEnabled", static_cast<bool>(onDrop));
    _relay->answer(QStringLiteral("accepts"), [accepts](QVariant const& payload) {
        std::optional<ui::Key> const key = decodeKey(payload.toString());
        return key.has_value() && accepts && accepts(*key);
    });
    _relay->on(QStringLiteral("drop"),
               [accepts = std::move(accepts), onDrop = std::move(onDrop)](QVariant const& payload) {
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

// NOLINTBEGIN(modernize-use-designated-initializers): a table of positional pairs, name beside role, reads as one.
constexpr std::array<RoleName, 20> kRoles = {
    {{"button", QAccessible::Button},     {"chart", QAccessible::Chart},
     {"checkbox", QAccessible::CheckBox}, {"dialog", QAccessible::Dialog},
     {"edit", QAccessible::EditableText}, {"group", QAccessible::Grouping},
     {"heading", QAccessible::Heading},   {"link", QAccessible::Link},
     {"list", QAccessible::List},         {"listitem", QAccessible::ListItem},
     {"menu", QAccessible::PopupMenu},    {"progressbar", QAccessible::ProgressBar},
     {"radio", QAccessible::RadioButton}, {"region", QAccessible::Section},
     {"slider", QAccessible::Slider},     {"status", QAccessible::StatusBar},
     {"tab", QAccessible::PageTab},       {"table", QAccessible::Table},
     {"text", QAccessible::StaticText},   {"textbox", QAccessible::EditableText}}};
// NOLINTEND(modernize-use-designated-initializers)

}  // namespace

void ItemBase::showAccessibleRole(std::string_view role) const {
    auto const index = static_cast<std::size_t>(
        std::ranges::distance(kRoles.begin(), std::ranges::find(kRoles, role, &RoleName::name)));
    int value = 0;
    if (index < kRoles.size()) {
        value = static_cast<int>(kRoles.at(index).role);
    }
    write("a11yRole", value);
}

void ItemBase::showTestId(std::string_view testId) const {
    if (_item == nullptr) {
        return;
    }
    QString const identifier = toQString(testId);
    // The root keeps the id, which `findWidget` matches; an input's accessible node is its control, so the control
    // carries the id too, where a driver walking the accessibility tree finds it.
    auto const identify = [&identifier](QQuickItem& target) {
        target.setObjectName(identifier);
        // `Accessible.id` is the automation id a platform driver reads; a Qt without the property resolves no such
        // property, and the objectName alone identifies the item there.
        QQmlProperty const accessibleId{&target, QStringLiteral("Accessible.id"), qmlContext(&target)};
        if (accessibleId.isValid()) {
            static_cast<void>(accessibleId.write(identifier));
        }
    };
    identify(*_item);
    if (auto* const node = qobject_cast<QQuickItem*>(part("accessibleItem")); node != nullptr && node != _item) {
        identify(*node);
    }
}

void ItemBase::showTooltip(std::string_view text) const { write("tooltip", toQString(text)); }

void ItemBase::showSurface(std::string_view surface) const {
    write("surface", toQString(surface));
    if (auto const* const container = dynamic_cast<ContainerCore const*>(this)) {
        container->passSchemeDown();
    }
}

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

void ItemBase::takeHover(std::function<void()> onHover) const {
    write("hoverWatched", static_cast<bool>(onHover));
    _relay->answer(QStringLiteral("hover"), [this, onHover = std::move(onHover)](QVariant const&) {
        if (!onHover || !reachable()) {
            return false;
        }
        onHover();
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
    // Every MorphUi item declares these; a custom component takes part in the fill and the scheme only if it does.
    CrossFill const fill = crossFill();
    child.offer("fillsWidth", fill.width);
    child.offer("fillsHeight", fill.height);
    child.offer("alignAcross", alignsAcross());
    child.offer("crossAlign", _crossAlign);
    if (QObject* const inherited = scheme()) {
        child.offer("inheritedScheme", QVariant::fromValue(inherited));
    }
    _children.push_back(&child);
    adopted(child);
}

void ContainerCore::alignChildren(ui::Align align) {
    _crossAlign = static_cast<int>(align);
    for (ItemBase const* const child : _children) {
        child->offer("crossAlign", _crossAlign);
    }
}

// NOLINTNEXTLINE(misc-no-recursion): a container's children may be containers, which pass it on in turn.
void ContainerCore::passSchemeDown() const {
    QObject* const inherited = scheme();
    if (inherited == nullptr) {
        return;
    }
    for (ItemBase const* const child : _children) {
        child->offer("inheritedScheme", QVariant::fromValue(inherited));
        auto const* const nested = dynamic_cast<ContainerCore const*>(child);
        if (nested != nullptr && child->read("surface").toString().isEmpty()) {
            nested->passSchemeDown();
        }
    }
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
    auto const origin = static_cast<std::size_t>(std::distance(_children.begin(), found));
    _children.erase(found);
    // `index` counts the children without `child`, as the contract's "the child now at index" does.
    std::size_t const destination = std::min(index, _children.size());
    _children.insert(std::next(_children.begin(), static_cast<std::ptrdiff_t>(destination)), &child);
    if (destination == origin) {
        return;
    }
    reorder(child, origin, destination);
    ++_restacks;
    moved();
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a move's two places, in the order moveChild gives them.
void ContainerCore::reorder(ItemBase& child, std::size_t origin, std::size_t destination) {
    static_cast<void>(origin);
    QQuickItem* const item = child.item();
    if (item == nullptr) {
        return;
    }
    if (destination + 1 < _children.size()) {
        if (QQuickItem const* const next = _children.at(destination + 1)->item()) {
            item->stackBefore(next);
        }
    } else if (destination > 0) {
        if (QQuickItem const* const previous = _children.at(destination - 1)->item()) {
            item->stackAfter(previous);
        }
    }
}

}  // namespace morph::qt_quick::detail
