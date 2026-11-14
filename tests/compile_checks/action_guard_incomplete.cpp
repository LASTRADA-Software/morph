// SPDX-License-Identifier: Apache-2.0
//
// Compile-check fixture for action guards (see the try_compile() block after the MORPH_REQUIRE_VETTED_HMAC probes in
// tests/CMakeLists.txt). The guard type is declared and never defined in the unit: must FAIL to compile.

#include <morph/core/action_guard.hpp>
#include <morph/core/bridge.hpp>

namespace probe {

struct Missing;

struct Probe {
    using guard = Missing;
};

struct Model {
    int execute(Probe const&) { return 0; }
};

}  // namespace probe

BRIDGE_REGISTER_MODEL(probe::Model, "P_GuardModel")
// The 4-argument form: MSVC's traditional preprocessor cannot dispatch the variadic BRIDGE_REGISTER_ACTION, and
// a negative probe would then fail on the macro instead of on the guard.
BRIDGE_REGISTER_ACTION_4(probe::Model, probe::Probe, "P_GuardAction", ::morph::model::Loggable::Yes)

int main() { return 0; }
