// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <expected>
#include <functional>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

#include "../session/session.hpp"
#include "args.hpp"
#include "source.hpp"

/// @file
/// @brief The generic client as an MCP server on stdio (`--mcp-stdio`): remote over a WebSocket to a running server,
///        or local over an in-process server on a linked application.
///
/// Specified in `docs/spec/client/client.md` and `docs/spec/ui/agent.md`.

namespace morph::client {

/// @brief Checks that the options given with `--mcp-stdio` make sense for a client that mounts nothing.
/// @param args The client's options.
/// @param rest What `readArgs` left of the command line, program name first: arguments meant for a frontend.
/// @return Nothing, or why it is refused: `--screen`, `--exit-after-mount` or `--dev`, or an argument no one reads.
[[nodiscard]] std::expected<void, std::string> checkMcpArgs(Args const& args, std::vector<std::string> const& rest);

/// @brief The session an MCP server's calls carry.
///
/// The principal is `--user`, and in remote mode `MORPH_MCP_PRINCIPAL` when `--user` is not given. The token is
/// `MORPH_MCP_TOKEN`, remote mode only: a local server checks no token, and a diagnostic says so when one is set.
/// @param args        The client's options.
/// @param environment Reads an environment variable; null for one that is not set.
/// @param diagnostics Where a token a local server ignores is reported.
/// @return The session.
[[nodiscard]] session::Context mcpSession(Args const& args, std::function<char const*(char const*)> const& environment,
                                          std::ostream& diagnostics);

/// @brief Serves MCP on @p input and @p output until @p input ends.
/// @param args        The client's options: `--server` selects remote mode, `--app` local mode.
/// @param apps        The applications this client links, for local mode.
/// @param session     The session every call carries.
/// @param input       Where JSON-RPC messages arrive, one per line.
/// @param output      Where replies go, one per line, and nothing else.
/// @param diagnostics Where problems are reported.
/// @return The process exit code: 0 once input ended; 2 when the application is not linked, its bundle does not
///         build, or its `setUp` throws; when neither `--server` nor `--app` is given; when the `--server` URL is not
///         ws:// or wss://; or when no built transport can reach it.
[[nodiscard]] int serveMcp(Args const& args, std::vector<LocalApp> const& apps, session::Context session,
                           std::istream& input, std::ostream& output, std::ostream& diagnostics);

/// @brief Sets the process's stdout aside as the JSON-RPC channel: duplicates it, and points descriptor 1 at stderr,
///        so anything else the process prints lands in the agent's log instead of corrupting the protocol.
/// @return The stream over the duplicate, or null when the descriptors cannot be duplicated.
[[nodiscard]] std::unique_ptr<std::ostream> claimStdout();

}  // namespace morph::client
