// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../../core/wire.hpp"
#include "../../session/session.hpp"
#include "../interpreter.hpp"
#include "../service_client.hpp"
#include "dictation.hpp"
#include "protocol.hpp"

/// @file
/// @brief The client's side of a chat. Specified in `docs/spec/ui/chat.md`.

namespace morph::ui::chat {

/// @brief Sends what a person says, and keeps what to show for each conversation.
///
/// Events are hints and the server's `TurnState` is the record: the client reads it when a turn starts, when an event
/// says one ended, and on `resync`, and what it reads replaces what events built. What it shows lasts the session, and
/// only for display. The server keeps the conversation itself.
///
/// Everything runs on the owner: every call, and the exchange's replies. A turn belongs to the conversation it started
/// in, so switching conversations does not move it. Neither copyable nor movable.
class ChatClient {
public:
    /// @brief One line to show.
    struct Entry {
        /// @brief `user`, `assistant`, `tool`, `error`, or `note`: a line the client adds itself, such as why a
        /// dictation
        ///        failed, which the server never sees.
        std::string kind;
        /// @brief Its text; a `tool` entry's is the tool's name.
        std::string text;
        /// @brief A `tool` entry's arguments as JSON text; empty otherwise.
        std::string arguments;
        /// @brief Whether a `tool` entry's action ran.
        bool ok = false;
    };

    /// @brief A draft the server accepted, and where the turn that proposed it was started.
    struct DraftProposal {
        /// @brief The screen the turn was started on.
        std::string screenId;
        /// @brief That screen's identity, canonically encoded, as the turn's request carried it.
        std::string identity;
        /// @brief The `origin` the turn was started with, which a shell uses to name the tab.
        std::uint64_t origin = 0;
        /// @brief The accepted document as JSON text.
        std::string document;
    };

    /// @brief A client over @p exchange.
    /// @param exchange What envelopes go through; it answers on the owner.
    /// @param session  What each envelope carries, read at each send so a sign-in takes effect.
    /// @param changed  Called after anything shown has changed.
    ChatClient(Exchange exchange, std::function<session::Context()> session, std::function<void()> changed)
        : _exchange{std::move(exchange)}, _session{std::move(session)}, _changed{std::move(changed)} {}

    ChatClient(ChatClient const&) = delete;
    ChatClient& operator=(ChatClient const&) = delete;
    ChatClient(ChatClient&&) = delete;
    ChatClient& operator=(ChatClient&&) = delete;
    ~ChatClient() = default;

    /// @brief Asks whether this server offers this principal a chat, and dictation, which `available` and `dictation`
    ///        then answer.
    void probe() {
        send(std::string{kStateKind}, "{}", [this](ExchangeReply const& reply) {
            _available = reply.has_value() && reply->kind == "ok";
            _dictation.reset();
            if (_available) {
                auto const state = readTurnState(reply->body);
                if (state && state->dictation && state->dictation->maxSeconds > 0 && state->dictation->maxBytes > 0) {
                    _dictation = DictationLimits{
                        .maxSeconds = std::chrono::seconds{static_cast<std::int64_t>(state->dictation->maxSeconds)},
                        .maxBytes = static_cast<std::size_t>(state->dictation->maxBytes)};
                }
            }
            notify();
        });
    }

    /// @brief Whether the last probe found a chat.
    /// @return Whether it did.
    [[nodiscard]] bool available() const noexcept { return _available; }

    /// @brief The dictation the last probe found offered.
    /// @return Its bounds, or nothing when the server offers none.
    [[nodiscard]] std::optional<DictationLimits> dictation() const noexcept { return _dictation; }

    /// @brief Hands a dictated clip to the server, and answers with its text once the server's state says it is done.
    ///
    /// @p done runs exactly once: with the text when the state reads `done`; with the reason when it reads `failed`,
    /// when the server refuses the clip, when the exchange fails, on a reconnect (the old connection's close dropped
    /// the transcription), when `end` runs, and at once when another transcription is still waiting.
    /// @param wav      The clip, WAV bytes.
    /// @param language The language spoken; nothing leaves it to the server's default.
    /// @param done     What gets the text or the reason.
    void transcribe(std::string wav, std::optional<std::string> language,
                    std::function<void(std::expected<std::string, std::string>)> done) {
        if (_transcribing) {
            if (done) {
                done(std::unexpected(std::string{"a dictation is already being transcribed"}));
            }
            return;
        }
        std::uint64_t const seq = ++_transcribeSeq;
        _transcribing = Transcribing{.seq = seq, .id = 0, .done = std::move(done)};
        TranscribeRequest const request{.audio = detail::base64Encode(wav),
                                        .mediaType = std::string{kDictationMediaType},
                                        .language = std::move(language)};
        wav = std::string{};
        send(std::string{kTranscribeKind}, writeMessage(request),
             [this, seq](ExchangeReply const& reply) { transcribeAccepted(seq, reply); });
    }

