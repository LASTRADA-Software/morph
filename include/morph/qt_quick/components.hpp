// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class QQmlComponent;
class QQmlEngine;
class QUrl;

/// @file
/// @brief `morph::qt_quick::Components`: the application's own QML components, by name, and the restyle map.
///
/// Specified in `docs/spec/qt_quick/backend.md`, "Components".

namespace morph::qt_quick {

/// @brief Why a component could not be loaded; reported once per component and reason.
struct ComponentFailure {
    /// @brief The component's name, or the kind a restyle names.
    std::string name;
    /// @brief What went wrong, for a person to read.
    std::string reason;

    /// @brief Memberwise equality.
    /// @return Whether name and reason match.
    bool operator==(ComponentFailure const&) const = default;
};

/// @brief The QML components a client renders beside the built-in kinds (spec 5 §10).
///
/// - A **host component** is QML the client carries, registered by name and version. It loads into the backend's
///   own engine, so it may import the application's other modules.
/// - A **bundle component** is QML text delivered in the application's bundle and named by its SHA-256 digest. It
///   loads only when the text matches the digest, and compiles in a dedicated engine whose root context holds
///   nothing, so it sees nothing but what is set on it.
/// - The **restyle map** names a component for a built-in kind. The component must declare every property and
///   function of the kind's `MorphUi` component; one that does not is refused when it is mapped.
///
/// Each component reports its events through `host.fire(event, payload)`, `host` being a `var` property it declares
/// and the renderer sets. A component that fails to load is reported once, through the callback, and the caller
/// renders its fallback. Neither copyable nor movable; it outlives every backend that uses it.
class Components {
public:
    /// @brief An empty set.
    /// @param report Called once for each component that fails to load, or restyle that is refused; may be empty.
    explicit Components(std::function<void(ComponentFailure const&)> report = {});
    /// @brief Releases the compiled components and the contained engine.
    ~Components();
    Components(Components const&) = delete;
    Components& operator=(Components const&) = delete;
    Components(Components&&) = delete;
    Components& operator=(Components&&) = delete;

    /// @brief Registers a host component, replacing one of the same name.
    /// @param name    The name a document uses.
    /// @param version The version the client carries; a document asking for a later one gets the fallback.
    /// @param source  The QML file, such as a `qrc:` URL.
    void addHost(std::string name, int version, QUrl const& source);

    /// @brief Registers a bundle component, replacing one of the same name.
    /// @param name   The name a document uses.
    /// @param qml    The QML text, as the bundle delivered it.
    /// @param digest Its SHA-256, as the bundle names it: `sha256:` followed by 64 hexadecimal digits.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a name, its text and its digest, as the bundle lists them.
    void addBundle(std::string name, std::string qml, std::string digest);

    /// @brief Maps a built-in kind to a registered component, checking that the component declares every property
    ///        and function of the kind's `MorphUi` component.
    /// @param engine    The engine the backend renders with. Borrowed for the call.
    /// @param kind      The kind as a document names it: `button`, `textInput`, `checkbox`, `select`, …
    /// @param component The registered component's name.
    /// @return Nothing, or why the mapping is refused; a refusal is also reported, and the kind keeps its
    ///         `MorphUi` component.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a kind, then what restyles it, as the bundle maps them.
    [[nodiscard]] std::expected<void, ComponentFailure> restyle(QQmlEngine& engine, std::string const& kind,
                                                                std::string const& component);

    /// @brief The compiled component a restyle maps a `MorphUi` type to, for a backend to instantiate.
    /// @param engine The backend's engine. Borrowed for the call.
    /// @param type   The `MorphUi` type, such as `MorphButton`.
    /// @return The component, or null when the type is not restyled.
    [[nodiscard]] std::unique_ptr<QQmlComponent> restyled(QQmlEngine& engine, std::string const& type);

    /// @brief Compiles a registered component, or says why it cannot be used.
    /// @param engine     The backend's engine, which host components load into. Borrowed for the call.
    /// @param name       The component's name.
    /// @param minVersion The lowest host component version that will do.
    /// @return The compiled component, owned by this set, or the failure, which is also reported once.
    [[nodiscard]] std::expected<QQmlComponent*, ComponentFailure> load(QQmlEngine& engine, std::string const& name,
                                                                       int minVersion = 0);

    /// @brief Reports a failure, once per name and reason.
    /// @param failure The failure.
    void report(ComponentFailure const& failure);

    /// @brief Every failure reported so far, oldest first.
    /// @return They.
    [[nodiscard]] std::vector<ComponentFailure> const& failures() const noexcept;

private:
    struct State;
    std::unique_ptr<State> _state;
};

}  // namespace morph::qt_quick
