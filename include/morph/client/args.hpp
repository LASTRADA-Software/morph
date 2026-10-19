// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <expected>
#include <optional>
#include <string>
#include <vector>

/// @file
/// @brief The generic client's command line: which source it opens and how it starts. Spec 4 §3.
///
/// Specified in `docs/spec/client/client.md`.

namespace morph::client {

/// @brief What the generic client reads from its command line.
struct Args {
    /// @brief `--server URL`: the server to fetch the screens from.
    std::optional<std::string> server;
    /// @brief `--app NAME`: the linked application to run in-process.
    std::optional<std::string> app;
    /// @brief `--db PATH`: the database a local application opens.
    std::string db;
    /// @brief `--user NAME`: the development principal a local application signs in.
    std::string user;
    /// @brief `--screen ID`: the screen mounted first; empty for the catalog's first.
    std::string screen;
    /// @brief `--exit-after-mount`: quit once the first screen mounted, with 0 when it mounted cleanly.
    bool exitAfterMount = false;
    /// @brief `--dev`, or `MORPH_UI_DEV=1`: development mode, which reloads the screens when the server's documents
    ///        change (spec 5 §12). A client built without the development tools ignores it.
    bool dev = false;
};

/// @brief Reads the client's options from @p args and removes them, leaving the rest for the frontend.
///
/// Each value option is `--name value` or `--name=value`, and the last one given wins. `args[0]` is the program
/// name, and nothing after a `--` is read.
/// @param args The command line; what was read is removed.
/// @return The options, or why they are refused: a value option without its value, or not exactly one of
///         `--server` and `--app`. @p args is unchanged on a refusal.
[[nodiscard]] std::expected<Args, std::string> readArgs(std::vector<std::string>& args);

}  // namespace morph::client
