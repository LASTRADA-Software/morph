// SPDX-License-Identifier: Apache-2.0
//
// Opposite-direction probe for declare_only_no_source_link.cpp (see that
// file's own comment, and tests/CMakeLists.txt's try_compile() block). Calls
// BRIDGE_REGISTER_MODEL_SOURCE/BRIDGE_REGISTER_ACTION_SOURCE for exactly the
// model/action declare_source_canary_shared.hpp declares, so a program built
// from this file plus declare_only_no_source_link.cpp must link and run
// cleanly -- proving the canary check is satisfiable, not merely never
// triggered.
#include "declare_source_canary_shared.hpp"

BRIDGE_REGISTER_MODEL_SOURCE(DeclareOnlyModel)
BRIDGE_REGISTER_ACTION_SOURCE(DeclareOnlyModel, DeclareOnlyAction)
