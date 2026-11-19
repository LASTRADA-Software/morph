// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "../../session/session.hpp"
#include "../value.hpp"
#include "conversations.hpp"
#include "protocol.hpp"
#include "provider.hpp"

/// @file
/// @brief A tool an application adds to the chat beside the tools its open screen names. Specified in
///        `docs/spec/ui/chat.md`.

namespace morph::ui {
struct ServedBundle;
}  // namespace morph::ui

namespace morph::ui::chat {

/// @brief What an extra tool is told about the turn that may call it.
///
/// Everything in it comes from the verified session or from the client's `TurnRequest`, which is a hint: a tool treats
/// the request's fields, and above all its draft, as data a person or a document's author may have written, and
/// checks what it does under `session`.
struct ChatToolContext {
    /// @brief The turn's session, its principal verified.
    session::Context session;
    /// @brief The screen the person is on: its id, title, identity, params, models and draft, and what they said.
    TurnRequest request;
    /// @brief The bundle the chat serves, which is where an application screen's own document is read.
    std::shared_ptr<ServedBundle const> bundle;
    /// @brief The chat's bounds.
    ChatLimits limits;
    /// @brief How many earlier calls of this tool, in this turn, came back as an error.
    std::size_t failedCalls = 0;
    /// @brief Whatever the tool's `offer` handed to `ChatToolOffer::carried`; null without one.
    std::shared_ptr<void const> carried;
};

/// @brief What a tool answers when it is asked whether this turn may call it.
struct ChatToolOffer {
    /// @brief Whether the tool is offered to the model in this turn.
    bool offered = false;
    /// @brief Text appended to the turn's system text, whether or not the tool is offered; empty for none. It is the
    ///        place to say what the tool is for, and, when it is not offered, why the assistant cannot do this.
    std::string system;
    /// @brief Whatever the tool's handler needs again from the work this answer took; reaches it as
    ///        `ChatToolContext::carried`.
    std::shared_ptr<void const> carried;
};

/// @brief What a tool call came to.
struct ChatToolOutcome {
    /// @brief What the model is told: the result's text, or why the call did not run.
    std::string text;
    /// @brief Whether the call failed. A failure is counted in `ChatToolContext::failedCalls` of later calls.
    bool isError = false;
    /// @brief A document the call accepted as the screen's draft: it becomes `TurnState::draft` and goes out as a
    ///        `draft` event. A tool sets it only for a document it has checked; a failed call's draft is dropped.
    std::optional<std::string> draft;
    /// @brief Set to end the turn after this call: the turn fails with this text, which the person reads. The call's
    ///        own result is recorded first.
    std::optional<std::string> stop;
};

/// @brief Where a handler delivers a call's outcome: from any thread, exactly once. A second delivery is ignored. A
///        turn that stopped meanwhile ignores it too.
using ChatToolDone = std::function<void(ChatToolOutcome)>;

/// @brief Where an offer delivers its answer: from any thread, exactly once.
using ChatToolOfferDone = std::function<void(ChatToolOffer)>;

/// @brief Runs one call of a tool.
///
/// Invoked on the turn's worker with the context by value, so a handler that finishes later keeps it. The arguments
/// are a JSON object: the service refuses any other call before it gets here. A handler that never delivers leaves the
/// turn waiting until it is cancelled, and one that throws reports the call as failed.
using ChatToolHandler = std::function<void(ChatToolContext, Json, ChatToolDone)>;

/// @brief Answers whether this turn may call a tool, once at its start. Invoked on the turn's worker.
using ChatToolOfferer = std::function<void(ChatToolContext, ChatToolOfferDone)>;

/// @brief Rewrites a call's arguments into what the conversation keeps of it; sees the arguments as JSON text and
///        answers JSON text for an object.
using ChatToolRecord = std::function<std::string(std::string const&)>;

/// @brief A tool added to a chat.
///
/// Its name is what the model calls it by: 1 to 64 characters of letters, digits, `_` and `-`, with no `__`, which
/// is what a screen's own tools' names carry, so the two kinds cannot be confused. Two tools of one chat cannot share
/// a name. `ChatService` refuses a tool that breaks either rule when it is built.
struct ChatTool {
    /// @brief What the model is offered: its name, what it does and the schema of its arguments.
    ToolSpec spec;
    /// @brief Whether this turn offers the tool. Empty: always offered, with nothing added to the system text.
    ChatToolOfferer offer;
    /// @brief What a call does. Required.
    ChatToolHandler handler;
    /// @brief What the conversation keeps of a call's arguments, so a large argument is not carried through every
    /// later
    ///        turn. Empty keeps them as the model wrote them. The turn itself still sees the whole call.
    ChatToolRecord record;
};

}  // namespace morph::ui::chat
