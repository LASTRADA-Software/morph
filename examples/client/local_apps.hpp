// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <morph/client/source.hpp>
#include <vector>

/// @file
/// The applications a client binary links for local mode: none in `morph_remote_client`, every example in
/// `morph_examples_client`. Each binary compiles one definition.

namespace morph::examples::client {

/// @brief The applications `--app` can name.
/// @return Them.
[[nodiscard]] std::vector<::morph::client::LocalApp> localApps();

}  // namespace morph::examples::client