    /// @brief Adds a line of the client's own to the selected conversation, such as why a dictation failed. It stays
    ///        when a running turn is redrawn, after the turn's lines, and is never sent.
    /// @param text The line.
    void note(std::string text) {
        _conversations[_selected].push_back(
            Entry{.kind = "note", .text = std::move(text), .arguments = {}, .ok = false});
        notify();
    }

    /// @brief Sets what receives a draft the server accepted. Called on the owner, once per distinct document of a
    ///        turn, from an event or from the state read; whichever carries it first.
    /// @param handler The receiver; empty drops drafts.
    void onDraft(std::function<void(DraftProposal const&)> handler) { _onDraft = std::move(handler); }

    /// @brief Starts a turn, and selects its conversation. Refused locally while a turn runs, as the server would.
    /// @param request What the person said, and where they are.
    /// @param origin  An opaque number given back with each draft the turn proposes, so a shell can tell which tab
    ///                the turn belongs to. The client never reads it.
    void send(TurnRequest const& request, std::uint64_t origin = 0) {
        if (_active) {
            return;
        }
        _selected = Key{request.screenId, request.identity};
        Conversation& conversation = _conversations[_selected];
        conversation.push_back(Entry{.kind = "user", .text = request.text, .arguments = {}, .ok = false});
        _active = Active{.key = _selected, .base = conversation.size(), .origin = origin};
        notify();
        send(std::string{kTurnKind}, writeMessage(request), [this](ExchangeReply const& reply) { accepted(reply); });
    }

    /// @brief Asks the server to stop the running turn.
    void cancel() {
        if (!_active) {
            return;
        }
        send(std::string{kCancelKind}, "{}", [this](ExchangeReply const& /*reply*/) { readState(); });
    }

    /// @brief Ends the chat: the server stops this connection's turn and drops the principal's conversations, and this
    ///        client forgets what it was showing. Sent ahead of a sign-out, while the session still proves who is
    ///        asking.
    /// @param done Called once the server answered or the exchange failed, so a sign-out never waits on a dead
    ///             connection.
    void end(std::function<void()> done) {
        _active.reset();
        _conversations.clear();
        _dictation.reset();
        if (_transcribing) {
            transcribed(std::unexpected(std::string{"the chat ended, so the dictation stopped"}));
        }
        notify();
        send(std::string{kEndKind}, "{}", [done = std::move(done)](ExchangeReply const& /*reply*/) {
            if (done) {
                done();
            }
        });
        // The analyzer loses track of the temporary std::function here; `send` moves it into a callback that owns it.
        // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks)
    }

    /// @brief Selects which conversation `entries` shows.
    /// @param screenId The screen.
    /// @param identity The screen's identity's canonical text; empty for none.
    void selectConversation(std::string screenId, std::string identity) {
        Key key{std::move(screenId), std::move(identity)};
        if (key != _selected) {
            _selected = std::move(key);
            notify();
        }
    }

    /// @brief Takes an event the server sent. A topic other than the chat's and `resync` is ignored.
    /// @param event The event.
    void deliverEvent(wire::EventBody const& event) {
        if (event.topic == wire::kResyncTopic) {
            readState();
            readTranscription();
            return;
        }
        if (event.topic != kChatTopic) {
            return;
        }
        if (auto const hint = readTranscriptionEvent(event.payload.str);
            hint && hint->kind == kTranscriptionEventKind) {
            if (_transcribing && _transcribing->id != 0 && hint->state.id == _transcribing->id) {
                readTranscription();
            }
            return;
        }
        if (!_active) {
            return;
        }
        auto const read = readTurnEvent(event.payload.str);
        if (!read || read->turnId != _active->turnId || read->seq <= _active->seq) {
            return;
        }
        _active->seq = read->seq;
        if (read->kind == "text") {
            // A piece joins the text before it unless a tool call came between, or the server said a round began.
            if (read->newRound || _active->parts.empty() || _active->parts.back().kind != "text") {
                _active->parts.push_back(TurnPart{.kind = "text", .text = read->text, .step = {}});
            } else {
                _active->parts.back().text += read->text;
            }
        } else if (read->kind == "tool") {
            _active->parts.push_back(TurnPart{
                .kind = "tool",
                .text = {},
                .step = ToolStep{.name = read->text, .ok = read->ok, .message = {}, .arguments = read->arguments}});
        } else if (read->kind == "draft") {
            hand(*_active, read->text);
            return;
        } else {
            readState();
            return;
        }
        show(*_active);
    }

