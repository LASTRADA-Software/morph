// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The smallest possible model for the WASM-remote spike: proves
/// registration + one round-trip action work over QtWebSocketBackend from a
/// WASM client, nothing more.
///
/// Deliberately at namespace scope, not inside an anonymous namespace: glz's
/// reflection (which `BRIDGE_REGISTER_MODEL`/`BRIDGE_REGISTER_ACTION` rely on
/// to serialize these types across the wire) needs external linkage on the
/// type — see glaze/reflection/get_name.hpp's `extern const T external`, and
/// examples/common/testkit/test_fault_proxy.cpp's identical note.

struct SpikeEchoAction {
    int value = 0;
};

struct SpikeEchoModel {
    int execute(SpikeEchoAction action) { return action.value; }
};
