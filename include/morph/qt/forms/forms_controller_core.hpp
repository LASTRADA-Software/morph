// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file
/// Model-agnostic core of the shipped Qt/QML forms renderer's controller:
/// owns the Bridge/BridgeHandler/executor wiring `examples/forms/gui_qml`'s
/// `FormsController` used to hardcode per-app, and exposes the two
/// operations `DynamicForm.qml` needs -- submit and options-fetch --
/// generically over `BridgeHandler<Model>::executeJson`, so an app depends on
/// this directly instead of re-deriving the wiring. A concrete
/// `QObject`/`QML_ELEMENT` wrapper per app (Qt cannot register a class
/// *template* for QML) forwards to this core and turns its callbacks into
/// signals -- see `examples/forms/gui_qml/FormsController.hpp` for the
/// reference wrapper.

#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/qt/qt_executor.hpp>
#include <string>
#include <utility>

namespace morph::qt::forms {

/// @brief Owns the Bridge/BridgeHandler/executor plumbing behind a
///        schema-driven QML forms controller, generic over the model type.
///
/// @tparam Model The registered model type (`BRIDGE_REGISTER_MODEL`) whose
///               actions the shipped `DynamicForm.qml` renders.
template <typename Model>
class FormsControllerCore {
public:
    /// @brief Constructs the core with the app's pre-assembled `{actionType:
    ///        schema}` JSON (e.g. hand-written like `lab::schemasJson()`).
    /// @param schemasJson The full schema set the QML renderer will parse.
    explicit FormsControllerCore(std::string schemasJson) : _schemasJson{std::move(schemasJson)} {}

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

    /// @brief Executes @p optionsAction with an empty JSON body (`"{}"`) to
    ///        fetch a `Choice` field's combo-box options, via the same
    ///        generic `executeJson` path `submitIfValid` uses -- @p
    ///        optionsAction is never hardcoded, unlike the pre-factoring
    ///        example controller.
    /// @tparam OnReply Callable invoked with the options-action result JSON on success.
    /// @tparam OnError Callable invoked with the `std::exception_ptr` on failure.
    /// @param optionsAction Registered action type id that serves the options.
    /// @param onReply       Success callback.
    /// @param onError       Failure callback.
    template <typename OnReply, typename OnError>
    void fetchOptions(std::string optionsAction, OnReply onReply, OnError onError) {
        _handler.executeJson(optionsAction, "{}")
            .then([onReply = std::move(onReply)](std::string resultJson) mutable { onReply(std::move(resultJson)); })
            .onError([onError = std::move(onError)](const std::exception_ptr& err) mutable { onError(err); });
    }

private:
    // Declaration order matters for destruction, exactly as in the
    // pre-factoring FormsController: _handler/_bridge must tear down before
    // _pool/_gui.
    morph::exec::ThreadPoolExecutor _pool{2};
    ::morph::qt::QtExecutor _gui;
    morph::bridge::Bridge _bridge{std::make_unique<morph::backend::LocalBackend>(_pool)};
    morph::bridge::BridgeHandler<Model> _handler{_bridge, &_gui};
    std::string _schemasJson;
};

}  // namespace morph::qt::forms
