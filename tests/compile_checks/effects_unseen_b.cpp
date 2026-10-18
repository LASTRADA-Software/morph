// SPDX-License-Identifier: Apache-2.0
//
// The half of morph_effects_unseen_links that registers the action but never
// sees its BRIDGE_REGISTER_EFFECTS.

#include "effects_unseen_shared.hpp"

std::optional<std::vector<morph::effects::Effect>> effects_unseen::effectsSeenFromB() {
    return morph::effects::effectsOf("Unseen_EditRows");
}
