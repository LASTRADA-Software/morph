// SPDX-License-Identifier: Apache-2.0
//
// Compile-check fixture for action guards (see the try_compile() block after the MORPH_REQUIRE_VETTED_HMAC probes in
// tests/CMakeLists.txt). The guard type is defined with the payload form: must compile.

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

struct Missing {
    template <typename Action>
    static void check(Action const&);
};

}  // namespace probe

BRIDGE_REGISTER_MODEL(probe::Model, "P_GuardModel")
BRIDGE_REGISTER_ACTION(probe::Model, probe::Probe, "P_GuardAction")

int main() { return 0; }
