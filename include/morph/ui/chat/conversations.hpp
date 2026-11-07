// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <chrono>
#include <compare>
#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "provider.hpp"

/// @file
/// @brief What a chat remembers between turns. Specified in `docs/spec/ui/chat.md`.

namespace morph::ui::chat {

/// @brief Bounds on what chats may consume.
struct ChatLimits {
    /// @brief Tool calls one turn may make, malformed ones included. A record's contents reach the model and could
    ///        steer it, so this is also what bounds the harm one turn can do.
    std::size_t maxToolCallsPerTurn = 16;
    /// @brief Turns one principal may have running.
    std::size_t maxTurnsPerPrincipal = 2;
    /// @brief Turns one connection may have running.
    std::size_t maxTurnsPerConnection = 1;
    /// @brief Conversations kept per principal; past it the least recently used is dropped.
    std::size_t maxConversationsPerPrincipal = 32;
    /// @brief Messages kept per conversation; past it the oldest are dropped.
    std::size_t maxMessagesPerConversation = 200;
    /// @brief The longest text one turn may carry.
    std::size_t maxTextBytes = std::size_t{64} * 1024;
    /// @brief How long an untouched conversation is kept.
    std::chrono::minutes idleRetention{std::chrono::hours{8}};
};

/// @brief Which conversation a turn belongs to.
struct ConversationKey {
    /// @brief The verified principal.
    std::string principal;
    /// @brief The screen's id.
    std::string screenId;
    /// @brief The screen's identity value, which is already what "this record" means to a document; empty for none.
    std::string identity;

    /// @brief Orders keys, for the store's map.
    /// @param other The other key.
    /// @return Their order.
    auto operator<=>(ConversationKey const& other) const = default;
};

/// @brief The conversations a server is holding, in memory, bounded by `ChatLimits`.
///
/// Not thread-safe: `ChatService` guards it. Nothing survives a restart, and the journal keeps what actually changed.
class ConversationStore {
public:
    /// @brief An empty store.
    /// @param limits Its bounds.
    explicit ConversationStore(ChatLimits const& limits) : _limits{limits} {}

    /// @brief A conversation's messages, oldest first. Reading it counts as using it.
    /// @param key The conversation.
    /// @param now The current time.
    /// @return Its messages; none when it does not exist or has been idle too long.
    [[nodiscard]] std::vector<Message> history(ConversationKey const& key, std::chrono::steady_clock::time_point now) {
        auto const found = _conversations.find(key);
        if (found == _conversations.end()) {
            return {};
        }
        if (now - found->second.touched > _limits.idleRetention) {
            _conversations.erase(found);
            return {};
        }
        found->second.touched = now;
        return found->second.messages;
    }

    /// @brief Adds a message, then applies every bound.
    /// @param key     The conversation, made if it does not exist.
    /// @param message The message.
    /// @param now     The current time.
    void append(ConversationKey const& key, Message message, std::chrono::steady_clock::time_point now) {
        std::erase_if(_conversations,
                      [&](auto const& held) { return now - held.second.touched > _limits.idleRetention; });
        Conversation& conversation = _conversations[key];
        conversation.touched = now;
        conversation.messages.push_back(std::move(message));
        trim(conversation.messages);
        evictFor(key);
    }

    /// @brief Ends every conversation of @p principal.
    /// @param principal The principal.
    void endAll(std::string_view principal) {
        std::erase_if(_conversations, [&](auto const& held) { return held.first.principal == principal; });
    }

    /// @brief The conversations held.
    /// @return Their number.
    [[nodiscard]] std::size_t size() const noexcept { return _conversations.size(); }

private:
    struct Conversation {
        std::vector<Message> messages;
        std::chrono::steady_clock::time_point touched;
    };

    // Past the bound the oldest go, then whatever would leave the history starting with anything but something a
    // person said: a provider refuses a history that opens with a reply, or with a result whose call is gone.
    void trim(std::vector<Message>& messages) const {
        if (messages.size() <= _limits.maxMessagesPerConversation) {
            return;
        }
        auto const excess = static_cast<std::ptrdiff_t>(messages.size() - _limits.maxMessagesPerConversation);
        messages.erase(messages.begin(), messages.begin() + excess);
        auto const start = std::ranges::find_if(
            messages, [](Message const& message) { return message.role == "user" && message.results.empty(); });
        messages.erase(messages.begin(), start);
        // A provider's opaque parts are valid only in the conversation that produced them, and this edited it.
        for (Message& message : messages) {
            message.opaque.clear();
        }
    }

    // Past the bound, the principal's least recently used conversation goes, but never the one in use.
    void evictFor(ConversationKey const& key) {
        std::size_t held = 0;
        auto oldest = _conversations.end();
        for (auto current = _conversations.begin(); current != _conversations.end(); ++current) {
            if (current->first.principal != key.principal) {
                continue;
            }
            ++held;
            if (current->first != key &&
                (oldest == _conversations.end() || current->second.touched < oldest->second.touched)) {
                oldest = current;
            }
        }
        if (held > _limits.maxConversationsPerPrincipal && oldest != _conversations.end()) {
            _conversations.erase(oldest);
        }
    }

    ChatLimits _limits;
    std::map<ConversationKey, Conversation> _conversations;
};

}  // namespace morph::ui::chat
