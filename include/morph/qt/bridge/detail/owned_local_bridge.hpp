// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file
/// Private helper shared by every bridge core that offers an "I'll build my
/// own Bridge" convenience constructor (`GenericModelBridgeCore`,
/// `MultiModelBridgeCore`) alongside the one composing over a caller-supplied
/// `Bridge`/executor. Not part of the public API: nothing outside
/// `morph::qt::bridge` names this type.

#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/qt/qt_executor.hpp>

namespace morph::qt::bridge::detail {

/// @brief The private pool/executor/backend bundle an owning bridge-core
///        constructor builds and owns, so it never has to duplicate this
///        shape per core.
///
/// Declaration order matters for destruction: `bridge` must tear down before
/// `pool`/`gui`, so it is declared last.
struct OwnedLocalBridge {
    /// @brief Backs `bridge`'s `LocalBackend`.
    ::morph::exec::ThreadPoolExecutor pool{2};
    /// @brief Delivers `Completion` callbacks on the GUI thread.
    ::morph::qt::QtExecutor gui;
    /// @brief The always-local `Bridge` every composed handler registers on.
    ::morph::bridge::Bridge bridge{std::make_unique<::morph::backend::LocalBackend>(pool)};
};

}  // namespace morph::qt::bridge::detail
