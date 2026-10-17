// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

/// @file
/// @brief `morph::ui::DevelopmentTools`: whether a client runs in development mode (spec 6 §21). The tools are in
///        every build; this decides at run time whether they are on. Specified in `docs/spec/ui/inspector.md`.

namespace morph::ui {

/// @brief Whether a session's screens record what the development inspector reads, and whether a frontend opens the
///        inspector (spec 6 §21).
enum class DevelopmentTools : std::uint8_t {
    Off,  ///< Nothing is recorded, no inspector is handed out, and the inspector's shortcut does nothing.
    On,   ///< Each screen records its lowered nodes, its live scopes and its latest command lists.
};

}  // namespace morph::ui
