// SPDX-License-Identifier: Apache-2.0

#include "qt_quick/context.hpp"

#include <QMetaObject>
#include <QObject>
#include <QQmlContext>
#include <QQmlError>
#include <QUrl>
#include <QVariant>
#include <functional>
#include <iostream>
#include <memory>
#include <morph/qt_quick/components.hpp>
#include <morph/ui/backend.hpp>
#include <stdexcept>
#include <string>
#include <utility>

#include "qt_quick/keys.hpp"
#include "qt_quick/relay.hpp"
#include "qt_quick/widget.hpp"

namespace morph::qt_quick::detail {

namespace {

/// The `Theme` singleton's object property @p name in @p engine, or null.
QObject* themeObjectOf(QQmlEngine& engine, char const* name) {
    QObject const* const theme =
        engine.singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    return theme != nullptr ? theme->property(name).value<QObject*>() : nullptr;
}

/// The backend's roots, top to bottom in the column the `MorphRoots` item holds.
class Roots final : public ContainerCore {
public:
    explicit Roots(QQuickItem* column) : _column{column} {}

protected:
    QQuickItem* contentFor(ItemBase& child) override {
        static_cast<void>(child);
        return _column;
    }
    // The roots draw with the theme's own scheme.
    [[nodiscard]] QObject* scheme() const override {
        QQmlEngine* const engine = _column != nullptr ? qmlEngine(_column) : nullptr;
        return engine != nullptr ? themeObjectOf(*engine, "themeScheme") : nullptr;
    }

private:
    QPointer<QQuickItem> _column;
};

QString errorsOf(QQmlComponent const& component) {
    QString text;
    for (QQmlError const& error : component.errors()) {
        text += error.toString() + QLatin1Char('\n');
    }
    return text;
}

}  // namespace

Context::Context(QQmlEngine& engine, QQuickItem& host, BackendOptions options)
    : _engine{&engine}, _options{std::move(options)} {
    QQmlComponent& roots = component(QStringLiteral("MorphRoots"));
    auto relay = std::make_unique<Relay>();
    std::unique_ptr<QObject> made{
        roots.createWithInitialProperties({{QStringLiteral("host"), QVariant::fromValue<QObject*>(relay.get())}})};
    auto* const holder = qobject_cast<QQuickItem*>(made.get());
    if (holder == nullptr) {
        throw std::runtime_error{"morph::qt_quick: the MorphRoots component did not make an item"};
    }
    // The context deletes the holder; until then the host item is its QObject parent too.
    _holder = qobject_cast<QQuickItem*>(made.release());
    QQmlEngine::setObjectOwnership(_holder, QQmlEngine::CppOwnership);
    _holder->setParentItem(&host);
    _holder->setParent(&host);
    _roots = std::make_unique<Roots>(qobject_cast<QQuickItem*>(_holder->property("content").value<QObject*>()));
    // The roots item reports its width in units, which follows both the host's width and the theme's font.
    relay->setParent(_holder);
    _rootsRelay = relay.release();
    _rootsRelay->on(QStringLiteral("widthUnits"), [this](QVariant const& units) { widthChanged(units.toDouble()); });
    widthChanged(_holder->property("widthUnits").toDouble());
}

Context::~Context() {
    if (_rootsRelay != nullptr) {
        _rootsRelay->kill();
    }
    _roots.reset();
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory): the holder is the context's, made in its constructor.
    delete _holder.data();
}

QObject* Context::themeObject(char const* name) const { return themeObjectOf(*_engine, name); }

void Context::widthChanged(double units) {
    std::string next = ui::widthClassOf(units);
    if (next == _widthClass) {
        return;
    }
    _widthClass = std::move(next);
    // Controls are touch targets while the window is compact. The theme is one per engine, so the last class
    // written holds for every backend in it.
    if (auto* const theme = _engine->singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"))) {
        theme->setProperty("touchTargets", _widthClass == "compact");
    }
    if (_onWidthClass) {
        // A copy: the listener re-renders, which may replace it.
        auto const listener = _onWidthClass;
        listener(_widthClass);
    }
}

QQmlComponent& Context::component(QString const& type) {
    auto const found = _components.find(type);
    if (found != _components.end()) {
        return *found->second;
    }
    std::unique_ptr<QQmlComponent> made;
    if (_options.components != nullptr) {
        made = _options.components->restyled(*_engine, toUtf8(type));
    }
    if (made == nullptr) {
        made = std::make_unique<QQmlComponent>(_engine);
        made->loadFromModule(QStringLiteral("MorphUi"), type, QQmlComponent::PreferSynchronous);
    }
    if (made->isError()) {
        throw std::runtime_error{"morph::qt_quick: " + toUtf8(type) + " does not compile:\n" +
                                 toUtf8(errorsOf(*made))};
    }
    return *_components.emplace(type, std::move(made)).first->second;
}

void Context::undeclared(QObject const& item, char const* name) {
    // A QML type's class name carries the engine's suffix (`MorphButton_QMLTYPE_3`); the type is what precedes it.
    std::string type = item.metaObject()->className();
    if (auto const suffix = type.find("_QML"); suffix != std::string::npos) {
        type.resize(suffix);
    }
    if (!_undeclared.emplace(type, name).second) {
        return;
    }
    report(type, type + " does not declare the property `" + name + "`; the write is refused");
}

void Context::checkIcon(std::string_view icon) {
    if (icon.empty() || _missingIcons.contains(icon)) {
        return;
    }
    QObject const* const theme =
        _engine->singletonInstance<QObject*>(QStringLiteral("MorphUi"), QStringLiteral("Theme"));
    if (theme == nullptr || !theme->property("iconSet").toBool() ||
        theme->property("icons").toMap().contains(toQString(icon))) {
        return;
    }
    _missingIcons.emplace(icon);
    report(std::string{icon}, "icon " + std::string{icon} + " is not in the theme's icon set");
}

void Context::report(std::string name, std::string reason) const {
    if (_options.components != nullptr) {
        _options.components->report(ComponentFailure{.name = std::move(name), .reason = std::move(reason)});
        return;
    }
    std::cerr << "morph::qt_quick: " << reason << '\n';
}

Context::Made Context::make(QString const& type, QVariantMap initial) {
    QQmlComponent& made = component(type);
    auto relay = std::make_unique<Relay>();
    initial.insert(QStringLiteral("host"), QVariant::fromValue<QObject*>(relay.get()));
    std::unique_ptr<QObject> object{made.createWithInitialProperties(initial)};
    auto* const item = qobject_cast<QQuickItem*>(object.get());
    if (item == nullptr) {
        throw std::runtime_error{"morph::qt_quick: " + toUtf8(type) + " did not make an item:\n" +
                                 toUtf8(errorsOf(made))};
    }
    // From here Qt's parents own both: the relay is the item's child, the item the holder's until its widget
    // deletes it.
    relay->setParent(item);
    item->setParent(_holder);
    QQmlEngine::setObjectOwnership(object.release(), QQmlEngine::CppOwnership);
    return Made{.item = item, .relay = relay.release()};
}

}  // namespace morph::qt_quick::detail
