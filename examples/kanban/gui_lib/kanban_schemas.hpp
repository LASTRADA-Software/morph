// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/forms/forms.hpp>
#include <string>

#include "kanban/dto/auth_dto.hpp"
#include "kanban/dto/board_dto.hpp"
#include "kanban/dto/project_dto.hpp"
#include "kanban/dto/rule_dto.hpp"

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
/// model serves (`ProjectAdminBridge`: `Login`, `CreateProject`,
/// `SetMemberRole`; `BoardBridge`: `CreateColumn`, `CreateSwimlane`,
/// `CreateTask`, `AddComment`, `CreateRule`). Splitting it in two would mean a
/// screen holding one bridge could not render a form the other owns, and this
/// rung's views already mix the two — `gui/qml/BoardView.qml` holds both. An
/// action a controller does not serve is reported, not silently dropped: see
/// each bridge's `submitIfValid`.
///
/// **What a renderer has to support for this list to be complete.**
/// `SetMemberRole` and `CreateRule` each carry a C++ `enum class` member
/// (`Role`, `RuleMutationType`) that `schemaJson` emits as a closed `oneOf` of
/// `const`s. A `DynamicForm` that drew that as a **free-text field** would
/// accept and submit any string at all, which is why neither action could be
/// listed here; the shipped renderer draws it as a combo box and refuses a value
/// outside the set, so both render like every other form.
/// `CreateRule::triggerColumnId` is a `morph::forms::Choice<…,
/// "GetBoardState">` rather than a raw `ColumnId` — the shape rule 3 prescribes
/// for a user-chosen foreign key; `GetBoardState`'s reply returns `columns` as
/// its first array member.
/// `MoveTaskPosition` is absent for a different reason: it is a drag
/// gesture, not a form (rule 2(a); see the README).

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
           ",\"SetMemberRole\":" + ::morph::forms::schemaJson<kanban::SetMemberRole>() +
           ",\"CreateColumn\":" + ::morph::forms::schemaJson<kanban::CreateColumn>() +
           ",\"CreateSwimlane\":" + ::morph::forms::schemaJson<kanban::CreateSwimlane>() +
           ",\"CreateTask\":" + ::morph::forms::schemaJson<kanban::CreateTask>() +
           ",\"AddComment\":" + ::morph::forms::schemaJson<kanban::AddComment>() +
           ",\"CreateRule\":" + ::morph::forms::schemaJson<kanban::CreateRule>() + "}";
}

}  // namespace kanban::gui
