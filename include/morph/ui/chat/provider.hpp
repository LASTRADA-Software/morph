// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../value.hpp"

/// @file
/// @brief The seam between a chat and the AI model behind it. Nothing outside an implementation of `IProvider` names a
///        provider. Specified in `docs/spec/ui/chat.md`.

namespace morph::ui::chat {

/// @brief A tool offered to the model.
struct ToolSpec {
    /// @brief The name it calls the tool by.
    std::string name;
    /// @brief What the tool does.
    std::string description;
    /// @brief The JSON schema of its arguments.
    Json inputSchema;
};

/// @brief A call the model made.
struct ToolCall {
    /// @brief The provider's id for it, which its result names.
    std::string id;
    /// @brief The tool's name, which may name no tool: a model's output is untrusted.
    std::string name;
    /// @brief The arguments as JSON text, which may not be an object.
    std::string argumentsJson;
};

/// @brief What a tool call came to.
struct ToolResult {
    /// @brief The call's `ToolCall::id`.
    std::string callId;
    /// @brief The result's text, or why it did not run.
    std::string text;
    /// @brief Whether it failed.
    bool isError = false;
};

/// @brief One message of a conversation.
struct Message {
    /// @brief `user` or `assistant`.
    std::string role;
    /// @brief Its text.
    std::string text;
    /// @brief The calls an assistant message made.
    std::vector<ToolCall> calls;
    /// @brief The results a user message reports.
    std::vector<ToolResult> results;
    /// @brief What a provider said it must get back unchanged for an assistant message, from `Response::opaque`; empty
    ///        once history around it has been edited, since it is valid only in the conversation that produced it.
    std::string opaque;
};

/// @brief What one provider call is asked.
struct Request {
    /// @brief The system text.
    std::string system;
    /// @brief The conversation so far, oldest first.
    std::vector<Message> messages;
    /// @brief The tools it may call.
    std::vector<ToolSpec> tools;
};

/// @brief What one provider call answered.
struct Response {
    /// @brief The whole text.
    std::string text;
    /// @brief The tools it wants called; none ends the turn.
    std::vector<ToolCall> calls;
    /// @brief Whatever this provider needs replayed unchanged for this message, such as a model's reasoning
    /// signatures.
    ///        Only the provider that wrote it reads it.
    std::string opaque;
};

/// @brief An outstanding provider call.
class IProviderCall {
public:
    IProviderCall() = default;
    IProviderCall(IProviderCall const&) = delete;
    IProviderCall& operator=(IProviderCall const&) = delete;
    IProviderCall(IProviderCall&&) = delete;
    IProviderCall& operator=(IProviderCall&&) = delete;
    virtual ~IProviderCall() = default;

    /// @brief Stops the call. Its sink hears nothing after this returns, unless a delivery had already begun. Callable
    ///        from any thread, more than once.
    virtual void cancel() = 0;
};

/// @brief Where a provider call delivers.
struct ProviderSink {
    /// @brief A piece of the text, from any thread, in order.
    std::function<void(std::string const&)> text;
    /// @brief The outcome, or why there is none, from any thread, exactly once unless cancelled. It may run before
    ///        `IProvider::stream` returns.
    std::function<void(std::expected<Response, std::string>)> done;
};

/// @brief An AI model behind a chat.
class IProvider {
public:
    IProvider() = default;
    IProvider(IProvider const&) = delete;
    IProvider& operator=(IProvider const&) = delete;
    IProvider(IProvider&&) = delete;
    IProvider& operator=(IProvider&&) = delete;
    virtual ~IProvider() = default;

    /// @brief Starts a call. Callable from any thread.
    /// @param request What to ask.
    /// @param sink    Where it delivers.
    /// @return The call, for cancelling it.
    [[nodiscard]] virtual std::shared_ptr<IProviderCall> stream(Request request, ProviderSink sink) = 0;
};

}  // namespace morph::ui::chat