    /// @brief Tells the client its connection was replaced. A running turn ended with the old connection, since a new
    ///        one has new instance ids; what was shown stays, and the conversation goes on from the next turn.
    void reconnected() {
        if (_active) {
            _active->error = "the connection was lost, so this turn stopped";
            show(*_active);
            _active.reset();
        }
        if (_transcribing) {
            transcribed(std::unexpected(std::string{"the connection was lost, so the dictation stopped"}));
        }
        probe();
    }

    /// @brief What the selected conversation shows.
    /// @return Its entries, oldest first.
    [[nodiscard]] std::vector<Entry> const& entries() const {
        static std::vector<Entry> const kNone;
        auto const found = _conversations.find(_selected);
        return found == _conversations.end() ? kNone : found->second;
    }

    /// @brief Whether a turn is running, in any conversation.
    /// @return Whether one is.
    [[nodiscard]] bool running() const noexcept { return _active.has_value(); }

private:
    using Key = std::pair<std::string, std::string>;
    using Conversation = std::vector<Entry>;

    // The running turn. `base` is where its entries start in its conversation, so what it shows can be redrawn.
    struct Active {
        Key key;
        std::size_t base = 0;
        std::uint64_t turnId = 0;
        std::uint64_t seq = 0;
        // What the turn made, in order: one part per round's text and per tool call.
        std::vector<TurnPart> parts;
        std::string error;
        // What `send` was given; handed back with each draft.
        std::uint64_t origin = 0;
        // The last draft handed on, so the event and the state that both carry it deliver it once.
        std::optional<std::string> handed;
    };

    // The transcription waiting for its text. `seq` tells this one's accept from an earlier one's; `id` is 0 until
    // the server accepted it.
    struct Transcribing {
        std::uint64_t seq = 0;
        std::uint64_t id = 0;
        std::function<void(std::expected<std::string, std::string>)> done;
    };

    void transcribeAccepted(std::uint64_t seq, ExchangeReply const& reply) {
        if (!_transcribing || _transcribing->seq != seq) {
            return;
        }
        if (!reply || reply->kind != "ok") {
            transcribed(std::unexpected(reply ? reply->message : reply.error()));
            return;
        }
        auto const read = readTranscribeAccepted(reply->body);
        if (!read || read->id == 0) {
            transcribed(
                std::unexpected(read ? std::string{"the dictation's reply names no transcription"} : read.error()));
            return;
        }
        _transcribing->id = read->id;
        readTranscription();
    }

    void readTranscription() {
        if (!_transcribing || _transcribing->id == 0) {
            return;
        }
        std::uint64_t const id = _transcribing->id;
        send(std::string{kTranscriptionKind}, writeMessage(TranscriptionQuery{.id = id}),
             [this, id](ExchangeReply const& reply) { transcriptionRead(id, reply); });
    }

    // A lost exchange leaves it waiting: the next event, a resync or a reconnect settles it.
    void transcriptionRead(std::uint64_t id, ExchangeReply const& reply) {
        if (!_transcribing || _transcribing->id != id || !reply) {
            return;
        }
        if (reply->kind != "ok") {
            transcribed(std::unexpected(reply->message));
            return;
        }
        auto const state = readTranscriptionState(reply->body);
        if (!state || state->status == TranscriptionStatus::Running) {
            return;
        }
        if (state->status == TranscriptionStatus::Done) {
            transcribed(state->text.value_or(std::string{}));
        } else {
            transcribed(std::unexpected(state->error.value_or(std::string{"the transcription failed"})));
        }
    }

    // Clears the waiting transcription before answering, so `done` may start the next one.
    void transcribed(std::expected<std::string, std::string> outcome) {
        auto done = std::move(_transcribing->done);
        _transcribing.reset();
        if (done) {
            done(std::move(outcome));
        }
    }

    // Hands a draft on unless this turn already did.
    void hand(Active& active, std::string const& document) {
        if (active.handed == document || !_onDraft) {
            active.handed = document;
            return;
        }
        active.handed = document;
        _onDraft(DraftProposal{.screenId = active.key.first,
                               .identity = active.key.second,
                               .origin = active.origin,
                               .document = document});
    }

