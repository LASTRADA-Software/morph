// SPDX-License-Identifier: Apache-2.0
//
// Half of a program in which only one translation unit sees an action's
// BRIDGE_REGISTER_EFFECTS. It defines no specialisation, so the two halves link
// into one program and read the same effects.

#include <cstdio>
#include "effects_unseen_shared.hpp"

BRIDGE_REGISTER_RECORD(effects_unseen::Row, "Unseen_Row")
BRIDGE_REGISTER_EFFECTS(effects_unseen::EditRows, morph::effects::updates<effects_unseen::Row>)

int main() {
    std::vector<morph::effects::Effect> const expected{{morph::effects::Kind::Updates, "Unseen_Row"}};
    auto const seenHere = morph::effects::effectsOf("Unseen_EditRows");
    auto const seenFromB = effects_unseen::effectsSeenFromB();
    if (seenHere != expected || seenFromB != expected) {
        std::puts("effects differ between the translation units, or were not registered");
        return 1;
    }
    return 0;
}
