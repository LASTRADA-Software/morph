// SPDX-License-Identifier: Apache-2.0
//
// Compile/link-check fixture proving the BRIDGE_DECLARE_MODEL/
// BRIDGE_DECLARE_ACTION link-time canary (see the try_compile() block in
// tests/CMakeLists.txt, and docs/spec/core/registry.md, "Moving a registrar
// out of the header"). This translation unit declares DeclareOnlyModel/
// DeclareOnlyAction via declare_source_canary_shared.hpp but never calls
// BRIDGE_REGISTER_MODEL_SOURCE/BRIDGE_REGISTER_ACTION_SOURCE for them --
// the exact mistake the canary exists to catch: a rung's model header moved
// its registrar call to a .cpp, and that .cpp was never written (or never
// linked into this target).
//
// Compiled alone, this must FAIL TO LINK with an unresolved external symbol
// naming morph::model::detail::modelSourceRegistrationRequired<DeclareOnlyModel>
// / actionSourceRegistrationRequired<DeclareOnlyModel, DeclareOnlyAction> --
// not compile clean and only fail at *runtime* the first time something
// dispatches "DeclareOnlyModel"/"DeclareOnlyAction" with
// ModelRegistryFactory::create's "unknown model type" (or
// ActionDispatcher::dispatch's "unknown action"), far from the actual cause.
//
// Compiled together with declare_only_source_provided.cpp (which does call
// both SOURCE macros for exactly this model/action), the same program must
// link and run cleanly -- the opposite-direction probe proving the canary
// is not vacuously unsatisfiable.
#include "declare_source_canary_shared.hpp"

int main() { return 0; }
