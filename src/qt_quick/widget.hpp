// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <QMetaObject>
#include <QPointer>
#include <QQuickItem>
#include <QString>
#include <QVariant>
#include <QVariantMap>
#include <cstddef>
#include <functional>
#include <morph/ui/backend.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "qt_quick/context.hpp"
#include "qt_quick/keys.hpp"
#include "qt_quick/relay.hpp"

namespace morph::qt_quick::detail {

class ContainerCore;

/// The item behind one widget, and what every kind does with it: the common properties of `ui::Widget`, reading
/// and writing the item's properties, and leaving its container when it is destroyed.
///
/// The destructor kills the relay first, so no handler runs once it has started, then takes the item out of the
/// scene and deletes it later: QML may be running one of the item's own handlers.
class ItemBase {
public:
    /// Makes the item of @p type and appends it to @p parent, or to the backend's roots when it is null.
    ItemBase(Context& context, QString const& type, ui::ContainerWidget* parent, QVariantMap initial = {});
    /// Takes an item already made and appends it to @p parent, or to the backend's roots when it is null.
    ItemBase(Context& context, Context::Made made, ui::ContainerWidget* parent);
    virtual ~ItemBase();
    ItemBase(ItemBase const&) = delete;
    ItemBase& operator=(ItemBase const&) = delete;
    ItemBase(ItemBase&&) = delete;
    ItemBase& operator=(ItemBase&&) = delete;

    /// The item, or null once the backend or its container deleted it.
    [[nodiscard]] QQuickItem* item() const noexcept { return _item; }
    /// The relay.
    [[nodiscard]] Relay& relay() const noexcept { return *_relay; }
    /// The context.
    [[nodiscard]] Context& context() const noexcept { return *_context; }
    /// The container it is in, or null.
    [[nodiscard]] ContainerCore* container() const noexcept { return _container; }
    /// Called by the container when it lets go of this child.
    void detachedFrom(ContainerCore const& container) noexcept;

    /// A property of the item.
    [[nodiscard]] QVariant read(char const* name) const;
    /// Writes a property of the item unless it already holds @p value. A name the item does not declare is not
    /// written, and is reported through the context.
    void write(char const* name, QVariant const& value) const;
    /// As `write`, for a property the item takes part through only when it declares it (a custom component's
    /// placement and scheme): a name it does not declare is skipped, not reported.
    void offer(char const* name, QVariant const& value) const;
    /// The object a property of the item names (`control`, `content`), or null.
    [[nodiscard]] QObject* part(char const* name) const;
    /// Calls a QML function of the item with arguments of the types it declares (`bool`, `int`, `double` for
    /// `real`, `QString` for `string`, `QVariant` for `var`).
    template <class... Args>
    void call(char const* method, Args const&... arguments) const {
        if (_item != nullptr) {
            QMetaObject::invokeMethod(_item, method, Qt::DirectConnection, arguments...);
        }
    }
    /// Calls a QML function of the item that returns a `bool`.
    template <class... Args>
    [[nodiscard]] bool askBool(char const* method, Args const&... arguments) const {
        bool result = false;
        if (_item != nullptr) {
            QMetaObject::invokeMethod(_item, method, Qt::DirectConnection, qReturnArg(result), arguments...);
        }
        return result;
    }

    // The common properties of ui::Widget.
    void showVisible(bool visible);
    void showEnabled(bool enabled);
    void showLayout(ui::LayoutHints const& hints) const;
    void showDragKey(std::optional<ui::Key> const& key) const;
    void takeDrops(std::function<bool(ui::Key const&)> accepts, std::function<void(ui::Key)> onDrop) const;
    void showAccessibleName(std::string_view name) const;
    void showAccessibleRole(std::string_view role) const;
    void showTestId(std::string_view testId) const;
    void showTooltip(std::string_view text) const;
    void showSurface(std::string_view surface) const;
    void takeChords(std::vector<std::string> const& chords, std::function<void(std::string)> onChord) const;
    void takeHover(std::function<void()> onHover) const;
    void takeFocus() const;

    /// The widget's own visibility flag, as its last `setVisible` left it.
    [[nodiscard]] bool ownVisible() const noexcept { return _visible; }
    /// The widget's own enablement flag, as its last `setEnabled` left it.
    [[nodiscard]] bool ownEnabled() const noexcept { return _enabled; }
    /// Whether the user can reach it: shown and enabled, with every item around it.
    [[nodiscard]] bool reachable() const;
    /// The item keyboard focus goes to.
    [[nodiscard]] QQuickItem* focusTarget() const;

