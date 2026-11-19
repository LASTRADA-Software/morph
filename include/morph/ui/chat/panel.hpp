// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include "../document.hpp"
#include "../host_vocabulary.hpp"
#include "../value.hpp"
#include "client.hpp"
#include "dictation.hpp"
#include "panel_id.hpp"
#include "protocol.hpp"

/// @file
/// @brief The chat's panel: a document like any screen, and what connects it to a `ChatClient`. Specified in
///        `docs/spec/ui/chat.md`.

namespace morph::ui::chat {

// clang-format off: the raw string is a JSON document laid out by hand, which clang-format would reflow.
/// @brief The panel. What it shows and does comes through the `chat.*` host values and commands a `ChatPanelBinding`
///        registers, so it is written in the same vocabulary as any screen and every frontend draws it.
inline constexpr std::string_view kChatPanelDocument = R"json({
  "vocab": "ui/1", "id": "morph.chat",
  "state": { "draft": { "init": "" } },
  "watch": [
    { "on": { "host": "chat.dictated" },
      "do": [ { "set": "draft", "value": { "if": [
        { "fn": "isEmpty", "args": [
          { "fn": "trim", "args": [ { "get": { "host": "chat.dictated" }, "path": "text" } ] } ] },
        { "ref": "draft" },
        { "if": [
        { "fn": "or", "args": [
          { "fn": "isEmpty", "args": [ { "ref": "draft" } ] },
          { "fn": "lt", "args": [
            { "fn": "length", "args": [
              { "fn": "trim", "args": [ { "fn": "concat", "args": [ "x", { "ref": "draft" } ] } ] } ] },
            { "fn": "length", "args": [ { "fn": "concat", "args": [ "x", { "ref": "draft" } ] } ] } ] } ] },
        { "fn": "concat", "args": [ { "ref": "draft" },
          { "fn": "trim", "args": [ { "get": { "host": "chat.dictated" }, "path": "text" } ] } ] },
        { "fn": "concat", "args": [ { "ref": "draft" }, " ",
          { "fn": "trim", "args": [ { "get": { "host": "chat.dictated" }, "path": "text" } ] } ] } ] } ] } } ] } ],
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
            "value": { "ref": "draft" }, "enabled": { "host": "chat.canSend" },
            "onChange": [ { "set": "draft", "value": { "ref": "event" } } ],
            "onSubmit": [ { "host": "chat.send", "args": { "obj": { "text": { "ref": "event" } } } },
                          { "set": "draft", "value": "" } ] },
          { "kind": "button", "testId": "chat_dictate", "icon": "chat.dictate",
            "label": { "if": [ { "fn": "eq", "args": [ { "host": "chat.dictation" }, "recording" ] },
                               { "fn": "format",
                                 "args": [ "Stop ({0} s)", { "host": "chat.dictationSeconds" } ] },
                               { "if": [ { "fn": "eq", "args": [ { "host": "chat.dictation" }, "waiting" ] },
                                         "Transcribing\u2026", "Dictate" ] } ] },
            "visible": { "fn": "or", "args": [ { "fn": "coalesce", "args": [ { "host": "chat.canDictate" }, false ] },
                                             { "fn": "eq", "args": [ { "host": "chat.dictation" }, "waiting" ] } ] },
            "enabled": { "fn": "ne", "args": [ { "host": "chat.dictation" }, "waiting" ] },
            "onClick": [ { "if": [ { "fn": "eq", "args": [ { "host": "chat.dictation" }, "recording" ] },
                                   [ { "host": "chat.dictateStop" } ],
                                   [ { "host": "chat.dictateStart" } ] ] } ] },
          { "kind": "button", "testId": "chat_send", "label": "Send", "enabled": { "host": "chat.canSend" },
            "onClick": [ { "host": "chat.send", "args": { "obj": { "text": { "ref": "draft" } } } },
                         { "set": "draft", "value": "" } ] },
          { "kind": "button", "testId": "chat_cancel", "label": "Stop", "emphasis": "ghost",
            "visible": { "host": "chat.running" }, "onClick": [ { "host": "chat.cancel" } ] } ] } ] } })json";
