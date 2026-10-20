// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

/// @file
/// @brief `morph::forms::FieldLayout`: where a form's cell puts a field's caption. The forms engine and the
///        authoring API share it. Specified in `docs/spec/forms/engine.md`, "Rendering".

namespace morph::forms {

/// @brief Where a form's cell puts a field's caption, relative to its control.
enum class FieldLayout : std::uint8_t {
    Stacked,  ///< The label over the control over the help.
    Inline,   ///< The label beside the control, at a fixed width, centred on it; the help under the control.
};

}  // namespace morph::forms
