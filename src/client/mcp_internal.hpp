// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <istream>
#include <morph/ui/agent.hpp>
#include <ostream>
#include <string>

/// @file
/// What `mcp.cpp` calls in a transport translation unit. Private: it names `ui::McpOptions`, which the client's
/// public headers keep out of their includes.

namespace morph::client::detail {

/// @brief Serves MCP over a `QtWebSocketBackend`; defined only where `morph::qt` is built.
///
/// The backend needs a Qt event loop, so this makes a `QCoreApplication` when the process has none and runs it here;
/// stdin is read on a thread of its own, and the call returns once @p input ends.
/// @param url         `ws://` or `wss://` server URL.
/// @param options     The adapter's options; `callbacks` is replaced by the loop's executor.
/// @param input       Where MCP messages arrive.
/// @param output      Where replies go.
/// @param diagnostics Where a refusal is explained.
/// @return The process exit code: 0 once input ended, 2 for a URL that is not `ws://` or `wss://`.
[[nodiscard]] int serveMcpOverQt(std::string const& url, ui::McpOptions options, std::istream& input,
                                 std::ostream& output, std::ostream& diagnostics);

}  // namespace morph::client::detail
