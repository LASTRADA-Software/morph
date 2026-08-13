// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "pastebin/models/paste_model.hpp"

#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>

#include <exception>
#include <string>
#include <utility>

namespace pastebin::gui {

/// @brief Same schema-driven surface as the shipped
///        `morph::qt::forms::FormsControllerCore<PasteModel>`
///        (`schemasJson()`/`submitIfValid()`), composed over an injected
///        `Bridge&`/`IExecutor*` instead of constructing its own
///        `LocalBackend`. The shipped core's own `(Bridge&, IExecutor*,
///        schemasJson)` constructor now supports this directly (finding
///        021), but this rung still owns a thin controller of its own —
///        `TESTING.md`'s presenter rule 2 forbids GUI code from
///        constructing its own backend/executor regardless, and this
///        controller predates the shipped core gaining that overload. Pure
///        glue, no domain logic (`IMPLEMENTATION.md` rule 2 justification
///        (b)) — the schema/validation/rendering machinery is untouched;
///        only the backend-wiring seam differs.
///
/// `fetchOptions()` is deliberately not present: it exists on the shipped
/// `FormsControllerCore` to serve a `morph::forms::Choice<T,...>` field's
/// combo-box options, and none of pastebin's DTOs
/// (`pastebin/dto/paste_dto.hpp`) declare a `Choice` field — `CreatePaste`'s
/// `Visibility`/`Editability` enums render as plain enum widgets, not a
/// server-fetched `Choice`. Adding an unused `fetchOptions()` here would be
/// a stub with nothing to call it; omitted rather than speculatively
/// implemented, per this task's own instruction.
class PasteFormsController {
  public:
    /// @param bridge      The shared `Bridge` `AppContext` owns.
    /// @param executor    The executor `Completion` callbacks land on.
    /// @param schemasJson Pre-assembled `{actionType: schemaJson<A>()}` map,
    ///        matching `FormsControllerCore`'s own constructor contract.
    ///        Built by whatever composes this controller (Task 12's GUI
    ///        shell), not by this class.
    PasteFormsController(::morph::bridge::Bridge& bridge, ::morph::exec::IExecutor* executor, std::string schemasJson);

    /// @brief The `{actionType: schema}` JSON supplied at construction.
    /// @return A reference to the cached schema-set JSON.
    [[nodiscard]] const std::string& schemasJson() const noexcept { return _schemasJson; }

    /// @brief Dispatches @p bodyJson as @p actionType's body via the generic
    ///        `executeJson` path, invoking @p onReply / @p onError on the GUI
    ///        thread once the reply arrives. Verbatim copy of
    ///        `FormsControllerCore::submitIfValid`'s logic
    ///        (`include/morph/qt/forms/forms_controller_core.hpp:53-58`):
    ///        `_handler` is the only thing that differs, since it is built
    ///        from the injected `Bridge&`/`IExecutor*` instead of a
    ///        hardcoded `LocalBackend`.
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

  private:
    ::morph::bridge::BridgeHandler<PasteModel> _handler;
    std::string _schemasJson;
};

}  // namespace pastebin::gui
