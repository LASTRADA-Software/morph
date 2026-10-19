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
#include <string>

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

    /// The components and the restyle map, or null.
    [[nodiscard]] Components* components() const noexcept { return _options.components; }

    /// The item every widget's item is owned by, as a QObject, until the widget deletes it.
    [[nodiscard]] QObject* owner() const noexcept { return _holder; }

    /// The component of the `MorphUi` type @p type, or of the restyle the map names for it, compiled once per
    /// context: what `make` instantiates, and what a container is given for a part it instantiates itself (a table
    /// row's background). Throws `std::runtime_error` when it does not compile.
    [[nodiscard]] QQmlComponent& component(QString const& type);

private:
    void widthChanged();

    QQmlEngine* _engine;
    BackendOptions _options;
    QPointer<QQuickItem> _holder;
    std::unique_ptr<ContainerCore> _roots;
    std::map<QString, std::unique_ptr<QQmlComponent>> _components;
    std::string _widthClass;
};

}  // namespace morph::qt_quick::detail
