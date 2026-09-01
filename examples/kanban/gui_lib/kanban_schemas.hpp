// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/forms/forms.hpp>
#include <string>

#include "kanban/dto/auth_dto.hpp"
#include "kanban/dto/board_dto.hpp"
#include "kanban/dto/project_dto.hpp"

/// @file
/// The `{actionType: schemaJson<A>()}` document kanban's schema-driven forms
/// render from.
///
/// One place, so every shell that builds a `ProjectAdminBridge` or a
/// `BoardBridge` — the desktop client and the tests alike — renders from the
/// same document rather than each assembling its own. Mirrors
/// `bookmarks/gui_lib/bookmark_schemas.hpp`, which established the shape.
///
/// **One document, two controllers.** Both QML bridges expose this same string
/// as their `schemasJson` property, and each routes only the actions its own
/// model serves (`ProjectAdminBridge`: `Login`, `CreateProject`;
/// `BoardBridge`: `CreateColumn`, `CreateSwimlane`, `CreateTask`,
/// `AddComment`). Splitting it in two would mean a screen holding one bridge
/// could not render a form the other owns, and this rung's views already mix
/// the two — `gui/qml/BoardView.qml` holds both. An action a controller does
/// not serve is reported, not silently dropped: see each bridge's
/// `submitIfValid`.
///
/// **What is deliberately absent**, and why, since an absence is otherwise
/// indistinguishable from an oversight: `SetMemberRole` and `CreateRule` are
/// not here. Both carry a C++ `enum class` member (`Role`, `RuleMutationType`)
/// that `schemaJson` emits as a closed `oneOf` of `const`s — which the shipped
/// `DynamicForm` renders as a **free-text field**, accepting and submitting any
/// string at all. Rendering them here would replace two working combo boxes
/// with two typo-accepting text boxes, so they stay hand-built under
/// `examples/IMPLEMENTATION.md` rule 2's justification (a). See the rung
/// README's "morph subsystems exercised" section and morph#386.
/// `MoveTaskPosition` is absent for the opposite reason: it is a drag gesture,
/// not a form (rule 2(a) again, and also in the README).

namespace kanban::gui {

/// @brief Builds the schema document for every action this rung renders.
///
/// Not cached here: `morph::forms::schemaJson<A>()` already memoises one
/// string per compiled action type, so the only cost repeated here is the
/// concatenation, which happens once per shell at construction.
/// @return `{actionType: schema}` JSON.
[[nodiscard]] inline std::string kanbanSchemasJson() {
    return std::string{"{\"Login\":"} + ::morph::forms::schemaJson<kanban::Login>() +
           ",\"CreateProject\":" + ::morph::forms::schemaJson<kanban::CreateProject>() +
           ",\"CreateColumn\":" + ::morph::forms::schemaJson<kanban::CreateColumn>() +
           ",\"CreateSwimlane\":" + ::morph::forms::schemaJson<kanban::CreateSwimlane>() +
           ",\"CreateTask\":" + ::morph::forms::schemaJson<kanban::CreateTask>() +
           ",\"AddComment\":" + ::morph::forms::schemaJson<kanban::AddComment>() + "}";
}

}  // namespace kanban::gui
