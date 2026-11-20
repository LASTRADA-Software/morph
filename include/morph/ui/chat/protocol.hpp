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
/// @brief The envelope kind that hands a dictated clip to the server. Its body is a `TranscribeRequest`, its reply a
///        `TranscribeAccepted`.
inline constexpr std::string_view kTranscribeKind = "ui-chat-transcribe";
/// @brief The envelope kind that reads a transcription's state. Its body is a `TranscriptionQuery`, its reply a
///        `TranscriptionState`.
inline constexpr std::string_view kTranscriptionKind = "ui-chat-transcription";
/// @brief The `kind` of a `TranscriptionEvent` on the `chat` topic, which tells it from a `TurnEvent`.
inline constexpr std::string_view kTranscriptionEventKind = "transcription";
/// @brief The one media type a dictated clip may have: 16 kHz mono 16-bit PCM in a WAV container.
inline constexpr std::string_view kDictationMediaType = "audio/wav";

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
    /// @brief The screen's title as the person sees it, which is how they know the record; empty for none.
    std::string title;
    /// @brief The screen's identity value, canonically encoded; empty for a screen with no record.
    std::string identity;
    /// @brief The screen's params as JSON text.
    std::string paramsJson;
    /// @brief The aliases the screen holds.
    std::vector<ModelRef> models;
    /// @brief What the person said.
    std::string text;
    /// @brief The document of the draft the person has open on this screen, as JSON text; absent with none. It is the
    ///        screen's document as the assistant should read it, so a conversation of edits builds on the last
    ///        accepted draft. The server refuses a turn whose draft is over `ChatLimits::maxDraftBytes` or is not a
    ///        JSON object, and treats the text as data: it grants nothing.
    std::optional<std::string> draft;
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
    /// @brief The call's arguments as JSON text, cut at a character's edge when long.
    std::string arguments;
};

/// @brief One piece of a turn, in the order the turn made it.
struct TurnPart {
    /// @brief `text` for one round's text, `tool` for a tool call.
    std::string kind;
    /// @brief A `text` part's text.
    std::string text;
    /// @brief A `tool` part's call.
    ToolStep step;
};

/// @brief How far a turn has got.
enum class TurnStatus : std::uint8_t {
    Running,    ///< Still working.
    Done,       ///< Finished.
    Failed,     ///< Stopped by a fault or a limit; `TurnState::error` says which.
    Cancelled,  ///< Stopped by the person, or by its connection closing.
};

/// @brief The dictation a server offers, as its probe answers it.
struct DictationOffer {
    /// @brief The longest clip, in seconds.
    std::uint64_t maxSeconds = 0;
    /// @brief The largest clip, in bytes, before base64.
    std::uint64_t maxBytes = 0;
};

/// @brief A turn's state as the server holds it, which outranks any event.
struct TurnState {
    /// @brief The turn's id; `0` when the principal has no such turn.
    std::uint64_t turnId = 0;
    /// @brief How far it has got.
    TurnStatus status = TurnStatus::Running;
    /// @brief The reply so far: the rounds' texts, a blank line between one round and the next.
    std::string text;
    /// @brief Why it failed; empty otherwise.
    std::string error;
    /// @brief The tool calls it has made, in order.
    std::vector<ToolStep> steps;
    /// @brief The rounds' texts and the tool calls, in the order the turn made them. Empty from a server that does not
    ///        send it, which a client shows as `steps` and then `text`.
    std::vector<TurnPart> parts;
    /// @brief The `TurnEvent::seq` this state covers, so a client does not apply an event it already includes.
    std::uint64_t seq = 0;
    /// @brief The document of the last draft the server accepted this turn, as JSON text; absent when it accepted
    /// none.
    ///        A draft reaches a client only through here and the `draft` event, and only after the server's checks
    ///        passed it.
    std::optional<std::string> draft;
    /// @brief The dictation this server offers; set only in the answer to a request with no turn (the probe), and
    ///        absent when the server offers none. An older peer ignores it.
    std::optional<DictationOffer> dictation;
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
    /// @brief `text`, `tool`, `draft`, `done` or `error`.
    std::string kind;
    /// @brief The text piece, the tool's name, the reason, or a `draft` event's accepted document.
    std::string text;
    /// @brief A `tool` event's arguments as JSON text.
    std::string arguments;
    /// @brief A `tool` event's outcome: whether the action ran.
    bool ok = false;
    /// @brief Whether a `text` event is the first piece of a round that follows tool calls, so it starts a new text.
    bool newRound = false;
};

/// @brief A dictated clip handed to the server for transcription.
struct TranscribeRequest {
    /// @brief The clip, base64 (RFC 4648).
    std::string audio;
    /// @brief Its media type, which must be `kDictationMediaType`.
    std::string mediaType;
    /// @brief The language spoken, such as `en`; absent leaves it to the server's configured default.
    std::optional<std::string> language;
};

