// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <QPointer>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QString>
#include <QVariantMap>
#include <functional>
#include <map>
#include <memory>
#include <morph/qt_quick/backend.hpp>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace morph::qt_quick::detail {

class ContainerCore;
class Relay;

/// What every widget of one backend shares: the engine, the compiled components, the column the roots go in, and
/// the restyle map.
class Context {
public:
    Context(QQmlEngine& engine, QQuickItem& host, BackendOptions options);
    ~Context();
    Context(Context const&) = delete;
    Context& operator=(Context const&) = delete;
    Context(Context&&) = delete;
    Context& operator=(Context&&) = delete;

    /// The item a widget made by `make` shows, and the relay its QML reports through.
    struct Made {
        QQuickItem* item = nullptr;
        Relay* relay = nullptr;
    };

    /// Makes an item of the `MorphUi` type @p type (`"MorphButton"`), or of the component the restyle map names for
    /// it, with @p initial set before its bindings first run and `host` set to a new relay. It is shown in no
    /// container yet. Throws `std::runtime_error` when the component does not compile.
    [[nodiscard]] Made make(QString const& type, QVariantMap initial = {});

    /// The container backend roots are appended to.
    [[nodiscard]] ContainerCore& roots() const noexcept { return *_roots; }

    /// The engine.
    [[nodiscard]] QQmlEngine& engine() const noexcept { return *_engine; }

    /// The engine's `Theme` singleton's object property @p name (`"cardScheme"`), or null when there is none.
    [[nodiscard]] QObject* themeObject(char const* name) const;

    /// The components and the restyle map, or null.
    [[nodiscard]] Components* components() const noexcept { return _options.components; }

    /// The item every widget's item is owned by, as a QObject, until the widget deletes it.
    [[nodiscard]] QObject* owner() const noexcept { return _holder; }

    /// The component of the `MorphUi` type @p type, or of the restyle the map names for it, compiled once per
    /// context: what `make` instantiates, and what a container is given for a part it instantiates itself (a table
    /// row's background). Throws `std::runtime_error` when it does not compile.
    [[nodiscard]] QQmlComponent& component(QString const& type);

    /// Reports, once per QML type and property, that C++ wrote @p name to @p item, which does not declare it: through
    /// the components' failure callback, or on `std::cerr` without components. Not thrown: a write runs from Qt
    /// signal handlers and from a table's restriping while a row is destroyed, where an exception cannot pass.
    void undeclared(QObject const& item, char const* name);

    /// The host item's width class: its width in `Theme.unitWidth`s, classed by `ui::widthClassOf`.
    [[nodiscard]] std::string const& widthClass() const noexcept { return _widthClass; }

    /// Registers the one listener told of each later change of `widthClass()`; empty clears it.
    void setOnWidthClass(std::function<void(std::string const&)> onWidthClass) {
        _onWidthClass = std::move(onWidthClass);
    }

    /// Reports, once per name, a node's icon that the bundle's declared icon set does not name; it is drawn as text.
    /// Without a declared set nothing is reported: every icon is drawn as text or as MorphUi's own.
    void checkIcon(std::string_view icon);

private:
    // The host's width in units changed: re-classes it, and tells the listener of a new class.
    void widthChanged(double units);
    // A problem of a widget: through the components' failure callback, or on `std::cerr` without components.
    void report(std::string name, std::string reason) const;

    QQmlEngine* _engine;
    BackendOptions _options;
    QPointer<QQuickItem> _holder;
    std::unique_ptr<ContainerCore> _roots;
    std::map<QString, std::unique_ptr<QQmlComponent>> _components;
    std::string _widthClass;
    std::function<void(std::string const&)> _onWidthClass;
    // What the roots item reports its width in units through.
    Relay* _rootsRelay = nullptr;
    std::set<std::pair<std::string, std::string>> _undeclared;
    std::set<std::string, std::less<>> _missingIcons;
};

}  // namespace morph::qt_quick::detail
