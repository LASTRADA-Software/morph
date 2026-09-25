// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file
/// Model-agnostic core of a schema-driven Qt/QML bridge: owns (or composes
/// over) the Bridge/BridgeHandler/executor wiring and exposes one operation
/// -- dispatch an action by string id with a JSON body -- generically over
/// `BridgeHandler<Model, Sharing>::executeJson`, so an app depends on this
/// directly instead of re-deriving the wiring per model. A concrete
/// `QObject`/`QML_ELEMENT` wrapper per model (Qt cannot register a class
/// *template* for QML) forwards to this core and turns its callbacks into
/// signals -- see `morph::qt::forms::FormsControllerCore`, which specialises
/// this core for the shipped forms renderer, for the reference shape.
///
/// Deliberately does not carry a `schemasJson` document itself: that is a
/// forms-rendering concept (the `{actionType: schema}` document
/// `DynamicForm.qml` parses), not a generic action-dispatch one, and nothing
/// in `execute()` reads it. A specialisation that does need to carry a
/// document alongside the dispatch wiring -- `FormsControllerCore` is the
/// shipped example -- adds it as its own member instead.
///
/// Two constructor overloads decide who owns the `Bridge`:
/// - The default constructor builds and owns a private `ThreadPoolExecutor` +
///   `QtExecutor` + `Bridge` over a `LocalBackend` -- the convenient default
///   for a demo or an app that has no `Bridge` of its own.
/// - The `(Bridge&, IExecutor*)` constructor composes over a caller-supplied
///   `Bridge`/executor instead -- the caller decides the deployment mode
///   (`LocalBackend`, `SimulatedRemoteBackend`, `QtWebSocketBackend`, ...) and
///   this core never builds a second, always-local `Bridge` of its own. The
///   caller's `Bridge`/executor must outlive this core.

#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/qt/qt_executor.hpp>
#include <optional>
#include <string>
#include <utility>

namespace morph::qt::bridge {

/// @brief Owns, or composes over, the Bridge/BridgeHandler/executor plumbing
///        behind a schema-driven QML bridge, generic over the model type and
///        the handler's sharing policy.
///
/// @tparam Model   The registered model type (`BRIDGE_REGISTER_MODEL`) whose
///                 actions this bridge dispatches.
/// @tparam Sharing The handler's sharing policy -- `morph::bridge::NoSharing`
///                 (the default: one private handler instance) or
///                 `morph::bridge::AllowShared` (joins the shared instance
///                 directory; see `morph::bridge::BridgeHandler`).
template <typename Model, typename Sharing = ::morph::bridge::NoSharing>
class GenericModelBridgeCore {
public:
    /// @brief Constructs the core with its own private, always-local `Bridge`
    ///        (`ThreadPoolExecutor` + `QtExecutor` + `LocalBackend`).
    ///
    /// Use the `(Bridge&, IExecutor*)` overload instead when the app already
    /// has a `Bridge` (remote/socket mode, or one shared across multiple
    /// presenters) that this core should compose over rather than duplicate.
    GenericModelBridgeCore() : _owned{std::in_place}, _handler{_owned->bridge, &_owned->gui} {}

    /// @brief Constructs the core over a caller-supplied `Bridge`/executor,
    ///        instead of building a private, always-local one.
    ///
    /// The core registers a `BridgeHandler<Model, Sharing>` on @p bridge
    /// exactly as the owning constructor's internal one does, so `execute`
    /// dispatches through whatever backend @p bridge currently has installed
    /// (`LocalBackend`, `SimulatedRemoteBackend`, `QtWebSocketBackend`, ...)
    /// -- including a backend @p bridge switches to later via
    /// `Bridge::switchBackend`, since the registered handler re-registers
    /// itself automatically.
    ///
    /// @param bridge  The bridge to register this core's handler on. Must
    ///                outlive this core.
    /// @param guiExec Executor used to deliver `Completion` callbacks (e.g. a
    ///                `QtExecutor` for the GUI thread). Must outlive this
    ///                core.
    GenericModelBridgeCore(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* guiExec)
        : _handler{bridge, guiExec} {}

    /// @brief Dispatches @p bodyJson as @p actionType's body via the generic
    ///        `executeJson` path, invoking @p onReply / @p onError on the GUI
    ///        thread once the reply arrives.
    ///
    /// Deliberately named after the call it forwards to
    /// (`BridgeHandler::executeJson`), not after a specific verb like
    /// "submit" or "fetch": the dispatch is identical whether @p actionType
    /// mutates state or only reads it, and the caller's own action-type
    /// naming already carries that distinction.
    /// @tparam OnReply Callable invoked with the result JSON (`std::string`) on success.
    /// @tparam OnError Callable invoked with the `std::exception_ptr` on failure.
    /// @param actionType Registered action type id.
    /// @param bodyJson   Fully-assembled JSON body for the action.
    /// @param onReply    Success callback.
    /// @param onError    Failure callback.
    template <typename OnReply, typename OnError>
    void execute(const std::string& actionType, const std::string& bodyJson, OnReply onReply, OnError onError) {
        _handler.executeJson(actionType, bodyJson)
            .then([onReply = std::move(onReply)](std::string resultJson) mutable { onReply(std::move(resultJson)); })
            .onError([onError = std::move(onError)](const std::exception_ptr& err) mutable { onError(err); });
    }

private:
    /// @brief The private pool/executor/backend bundle the schema-only
    ///        constructor builds and owns. Absent (`_owned` unengaged) when the
    ///        core instead composes over a caller-supplied `Bridge`/executor.
    ///
    /// Declaration order within the struct matters for destruction: `bridge`
    /// must tear down before `pool`/`gui`, so it is declared last.
    struct OwnedBridge {
        ::morph::exec::ThreadPoolExecutor pool{2};
        ::morph::qt::QtExecutor gui;
        ::morph::bridge::Bridge bridge{std::make_unique<::morph::backend::LocalBackend>(pool)};
    };

    // Declaration order matters for destruction: _handler must tear down
    // before _owned (its bridge and executor, when this core owns them), so
    // _owned is declared first and _handler after it. When the caller-
    // supplied-Bridge constructor is used, _owned stays unengaged and
    // _handler instead references the caller's Bridge/executor directly --
    // the caller is responsible for outliving _handler in that case.
    std::optional<OwnedBridge> _owned;
    ::morph::bridge::BridgeHandler<Model, Sharing> _handler;
};

}  // namespace morph::qt::bridge