    /// What the widget shows as its text, for tests: a text's or a field's text, a button's or a check box's label,
    /// the label of the option a select marks.
    [[nodiscard]] virtual std::string shownText() const { return {}; }

private:
    Context* _context;
    QPointer<QQuickItem> _item;
    Relay* _relay;
    ContainerCore* _container = nullptr;
    bool _visible = true;
    bool _enabled = true;
};

/// The children of a container, in the order the mount keeps them, and the item they are shown in.
///
/// A child's item is appended to `contentFor(child)`; `move` applies `ui::ContainerWidget::moveChild` as one
/// stacking-order change, which the Qt Quick layouts follow, so a moved child keeps its item and its state.
/// Which way a container's content-sized children fill it: across its axis, as the backend contract asks.
struct CrossFill {
    bool width = false;   ///< A child whose width is content takes the container's width.
    bool height = false;  ///< A child whose height is content takes the container's height.
};

class ContainerCore {
public:
    ContainerCore() = default;
    virtual ~ContainerCore();
    ContainerCore(ContainerCore const&) = delete;
    ContainerCore& operator=(ContainerCore const&) = delete;
    ContainerCore(ContainerCore&&) = delete;
    ContainerCore& operator=(ContainerCore&&) = delete;

    /// Appends @p child as the last child.
    void adopt(ItemBase& child);
    /// Writes this container's scheme onto each child again, and on through the children that have no surface of
    /// their own: what a surface set after the children were adopted needs, as a frame built in code does.
    void passSchemeDown() const;
    /// Forgets @p child, which is being destroyed.
    void release(ItemBase& child) noexcept;
    /// Moves @p child before the child now at @p index; past the end means last.
    void move(ItemBase& child, std::size_t index);
    /// Places the children across this container's axis: writes `crossAlign` on every child it holds, and on each
    /// one it adopts later. Adoption runs in a child's constructor, so a container aligned after its children were
    /// made reaches them here.
    void alignChildren(ui::Align align);
    /// The children, in order.
    [[nodiscard]] std::vector<ItemBase*> const& children() const noexcept { return _children; }
    /// How many stacking-order changes `move` has made.
    [[nodiscard]] std::size_t restacks() const noexcept { return _restacks; }

protected:
    /// The item a new child's item is appended to; null leaves the placing to `adopted`.
    [[nodiscard]] virtual QQuickItem* contentFor(ItemBase& child) = 0;
    /// Shows @p child, now at @p destination in `children()`, in its new place: a stacking-order change by default.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a move's two places, in the order moveChild gives them.
    virtual void reorder(ItemBase& child, std::size_t origin, std::size_t destination);
    /// Which way a content-sized child fills this container; written on each child as it is adopted. A container that
    /// shows its children one above the other fills their width, which is the default.
    [[nodiscard]] virtual CrossFill crossFill() const noexcept { return CrossFill{.width = true, .height = false}; }
    /// The colour scheme this container's children inherit: its item's `scheme`, or null when it has none.
    [[nodiscard]] virtual QObject* scheme() const = 0;
    /// Which way this container's alignment places a child, written on each child as `alignAcross`: 0 not at all,
    /// 1 horizontally (a column's children), 2 vertically (a row's children, a grid cell's content in its row).
    [[nodiscard]] virtual int alignsAcross() const noexcept { return 0; }
    /// Called once a child was appended.
    virtual void adopted(ItemBase& child) { static_cast<void>(child); }
    /// Called once a child was moved.
    virtual void moved() {}
    /// Called before a child goes.
    virtual void releasing(ItemBase& child) noexcept { static_cast<void>(child); }

private:
    std::vector<ItemBase*> _children;
    std::size_t _restacks = 0;
    // The `ui::Align` the children are placed by, as its index.
    int _crossAlign = 0;
};

/// Every live widget of every backend in the process, in creation order.
[[nodiscard]] std::vector<ItemBase*> const& liveWidgets() noexcept;

/// The first live widget, in creation order, whose test id is @p testId; null when there is none.
[[nodiscard]] ItemBase* findWidget(QString const& testId) noexcept;

/// The `ItemBase` behind a widget of this backend, or null.
[[nodiscard]] ItemBase* baseOf(ui::Widget const& widget) noexcept;

/// The `ContainerCore` behind a container of this backend, or null.
[[nodiscard]] ContainerCore* containerOf(ui::ContainerWidget* widget) noexcept;

/// `ui::Widget`'s common setters, over `ItemBase`.
// NOLINTBEGIN(portability-template-virtual-member-function): these templates exist to implement the widget
// interfaces' virtual functions, and each is instantiated only as the base of a final widget class in kinds.hpp,
// whose virtual functions are all used through the interface; every compiler then instantiates all of them.
template <class Interface>
class ItemWidget : public Interface, public ItemBase {
public:
    ItemWidget(Context& context, QString const& type, ui::ContainerWidget* parent, QVariantMap initial = {})
        : ItemBase{context, type, parent, std::move(initial)} {}
    ItemWidget(Context& context, Context::Made made, ui::ContainerWidget* parent) : ItemBase{context, made, parent} {}

