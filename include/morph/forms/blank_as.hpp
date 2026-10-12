// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file forms/blank_as.hpp
/// @brief `morph::forms::BlankAs`: what a blank string field submits.
///
/// Its own header because both the schema emitters (`forms.hpp`) and the forms engine's field model
/// (`forms/engine/field_model.hpp`) name it, and the field model must not include the emitters.

#include <cstdint>

namespace morph::forms {

/// @brief What a renderer submits for a string field the user left blank
///        (`FieldMeta::blankAs`).
enum class BlankAs : std::uint8_t {
    /// Blank is "no value": the member is left out of the payload (the default).
    Omit,
    /// Blank, once the field was prefilled or edited, is an explicit `""`;
    /// emitted as `"x-blankAs": "empty"`.
    Empty,
};

}  // namespace morph::forms
