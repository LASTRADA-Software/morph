// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <glaze/glaze.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../bundle.hpp"

/// @file
/// @brief What a client and the chat service say to each other. Specified in `docs/spec/ui/chat.md`.

namespace morph::ui::chat {

/// @brief The envelope kind that starts a turn. Its body is a `TurnRequest`, its reply a `TurnAccepted`.
inline constexpr std::string_view kTurnKind = "ui-chat-turn";
/// @brief The envelope kind that reads a turn's state. Its body is a `StateRequest`, its reply a `TurnState`.
inline constexpr std::string_view kStateKind = "ui-chat-state";
/// @brief The envelope kind that cancels the connection's running turn.
inline constexpr std::string_view kCancelKind = "ui-chat-cancel";
/// @brief The envelope kind that ends the principal's chat: it stops their turns and drops their conversations.
inline constexpr std::string_view kEndKind = "ui-chat-end";
/// @brief The event topic a turn's progress goes out on. Its payload is a `TurnEvent`.
inline constexpr std::string_view kChatTopic = "chat";

/// @brief One model alias a screen holds, as the client knows it.
struct ModelRef {
    /// @brief The scope the alias is declared in; empty for the screen's own.
    std::string path;
    /// @brief The alias.
    std::string alias;
    /// @brief The model's registered type id.
    std::string modelType;
    /// @brief The instance the alias is bound to on the server.
    std::uint64_t instanceId = 0;
    /// @brief The instance's primary key; empty for one with none.
    std::string primary;
};

/// @brief One thing a person said, and where they were when they said it.
///
/// Everything here but `text` is a hint about what the person means. It grants nothing: every action it leads to is
/// checked under the session of the envelope that carried it.
struct TurnRequest {
    /// @brief The open screen's id.
    std::string screenId;
    /// @brief The screen's identity value, canonically encoded; empty for a screen with no record.
    std::string identity;
    /// @brief The screen's params as JSON text.
    std::string paramsJson;
    /// @brief The aliases the screen holds.
    std::vector<ModelRef> models;
    /// @brief What the person said.
    std::string text;
};

/// @brief The reply to a `TurnRequest`: the turn has been taken and runs on.
struct TurnAccepted {
    /// @brief The turn's id, which its events and state carry.
    std::uint64_t turnId = 0;
};

/// @brief One tool call a turn made.
struct ToolStep {
    /// @brief The tool's name.
    std::string name;
    /// @brief Whether the action ran.
    bool ok = false;
    /// @brief The action's result, or why it did not run.
    std::string message;
};

/// @brief How far a turn has got.
enum class TurnStatus : std::uint8_t {
    Running,    ///< Still working.
    Done,       ///< Finished.
    Failed,     ///< Stopped by a fault or a limit; `TurnState::error` says which.
    Cancelled,  ///< Stopped by the person, or by its connection closing.
};

/// @brief A turn's state as the server holds it, which outranks any event.
struct TurnState {
    /// @brief The turn's id; `0` when the principal has no such turn.
    std::uint64_t turnId = 0;
    /// @brief How far it has got.
    TurnStatus status = TurnStatus::Running;
    /// @brief The reply so far.
    std::string text;
    /// @brief Why it failed; empty otherwise.
    std::string error;
    /// @brief The tool calls it has made, in order.
    std::vector<ToolStep> steps;
};

/// @brief Asks for a turn's state.
struct StateRequest {
    /// @brief The turn. Empty asks only whether a chat is available to this principal.
    std::optional<std::uint64_t> turnId;
};

/// @brief A hint that a turn moved. Events can be lost, so `TurnState` is the record.
struct TurnEvent {
    /// @brief The turn's id.
    std::uint64_t turnId = 0;
    /// @brief Increases by one per event of a turn.
    std::uint64_t seq = 0;
    /// @brief `text`, `tool`, `done` or `error`.
    std::string kind;
    /// @brief The text piece, the tool's name, or the reason.
    std::string text;
};

/// @brief Writes a chat message as JSON.
/// @tparam T The message type.
/// @param value The message.
/// @return The JSON text.
template <typename T>
[[nodiscard]] std::string writeMessage(T const& value) {
    return ui::detail::writeMessage(value);
}

/// @brief Reads a `TurnRequest`.
/// @param text The body.
/// @return The request, or why it was refused.
[[nodiscard]] inline std::expected<TurnRequest, std::string> readTurnRequest(std::string_view text) {
    return ui::detail::readMessage<TurnRequest>(text, "the chat turn", {"models"}, {});
}

/// @brief Reads a `TurnAccepted`.
/// @param text The body.
/// @return The reply, or why it was refused.
[[nodiscard]] inline std::expected<TurnAccepted, std::string> readTurnAccepted(std::string_view text) {
    return ui::detail::readMessage<TurnAccepted>(text, "the chat turn's reply", {}, {});
}

/// @brief Reads a `TurnState`.
/// @param text The body.
/// @return The state, or why it was refused.
[[nodiscard]] inline std::expected<TurnState, std::string> readTurnState(std::string_view text) {
    return ui::detail::readMessage<TurnState>(text, "the chat turn's state", {"steps"}, {});
}

/// @brief Reads a `StateRequest`. An empty body is an empty request.
/// @param text The body.
/// @return The request, or why it was refused.
[[nodiscard]] inline std::expected<StateRequest, std::string> readStateRequest(std::string_view text) {
    return ui::detail::readMessage<StateRequest>(text.empty() ? std::string_view{"{}"} : text, "the chat state request",
                                                 {}, {});
}

/// @brief Reads a `TurnEvent`.
/// @param text The event's payload.
/// @return The event, or why it was refused.
[[nodiscard]] inline std::expected<TurnEvent, std::string> readTurnEvent(std::string_view text) {
    return ui::detail::readMessage<TurnEvent>(text, "the chat event", {}, {});
}

}  // namespace morph::ui::chat

/// @brief Names a turn's status on the wire.
template <>
struct glz::meta<morph::ui::chat::TurnStatus> {
    using enum morph::ui::chat::TurnStatus;
    /// @brief The names.
    static constexpr auto value = glz::enumerate("running", Running, "done", Done, "failed", Failed, "cancelled",
                                                 Cancelled);
};