    void setVisible(bool visible) override { showVisible(visible); }
    void setEnabled(bool enabled) override { showEnabled(enabled); }
    void setLayout(ui::LayoutHints const& hints) override { showLayout(hints); }
    void setDragKey(std::optional<ui::Key> const& key) override { showDragKey(key); }
    void setDropHandler(std::function<bool(ui::Key const&)> accepts, std::function<void(ui::Key)> onDrop) override {
        takeDrops(std::move(accepts), std::move(onDrop));
    }
    void setAccessibleName(std::string_view name) override { showAccessibleName(name); }
    void setAccessibleRole(std::string_view role) override { showAccessibleRole(role); }
    void setTestId(std::string_view testId) override { showTestId(testId); }
    void setTooltip(std::string_view text) override { showTooltip(text); }
    void setSurface(std::string_view surface) override { showSurface(surface); }
    void setKeys(std::vector<std::string> const& chords, std::function<void(std::string)> onChord) override {
        takeChords(chords, std::move(onChord));
    }
    void focus() override { takeFocus(); }
    void setOnHover(std::function<void()> onHover) override { takeHover(std::move(onHover)); }
};

/// `ui::FieldWidget`'s setters, over the `MorphField` properties.
template <class Interface>
class FieldItemWidget : public ItemWidget<Interface> {
public:
    using ItemWidget<Interface>::ItemWidget;

    void setReadOnly(bool readonly) override { this->write("readOnly", readonly); }
    void setRequired(bool required) override { this->write("required", required); }
    void setErrors(std::vector<std::string> const& errors) override;
    void setStale(bool stale) override { this->write("stale", stale); }

protected:
    /// The field's label as `caption`, and `labelShown`: what `setCaption` writes for the kinds that have one.
    void showCaption(std::string_view label, bool shown) const {
        this->write("caption", toQString(label));
        this->write("labelShown", shown);
    }
};

/// The messages as a QML string list.
[[nodiscard]] QStringList toStringList(std::vector<std::string> const& texts);

template <class Interface>
void FieldItemWidget<Interface>::setErrors(std::vector<std::string> const& errors) {
    this->write("errors", toStringList(errors));
}

/// A container whose children go in the item its `content` property names.
template <class Interface>
class ContainerItemWidget : public ItemWidget<Interface>, public ContainerCore {
public:
    using ItemWidget<Interface>::ItemWidget;
    ~ContainerItemWidget() override = default;
    ContainerItemWidget(ContainerItemWidget const&) = delete;
    ContainerItemWidget& operator=(ContainerItemWidget const&) = delete;
    ContainerItemWidget(ContainerItemWidget&&) = delete;
    ContainerItemWidget& operator=(ContainerItemWidget&&) = delete;

    void moveChild(ui::Widget& child, std::size_t index) override {
        if (ItemBase* const base = baseOf(child)) {
            move(*base, index);
        }
    }

protected:
    [[nodiscard]] QQuickItem* contentFor(ItemBase& child) override {
        static_cast<void>(child);
        return qobject_cast<QQuickItem*>(this->part("content"));
    }
    [[nodiscard]] QObject* scheme() const override { return this->part("scheme"); }
};
// NOLINTEND(portability-template-virtual-member-function)

}  // namespace morph::qt_quick::detail
