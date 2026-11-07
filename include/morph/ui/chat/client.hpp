// SPDX-License-Identifier: Apache-2.0
#pragma once

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
#include "../service_client.hpp"
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
        /// @brief `user`, `assistant`, `tool` or `error`.
        std::string kind;
        /// @brief Its text.
        std::string text;
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

    /// @brief Asks whether this server offers this principal a chat, which `available` then answers.
    void probe() {
        send(std::string{kStateKind}, "{}", [this](ExchangeReply const& reply) {
            _available = reply.has_value() && reply->kind == "ok";
            notify();
        });
    }

    /// @brief Whether the last probe found a chat.
    /// @return Whether it did.
    [[nodiscard]] bool available() const noexcept { return _available; }

    /// @brief Starts a turn, and selects its conversation. Refused locally while a turn runs, as the server would.
    /// @param request What the person said, and where they are.
    void send(TurnRequest const& request) {
        if (_active) {
            return;
        }
        _selected = Key{request.screenId, request.identity};
        Conversation& conversation = _conversations[_selected];
        conversation.push_back(Entry{.kind = "user", .text = request.text});
        _active = Active{.key = _selected, .base = conversation.size()};
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
            return;
        }
        if (event.topic != kChatTopic || !_active) {
            return;
        }
        auto const read = readTurnEvent(event.payload.str);
        if (!read || read->turnId != _active->turnId || read->seq <= _active->seq) {
            return;
        }
        _active->seq = read->seq;
        if (read->kind == "text") {
            _active->text += read->text;
        } else if (read->kind == "tool") {
            _active->tools.push_back(read->text);
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
        std::vector<std::string> tools;
        std::string text;
        std::string error;
    };

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
        _active->text = state->text;
        _active->error = state->error;
        _active->tools.clear();
        for (ToolStep const& step : state->steps) {
            _active->tools.push_back(step.name);
        }
        bool const running = state->status == TurnStatus::Running;
        show(*_active);
        if (!running) {
            _active.reset();
            notify();
        }
    }

    // Redraws the running turn's entries.
    void show(Active const& active) {
        Conversation& conversation = _conversations[active.key];
        conversation.resize(active.base);
        for (std::string const& tool : active.tools) {
            conversation.push_back(Entry{.kind = "tool", .text = tool});
        }
        if (!active.text.empty()) {
            conversation.push_back(Entry{.kind = "assistant", .text = active.text});
        }
        if (!active.error.empty()) {
            conversation.push_back(Entry{.kind = "error", .text = active.error});
        }
        notify();
    }

    Exchange _exchange;
    std::function<session::Context()> _session;
    std::function<void()> _changed;
    std::shared_ptr<bool> _alive = std::make_shared<bool>(true);
    std::map<Key, Conversation> _conversations;
    Key _selected;
    std::optional<Active> _active;
    bool _available = false;
};

}  // namespace morph::ui::chat
