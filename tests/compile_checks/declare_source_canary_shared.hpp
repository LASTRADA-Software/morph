// SPDX-License-Identifier: Apache-2.0
//
// Shared fixture for the BRIDGE_DECLARE_MODEL/BRIDGE_DECLARE_ACTION link-time
// canary check (see the try_compile() block in tests/CMakeLists.txt).
// DeclareOnlyModel/DeclareOnlyAction are a real, fully-defined model and
// action -- unlike client_only_no_model_link.cpp's probe, nothing here is
// deliberately left undefined -- so the only thing that can make either of
// the two probes below fail to link is whether
// BRIDGE_REGISTER_MODEL_SOURCE/BRIDGE_REGISTER_ACTION_SOURCE was ever called
// for them anywhere in that probe's link.
//
// Uses BRIDGE_DECLARE_ACTION_4 directly rather than the public
// BRIDGE_DECLARE_ACTION(...) variadic-dispatch macro: MSVC's preprocessor
// emits "not enough arguments for function-like macro invocation
// BRIDGE_DECLARE_ACTION_PICK" (C4003) for the 3-arg form, the same MSVC
// quirk client_only_no_model_link.cpp's own comment documents for
// BRIDGE_REGISTER_ACTION -- confirmed here too (CI's Windows/cl-debug job).
// Calling _4 directly sidesteps the variadic dispatch while still exercising
// the exact declare/source split this probe exists to prove.
#pragma once

#include <morph/core/bridge.hpp>
#include <morph/core/registry.hpp>

struct DeclareOnlyAction {
    int x = 0;
};

struct DeclareOnlyModel {
    int execute(const DeclareOnlyAction& action) { return action.x; }
};

BRIDGE_DECLARE_MODEL(DeclareOnlyModel, "DeclareOnlyModel")
BRIDGE_DECLARE_ACTION_4(DeclareOnlyModel, DeclareOnlyAction, "DeclareOnlyAction", ::morph::model::Loggable::Yes)
