// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The lims guard's declaration, which an action's DTO names with `using guard = ::lims::DeclaredRuleGuard;`. Its
/// definition is in `declared.hpp`, which must be included wherever the action is registered or executed.

namespace lims {
struct DeclaredRuleGuard;
}  // namespace lims
