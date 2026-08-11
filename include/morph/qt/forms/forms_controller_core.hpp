// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file
/// Model-agnostic core of the shipped Qt/QML forms renderer's controller:
/// owns (or composes over) the Bridge/BridgeHandler/executor wiring
/// `examples/forms/gui_qml`'s `FormsController` used to hardcode per-app, and
/// exposes the two operations `DynamicForm.qml` needs -- submit and
/// options-fetch -- generically over `BridgeHandler<Model>::executeJson`, so
/// an app depends on this directly instead of re-deriving the wiring. A
/// concrete `QObject`/`QML_ELEMENT` wrapper per app (Qt cannot register a
/// class *template* for QML) forwards to this core and turns its callbacks
/// into signals -- see `examples/forms/gui_qml/FormsController.hpp` for the
/// reference wrapper.
///
/// Two constructor overloads decide who owns the `Bridge`:
/// - The single-argument (schema-only) constructor builds and owns a private
///   `ThreadPoolExecutor` + `QtExecutor` + `Bridge` over a `LocalBackend`,
///   exactly as before -- the convenient default for a demo or an app that
///   has no `Bridge` of its own.
/// - The `(Bridge&, IExecutor*, schemasJson)` constructor composes over a
///   caller-supplied `Bridge`/executor instead -- the caller decides the
///   deployment mode (`LocalBackend`, `SimulatedRemoteBackend`,
///   `QtWebSocketBackend`, ...) and this core never builds a second, always-
///   local `Bridge` of its own. The caller's `Bridge`/executor must outlive
///   this core.

#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/qt/qt_executor.hpp>
#include <optional>
#include <string>
#include <utility>

namespace morph::qt::forms {

/// @brief Owns, or composes over, the Bridge/BridgeHandler/executor plumbing
///        behind a schema-driven QML forms controller, generic over the
///        model type.
///
/// @tparam Model The registered model type (`BRIDGE_REGISTER_MODEL`) whose
///               actions the shipped `DynamicForm.qml` renders.
template <typename Model>
class FormsControllerCore {
public:
    /// @brief Constructs the core with its own private, always-local `Bridge`
    ///        (`ThreadPoolExecutor` + `QtExecutor` + `LocalBackend`), and the
    ///        app's pre-assembled `{actionType: schema}` JSON (e.g.
    ///        hand-written like `lab::schemasJson()`).
    ///
    /// Use the `(Bridge&, IExecutor*, schemasJson)` overload instead when the
    /// app already has a `Bridge` (remote/socket mode, or one shared across
    /// multiple presenters) that this core should compose over rather than
    /// duplicate.
    /// @param schemasJson The full schema set the QML renderer will parse.
    explicit FormsControllerCore(std::string schemasJson)
        : _owned{std::in_place}, _handler{_owned->bridge, &_owned->gui}, _schemasJson{std::move(schemasJson)} {}

    /// @brief Constructs the core over a caller-supplied `Bridge`/executor,
    ///        instead of building a private, always-local one.
    ///
    /// The core registers a `BridgeHandler<Model>` on @p bridge exactly as
    /// the owning constructor's internal one does, so `submitIfValid`/
    /// `fetchOptions` dispatch through whatever backend @p bridge currently
    /// has installed (`LocalBackend`, `SimulatedRemoteBackend`,
    /// `QtWebSocketBackend`, ...) -- including a backend @p bridge switches
    /// to later via `Bridge::switchBackend`, since the registered handler
    /// re-registers itself automatically.
    ///
    /// @param bridge      The bridge to register this core's handler on. Must
    ///                     outlive this core.
    /// @param guiExec     Executor used to deliver `Completion` callbacks
    ///                    (e.g. a `QtExecutor` for the GUI thread). Must
    ///                    outlive this core.
    /// @param schemasJson The full schema set the QML renderer will parse.
    FormsControllerCore(morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* guiExec, std::string schemasJson)
        : _handler{bridge, guiExec}, _schemasJson{std::move(schemasJson)} {}

    /// @brief The `{actionType: schema}` JSON supplied at construction.
    /// @return A reference to the cached schema-set JSON.
    [[nodiscard]] const std::string& schemasJson() const noexcept { return _schemasJson; }

    /// @brief Dispatches @p bodyJson as @p actionType's body via the generic
    ///        `executeJson` path, invoking @p onReply / @p onError on the GUI
    ///        thread once the reply arrives.
    /// @tparam OnReply Callable invoked with the result JSON (`std::string`) on success.
    /// @tparam OnError Callable invoked with the `std::exception_ptr` on failure.
    /// @param actionType Registered action type id.
    /// @param bodyJson   Fully-assembled JSON body for the action.
    /// @param onReply    Success callback.
    /// @param onError    Failure callback.
    template <typename OnReply, typename OnError>
    void submitIfValid(std::string actionType, std::string bodyJson, OnReply onReply, OnError onError) {
        _handler.executeJson(actionType, bodyJson)
            .then([onReply = std::move(onReply)](std::string resultJson) mutable { onReply(std::move(resultJson)); })
            .onError([onError = std::move(onError)](const std::exception_ptr& err) mutable { onError(err); });
    }

    /// @brief Executes @p optionsAction with @p bodyJson to fetch a `Choice`
    ///        field's combo-box options, via the same generic `executeJson`
    ///        path `submitIfValid` uses -- @p optionsAction is never
    ///        hardcoded, unlike the pre-factoring example controller, and
    ///        @p bodyJson is a true pass-through (not always `"{}"`), so a
    ///        dependent `Choice` (`x-optionsDependsOn`) can send
    ///        `{parentField: value, ...}` instead of an empty body.
    /// @tparam OnReply Callable invoked with the options-action result JSON on success.
    /// @tparam OnError Callable invoked with the `std::exception_ptr` on failure.
    /// @param optionsAction Registered action type id that serves the options.
    /// @param bodyJson      Fully-assembled JSON body for the options action
    ///                      (`"{}"` for an independent `Choice`).
    /// @param onReply       Success callback.
    /// @param onError       Failure callback.
    template <typename OnReply, typename OnError>
    void fetchOptions(std::string optionsAction, std::string bodyJson, OnReply onReply, OnError onError) {
        _handler.executeJson(optionsAction, bodyJson)
            .then([onReply = std::move(onReply)](std::string resultJson) mutable { onReply(std::move(resultJson)); })
            .onError([onError = std::move(onError)](const std::exception_ptr& err) mutable { onError(err); });
    }

private:
    /// @brief The private pool/executor/backend bundle the schema-only
    ///        constructor builds and owns, exactly as `FormsControllerCore`
    ///        always did before the `(Bridge&, IExecutor*, ...)` overload
    ///        existed. Absent (`_owned` unengaged) when the core instead
    ///        composes over a caller-supplied `Bridge`/executor.
    ///
    /// Declaration order within the struct matters for destruction: `bridge`
    /// must tear down before `pool`/`gui`, exactly as the pre-factoring
    /// `FormsController` required.
    struct OwnedBridge {
        morph::exec::ThreadPoolExecutor pool{2};
        ::morph::qt::QtExecutor gui;
        morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};
    };

    // Declaration order matters for destruction: _handler must tear down
    // before _owned (its bridge and executor, when this core owns them), so
    // _owned is declared first and _handler after it. When the caller-
    // supplied-Bridge constructor is used, _owned stays unengaged and
    // _handler instead references the caller's Bridge/executor directly --
    // the caller is responsible for outliving _handler in that case.
    std::optional<OwnedBridge> _owned;
    morph::bridge::BridgeHandler<Model> _handler;
    std::string _schemasJson;
};

}  // namespace morph::qt::forms