// clang-format on

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
///        `chat.onScreen` and `chat.canSend` to read, `chat.send` and `chat.cancel` to run, and, for the panel
///        document alone, the dictation names.
///
/// The commands are safe for an untrusted document: they do only what the person could do by typing, under their own
/// session, and every action they lead to is checked on the server. A shell calls `refresh` from the client's change
/// callback, and when the active tab changes. Neither copyable nor movable; on the runtime's owner.
class ChatPanelBinding {
public:
    /// @brief Registers the values and the commands.
    /// @param host     The vocabulary. Borrowed: it outlives this binding.
    /// @param client   The client. Borrowed: it outlives this binding.
    /// @param context  Where the person is now: the open screen's context, with no text. Read at each send, so a
    ///                 message goes to the screen they are looking at when they send it. Nothing for a screen with no
    ///                 document behind it, such as a host screen, which disables the input.
    /// @param origin   What the client hands back with each draft the turn proposes (`ChatClient::send`): read at
    ///                 each send, with the context, so it names the tab the message goes from. May be empty, which is
    ///                 0.
    /// @param draft    The text of the draft the open screen holds, read at each send and carried as the turn's
    ///                 `TurnRequest::draft`; nothing for a screen with no draft. May be empty, which is nothing.
    /// @param recorder What records a dictated clip; null offers no dictation. Borrowed: it outlives this binding,
    ///                 which sets its callbacks and clears them when it goes.
    /// @param visible  Whether the panel is in view, read at each `refresh`. A microphone never runs without its
    ///                 indicator in view, so empty, like `false`, offers no dictation.
    ChatPanelBinding(HostVocabulary& host, ChatClient& client, std::function<std::optional<TurnRequest>()> context,
                     std::function<std::uint64_t()> origin = {},
                     std::function<std::optional<std::string>()> draft = {}, IDictationRecorder* recorder = nullptr,
                     std::function<bool()> visible = {})
        : _host{&host},
          _client{&client},
          _context{std::move(context)},
          _origin{std::move(origin)},
          _draft{std::move(draft)},
          _recorder{recorder},
          _visible{std::move(visible)} {
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
        addDictation(host);
        refresh();
    }

    ChatPanelBinding(ChatPanelBinding const&) = delete;
    ChatPanelBinding& operator=(ChatPanelBinding const&) = delete;
    ChatPanelBinding(ChatPanelBinding&&) = delete;
    ChatPanelBinding& operator=(ChatPanelBinding&&) = delete;

    /// @brief Clears the recorder's callbacks, then stops a recording, whose clip then reaches no one.
    ~ChatPanelBinding() {
        if (_recorder == nullptr) {
            return;
        }
        _recorder->onStarted = nullptr;
        _recorder->onProgress = nullptr;
        _recorder->onClip = nullptr;
        if (_state == DictationState::Recording) {
            _recorder->stop();
        }
    }

    /// @brief Publishes what the client shows now, whether the person is on a screen, and the dictation's state. Stops
    ///        a recording and drops its clip when the panel is no longer visible, the dictation offer is gone (as
    ///        after the client's `end` at sign-out) or no document is behind the panel. The microphone never runs
    ///        without its indicator; the button also stays shown while a clip is being transcribed.
    void refresh() {
        ValueList entries;
        std::int64_t key = 0;
        for (ChatClient::Entry const& entry : _client->entries()) {
            entries.emplace_back(ValueRecord{{"key", Value{key++}}, {"role", roleOf(entry)}, {"text", textOf(entry)}});
        }
        bool const onScreen = _context && _context().has_value();
        bool const running = _client->running();
        if (_state == DictationState::Recording && !canDictate()) {
            dropRecording();
        }
        _host->set("chat.entries", Value{std::move(entries)});
        _host->set("chat.running", Value{running});
        _host->set("chat.onScreen", Value{onScreen});
        _host->set("chat.canSend", Value{onScreen && !running});
        publishDictation();
    }

private:
    enum class DictationState : std::uint8_t { Idle, Recording, Waiting };

    // Registered for the panel document alone: no screen, custom screen or draft may start the microphone or read a
    // transcript. Safe for an untrusted connection, since the panel document is the client's own.
    void addDictation(HostVocabulary& host) {
        std::set<std::string, std::less<>> const panel{std::string{kChatPanelId}};
        host.add(HostValue{
            .name = "chat.canDictate", .version = 1, .type = "bool", .initial = Value{false}, .documents = panel});
        host.add(HostValue{.name = "chat.dictation",
                           .version = 1,
                           .type = "string",
                           .initial = Value{std::string{"idle"}},
                           .documents = panel});
        host.add(HostValue{.name = "chat.dictationSeconds",
                           .version = 1,
                           .type = "int",
                           .initial = Value{std::int64_t{0}},
                           .documents = panel});
        host.add(
            HostValue{.name = "chat.dictated",
                      .version = 1,
                      .type = "record",
                      .initial = Value{ValueRecord{{"seq", Value{std::int64_t{0}}}, {"text", Value{std::string{}}}}},
                      .documents = panel});
        host.add(HostCommand{.name = "chat.dictateStart",
                             .version = 1,
                             .args = {},
                             .safeForUntrusted = true,
                             .run = [this](ValueRecord const& /*args*/) { startDictation(); },
                             .documents = panel});
        host.add(HostCommand{.name = "chat.dictateStop",
                             .version = 1,
                             .args = {},
                             .safeForUntrusted = true,
                             .run = [this](ValueRecord const& /*args*/) { stopDictation(); },
                             .documents = panel});
        if (_recorder == nullptr) {
            return;
        }
        _recorder->onStarted = [this](std::expected<void, std::string> started) {
            if (!started && _state == DictationState::Recording) {
                _state = DictationState::Idle;
                _client->note(std::move(started.error()));
                publishDictation();
            }
        };
        _recorder->onProgress = [this](std::chrono::seconds seconds) {
            if (_state == DictationState::Recording) {
                _seconds = seconds.count();
                publishDictation();
            }
        };
        _recorder->onClip = [this](std::expected<std::string, std::string> clip) { clipped(std::move(clip)); };
    }

