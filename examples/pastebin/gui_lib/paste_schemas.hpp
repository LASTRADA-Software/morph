// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/forms/forms.hpp>

#include <string>

#include "pastebin/dto/paste_dto.hpp"

/// @file
/// The one schema document pastebin's create form renders from, in one place
/// so every shell that builds a `FormsBridge` — the desktop client
/// (`gui/main.cpp`), the WASM client (`gui_wasm/main_wasm.cpp`) and the
/// presenter tests — builds the *identical* map instead of each assembling
/// its own (`examples/TESTING.md`'s "same client code" requirement: the two
/// clients must differ only in their `main()`).

namespace pastebin::gui {

/// @brief The `{actionType: schema}` document the create form renders from.
///
/// Only `CreatePaste` is schema-driven: it is the one action a user *enters*.
/// Reading, listing and deleting are parameterised by a paste id the user
/// picks from the list, never typed, so they route through `PastePresenter`
/// and need no form. Assembled here rather than in the bridge
/// because that class takes the document as a constructor argument by design
/// (whatever composes it decides which actions it serves) — the same split
/// `morph::qt::forms::FormsControllerCore` and `lab::schemasJson()` use.
///
/// @return `{"CreatePaste": <schemaJson<CreatePaste>()>}`.
[[nodiscard]] inline std::string pasteSchemasJson() {
    return std::string{"{\"CreatePaste\":"} + ::morph::forms::schemaJson<pastebin::CreatePaste>() + "}";
}

}  // namespace pastebin::gui
