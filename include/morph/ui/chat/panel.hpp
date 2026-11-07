// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "../document.hpp"
#include "../host_vocabulary.hpp"
#include "../value.hpp"
#include "client.hpp"
#include "protocol.hpp"

/// @file
/// @brief The chat's panel: a document like any screen, and what connects it to a `ChatClient`. Specified in
///        `docs/spec/ui/chat.md`.

namespace morph::ui::chat {

/// @brief The panel document's id.
inline constexpr std::string_view kChatPanelId = "morph.chat";

/// @brief The panel. What it shows and does comes through the `chat.*` host values and commands a `ChatPanelBinding`
///        registers, so it is written in the same vocabulary as any screen and every frontend draws it.
inline constexpr std::string_view kChatPanelDocument = R"({
  "vocab": "ui/1", "id": "morph.chat",
  "state": { "draft": { "init": "" } },
  "view": { "kind": "column", "testId": "chat_panel", "gap": 2,
    "sizing": { "lit": { "width": { "stretch": 1 }, "height": { "stretch": 1 } } },
    "children": [
      { "kind": "scroll", "testId": "chat_entries",
        "sizing": { "lit": { "width": { "stretch": 1 }, "height": { "stretch": 1 } } },
        "children": [
          { "kind": "forEach", "gap": 2, "key": { "row": "key" },
            "rows": { "fn": "coalesce", "args": [ { "host": "chat.entries" }, { "lit": [] } ] },
            "children": [
              { "kind": "text", "testId": "chat_entry", "text": { "row": "text" }, "role": { "row": "role" } } ] } ] },
      { "kind": "row", "gap": 1, "align": "center",
        "sizing": { "lit": { "width": { "stretch": 1 } } },
        "children": [
          { "kind": "textInput", "testId": "chat_input", "placeholder": "Message",
            "sizing": { "lit": { "width": { "stretch": 1 } } },
            "value": { "ref": "draft" }, "enabled": { "host": "chat.onScreen" },
            "onChange": [ { "set": "draft", "value": { "ref": "event" } } ],
            "onSubmit": [ { "host": "chat.send", "args": { "obj": { "text": { "ref": "event" } } } },
                          { "set": "draft", "value": "" } ] },
          { "kind": "button", "testId": "chat_send", "label": "Send", "enabled": { "host": "chat.canSend" },
            "onClick": [ { "host": "chat.send", "args": { "obj": { "text": { "ref": "draft" } } } },
                         { "set": "draft", "value": "" } ] },
          { "kind": "button", "testId": "chat_cancel", "label": "Stop", "emphasis": "ghost",
            "visible": { "host": "chat.running" }, "onClick": [ { "host": "chat.cancel" } ] } ] } ] } })";

/// @brief Loads the panel document.
/// @return The document, or why it was refused.
[[nodiscard]] inline std::expected<std::shared_ptr<Document const>, Refusal> chatPanelDocument() {
    auto loaded = loadDocumentText(kChatPanelDocument);
    if (!loaded) {
        return std::unexpected(std::move(loaded.error()));
    }
    return std::make_shared<Document const>(std::move(*loaded));
}

/// @brief Connects the panel document to a client, through a `HostVocabulary`: `chat.entries`, `chat.running`,
///        `chat.onScreen` and `chat.canSend` to read, and `chat.send` and `chat.cancel` to run.
///
/// The commands are safe for an untrusted document: they do only what the person could do by typing, under their own
/// session, and every action they lead to is checked on the server. A shell calls `refresh` from the client's change
/// callback, and when the active tab changes. Neither copyable nor movable; on the runtime's owner.
class ChatPanelBinding {
public:
    /// @brief Registers the values and the commands.
    /// @param host    The vocabulary. Borrowed: it outlives this binding.
    /// @param client  The client. Borrowed: it outlives this binding.
    /// @param context Where the person is now: the open screen's context, with no text. Read at each send, so a
    /// message
    ///                goes to the screen they are looking at when they send it. Nothing for a screen with no document
    ///                behind it, such as a host screen, which disables the input.
    ChatPanelBinding(HostVocabulary& host, ChatClient& client, std::function<std::optional<TurnRequest>()> context)
        : _host{&host}, _client{&client}, _context{std::move(context)} {
        host.add(
            HostValue{.name = "chat.entries", .version = 1, .type = "list<record>", .initial = Value{ValueList{}}});
        host.add(HostValue{.name = "chat.running", .version = 1, .type = "bool", .initial = Value{false}});
        host.add(HostValue{.name = "chat.onScreen", .version = 1, .type = "bool", .initial = Value{false}});
        host.add(HostValue{.name = "chat.canSend", .version = 1, .type = "bool", .initial = Value{false}});
        host.add(HostCommand{.name = "chat.send",
                             .version = 1,
                             .args = {HostArg{.name = "text", .type = "string"}},
                             .safeForUntrusted = true,
                             .run = [this](ValueRecord const& args) { send(args); }});
        host.add(HostCommand{.name = "chat.cancel",
                             .version = 1,
                             .args = {},
                             .safeForUntrusted = true,
                             .run = [this](ValueRecord const& /*args*/) { _client->cancel(); }});
        refresh();
    }

    ChatPanelBinding(ChatPanelBinding const&) = delete;
    ChatPanelBinding& operator=(ChatPanelBinding const&) = delete;
    ChatPanelBinding(ChatPanelBinding&&) = delete;
    ChatPanelBinding& operator=(ChatPanelBinding&&) = delete;
    ~ChatPanelBinding() = default;

    /// @brief Publishes what the client shows now, and whether the person is on a screen.
    void refresh() {
        ValueList entries;
        std::int64_t key = 0;
        for (ChatClient::Entry const& entry : _client->entries()) {
            entries.emplace_back(ValueRecord{{"key", Value{key++}}, {"role", roleOf(entry)}, {"text", textOf(entry)}});
        }
        bool const onScreen = _context && _context().has_value();
        bool const running = _client->running();
        _host->set("chat.entries", Value{std::move(entries)});
        _host->set("chat.running", Value{running});
        _host->set("chat.onScreen", Value{onScreen});
        _host->set("chat.canSend", Value{onScreen && !running});
    }

private:
    [[nodiscard]] static Value roleOf(ChatClient::Entry const& entry) {
        if (entry.kind == "tool") {
            return Value{std::string{"muted"}};
        }
        return Value{std::string{entry.kind == "error" ? "error" : "normal"}};
    }

    [[nodiscard]] static Value textOf(ChatClient::Entry const& entry) {
        if (entry.kind == "user") {
            return Value{"You: " + entry.text};
        }
        return Value{entry.kind == "tool" ? "Ran " + entry.text : entry.text};
    }

    void send(ValueRecord const& args) {
        auto const given = args.find("text");
        if (given == args.end() || _client->running() || !_context) {
            return;
        }
        std::string const* const text = given->second.asString();
        if (text == nullptr ||
            std::ranges::all_of(*text, [](unsigned char const letter) { return std::isspace(letter); })) {
            return;
        }
        std::optional<TurnRequest> request = _context();
        if (!request) {
            return;
        }
        request->text = *text;
        _client->send(*request);
    }

    HostVocabulary* _host;
    ChatClient* _client;
    std::function<std::optional<TurnRequest>()> _context;
};

}  // namespace morph::ui::chat