    [[nodiscard]] bool shown() const { return _visible && _visible(); }

    [[nodiscard]] bool canDictate() const {
        return _recorder != nullptr && _client->dictation().has_value() && !_recorder->unavailable().has_value() &&
               _context && _context().has_value() && shown();
    }

    void publishDictation() {
        static constexpr std::array<std::string_view, 3> kNames{"idle", "recording", "waiting"};
        _host->set("chat.canDictate", Value{canDictate()});
        _host->set("chat.dictation", Value{std::string{kNames.at(static_cast<std::size_t>(_state))}});
        _host->set("chat.dictationSeconds", Value{_seconds});
        // A transcript is not kept once the offer is gone: it is cleared, so nothing reads it after a sign-out.
        if (_hasDictated && !_client->dictation().has_value()) {
            _hasDictated = false;
            _host->set("chat.dictated",
                       Value{ValueRecord{{"seq", Value{_dictatedSeq}}, {"text", Value{std::string{}}}}});
        }
    }

    void startDictation() {
        if (_state != DictationState::Idle || !canDictate()) {
            return;
        }
        std::optional<DictationLimits> const offered = _client->dictation();
        if (!offered.has_value()) {
            return;
        }
        DictationLimits const limits = *offered;
        _state = DictationState::Recording;
        _seconds = 0;
        publishDictation();
        _recorder->start(limits);
    }

    void stopDictation() {
        if (_state == DictationState::Recording) {
            _recorder->stop();
        }
    }

    // The clip a stop now delivers is dropped: `stop` is always answered by exactly one `onClip`.
    void dropRecording() {
        _state = DictationState::Idle;
        _seconds = 0;
        ++_droppedClips;
        _recorder->stop();
    }

    void clipped(std::expected<std::string, std::string> clip) {
        if (_droppedClips > 0) {
            --_droppedClips;
            return;
        }
        if (_state != DictationState::Recording) {
            return;
        }
        if (!clip) {
            _state = DictationState::Idle;
            _client->note(std::move(clip.error()));
            publishDictation();
            return;
        }
        _state = DictationState::Waiting;
        publishDictation();
        _client->transcribe(std::move(*clip), std::nullopt,
                            [this, alive = std::weak_ptr<bool>{_alive}](std::expected<std::string, std::string> text) {
                                if (!alive.expired()) {
                                    transcribed(std::move(text));
                                }
                            });
    }

    // A transcript goes into the input through `chat.dictated`, never into a turn: the panel's watch appends it.
    void transcribed(std::expected<std::string, std::string> text) {
        _state = DictationState::Idle;
        if (!text) {
            _client->note(std::move(text.error()));
        } else if (std::ranges::all_of(*text, [](unsigned char const letter) { return std::isspace(letter); })) {
            _client->note("Nothing was heard in the recording");
        } else {
            ++_dictatedSeq;
            _hasDictated = true;
            _host->set("chat.dictated",
                       Value{ValueRecord{{"seq", Value{_dictatedSeq}}, {"text", Value{std::move(*text)}}}});
        }
        publishDictation();
    }

    [[nodiscard]] static Value roleOf(ChatClient::Entry const& entry) {
        if (entry.kind == "tool") {
            return Value{std::string{"muted"}};
        }
        return Value{std::string{entry.kind == "error" || entry.kind == "note" ? "error" : "normal"}};
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
        if (_draft) {
            request->draft = _draft();
        }
        _client->send(*request, _origin ? _origin() : std::uint64_t{0});
    }

    HostVocabulary* _host;
    ChatClient* _client;
    std::function<std::optional<TurnRequest>()> _context;
    std::function<std::uint64_t()> _origin;
    std::function<std::optional<std::string>()> _draft;
    IDictationRecorder* _recorder;
    std::function<bool()> _visible;
    DictationState _state = DictationState::Idle;
    std::int64_t _seconds = 0;
    std::int64_t _dictatedSeq = 0;
    bool _hasDictated = false;
    std::size_t _droppedClips = 0;
    std::shared_ptr<bool> _alive = std::make_shared<bool>(true);
};

}  // namespace morph::ui::chat
