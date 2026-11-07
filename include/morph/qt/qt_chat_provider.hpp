// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <expected>
#include <memory>
#include <string>

#include "../ui/chat/provider.hpp"

class QObject;

/// @file
/// @brief The one place that names a model provider. Specified in `docs/spec/ui/chat.md`.

namespace morph::qt {

/// @brief Which model provider a chat uses, and how to reach it.
struct ChatProviderConfig {
    /// @brief The provider's name. `anthropic` is the one there is.
    std::string name;
    /// @brief Where it is reached; empty for its public address.
    std::string baseUrl;
    /// @brief The secret it is called with. It appears in no log and no chat message.
    std::string apiKey;
    /// @brief The model. It has no default: which model a deployment pays for is the deployer's choice.
    std::string model;
    /// @brief The longest answer one call may give.
    int maxTokens = 64000;
};

/// @brief The provider @p config names.
/// @param config What it is reached with.
/// @param owner  A QObject on a thread running a Qt event loop, where its network calls run. Borrowed: it outlives the
///               provider. `IProvider::stream` may be called from any thread.
/// @return The provider, or why not: an unknown name, or a missing key or model.
[[nodiscard]] std::expected<std::shared_ptr<ui::chat::IProvider>, std::string> makeQtChatProvider(
    ChatProviderConfig config, QObject* owner);

}  // namespace morph::qt
