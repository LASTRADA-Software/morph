// SPDX-License-Identifier: Apache-2.0
//
// Compile/link-check fixture for the MORPH_CLIENT_ONLY guard (see the
// try_compile() block at the end of tests/CMakeLists.txt). ClientOnlyModel's
// constructor and execute() are declared but never defined anywhere in this
// link. If MORPH_CLIENT_ONLY successfully suppresses the two model-owning
// registrars (registerModelOnce, registerActionOnce -- see
// docs/spec/core/registry.md, "MORPH_CLIENT_ONLY"), nothing in this program
// ever references either symbol and it links; without the guard, both
// registrars' lambda bodies call them, and the link fails with an unresolved
// external symbol.
//
// Uses BRIDGE_REGISTER_ACTION_4 directly rather than the public
// BRIDGE_REGISTER_ACTION(...) variadic-dispatch macro: MSVC's preprocessor
// emits "not enough arguments for function-like macro invocation
// BRIDGE_REGISTER_ACTION_PICK" (C4003) for the 3-arg form, which is normally
// benign, but combined with MORPH_CLIENT_ONLY's empty
// MORPH_DETAIL_REGISTER_ACTION_LOCAL expansion produced hard compile errors
// in this probe on cl.exe specifically (not reproducible on Clang/GCC).
// Calling _4 directly sidesteps the variadic dispatch while still exercising
// the exact registrar-suppression path this guard exists to prove.

#include <morph/core/bridge.hpp>
#include <morph/core/registry.hpp>

struct ClientOnlyAction {
    int x = 0;
};

struct ClientOnlyModel {
    ClientOnlyModel();                             // declared, deliberately never defined
    int execute(const ClientOnlyAction& action);   // declared, deliberately never defined
};

BRIDGE_REGISTER_MODEL(ClientOnlyModel, "ClientOnlyModel")
BRIDGE_REGISTER_ACTION_4(ClientOnlyModel, ClientOnlyAction, "ClientOnlyAction", ::morph::model::Loggable::Yes)

int main() {
    return 0;
}
