// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

/// @file
/// @brief `morph::ui::Align` and `morph::ui::Justify`: where a container places its children across its axis and
///        along it. The view tree, the authoring API and the backends share them. Specified in
///        `docs/spec/ui/backend_contract.md`, "The cross axis".

namespace morph::ui {

/// @brief Where a child is placed across its container's axis: a column's horizontally, a row's vertically, a
///        grid cell's content vertically in its row.
enum class Align : std::uint8_t {
    Stretch,  ///< A content child fills the cross extent; one its maximum stops sits at the start.
    Start,    ///< At the start: left in a column, top in a row or grid.
    Center,   ///< Centred.
    End,      ///< At the end: right in a column, bottom in a row or grid.
};

/// @brief Where a stack places its spare length along its axis, while no shown child stretches.
enum class Justify : std::uint8_t {
    Start,    ///< Packed at the start.
    Center,   ///< Packed in the middle.
    End,      ///< Packed at the end.
    Between,  ///< In equal gaps between the children, never less than the stack's gap.
};

}  // namespace morph::ui
