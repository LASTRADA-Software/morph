// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// @brief The switch that builds the UI development tools (spec 6 §21) into a client.
///
/// The build sets `MORPH_UI_DEV_TOOLS` to 1 or 0 for every translation unit (the CMake option of the same name, off
/// by default in release build types); a build that sets nothing gets 0, so a client built outside that build
/// contains none of the tools. Specified in `docs/spec/ui/service.md`.

#ifndef MORPH_UI_DEV_TOOLS
/// @brief 1 when the development tools (document reload in the generic client, the inspector) are built, 0 when not.
#define MORPH_UI_DEV_TOOLS 0
#endif
