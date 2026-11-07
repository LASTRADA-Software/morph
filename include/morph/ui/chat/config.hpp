// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <glaze/glaze.hpp>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include "conversations.hpp"

/// @file
/// @brief What a chat is configured with, and an optional reader that fills it from a JSON file. An application may
///        fill `ChatConfig` from anywhere: the file is one way, not the way. Specified in `docs/spec/ui/chat.md`.

namespace morph::ui::chat {

/// @brief Which model provider a chat uses, and how to reach it.
struct ProviderConfig {
    /// @brief The provider's name, as whatever makes providers knows it.
    std::string name;
    /// @brief Where it is reached; empty for its public address.
    std::string baseUrl;
    /// @brief The secret it is called with. It appears in no log and no chat message.
    std::string apiKey;
    /// @brief The model. It has no default: which model a deployment pays for is the deployer's choice.
    std::string model;
    /// @brief The longest answer one call may give; empty leaves it to the provider's own handling.
    std::optional<int> maxTokens;
};

/// @brief Everything a chat is configured with.
struct ChatConfig {
    /// @brief The provider.
    ProviderConfig provider;
    /// @brief The bounds.
    ChatLimits limits;
    /// @brief What the assistant is told about how to behave; empty for the framework's default.
    std::string systemPrompt;
};

/// @brief Looks an environment variable up; nothing when it is not set.
using EnvironmentLookup = std::function<std::optional<std::string>(std::string_view)>;

namespace detail {

// The file's shape. Separate from `ChatConfig`, so a limit left out can be told from one given, and so the time limit
// can be spelled in minutes.
struct ProviderFile {
    std::string name;
    std::string baseUrl;
    std::optional<std::string> apiKey;
    std::optional<std::string> apiKeyEnv;
    std::string model;
    std::optional<int> maxTokens;
};

struct LimitsFile {
    std::optional<std::size_t> maxToolCallsPerTurn;
    std::optional<std::size_t> maxTurnsPerPrincipal;
    std::optional<std::size_t> maxTurnsPerConnection;
    std::optional<std::size_t> maxConversationsPerPrincipal;
    std::optional<std::size_t> maxMessagesPerConversation;
    std::optional<std::size_t> maxTextBytes;
    std::optional<std::size_t> maxToolResultBytes;
    std::optional<std::size_t> maxConversationBytes;
    std::optional<std::int64_t> idleRetentionMinutes;
};

struct ConfigFile {
    ProviderFile provider;
    LimitsFile limits;
    std::string systemPrompt;
};

/// @brief Looks a variable up in the process's environment.
/// @param name The variable.
/// @return Its value; nothing when it is not set.
[[nodiscard]] inline std::optional<std::string> processEnvironment(std::string_view name) {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read at startup, before a server's threads run.
    char const* const value = std::getenv(std::string{name}.c_str());
    return value == nullptr ? std::nullopt : std::optional<std::string>{std::string{value}};
}

}  // namespace detail

/// @brief Reads a chat's configuration from JSON text.
///
/// A key the format does not have is refused, so a misspelled limit cannot silently leave the default in force. What
/// is left out keeps its default. The key comes from `apiKeyEnv`, the name of an environment variable, or from an
/// inline `apiKey`; naming both is refused. A refusal says what is wrong without quoting the text, which may hold the
/// key.
/// @param text        The JSON text.
/// @param environment How `apiKeyEnv` is looked up; the process's environment by default.
/// @return The configuration, or why not.
[[nodiscard]] inline std::expected<ChatConfig, std::string> parseChatConfig(
    std::string_view text, EnvironmentLookup const& environment = detail::processEnvironment) {
    detail::ConfigFile file;
    if (auto const error = glz::read<glz::opts{.null_terminated = false, .error_on_unknown_keys = true}>(file, text)) {
        // Without the text, so the reason cannot quote a key held in it.
        return std::unexpected("the chat configuration cannot be read: " + std::string{glz::format_error(error)});
    }
    ChatConfig config;
    config.provider.name = std::move(file.provider.name);
    config.provider.baseUrl = std::move(file.provider.baseUrl);
    config.provider.model = std::move(file.provider.model);
    config.systemPrompt = std::move(file.systemPrompt);
    if (file.provider.apiKey && file.provider.apiKeyEnv) {
        return std::unexpected(std::string{"the chat configuration names both `apiKey` and `apiKeyEnv`"});
    }
    if (file.provider.apiKey) {
        config.provider.apiKey = std::move(*file.provider.apiKey);
    } else if (file.provider.apiKeyEnv) {
        auto key = environment ? environment(*file.provider.apiKeyEnv) : std::nullopt;
        if (!key) {
            return std::unexpected("the environment variable `" + *file.provider.apiKeyEnv +
                                   "` named by `apiKeyEnv` is not set");
        }
        config.provider.apiKey = std::move(*key);
    }
    if (file.provider.maxTokens) {
        if (*file.provider.maxTokens <= 0) {
            return std::unexpected(std::string{"the chat configuration's `maxTokens` must be above 0"});
        }
        config.provider.maxTokens = *file.provider.maxTokens;
    }
    // A limit of 0 would refuse every turn, so it can only be a mistake.
    auto const apply = [](std::optional<std::size_t> const& given, std::size_t& into, std::string_view name,
                          std::string& problem) {
        if (given) {
            if (*given == 0 && problem.empty()) {
                problem = "the chat configuration's `" + std::string{name} + "` must be above 0";
            }
            into = *given;
        }
    };
    std::string problem;
    detail::LimitsFile const& limits = file.limits;
    apply(limits.maxToolCallsPerTurn, config.limits.maxToolCallsPerTurn, "maxToolCallsPerTurn", problem);
    apply(limits.maxTurnsPerPrincipal, config.limits.maxTurnsPerPrincipal, "maxTurnsPerPrincipal", problem);
    apply(limits.maxTurnsPerConnection, config.limits.maxTurnsPerConnection, "maxTurnsPerConnection", problem);
    apply(limits.maxConversationsPerPrincipal, config.limits.maxConversationsPerPrincipal,
          "maxConversationsPerPrincipal", problem);
    apply(limits.maxMessagesPerConversation, config.limits.maxMessagesPerConversation, "maxMessagesPerConversation",
          problem);
    apply(limits.maxTextBytes, config.limits.maxTextBytes, "maxTextBytes", problem);
    apply(limits.maxToolResultBytes, config.limits.maxToolResultBytes, "maxToolResultBytes", problem);
    apply(limits.maxConversationBytes, config.limits.maxConversationBytes, "maxConversationBytes", problem);
    if (!problem.empty()) {
        return std::unexpected(std::move(problem));
    }
    if (limits.idleRetentionMinutes) {
        if (*limits.idleRetentionMinutes <= 0) {
            return std::unexpected(std::string{"the chat configuration's `idleRetentionMinutes` must be above 0"});
        }
        config.limits.idleRetention = std::chrono::minutes{*limits.idleRetentionMinutes};
    }
    return config;
}

/// @brief Reads a chat's configuration from a JSON file. See `parseChatConfig`.
/// @param path        The file.
/// @param environment How `apiKeyEnv` is looked up; the process's environment by default.
/// @return The configuration, or why not.
[[nodiscard]] inline std::expected<ChatConfig, std::string> loadChatConfig(
    std::filesystem::path const& path, EnvironmentLookup const& environment = detail::processEnvironment) {
    std::ifstream file{path};
    if (!file) {
        return std::unexpected("the chat configuration file `" + path.string() + "` cannot be opened");
    }
    std::ostringstream text;
    text << file.rdbuf();
    return parseChatConfig(text.str(), environment);
}

}  // namespace morph::ui::chat
