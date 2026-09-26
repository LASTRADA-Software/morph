// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file
/// Multi-model sibling of `morph::qt::forms::FormsControllerCore`: same
/// `schemasJson()`/`submitIfValid()`/`fetchOptions()` surface
/// `DynamicForm.qml` expects of a controller, over
/// `morph::qt::bridge::MultiModelBridgeCore` instead of
/// `morph::qt::bridge::GenericModelBridgeCore` -- for a rung whose forms span
/// more than one registered model, and whose action-type-to-model routing
/// that implies.

#include <morph/qt/bridge/multi_model_bridge_core.hpp>
#include <string>
#include <utility>

namespace morph::qt::forms {

/// @brief `DynamicForm.qml`-facing facade over
///        `morph::qt::bridge::MultiModelBridgeCore`, naming its one routed
///        dispatch operation `submitIfValid`/`fetchOptions` instead of
///        `execute`, and carrying the `{actionType: schema}` document
///        `DynamicForm.qml` renders from -- a forms-specific concept the
///        composed core deliberately does not know about.
///
/// @tparam Sharing Forwarded to `MultiModelBridgeCore` unchanged. Named
///                 first, for the same reason `MultiModelBridgeCore`
///                 documents on its own `Sharing` parameter: it precedes a
///                 variadic pack, which must be the last template parameter,
///                 so it cannot also carry a usable default.
/// @tparam Model   Two or more registered model types
///                 (`BRIDGE_REGISTER_MODEL`) whose actions the shipped
///                 `DynamicForm.qml` renders, in the order their handlers are
///                 tried when routing an action-type id.
template <typename Sharing, typename... Model>
class MultiModelFormsControllerCore {
public:
    /// @brief Constructs the core with its own private, always-local `Bridge`.
    ///        See `MultiModelBridgeCore`'s own constructor for the full
    ///        contract.
    /// @param schemasJson The full schema set the QML renderer will parse.
    explicit MultiModelFormsControllerCore(std::string schemasJson) : _schemasJson{std::move(schemasJson)} {}

    /// @brief Constructs the core over a caller-supplied `Bridge`/executor.
    ///        See `MultiModelBridgeCore`'s own constructor for the full
    ///        contract.
    /// @param bridge      The bridge every model's handler registers on. Must
    ///                     outlive this core.
    /// @param guiExec     Executor used to deliver `Completion` callbacks.
    ///                    Must outlive this core.
    /// @param schemasJson The full schema set the QML renderer will parse.
    MultiModelFormsControllerCore(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* guiExec,
                                  std::string schemasJson)
        : _core{bridge, guiExec}, _schemasJson{std::move(schemasJson)} {}

    /// @brief The `{actionType: schema}` JSON supplied at construction.
    /// @return A reference to the cached schema-set JSON.
    [[nodiscard]] const std::string& schemasJson() const noexcept { return _schemasJson; }

    /// @brief Routes @p bodyJson as @p actionType's body to whichever `Model`
    ///        serves it, invoking @p onReply / @p onError on the GUI thread
    ///        once the reply (or the routing failure) arrives.
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
    ///        field's combo-box options, via the same routed dispatch
    ///        `submitIfValid` uses. @p optionsAction is a parameter rather
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
    ::morph::qt::bridge::MultiModelBridgeCore<Sharing, Model...> _core;
    std::string _schemasJson;
};

}  // namespace morph::qt::forms
