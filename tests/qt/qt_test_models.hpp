// SPDX-License-Identifier: Apache-2.0
//
// Shared models used by the process-separation tests. Each helper binary
// (qt_test_server, qt_test_client) includes this header so its static-init
// time registers the same model/action ids on both sides of the wire.
//
// Names are kept un-namespaced because BRIDGE_REGISTER_* uses token pasting
// (`bridge_model_reg_##M`) which can't accept `::` in the type name.

#pragma once
#include <morph/core/bridge.hpp>
#include <morph/core/registry.hpp>
#include <stdexcept>

struct ProcTestEchoAction {
    int value = 0;
};
struct ProcTestEchoFailAction {};

struct ProcTestEchoModel {
    int execute(ProcTestEchoAction action) { return action.value * 2; }
    int execute(ProcTestEchoFailAction) { throw std::runtime_error("intentional failure"); }
};

BRIDGE_REGISTER_MODEL(ProcTestEchoModel, "ProcTestEchoModel")
BRIDGE_REGISTER_ACTION(ProcTestEchoModel, ProcTestEchoAction, "ProcTestEchoAction")
BRIDGE_REGISTER_ACTION(ProcTestEchoModel, ProcTestEchoFailAction, "ProcTestEchoFailAction")
