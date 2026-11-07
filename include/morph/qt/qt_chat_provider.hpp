// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <expected>
#include <memory>
#include <string>

#include "../ui/chat/config.hpp"
#include "../ui/chat/provider.hpp"

class QObject;

/// @file
/// @brief The one place that names a model provider. Specified in `docs/spec/ui/chat.md`.

namespace morph::qt {

/// @brief The provider @p config names.
/// @param config What it is reached with.
/// @param owner  A QObject on a thread running a Qt event loop, where its network calls run. Borrowed: it outlives the
///               provider. `IProvider::stream` may be called from any thread.
/// @return The provider, or why not: an unknown name, or a missing key or model.
[[nodiscard]] std::expected<std::shared_ptr<ui::chat::IProvider>, std::string> makeQtChatProvider(
    ui::chat::ProviderConfig config, QObject* owner);

}  // namespace morph::qt