/// @brief The reply to a `TranscribeRequest`: the clip was taken and is being transcribed.
struct TranscribeAccepted {
    /// @brief The transcription's id, which its event and state carry.
    std::uint64_t id = 0;
};

/// @brief Asks for a transcription's state.
struct TranscriptionQuery {
    /// @brief The transcription.
    std::uint64_t id = 0;
};

/// @brief How far a transcription has got.
enum class TranscriptionStatus : std::uint8_t {
    Running,  ///< Still being transcribed.
    Done,     ///< Finished; `TranscriptionState::text` holds the text.
    Failed,   ///< Stopped by a fault, a limit or a cancel; `TranscriptionState::error` says which.
};

/// @brief A transcription's state as the server holds it, which outranks any event.
struct TranscriptionState {
    /// @brief The transcription's id.
    std::uint64_t id = 0;
    /// @brief How far it has got.
    TranscriptionStatus status = TranscriptionStatus::Running;
    /// @brief The text, once done.
    std::optional<std::string> text;
    /// @brief Why it failed, once failed.
    std::optional<std::string> error;
};

/// @brief A hint that a transcription ended, on the `chat` topic. Events can be lost, so `TranscriptionState` is the
///        record.
struct TranscriptionEvent {
    /// @brief Always `kTranscriptionEventKind`, which an older client reads as a turn event's kind and ignores.
    std::string kind = std::string{kTranscriptionEventKind};
    /// @brief The state it ended in.
    TranscriptionState state;
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
    return ui::detail::readMessage<TurnState>(text, "the chat turn's state", {"steps", "parts"}, {});
}

/// @brief Reads a `StateRequest`. An empty body is an empty request.
/// @param text The body.
/// @return The request, or why it was refused.
[[nodiscard]] inline std::expected<StateRequest, std::string> readStateRequest(std::string_view text) {
    return ui::detail::readMessage<StateRequest>(text.empty() ? std::string_view{"{}"} : text,
                                                 "the chat state request", {}, {});
}

/// @brief Reads a `TurnEvent`.
/// @param text The event's payload.
/// @return The event, or why it was refused.
[[nodiscard]] inline std::expected<TurnEvent, std::string> readTurnEvent(std::string_view text) {
    return ui::detail::readMessage<TurnEvent>(text, "the chat event", {}, {});
}

/// @brief Reads a `TranscribeRequest`.
/// @param text The body.
/// @return The request, or why it was refused.
[[nodiscard]] inline std::expected<TranscribeRequest, std::string> readTranscribeRequest(std::string_view text) {
    return ui::detail::readMessage<TranscribeRequest>(text, "the dictation", {}, {});
}

/// @brief Reads a `TranscribeAccepted`.
/// @param text The body.
/// @return The reply, or why it was refused.
[[nodiscard]] inline std::expected<TranscribeAccepted, std::string> readTranscribeAccepted(std::string_view text) {
    return ui::detail::readMessage<TranscribeAccepted>(text, "the dictation's reply", {}, {});
}

/// @brief Reads a `TranscriptionQuery`.
/// @param text The body.
/// @return The query, or why it was refused.
[[nodiscard]] inline std::expected<TranscriptionQuery, std::string> readTranscriptionQuery(std::string_view text) {
    return ui::detail::readMessage<TranscriptionQuery>(text, "the transcription query", {}, {});
}

/// @brief Reads a `TranscriptionState`.
/// @param text The body.
/// @return The state, or why it was refused.
[[nodiscard]] inline std::expected<TranscriptionState, std::string> readTranscriptionState(std::string_view text) {
    return ui::detail::readMessage<TranscriptionState>(text, "the transcription's state", {}, {});
}

/// @brief Reads a `TranscriptionEvent`.
/// @param text The event's payload.
/// @return The event, or why it was refused.
[[nodiscard]] inline std::expected<TranscriptionEvent, std::string> readTranscriptionEvent(std::string_view text) {
    return ui::detail::readMessage<TranscriptionEvent>(text, "the transcription event", {}, {});
}

}  // namespace morph::ui::chat

/// @brief Names a turn's status on the wire.
template <>
struct glz::meta<morph::ui::chat::TurnStatus> {
    using enum morph::ui::chat::TurnStatus;
    /// @brief The names.
    static constexpr auto value =
        glz::enumerate("running", Running, "done", Done, "failed", Failed, "cancelled", Cancelled);
};

/// @brief Names a transcription's status on the wire.
template <>
struct glz::meta<morph::ui::chat::TranscriptionStatus> {
    using enum morph::ui::chat::TranscriptionStatus;
    /// @brief The names.
    static constexpr auto value = glz::enumerate("running", Running, "done", Done, "failed", Failed);
};