    void notify() const {
        if (_changed) {
            _changed();
        }
    }

    // The callback holds a weak token, so a reply after this client is gone reaches nothing.
    void send(std::string kind, std::string body, std::function<void(ExchangeReply const&)> then) {
        wire::Envelope request;
        request.kind = std::move(kind);
        request.body = std::move(body);
        request.session = _session ? _session() : session::Context{};
        auto answer = [alive = std::weak_ptr<bool>{_alive}, then = std::move(then)](ExchangeReply const& reply) {
            if (!alive.expired()) {
                then(reply);
            }
        };
        // A transport that throws rather than answers is one more failed exchange: a sign-out waits on this answer.
        try {
            _exchange(std::move(request), answer);
        } catch (std::exception const& error) {
            answer(std::unexpected(std::string{error.what()}));
        }
    }

    void accepted(ExchangeReply const& reply) {
        if (!_active) {
            return;
        }
        if (!reply || reply->kind != "ok") {
            refused(*_active, reply ? reply->message : reply.error());
            return;
        }
        auto const read = readTurnAccepted(reply->body);
        if (!read) {
            refused(*_active, read.error());
            return;
        }
        _active->turnId = read->turnId;
        readState();
    }

    void refused(Active& active, std::string reason) {
        active.error = std::move(reason);
        show(active);
        _active.reset();
        notify();
    }

    void readState() {
        if (!_active || _active->turnId == 0) {
            return;
        }
        std::uint64_t const turnId = _active->turnId;
        send(std::string{kStateKind}, writeMessage(StateRequest{.turnId = turnId}),
             [this, turnId](ExchangeReply const& reply) { stateRead(turnId, reply); });
    }

    void stateRead(std::uint64_t turnId, ExchangeReply const& reply) {
        if (!_active || _active->turnId != turnId || !reply || reply->kind != "ok") {
            return;
        }
        auto const state = readTurnState(reply->body);
        if (!state) {
            return;
        }
        _active->seq = state->seq;
        _active->error = state->error;
        _active->parts = state->parts;
        if (_active->parts.empty()) {
            // A server that sends no parts is shown as it always was: the tool calls, then the text.
            for (ToolStep const& step : state->steps) {
                _active->parts.push_back(TurnPart{.kind = "tool", .text = {}, .step = step});
            }
            if (!state->text.empty()) {
                _active->parts.push_back(TurnPart{.kind = "text", .text = state->text, .step = {}});
            }
        }
        bool const running = state->status == TurnStatus::Running;
        show(*_active);
        if (_active && state->draft) {
            hand(*_active, *state->draft);
        }
        if (!running && _active) {
            _active.reset();
            notify();
        }
    }

    // Redraws the running turn's entries. Notes added since the turn began are the client's own lines: they are kept,
    // after the turn's.
    void show(Active const& active) {
        Conversation& conversation = _conversations[active.key];
        std::vector<Entry> notes;
        for (std::size_t index = active.base; index < conversation.size(); ++index) {
            if (conversation.at(index).kind == "note") {
                notes.push_back(std::move(conversation.at(index)));
            }
        }
        conversation.resize(active.base);
        for (TurnPart const& part : active.parts) {
            if (part.kind == "tool") {
                conversation.push_back(Entry{
                    .kind = "tool", .text = part.step.name, .arguments = part.step.arguments, .ok = part.step.ok});
            } else if (!part.text.empty()) {
                conversation.push_back(Entry{.kind = "assistant", .text = part.text, .arguments = {}, .ok = false});
            }
        }
        if (!active.error.empty()) {
            conversation.push_back(Entry{.kind = "error", .text = active.error, .arguments = {}, .ok = false});
        }
        for (Entry& kept : notes) {
            conversation.push_back(std::move(kept));
        }
        notify();
    }

    Exchange _exchange;
    std::function<session::Context()> _session;
    std::function<void()> _changed;
    std::function<void(DraftProposal const&)> _onDraft;
    std::shared_ptr<bool> _alive = std::make_shared<bool>(true);
    std::map<Key, Conversation> _conversations;
    Key _selected;
    std::optional<Active> _active;
    bool _available = false;
    std::optional<DictationLimits> _dictation;
    std::optional<Transcribing> _transcribing;
    std::uint64_t _transcribeSeq = 0;
};

}  // namespace morph::ui::chat
