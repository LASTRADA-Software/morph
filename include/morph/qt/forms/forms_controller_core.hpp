// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file
/// Forms-specific facade over `morph::qt::bridge::GenericModelBridgeCore`:
/// gives the shipped Qt/QML forms renderer's controller the two operation
/// names `DynamicForm.qml` actually calls -- `submitIfValid` and
/// `fetchOptions` -- over the same generic dispatch the composed core
/// already provides. A concrete `QObject`/`QML_ELEMENT` wrapper per app (Qt
/// cannot register a class *template* for QML) forwards to this facade and
/// turns its callbacks into signals -- see
/// `examples/forms/gui_qml/FormsController.hpp` for the reference wrapper.
///
/// Composes rather than inherits `GenericModelBridgeCore`: neither type
/// declares a virtual destructor (there is no polymorphic dispatch anywhere
/// in this seam -- every consumer names its concrete `Model` type), and
/// public inheritance from a class not designed for it invites deleting a
/// derived object through a base pointer, which is undefined behaviour
/// without one. Composition sidesteps the question entirely.
///
/// See `morph::qt::bridge::GenericModelBridgeCore` for the constructor
/// overloads (owning vs. composing a `Bridge`) and the underlying dispatch
/// mechanism.

#include <morph/qt/bridge/generic_model_bridge_core.hpp>
#include <string>
#include <utility>

namespace morph::qt::forms {

/// @brief `DynamicForm.qml`-facing facade over
///        `morph::qt::bridge::GenericModelBridgeCore`, naming its one generic
///        dispatch operation `submitIfValid`/`fetchOptions` instead of
///        `execute`, and carrying the `{actionType: schema}` document
///        `DynamicForm.qml` renders from -- a forms-specific concept the
///        composed core deliberately does not know about.
///
/// @tparam Model   The registered model type (`BRIDGE_REGISTER_MODEL`) whose
///                 actions the shipped `DynamicForm.qml` renders.
/// @tparam Sharing Forwarded to `GenericModelBridgeCore` unchanged --
///                 `morph::bridge::NoSharing` (the default) or
///                 `morph::bridge::AllowShared`. Every shipped forms
///                 controller uses the default; `AllowShared` compiles and
///                 type-checks but has no exercising caller in this repo yet.
template <typename Model, typename Sharing = ::morph::bridge::NoSharing>
class FormsControllerCore {
public:
    /// @brief Constructs the core with its own private, always-local `Bridge`.
    ///        See `GenericModelBridgeCore`'s own constructor for the full
    ///        contract.
    /// @param schemasJson The full schema set the QML renderer will parse.
    explicit FormsControllerCore(std::string schemasJson) : _schemasJson{std::move(schemasJson)} {}

    /// @brief Constructs the core over a caller-supplied `Bridge`/executor.
    ///        See `GenericModelBridgeCore`'s own constructor for the full
    ///        contract.
    /// @param bridge      The bridge to register this core's handler on. Must
    ///                     outlive this core.
    /// @param guiExec     Executor used to deliver `Completion` callbacks.
    ///                    Must outlive this core.
    /// @param schemasJson The full schema set the QML renderer will parse.
    FormsControllerCore(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* guiExec, std::string schemasJson)
        : _core{bridge, guiExec}, _schemasJson{std::move(schemasJson)} {}

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
    void submitIfValid(const std::string& actionType, const std::string& bodyJson, OnReply onReply, OnError onError) {
        _core.execute(actionType, bodyJson, std::move(onReply), std::move(onError));
    }

    /// @brief Executes @p optionsAction with @p bodyJson to fetch a `Choice`
    ///        field's combo-box options, via the same generic `executeJson`
    ///        path `submitIfValid` uses. @p optionsAction is a parameter rather
    ///        than a hardcoded id, and @p bodyJson is a true pass-through (not
    ///        always `"{}"`), so a dependent `Choice` (`x-optionsDependsOn`)
    ///        can send `{parentField: value, ...}` instead of an empty body.
    /// @tparam OnReply Callable invoked with the options-action result JSON on success.
    /// @tparam OnError Callable invoked with the `std::exception_ptr` on failure.
    /// @param optionsAction Registered action type id that serves the options.
    /// @param bodyJson      Fully-assembled JSON body for the options action
    ///                      (`"{}"` for an independent `Choice`).
    /// @param onReply       Success callback.
    /// @param onError       Failure callback.
    template <typename OnReply, typename OnError>
    void fetchOptions(const std::string& optionsAction, const std::string& bodyJson, OnReply onReply,
                      OnError onError) {
        _core.execute(optionsAction, bodyJson, std::move(onReply), std::move(onError));
    }

private:
    ::morph::qt::bridge::GenericModelBridgeCore<Model, Sharing> _core;
    std::string _schemasJson;
};

}  // namespace morph::qt::forms
