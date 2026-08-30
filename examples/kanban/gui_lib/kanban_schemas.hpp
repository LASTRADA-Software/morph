// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/forms/forms.hpp>
#include <string>

#include "kanban/dto/auth_dto.hpp"

/// @file
/// The `{actionType: schemaJson<A>()}` document kanban's schema-driven forms
/// render from.
///
/// One place, so every shell that builds a `ProjectAdminBridge` — the desktop
/// client and the tests alike — renders from the same document rather than
/// each assembling its own. Mirrors `bookmarks/gui_lib/bookmark_schemas.hpp`,
/// which established the shape.
///
/// **Only `Login` is here today.** This rung's other inputs are still
/// hand-built Qt Quick, which `examples/IMPLEMENTATION.md` rule 2 forbids by
/// default — see the rung README's "morph subsystems exercised" section for
/// the per-element accounting and morph#344 for why they are not all converted
/// at once.

namespace kanban::gui {

/// @brief Builds the schema document for every action this rung renders.
///
/// Not cached here: `morph::forms::schemaJson<A>()` already memoises one
/// string per compiled action type, so the only cost repeated here is the
/// concatenation, which happens once per shell at construction.
/// @return `{actionType: schema}` JSON.
[[nodiscard]] inline std::string kanbanSchemasJson() {
    return std::string{"{\"Login\":"} + ::morph::forms::schemaJson<kanban::Login>() + "}";
}

}  // namespace kanban::gui
