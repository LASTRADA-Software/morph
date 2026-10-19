// SPDX-License-Identifier: Apache-2.0
//
// An action registered in a header, as an application's model header would. Both
// effects_unseen_a.cpp and effects_unseen_b.cpp include it; only the first also
// sees the action's BRIDGE_REGISTER_EFFECTS.
#pragma once

#include <cstdint>
#include <morph/core/bridge.hpp>
#include <morph/core/effects.hpp>
#include <morph/core/registry.hpp>
#include <optional>
#include <vector>

namespace effects_unseen {

struct Row {
    std::int64_t id{};
};

struct EditRows {
    int count{};
};

struct RowsModel {
    std::vector<Row> execute(EditRows const& /*action*/) { return {}; }
};

/// The effects of `Unseen_EditRows` as read from effects_unseen_b.cpp, which does not see their registration.
std::optional<std::vector<morph::effects::Effect>> effectsSeenFromB();

}  // namespace effects_unseen

BRIDGE_REGISTER_MODEL(effects_unseen::RowsModel, "Unseen_RowsModel")
BRIDGE_REGISTER_ACTION(effects_unseen::RowsModel, effects_unseen::EditRows, "Unseen_EditRows")
