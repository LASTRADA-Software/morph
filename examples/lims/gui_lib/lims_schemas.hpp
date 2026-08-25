// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/forms/forms.hpp>
#include <string>

#include "lims/dto/offline_dto.hpp"
#include "lims/dto/result_dto.hpp"
#include "lims/dto/sample_dto.hpp"

/// @file
/// The one schema document this rung's QML renders from — same split as
/// `bookmarks::gui::bookmarkSchemasJson()` and for the same reason: whatever
/// composes a client (the desktop shell, a future WASM client, the tests)
/// builds the identical `{actionType: schema}` map, never its own.
///
/// @par Which actions are here, and the rule that decides
/// **A field a person types is rendered from the schema; a value the model
/// already supplied is a typed call.** So:
///
/// - `RegisterClient`, `RegisterSample`, `RejectSample`, `ReturnForRework`,
///   `CaptureConcentration` and `ResolveConflict` are all here: every one of
///   them has at least one field somebody fills in.
/// - `ReceiveSample`, `StartWork`, `SubmitForVerification`, `PublishSample`
///   and `GetSample` are not: they carry **no fields at all**, so there is no
///   form to generate. A button that dispatches an empty body is not a
///   hand-built input widget.
/// - `VerifyResult` is not: its one field is a `ResultId` the result table
///   already holds, so the row's own button supplies it. Rendering a form
///   that asks a verifier to retype an id they are looking at would be worse,
///   not more conformant.
///
/// `CaptureConcentration` is the one that matters. It is the rung's headline
/// form and the reason this document exists at all: rendering it through the
/// shipped `DynamicForm` is what puts the served `x-rules`
/// (`exactlyOneOf(value, qualifier)`, and the `requiredWhen`/`visibleWhen`
/// pair over the dilution factor) in front of the framework's *own*
/// evaluator, rather than only the one `test_conditional_logic.cpp` writes.

namespace lims::gui {

/// @brief The `{actionType: schemaJson<A>()}` document every client renders
///        from.
///
/// Assembled by hand rather than by a helper because `morph::forms` has no
/// "schema set" primitive — the same hand-assembly bookmarks and polls each
/// do. Keys are the registered action type ids, so a renderer can look up a
/// form by the same name it dispatches under.
/// @return The document, as JSON text.
[[nodiscard]] inline std::string limsSchemasJson() {
    return std::string{"{"} + R"("RegisterClient":)" + ::morph::forms::schemaJson<RegisterClient>() +
           R"(,"RegisterSample":)" + ::morph::forms::schemaJson<RegisterSample>() + R"(,"RejectSample":)" +
           ::morph::forms::schemaJson<RejectSample>() + R"(,"ReturnForRework":)" +
           ::morph::forms::schemaJson<ReturnForRework>() + R"(,"CaptureConcentration":)" +
           ::morph::forms::schemaJson<CaptureConcentration>() + R"(,"ResolveConflict":)" +
           ::morph::forms::schemaJson<ResolveConflict>() + "}";
}

}  // namespace lims::gui
