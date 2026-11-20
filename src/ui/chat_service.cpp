// SPDX-License-Identifier: Apache-2.0

#if defined(__GNUC__) && !defined(__clang__)
// GCC 16 at -O3 reports a memcpy of 2^63 bytes from the std::string copy inside `readMessage`'s refusal path once it
// inlines into a turn's callbacks. The length is a std::string's, bounded by max_size(); the report is a false
// positive.
#pragma GCC diagnostic ignored "-Wrestrict"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <morph/core/effects.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/logger.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/timeout_scheduler.hpp>
#include <morph/core/wire.hpp>
#include <morph/session/session.hpp>
#include <morph/ui/bundle.hpp>
#include <morph/ui/chat/config.hpp>
#include <morph/ui/chat/conversations.hpp>
#include <morph/ui/chat/extra_tool.hpp>
#include <morph/ui/chat/protocol.hpp>
#include <morph/ui/chat/provider.hpp>
#include <morph/ui/chat/screen_tools.hpp>
#include <morph/ui/chat/service.hpp>
#include <morph/ui/chat/transcriber.hpp>
#include <morph/ui/document.hpp>
#include <morph/ui/interpreter.hpp>
#include <morph/ui/service.hpp>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace morph::ui::chat {

void ChatOptions::configure(ChatConfig const& config) {
    limits = config.limits;
    if (!config.systemPrompt.empty()) {
        systemPrompt = config.systemPrompt;
    }
    transcriptionLanguage = config.transcription ? config.transcription->language : std::nullopt;
}

// A turn's fields other than `state`, `seq` and `call` belong to whichever worker task is running its one next
// step.
struct ChatService::Turn {
    std::uint64_t id = 0;
    backend::ConnectionId cid = 0;
    session::Context session;
    TurnRequest request;
    std::string system;
    std::vector<BoundTool> tools;
    // The extra tools this turn offers, which point into the service's options.
    std::vector<ChatTool const*> extras;
    // How many calls of each extra tool failed this turn.
    std::map<std::string, std::size_t, std::less<>> extraFailures;
    // What the document source read for a screen outside the bundle; read once, for every tool.
    std::optional<ChatScreenSource> external;
    // The schemas the turn read, by action id, for every extra tool.
    std::shared_ptr<std::map<std::string, Json, std::less<>> const> schemas;
    // What each offered tool's `offer` carried to its handler, by tool name.
    std::map<std::string, std::shared_ptr<void const>, std::less<>> carried;
    std::vector<Message> messages;
    std::deque<ToolCall> pending;
    std::vector<ToolResult> results;
    std::vector<ToolCall> roundCalls;
    std::string roundText;
    bool roundStreamed = false;
    std::size_t toolCalls = 0;
    // What the person said, until a round of the model's answer goes into the conversation with it.
    std::optional<Message> said;
    // The pending extra call's answer for when its deadline passes, and the timer that fires it. Guarded by
    // Shared::mutex; `deadlineSeq` tells a timer that fired late from the call it was set for.
    std::function<void()> deadline;
    std::uint64_t deadlineSeq = 0;
    ::morph::async::detail::TimeoutScheduler::Handle deadlineHandle = 0;
    // Guarded by Shared::mutex.
    TurnState state;
    std::uint64_t seq = 0;
    std::shared_ptr<IProviderCall> call;
};

// A dictated clip being transcribed, or one that ended and is kept for `transcriptionRetention`. `id`, `principal` and
// `cid` never change; the rest is guarded by Shared::mutex.
struct ChatService::Transcription {
    std::uint64_t id = 0;
    std::string principal;
    backend::ConnectionId cid = 0;
    TranscriptionStatus status = TranscriptionStatus::Running;
    std::string text;
    std::string error;
    std::shared_ptr<ITranscriberCall> call;
    ::morph::async::detail::TimeoutScheduler::Handle deadline = 0;
    std::chrono::steady_clock::time_point finishedAt;
};

// Everything a turn's callbacks need, kept alive by them after the service is gone.
struct ChatService::Shared {
    explicit Shared(ChatOptions given) : options{std::move(given)}, conversations{options.limits} {}

    // Finished turns are kept so a client that lost the last event can still read how one ended.
    static constexpr std::size_t kFinishedTurnsKept = 8;
    // A tool step is shown in a turn's state, which must fit one envelope, whatever the tool returned.
    static constexpr std::size_t kStepMessageBytes = 512;

    ChatOptions options;
    std::mutex mutex;
    // Guarded by `mutex`.
    ConversationStore conversations;
    std::weak_ptr<backend::RemoteServer> server;
    // Made at the first deadline, since a chat with no extra tool has none. Its thread only posts to the worker.
    std::unique_ptr<::morph::async::detail::TimeoutScheduler> timers;
    std::map<std::uint64_t, TurnPtr> turns;
    std::map<std::string, std::vector<std::uint64_t>, std::less<>> finished;
    std::atomic<std::uint64_t> nextTurn{1};
    std::atomic<std::uint64_t> nextCall{1};

    // Guarded by `mutex`.
    std::map<std::uint64_t, std::shared_ptr<Transcription>> transcriptions;
    std::atomic<std::uint64_t> nextTranscription{1};

    using TranscriptionPtr = std::shared_ptr<Transcription>;
    // Whether ending a transcription cancels its call, and whether its connection hears of it.
    enum class CancelCall : std::uint8_t { No, Yes };
    enum class Tell : std::uint8_t { No, Yes };

    void warnAboutOptOuts() const {
        if (options.bundle == nullptr) {
            return;
        }
        for (ActionRef const& action : options.bundle->allActions) {
            if (changesData(action.action) && !options.dispatcher->isLoggable(action.modelType, action.action)) {
                log::logWarn(
                    "[chat] {} changes data but opts out of logging, so what an assistant does with it "
                    "is not journaled",
                    action.action);
            }
        }
    }

    [[nodiscard]] static bool changesData(std::string_view action) {
        auto const declared = effects::effectsOf(action);
        return declared && std::ranges::any_of(*declared, [](effects::Effect const& effect) {
                   return effect.kind != effects::Kind::None;
               });
    }

    [[nodiscard]] std::string readState(wire::Envelope const& request) {
        auto const asked = readStateRequest(request.body);
        if (!asked) {
            return err(request, asked.error());
        }
        TurnState state;
        if (asked->turnId) {
            std::scoped_lock const lock{mutex};
            auto const found = turns.find(*asked->turnId);
            // The same answer for another principal's turn and for none, so ids reveal nothing.
            if (found != turns.end() && found->second->session.principal == request.session.principal) {
                state = found->second->state;
                state.seq = found->second->seq;
            }
        } else if (options.transcriber != nullptr) {
            state.dictation =
                DictationOffer{.maxSeconds = static_cast<std::uint64_t>(options.limits.maxDictationSeconds),
                               .maxBytes = static_cast<std::uint64_t>(options.limits.maxDictationBytes)};
        }
        return wire::encode(wire::makeOk(request.callId, writeMessage(state)));
    }

    // Stops the principal's turn on this connection and drops their conversations. A turn stopped here remembers
    // nothing afterwards, because `remember` checks that it is still running under the same lock.
    [[nodiscard]] static std::string endChat(std::shared_ptr<Shared> const& self, wire::Envelope const& request,
                                             backend::ConnectionId cid) {
        auto const& principal = request.session.principal;
        stopWhere(self, [&](Turn const& turn) { return turn.cid == cid && turn.session.principal == principal; });
        stopTranscriptionsWhere(
            self, [&](Transcription const& entry) { return entry.cid == cid && entry.principal == principal; },
            "cancelled", Tell::Yes);
        {
            std::scoped_lock const lock{self->mutex};
            self->conversations.endAll(principal);
        }
        return wire::encode(wire::makeOk(request.callId));
    }

    [[nodiscard]] std::chrono::steady_clock::time_point now() const {
        return options.now ? options.now() : std::chrono::steady_clock::now();
    }

    [[nodiscard]] static ConversationKey keyOf(Turn const& turn) {
        return ConversationKey{
            .principal = turn.session.principal, .screenId = turn.request.screenId, .identity = turn.request.identity};
    }

    // Adds complete messages to the turn's conversation, with what the person said ahead of the first. So a turn
    // stopped before the model answered leaves no trace: the next turn must not read a stopped request as still
    // wanted. One stopped later keeps what it did, since those changes happened. Whole rounds only, a call with
    // its results, so nothing is left that a provider would refuse.
    static void remember(Shared& self, Turn& turn, std::vector<Message> const& messages) {
        std::scoped_lock const lock{self.mutex};
        if (turn.state.status != TurnStatus::Running) {
            return;
        }
        if (turn.said) {
            self.conversations.append(keyOf(turn), *turn.said, self.now());
            turn.said.reset();
        }
        for (Message const& message : messages) {
            self.conversations.append(keyOf(turn), recorded(self, message), self.now());
        }
    }

    // @p message as the conversation keeps it: a call of an extra tool with the arguments that tool says to keep.
    [[nodiscard]] static Message recorded(Shared const& self, Message message) {
        for (ToolCall& call : message.calls) {
            ChatTool const* const tool = extraNamed(self, call.name);
            if (tool == nullptr || !tool->record) {
                continue;
            }
            try {
                call.argumentsJson = tool->record(call.argumentsJson);
            } catch (...) {
                call.argumentsJson = "{}";
            }
            // A provider's opaque replay holds the message as it wrote it, calls included, so it would carry what
            // `record` removed; it is valid only for an unedited history, and this edits it.
            message.opaque.clear();
        }
        return message;
    }

    // Why a client's draft cannot be used as the screen's document, or nothing. A draft is text from the client, so it
    // is bounded and parsed before anything reads it; being a JSON object is all this layer asks of it, since the
    // checks that decide whether a document may be saved belong to whatever proposes one.
    [[nodiscard]] static std::optional<std::string> refuseDraft(std::string const& draft, std::size_t limit) {
        if (draft.size() > limit) {
            return "the draft is longer than " + std::to_string(limit) + " bytes";
        }
        auto const parsed = parseJson(draft);
        if (!parsed || !std::holds_alternative<Json::object_t>(parsed->data)) {
            return "the draft is not a JSON object";
        }
        return std::nullopt;
    }

    [[nodiscard]] static std::string startTurn(std::shared_ptr<Shared> const& self, wire::Envelope const& request,
                                               backend::ConnectionId cid) {
        auto parsed = readTurnRequest(request.body);
        if (!parsed) {
            return err(request, parsed.error());
        }
        if (cid == 0) {
            return err(request, "a chat needs a connection");
        }
        if (parsed->text.empty()) {
            return err(request, "the message is empty");
        }
        if (parsed->text.size() > self->options.limits.maxTextBytes) {
            return err(request, "the message is too long");
        }
        if (parsed->draft) {
            if (auto const refused = refuseDraft(*parsed->draft, self->options.limits.maxDraftBytes)) {
                return err(request, *refused);
            }
        }
        auto turn = std::make_shared<Turn>();
        turn->id = self->nextTurn.fetch_add(1);
        turn->cid = cid;
        turn->session = request.session;
        turn->request = std::move(*parsed);
        turn->state.turnId = turn->id;
        {
            std::scoped_lock const lock{self->mutex};
            std::size_t onConnection = 0;
            std::size_t ofPrincipal = 0;
            for (auto const& [turnId, held] : self->turns) {
                static_cast<void>(turnId);
                if (held->state.status == TurnStatus::Running) {
                    onConnection += held->cid == cid ? 1U : 0U;
                    ofPrincipal += held->session.principal == turn->session.principal ? 1U : 0U;
                }
            }
            if (onConnection >= self->options.limits.maxTurnsPerConnection) {
                return err(request, "a turn is already running on this connection");
            }
            if (ofPrincipal >= self->options.limits.maxTurnsPerPrincipal) {
                return err(request, "too many turns are running");
            }
            self->turns.insert_or_assign(turn->id, turn);
        }
        step(self, turn, [self, turn] { readCatalog(self, turn); });
        return wire::encode(wire::makeOk(request.callId, writeMessage(TurnAccepted{.turnId = turn->id})));
    }

    // Runs one step of @p turn on the worker, unless it has stopped. A throw fails the turn and goes no further.
    static void step(std::shared_ptr<Shared> const& self, TurnPtr const& turn, std::function<void()> body) {
        try {
            self->options.worker->post([self, turn, body = std::move(body)] {
                if (!running(*self, *turn)) {
                    return;
                }
                try {
                    body();
                } catch (std::exception const& error) {
                    finish(self, turn, TurnStatus::Failed, error.what());
                } catch (...) {
                    finish(self, turn, TurnStatus::Failed, "the turn failed");
                }
            });
        } catch (std::exception const& error) {
            finish(self, turn, TurnStatus::Failed, error.what());
        }
    }

    [[nodiscard]] static bool running(Shared& self, Turn const& turn) {
        std::scoped_lock const lock{self.mutex};
        return turn.state.status == TurnStatus::Running;
    }

    // Takes the turn's pending deadline out, so it cannot fire, and the timer's handle with it. The caller holds the
    // lock and destroys the function outside it, since it holds the call's completion.
    [[nodiscard]] static std::function<void()> takeDeadline(
        Shared const& /*self*/, Turn& turn, std::optional<::morph::async::detail::TimeoutScheduler::Handle>& handle) {
        std::function<void()> pending = std::move(turn.deadline);
        turn.deadline = nullptr;
        ++turn.deadlineSeq;
        if (turn.deadlineHandle != 0) {
            handle = turn.deadlineHandle;
        }
        turn.deadlineHandle = 0;
        return pending;
    }

    static void cancelTimer(Shared& self,
                            std::optional<::morph::async::detail::TimeoutScheduler::Handle> const& handle) {
        if (!handle) {
            return;
        }
        ::morph::async::detail::TimeoutScheduler* timers = nullptr;
        {
            std::scoped_lock const lock{self.mutex};
            timers = self.timers.get();
        }
        if (timers != nullptr) {
            timers->cancel(*handle);
        }
    }

    // Ends a turn, once, with a status. A cancelled one also cancels its provider call.
    static void finish(std::shared_ptr<Shared> const& self, TurnPtr const& turn, TurnStatus status,
                       std::string error = {}) {
        std::shared_ptr<IProviderCall> call;
        std::function<void()> pending;
        std::optional<::morph::async::detail::TimeoutScheduler::Handle> handle;
        {
            std::scoped_lock const lock{self->mutex};
            if (turn->state.status != TurnStatus::Running) {
                return;
            }
            turn->state.status = status;
            turn->state.error = std::move(error);
            call = std::move(turn->call);
            turn->call = nullptr;
            pending = takeDeadline(*self, *turn, handle);
            auto& kept = self->finished[turn->session.principal];
            kept.push_back(turn->id);
            if (kept.size() > kFinishedTurnsKept) {
                self->turns.erase(kept.front());
                kept.erase(kept.begin());
            }
        }
        cancelTimer(*self, handle);
        pending = nullptr;
        if (call != nullptr) {
            call->cancel();
        }
        announce(*self, *turn, status == TurnStatus::Failed ? "error" : "done",
                 status == TurnStatus::Failed ? turn->state.error : std::string{});
    }

    // Stops each running turn @p which picks. Never blocks: a service callback runs on the server strand.
    template <typename Which>
    static void stopWhere(std::shared_ptr<Shared> const& self, Which const& which) {
        std::vector<TurnPtr> matched;
        {
            std::scoped_lock const lock{self->mutex};
            for (auto const& [turnId, turn] : self->turns) {
                static_cast<void>(turnId);
                if (turn->state.status == TurnStatus::Running && which(*turn)) {
                    matched.push_back(turn);
                }
            }
        }
        for (TurnPtr const& turn : matched) {
            finish(self, turn, TurnStatus::Cancelled);
        }
    }

    // A hint to the turn's connection. Lost events are expected, so nothing depends on this arriving.
    struct EventExtras {
        std::string arguments;
        bool ok = false;
        bool newRound = false;
    };

    static void announce(Shared& self, Turn& turn, std::string_view kind, std::string text) {
        announce(self, turn, kind, std::move(text), EventExtras{.arguments = {}, .ok = false, .newRound = false});
    }

    static void announce(Shared& self, Turn& turn, std::string_view kind, std::string text, EventExtras extras) {
        std::shared_ptr<backend::RemoteServer> server;
        std::uint64_t seq = 0;
        {
            std::scoped_lock const lock{self.mutex};
            server = self.server.lock();
            seq = ++turn.seq;
        }
        if (server == nullptr) {
            return;
        }
        server->post(turn.cid, wire::encode(wire::makeEvent(
                                   kChatTopic, writeMessage(TurnEvent{.turnId = turn.id,
                                                                      .seq = seq,
                                                                      .kind = std::string{kind},
                                                                      .text = std::move(text),
                                                                      .arguments = std::move(extras.arguments),
                                                                      .ok = extras.ok,
                                                                      .newRound = extras.newRound}))));
    }

    // Sends an envelope as the turn's client, unscoped, and continues on the worker.
    static void exchange(std::shared_ptr<Shared> const& self, TurnPtr const& turn, wire::Envelope env,
                         EnvelopeReply then) {
        auto const server = serverOf(*self);
        if (server == nullptr) {
            throw std::runtime_error{"the server is gone"};
        }
        env.callId = self->nextCall.fetch_add(1);
        env.session = turn->session;
        server->handle(
            wire::encode(env),
            [self, turn, then = std::move(then)](std::string const& text) {
                step(self, turn, [text, then] { then(wire::decode(text)); });
            },
            0);
    }

    [[nodiscard]] static std::shared_ptr<backend::RemoteServer> serverOf(Shared& self) {
        std::scoped_lock const lock{self.mutex};
        return self.server.lock();
    }

    // Drops finished transcriptions past their retention. The caller holds `mutex`.
    void forgetExpired() {
        auto const cutoff = now() - options.limits.transcriptionRetention;
        std::erase_if(transcriptions, [cutoff](auto const& held) {
            return held.second->status != TranscriptionStatus::Running && held.second->finishedAt <= cutoff;
        });
    }

    [[nodiscard]] static TranscriptionState stateOf(Transcription const& entry) {
        TranscriptionState state{.id = entry.id, .status = entry.status, .text = std::nullopt, .error = std::nullopt};
        if (entry.status == TranscriptionStatus::Done) {
            state.text = entry.text;
        } else if (entry.status == TranscriptionStatus::Failed) {
            state.error = entry.error;
        }
        return state;
    }

    // A hint to the transcription's connection. Lost events are expected: the client reads the state.
    static void tellTranscription(Shared& self, backend::ConnectionId cid, TranscriptionState state) {
        auto const server = serverOf(self);
        if (server == nullptr) {
            return;
        }
        server->post(cid, wire::encode(wire::makeEvent(
                              kChatTopic, writeMessage(TranscriptionEvent{.kind = std::string{kTranscriptionEventKind},
                                                                          .state = std::move(state)}))));
    }

    // Ends a transcription, once: with its text, or with why it failed. Whatever ends it second finds it ended and
    // does nothing, which is what makes a late answer harmless.
    static void endTranscription(std::shared_ptr<Shared> const& self, TranscriptionPtr const& entry,
                                 std::expected<std::string, std::string> outcome, CancelCall cancel, Tell tell) {
        std::shared_ptr<ITranscriberCall> call;
        std::optional<::morph::async::detail::TimeoutScheduler::Handle> handle;
        TranscriptionState state;
        {
            std::scoped_lock const lock{self->mutex};
            if (entry->status != TranscriptionStatus::Running) {
                return;
            }
            if (outcome) {
                entry->status = TranscriptionStatus::Done;
                entry->text = std::move(*outcome);
            } else {
                entry->status = TranscriptionStatus::Failed;
                entry->error = std::move(outcome.error());
            }
            entry->finishedAt = self->now();
            call = std::move(entry->call);
            entry->call = nullptr;
            if (entry->deadline != 0) {
                handle = entry->deadline;
            }
            entry->deadline = 0;
            state = stateOf(*entry);
        }
        cancelTimer(*self, handle);
        if (call != nullptr && cancel == CancelCall::Yes) {
            call->cancel();
        }
        if (tell == Tell::Yes) {
            tellTranscription(*self, entry->cid, std::move(state));
        }
    }

    [[nodiscard]] static bool transcribing(Shared& self, Transcription const& entry) {
        std::scoped_lock const lock{self.mutex};
        return entry.status == TranscriptionStatus::Running;
    }

    // Starts the transcriber's call on the worker. The clip moves into the call, so the service holds no audio once
    // it started. An entry that ended before its call existed has the call cancelled at once; cancelling one that
    // already answered does nothing.
    static void callTranscriber(std::shared_ptr<Shared> const& self, TranscriptionPtr const& entry,
                                TranscriptionInput input) {
        if (!transcribing(*self, *entry)) {
            return;
        }
        TranscriberSink sink{
            .done = [weakSelf = std::weak_ptr<Shared>{self}, weakEntry = std::weak_ptr<Transcription>{entry}](
                        std::expected<std::string, std::string> outcome) {
                auto const lockedSelf = weakSelf.lock();
                auto const lockedEntry = weakEntry.lock();
                if (lockedSelf != nullptr && lockedEntry != nullptr) {
                    endTranscription(lockedSelf, lockedEntry, std::move(outcome), CancelCall::No, Tell::Yes);
                }
            }};
        std::shared_ptr<ITranscriberCall> call;
        try {
            call = self->options.transcriber->transcribe(std::move(input), std::move(sink));
        } catch (std::exception const& error) {
            endTranscription(self, entry, std::unexpected("the transcription failed: " + std::string{error.what()}),
                             CancelCall::No, Tell::Yes);
            return;
        } catch (...) {
            endTranscription(self, entry, std::unexpected(std::string{"the transcription failed"}), CancelCall::No,
                             Tell::Yes);
            return;
        }
        bool ended = false;
        {
            std::scoped_lock const lock{self->mutex};
            if (entry->status == TranscriptionStatus::Running) {
                entry->call = call;
            } else {
                ended = true;
            }
        }
        if (ended && call != nullptr) {
            call->cancel();
        }
    }

    // Fails the entry when the transcriber has not answered within `toolCallTimeout`, and cancels its call. The timer
    // holds both weakly and hands the ending to the worker, as a turn's deadline does.
    static void armTranscriptionDeadline(std::shared_ptr<Shared> const& self, TranscriptionPtr const& entry) {
        auto const limit = self->options.limits.toolCallTimeout;
        if (limit.count() <= 0) {
            return;
        }
        // NOLINTNEXTLINE(misc-const-correctness): `schedule` is not const.
        ::morph::async::detail::TimeoutScheduler* timers = nullptr;
        {
            std::scoped_lock const lock{self->mutex};
            if (self->timers == nullptr) {
                self->timers = std::make_unique<::morph::async::detail::TimeoutScheduler>();
            }
            timers = self->timers.get();
        }
        auto const handle =
            timers->schedule(limit, [weakSelf = std::weak_ptr<Shared>{self},
                                     weakEntry = std::weak_ptr<Transcription>{entry}, worker = self->options.worker] {
                auto lockedSelf = weakSelf.lock();
                auto lockedEntry = weakEntry.lock();
                if (lockedSelf == nullptr || lockedEntry == nullptr) {
                    return;
                }
                try {
                    worker->post([lockedSelf = std::move(lockedSelf), lockedEntry = std::move(lockedEntry)] {
                        endTranscription(lockedSelf, lockedEntry,
                                         std::unexpected(std::string{"the transcription did not answer in time"}),
                                         CancelCall::Yes, Tell::Yes);
                    });
                    // NOLINTNEXTLINE(bugprone-empty-catch): the worker is gone, so the service is being torn down.
                } catch (...) {
                }
            });
        std::optional<::morph::async::detail::TimeoutScheduler::Handle> stale;
        {
            std::scoped_lock const lock{self->mutex};
            if (entry->status == TranscriptionStatus::Running) {
                entry->deadline = handle;
            } else {
                stale = handle;
            }
        }
        cancelTimer(*self, stale);
    }

    // Ends each running transcription @p which picks, cancelling its call. Never blocks: a service callback runs on
    // the server strand.
    template <typename Which>
    static void stopTranscriptionsWhere(std::shared_ptr<Shared> const& self, Which const& which,
                                        std::string const& reason, Tell tell) {
        std::vector<TranscriptionPtr> matched;
        {
            std::scoped_lock const lock{self->mutex};
            for (auto const& [key, held] : self->transcriptions) {
                static_cast<void>(key);
                if (held->status == TranscriptionStatus::Running && which(*held)) {
                    matched.push_back(held);
                }
            }
        }
        for (TranscriptionPtr const& entry : matched) {
            endTranscription(self, entry, std::unexpected(reason), CancelCall::Yes, tell);
        }
    }

    // `handle` runs on the server strand and must not block, so a clip is checked, recorded and accepted here, and
    // transcribed on the worker. The base64 text's size is checked before decoding, so an oversized body is refused
    // for its size, not for what it holds.
    [[nodiscard]] static std::string startTranscription(std::shared_ptr<Shared> const& self,
                                                        wire::Envelope const& request, backend::ConnectionId cid) {
        auto parsed = readTranscribeRequest(request.body);
        if (!parsed) {
            return err(request, parsed.error());
        }
        if (cid == 0) {
            return err(request, "dictation needs a connection");
        }
        ChatLimits const& limits = self->options.limits;
        std::string const bound =
            "the recording is larger than " + std::to_string(limits.maxDictationBytes) + " bytes";
        if (parsed->audio.size() > ((limits.maxDictationBytes + 2) / 3) * 4) {
            return err(request, bound);
        }
        std::optional<std::string> audio = ui::detail::base64Decode(parsed->audio);
        if (!audio) {
            return err(request, "the recording is not base64");
        }
        if (audio->size() > limits.maxDictationBytes) {
            return err(request, bound);
        }
        if (parsed->mediaType != kDictationMediaType) {
            return err(request, "the recording is not " + std::string{kDictationMediaType});
        }
        parsed->audio = std::string{};
        auto entry = std::make_shared<Transcription>();
        entry->id = self->nextTranscription.fetch_add(1);
        entry->principal = request.session.principal;
        entry->cid = cid;
        {
            std::scoped_lock const lock{self->mutex};
            self->forgetExpired();
            std::size_t onConnection = 0;
            std::size_t ofPrincipal = 0;
            for (auto const& [key, held] : self->transcriptions) {
                static_cast<void>(key);
                if (held->status == TranscriptionStatus::Running) {
                    onConnection += held->cid == cid ? 1U : 0U;
                    ofPrincipal += held->principal == entry->principal ? 1U : 0U;
                }
            }
            if (onConnection > 0) {
                return err(request, "busy: a dictation is already being transcribed on this connection");
            }
            if (ofPrincipal >= limits.maxTranscriptionsPerPrincipal) {
                return err(request, "too many dictations are being transcribed");
            }
            self->transcriptions.insert_or_assign(entry->id, entry);
        }
        TranscriptionInput input{
            .audio = std::move(*audio),
            .mediaType = std::move(parsed->mediaType),
            .language = parsed->language ? std::move(parsed->language) : self->options.transcriptionLanguage};
        try {
            self->options.worker->post(
                [self, entry, input = std::move(input)]() mutable { callTranscriber(self, entry, std::move(input)); });
        } catch (std::exception const& error) {
            std::scoped_lock const lock{self->mutex};
            self->transcriptions.erase(entry->id);
            return err(request, error.what());
        }
        // The entry is recorded and its call posted, so a deadline that cannot be armed ends the entry as failed: the
        // client hears of it through the event and the state, rather than waiting on a call nothing times.
        try {
            armTranscriptionDeadline(self, entry);
        } catch (...) {
            endTranscription(self, entry, std::unexpected(std::string{"the transcription could not be timed"}),
                             CancelCall::Yes, Tell::Yes);
        }
        return wire::encode(wire::makeOk(request.callId, writeMessage(TranscribeAccepted{.id = entry->id})));
    }

    // The same answer for another principal's transcription, one forgotten and one that never was, so ids reveal
    // nothing.
    [[nodiscard]] std::string readTranscription(wire::Envelope const& request) {
        auto const asked = readTranscriptionQuery(request.body);
        if (!asked) {
            return err(request, asked.error());
        }
        TranscriptionState state{.id = asked->id,
                                 .status = TranscriptionStatus::Failed,
                                 .text = std::nullopt,
                                 .error = std::string{"no such transcription"}};
        {
            std::scoped_lock const lock{mutex};
            forgetExpired();
            auto const found = transcriptions.find(asked->id);
            if (found != transcriptions.end() && found->second->principal == request.session.principal) {
                state = stateOf(*found->second);
            }
        }
        return wire::encode(wire::makeOk(request.callId, writeMessage(state)));
    }

    static void readCatalog(std::shared_ptr<Shared> const& self, TurnPtr const& turn) {
        wire::Envelope request;
        request.kind = std::string{kCatalogKind};
        exchange(self, turn, std::move(request), [self, turn](wire::Envelope const& reply) {
            if (reply.kind != "ok") {
                throw std::runtime_error{"the catalog could not be read: " + reply.message};
            }
            auto catalog = parseCatalogReply(reply.body);
            if (!catalog) {
                throw std::runtime_error{catalog.error()};
            }
            readSchemas(self, turn, std::make_shared<CatalogReply>(std::move(*catalog)));
        });
    }

    // The schemas of each model type the context bound, one at a time.
    static void readSchemas(std::shared_ptr<Shared> const& self, TurnPtr const& turn,
                            std::shared_ptr<CatalogReply> const& catalog) {
        auto const remaining = std::make_shared<std::set<std::string>>();
        for (ModelRef const& model : turn->request.models) {
            remaining->insert(model.modelType);
        }
        readNextSchema(self, turn, catalog, remaining, std::make_shared<std::map<std::string, Json, std::less<>>>());
    }

    static void readNextSchema(std::shared_ptr<Shared> const& self, TurnPtr const& turn,
                               std::shared_ptr<CatalogReply> const& catalog,
                               std::shared_ptr<std::set<std::string>> const& remaining,
                               std::shared_ptr<std::map<std::string, Json, std::less<>>> const& schemas) {
        if (remaining->empty()) {
            buildTools(self, turn, *catalog, *schemas);
            return;
        }
        std::string const type = *remaining->begin();
        remaining->erase(remaining->begin());
        exchange(self, turn, wire::makeSchemas(type),
                 [self, turn, catalog, remaining, schemas](wire::Envelope const& reply) {
                     if (reply.kind == "ok") {
                         if (auto parsed = parseJson(reply.body)) {
                             if (auto* const actions = std::get_if<Json::object_t>(&parsed->data)) {
                                 for (auto& [action, schema] : *actions) {
                                     schemas->insert_or_assign(action, std::move(schema));
                                 }
                             }
                         }
                     }
                     readNextSchema(self, turn, catalog, remaining, schemas);
                 });
    }

    // The document of a screen the principal may open, or why not. An unknown screen and a denied one read alike.
    [[nodiscard]] static std::expected<Document, std::string> documentOf(Shared const& self,
                                                                         CatalogReply const& catalog,
                                                                         std::string const& screenId) {
        auto const listed = std::ranges::find_if(
            catalog.entries, [&](CatalogEntry const& entry) { return entry.id == screenId && entry.available; });
        auto const served = std::ranges::find_if(
            self.options.bundle->screens, [&](ServedScreen const& screen) { return screen.entry.id == screenId; });
        if (listed == catalog.entries.end() || served == self.options.bundle->screens.end()) {
            return std::unexpected("this screen is not available to the chat");
        }
        auto const file = self.options.bundle->files.find(served->hash);
        if (file == self.options.bundle->files.end()) {
            return std::unexpected("this screen is not available to the chat");
        }
        auto loaded = loadDocumentText(file->second.bytes);
        if (!loaded) {
            return std::unexpected(loaded.error().message());
        }
        return std::move(*loaded);
    }

    static void buildTools(std::shared_ptr<Shared> const& self, TurnPtr const& turn, CatalogReply const& catalog,
                           std::map<std::string, Json, std::less<>> const& schemas) {
        auto const document = documentOf(*self, catalog, turn->request.screenId);
        if (document) {
            assemble(self, turn, catalog, schemas, *document);
            return;
        }
        bool const inBundle = std::ranges::any_of(self->options.bundle->screens, [&](ServedScreen const& screen) {
            return screen.entry.id == turn->request.screenId;
        });
        if (inBundle || !self->options.documentSource) {
            throw std::runtime_error{document.error()};
        }
        // A screen outside the bundle: its document comes from the source, under the turn's session.
        auto const held =
            std::make_shared<std::pair<CatalogReply, std::map<std::string, Json, std::less<>>>>(catalog, schemas);
        self->options.documentSource(turn->session, turn->request.screenId,
                                     onWorker<std::expected<ChatScreenSource, std::string>>(
                                         self, turn,
                                         [held](std::shared_ptr<Shared> const& liveSelf, TurnPtr const& liveTurn,
                                                std::expected<ChatScreenSource, std::string> read) {
                                             if (!read) {
                                                 throw std::runtime_error{read.error()};
                                             }
                                             auto loaded = loadDocumentText(read->document);
                                             if (!loaded) {
                                                 throw std::runtime_error{loaded.error().message()};
                                             }
                                             liveTurn->external = std::move(*read);
                                             assemble(liveSelf, liveTurn, held->first, held->second, *loaded);
                                         },
                                         std::optional<std::expected<ChatScreenSource, std::string>>{std::unexpected(
                                             std::string{"the screen's document could not be read in time"})}));
    }

    static void assemble(std::shared_ptr<Shared> const& self, TurnPtr const& turn, CatalogReply const& catalog,
                         std::map<std::string, Json, std::less<>> const& schemas, Document const& document) {
        std::set<std::string, std::less<>> const permitted(catalog.permitted.begin(), catalog.permitted.end());
        turn->schemas = std::make_shared<std::map<std::string, Json, std::less<>> const>(schemas);
        turn->tools = screenTools(document, turn->request.models, permitted, schemas,
                                  catalog.actions ? &*catalog.actions : nullptr);
        // An extra tool's name is the one the model is told, so a screen tool that took it is withheld.
        std::erase_if(turn->tools, [&](BoundTool const& tool) {
            bool const taken = extraNamed(*self, tool.spec.name) != nullptr;
            if (taken) {
                log::logWarn("[chat] the screen tool {} is withheld: an extra tool has its name", tool.spec.name);
            }
            return taken;
        });
        turn->system = systemText(*self, turn->request);
        offerExtras(self, turn, 0);
    }

    [[nodiscard]] static ChatTool const* extraNamed(Shared const& self, std::string_view name) {
        auto const found =
            std::ranges::find_if(self.options.tools, [&](ChatTool const& tool) { return tool.spec.name == name; });
        return found == self.options.tools.end() ? nullptr : &*found;
    }

    [[nodiscard]] static ChatToolContext contextOf(Shared& self, Turn const& turn, ChatTool const& tool,
                                                   std::shared_ptr<void const> carried) {
        auto const failed = turn.extraFailures.find(tool.spec.name);
        // The latest accepted draft, else the draft the request began with. `state.draft` is guarded by the mutex.
        std::optional<std::string> document = turn.request.draft;
        {
            std::scoped_lock const lock{self.mutex};
            if (turn.state.draft) {
                document = turn.state.draft;
            }
        }
        return ChatToolContext{.session = turn.session,
                               .request = turn.request,
                               .document = std::move(document),
                               .bundle = self.options.bundle,
                               .limits = self.options.limits,
                               .external = turn.external,
                               .failedCalls = failed == turn.extraFailures.end() ? 0U : failed->second,
                               .carried = std::move(carried),
                               .schemas = turn.schemas};
    }

    // What a completion runs once its answer reaches the worker: the service, the turn and the answer.
    template <typename Value>
    using Continuation = std::function<void(std::shared_ptr<Shared> const&, TurnPtr const&, Value)>;

    // Wraps a completion so it runs once, on the worker, and only while the turn does: a handler, an offer or the
    // document source may answer from any thread, twice, or after the turn stopped. With @p onTimeout, the call is
    // answered with it once `ChatLimits::toolCallTimeout` passes without an answer.
    //
    // The completion holds the service and the turn weakly, and @p then must not own either: a tool may keep the
    // completion for as long as it likes, and one that owned them would keep a stopped turn, or the whole service,
    // alive through its own handler, which the service owns. An answer that finds either gone is dropped.
    template <typename Value>
    [[nodiscard]] static std::function<void(Value)> onWorker(std::shared_ptr<Shared> const& self, TurnPtr const& turn,
                                                             Continuation<Value> then,
                                                             std::optional<Value> onTimeout = std::nullopt) {
        auto const fired = std::make_shared<std::atomic<bool>>(false);
        std::function<void(Value)> deliver = [weakSelf = std::weak_ptr<Shared>{self},
                                              weakTurn = std::weak_ptr<Turn>{turn}, then = std::move(then),
                                              fired](Value value) {
            if (fired->exchange(true)) {
                return;
            }
            auto lockedSelf = weakSelf.lock();
            auto lockedTurn = weakTurn.lock();
            if (lockedSelf == nullptr || lockedTurn == nullptr) {
                return;
            }
            auto const held = std::make_shared<Value>(std::move(value));
            step(lockedSelf, lockedTurn, [lockedSelf, lockedTurn, then, held] {
                disarm(lockedSelf, lockedTurn);
                then(lockedSelf, lockedTurn, std::move(*held));
            });
        };
        if (onTimeout) {
            armDeadline(self, turn, deliver, std::move(*onTimeout));
        }
        return deliver;
    }

    // Answers @p deliver with @p onTimeout when the call's time is up. The timer holds the turn and the service weakly
    // and hands the answer to the worker. The timer's thread may still end up releasing the last reference to either,
    // which destroys the scheduler from its own callback; that is safe because `TimeoutScheduler::fire` touches
    // nothing after running the callback, and `~IoLoop` detaches instead of joining when it runs on its own thread.
    template <typename Value>
    static void armDeadline(std::shared_ptr<Shared> const& self, TurnPtr const& turn,
                            std::function<void(Value)> const& deliver, Value onTimeout) {
        auto const limit = self->options.limits.toolCallTimeout;
        if (limit.count() <= 0) {
            return;
        }
        // NOLINTNEXTLINE(misc-const-correctness): `schedule` is not const; the template hides that from the check.
        ::morph::async::detail::TimeoutScheduler* timers = nullptr;
        std::uint64_t seq = 0;
        {
            std::scoped_lock const lock{self->mutex};
            if (self->timers == nullptr) {
                self->timers = std::make_unique<::morph::async::detail::TimeoutScheduler>();
            }
            timers = self->timers.get();
            seq = ++turn->deadlineSeq;
            turn->deadline = [deliver, value = std::move(onTimeout)] { deliver(value); };
        }
        auto const handle =
            timers->schedule(limit, [weakSelf = std::weak_ptr<Shared>{self}, weakTurn = std::weak_ptr<Turn>{turn}, seq,
                                     worker = self->options.worker] {
                auto lockedSelf = weakSelf.lock();
                auto lockedTurn = weakTurn.lock();
                if (lockedSelf == nullptr || lockedTurn == nullptr) {
                    return;
                }
                std::function<void()> fire;
                {
                    std::scoped_lock const lock{lockedSelf->mutex};
                    if (lockedTurn->deadlineSeq == seq) {
                        fire = std::move(lockedTurn->deadline);
                        lockedTurn->deadline = nullptr;
                    }
                }
                if (!fire) {
                    return;
                }
                try {
                    worker->post([fire = std::move(fire), lockedSelf = std::move(lockedSelf),
                                  lockedTurn = std::move(lockedTurn)] { fire(); });
                    // NOLINTNEXTLINE(bugprone-empty-catch): the worker is gone, so the turn is being torn down.
                } catch (...) {
                }
            });
        std::scoped_lock const lock{self->mutex};
        if (turn->deadlineSeq == seq) {
            turn->deadlineHandle = handle;
        }
    }

    // The call's answer arrived: its deadline is no longer needed.
    static void disarm(std::shared_ptr<Shared> const& self, TurnPtr const& turn) {
        std::function<void()> pending;
        std::optional<::morph::async::detail::TimeoutScheduler::Handle> handle;
        {
            std::scoped_lock const lock{self->mutex};
            pending = takeDeadline(*self, *turn, handle);
        }
        cancelTimer(*self, handle);
    }

    // Asks each extra tool, one at a time, whether this turn offers it, and then starts the model.
    // NOLINTNEXTLINE(misc-no-recursion): each pass takes the next of the service's fixed list of tools.
    static void offerExtras(std::shared_ptr<Shared> const& self, TurnPtr const& turn, std::size_t index) {
        if (index >= self->options.tools.size()) {
            Message said{.role = "user", .text = turn->request.text, .calls = {}, .results = {}, .opaque = {}};
            {
                std::scoped_lock const lock{self->mutex};
                turn->messages = self->conversations.history(keyOf(*turn), self->now());
            }
            turn->said = said;
            turn->messages.push_back(std::move(said));
            callProvider(self, turn);
            return;
        }
        ChatTool const& tool = self->options.tools.at(index);
        // NOLINTNEXTLINE(misc-no-recursion): each pass takes the next of the service's fixed list of tools.
        auto const accepted = [index, &tool](std::shared_ptr<Shared> const& liveSelf, TurnPtr const& liveTurn,
                                             ChatToolOffer const& offer) {
            if (!offer.system.empty()) {
                liveTurn->system += "\n\n" + offer.system;
            }
            if (offer.offered) {
                liveTurn->extras.push_back(&tool);
                liveTurn->carried.insert_or_assign(tool.spec.name, offer.carried);
            }
            offerExtras(liveSelf, liveTurn, index + 1);
        };
        if (!tool.offer) {
            accepted(self, turn, ChatToolOffer{.offered = true, .system = {}, .carried = nullptr});
            return;
        }
        auto const answered =
            onWorker<ChatToolOffer>(self, turn, accepted,
                                    ChatToolOffer{.offered = false,
                                                  .system = "You cannot use the tool " + tool.spec.name +
                                                            " in this turn: it did not answer in time.",
                                                  .carried = nullptr});
        try {
            tool.offer(contextOf(*self, *turn, tool, nullptr), answered);
        } catch (std::exception const& error) {
            log::logWarn("[chat] the extra tool {} failed to answer whether it is offered: {}", tool.spec.name,
                         error.what());
            answered(ChatToolOffer{.offered = false, .system = {}, .carried = nullptr});
        } catch (...) {
            answered(ChatToolOffer{.offered = false, .system = {}, .carried = nullptr});
        }
    }

    // Adds a piece of the model's text to the turn, which the caller's lock guards. A round's first piece opens a
    // new text part, and a blank line separates it from the rounds before, so one round's end and the next one's
    // start do not run together. Returns whether the piece opens a round that follows tool calls.
    static bool addText(Turn& turn, std::string const& piece) {
        TurnState& state = turn.state;
        bool const opens = !turn.roundStreamed;
        turn.roundStreamed = true;
        if (!opens) {
            state.parts.back().text += piece;
            state.text += piece;
            return false;
        }
        bool const afterTools = !state.parts.empty() && state.parts.back().kind == "tool";
        if (!state.text.empty()) {
            state.text += "\n\n";
        }
        state.parts.push_back(TurnPart{.kind = "text", .text = piece, .step = {}});
        state.text += piece;
        return afterTools;
    }

    [[nodiscard]] static std::string systemText(Shared const& self, TurnRequest const& request) {
        std::string text = self.options.systemPrompt;
        text += "\n\nThe open screen is " + request.screenId + ".";
        if (!request.title.empty()) {
            text += " The person sees it titled \"" + request.title +
                    "\" and knows the record by that name, so call it so, not by a key or an internal number.";
        }
        if (!request.paramsJson.empty()) {
            text += " Its params are " + request.paramsJson + ".";
        }
        for (ModelRef const& model : request.models) {
            if (!model.primary.empty()) {
                text += " Its model " + model.alias + " is working on " + model.primary + ".";
            }
        }
        return text;
    }

    static void callProvider(std::shared_ptr<Shared> const& self, TurnPtr const& turn) {
        turn->roundText.clear();
        turn->roundStreamed = false;
        Request request{.system = turn->system, .messages = turn->messages, .tools = {}};
        request.tools.reserve(turn->tools.size() + turn->extras.size());
        for (BoundTool const& tool : turn->tools) {
            request.tools.push_back(tool.spec);
        }
        for (ChatTool const* const tool : turn->extras) {
            request.tools.push_back(tool->spec);
        }
        ProviderSink sink;
        sink.text = [self, turn](std::string const& piece) {
            if (piece.empty()) {
                return;
            }
            bool newRound = false;
            {
                std::scoped_lock const lock{self->mutex};
                if (turn->state.status != TurnStatus::Running) {
                    return;
                }
                newRound = addText(*turn, piece);
            }
            announce(*self, *turn, "text", piece, EventExtras{.arguments = {}, .ok = false, .newRound = newRound});
        };
        sink.done = [self, turn](std::expected<Response, std::string> outcome) {
            auto const held = std::make_shared<std::expected<Response, std::string>>(std::move(outcome));
            step(self, turn, [self, turn, held] { answered(self, turn, std::move(*held)); });
        };
        auto const call = self->options.provider->stream(std::move(request), std::move(sink));
        bool stale = false;
        {
            std::scoped_lock const lock{self->mutex};
            stale = turn->state.status != TurnStatus::Running;
            if (!stale) {
                turn->call = call;
            }
        }
        // The turn stopped while the call was being started, so nothing else will cancel it.
        if (stale && call != nullptr) {
            call->cancel();
        }
    }

    static void answered(std::shared_ptr<Shared> const& self, TurnPtr const& turn,
                         std::expected<Response, std::string> outcome) {
        {
            std::scoped_lock const lock{self->mutex};
            turn->call = nullptr;
        }
        if (!outcome) {
            finish(self, turn, TurnStatus::Failed, outcome.error());
            return;
        }
        bool announceText = false;
        bool newRound = false;
        {
            std::scoped_lock const lock{self->mutex};
            if (!turn->roundStreamed && !outcome->text.empty()) {
                newRound = addText(*turn, outcome->text);
                announceText = true;
            }
        }
        if (announceText) {
            announce(*self, *turn, "text", outcome->text,
                     EventExtras{.arguments = {}, .ok = false, .newRound = newRound});
        }
        turn->messages.push_back(Message{.role = "assistant",
                                         .text = outcome->text,
                                         .calls = outcome->calls,
                                         .results = {},
                                         .opaque = std::move(outcome->opaque)});
        if (outcome->calls.empty()) {
            remember(*self, *turn, {turn->messages.back()});
            finish(self, turn, TurnStatus::Done);
            return;
        }
        turn->pending.assign(outcome->calls.begin(), outcome->calls.end());
        turn->results.clear();
        nextTool(self, turn);
    }

    // NOLINTNEXTLINE(misc-no-recursion): each pass takes one pending call, and maxToolCallsPerTurn bounds them.
    static void nextTool(std::shared_ptr<Shared> const& self, TurnPtr const& turn) {
        if (turn->pending.empty()) {
            Message const asked = turn->messages.back();
            turn->messages.push_back(
                Message{.role = "user", .text = {}, .calls = {}, .results = std::move(turn->results), .opaque = {}});
            turn->results.clear();
            remember(*self, *turn, {asked, turn->messages.back()});
            callProvider(self, turn);
            return;
        }
        std::size_t const cap = self->options.limits.maxToolCallsPerTurn;
        if (turn->toolCalls >= cap) {
            finish(self, turn, TurnStatus::Failed,
                   "the turn reached its limit of " + std::to_string(cap) + " tool calls");
            return;
        }
        ++turn->toolCalls;
        ToolCall const call = std::move(turn->pending.front());
        turn->pending.pop_front();
        auto const tool = std::ranges::find_if(
            turn->tools, [&](BoundTool const& candidate) { return candidate.spec.name == call.name; });
        auto const extra = std::ranges::find_if(
            turn->extras, [&](ChatTool const* candidate) { return candidate->spec.name == call.name; });
        if (tool == turn->tools.end() && extra == turn->extras.end()) {
            report(self, turn, call, false, "no tool on this screen is named " + call.name);
            return;
        }
        auto arguments = parseJson(call.argumentsJson.empty() ? std::string_view{"{}"} : call.argumentsJson);
        if (!arguments || !std::holds_alternative<Json::object_t>(arguments->data)) {
            report(self, turn, call, false, "the arguments are not a JSON object");
            return;
        }
        if (extra != turn->extras.end()) {
            runExtra(self, turn, call, **extra, std::move(*arguments));
            return;
        }
        run(self, turn, call, *tool);
    }

    // Runs an extra tool's handler. Whatever it does, the call ends as a result the model reads: a handler that
    // throws is a failed call, not a fault of the turn.
    static void runExtra(std::shared_ptr<Shared> const& self, TurnPtr const& turn, ToolCall const& call,
                         ChatTool const& tool, Json arguments) {
        auto const carried = turn->carried.find(tool.spec.name);
        auto context = contextOf(*self, *turn, tool, carried == turn->carried.end() ? nullptr : carried->second);
        auto const done = onWorker<ChatToolOutcome>(
            self, turn,
            [call](std::shared_ptr<Shared> const& stillSelf, TurnPtr const& stillTurn, ChatToolOutcome outcome) {
                finishExtra(stillSelf, stillTurn, call, std::move(outcome));
            },
            ChatToolOutcome{.text = "the tool did not answer in time",
                            .isError = true,
                            .draft = std::nullopt,
                            .stop = std::nullopt});
        try {
            tool.handler(std::move(context), std::move(arguments), done);
        } catch (std::exception const& error) {
            log::logWarn("[chat] the extra tool {} threw: {}", tool.spec.name, error.what());
            done(ChatToolOutcome{
                .text = "the tool failed", .isError = true, .draft = std::nullopt, .stop = std::nullopt});
        } catch (...) {
            done(ChatToolOutcome{
                .text = "the tool failed", .isError = true, .draft = std::nullopt, .stop = std::nullopt});
        }
    }

    static void finishExtra(std::shared_ptr<Shared> const& self, TurnPtr const& turn, ToolCall const& call,
                            ChatToolOutcome outcome) {
        // A draft is bounded here whatever tool made it, so none can send the client more than the limit.
        std::size_t const limit = self->options.limits.maxDraftBytes;
        if (!outcome.isError && outcome.draft && outcome.draft->size() > limit) {
            outcome = ChatToolOutcome{.text = "the tool's draft is longer than " + std::to_string(limit) + " bytes",
                                      .isError = true,
                                      .draft = std::nullopt,
                                      .stop = std::move(outcome.stop)};
        }
        if (outcome.isError) {
            ++turn->extraFailures[call.name];
        }
        // A failed call accepted nothing, whatever it carries. A turn that stopped meanwhile records nothing, as
        // `remember` does: the status is read under the lock the draft is set under.
        bool const accepted = outcome.draft && !outcome.isError;
        {
            std::scoped_lock const lock{self->mutex};
            if (turn->state.status != TurnStatus::Running) {
                return;
            }
            if (accepted) {
                turn->state.draft = *outcome.draft;
            }
        }
        if (accepted) {
            announce(*self, *turn, "draft", *outcome.draft);
        }
        report(self, turn, call, !outcome.isError, outcome.text, std::move(outcome.stop));
    }

    static void run(std::shared_ptr<Shared> const& self, TurnPtr const& turn, ToolCall const& call,
                    BoundTool const& tool) {
        auto const server = serverOf(*self);
        if (server == nullptr) {
            throw std::runtime_error{"the server is gone"};
        }
        wire::Envelope env;
        env.modelId = tool.model.instanceId;
        env.modelType = tool.model.modelType;
        env.actionType = tool.action;
        env.body = call.argumentsJson.empty() ? std::string{"{}"} : call.argumentsJson;
        env.callId = self->nextCall.fetch_add(1);
        env.session = turn->session;
        bool const unrecorded =
            changesData(tool.action) && !self->options.dispatcher->isLoggable(tool.model.modelType, tool.action);
        server->executeWithOrigin(
            std::move(env), session::RunOrigin{.kind = "assistant", .text = turn->request.text},
            [self, turn, call, unrecorded](std::string const& text) {
                step(self, turn, [self, turn, call, unrecorded, text] {
                    auto const reply = wire::decode(text);
                    if (reply.kind == "ok") {
                        report(self, turn, call, true,
                               unrecorded ? reply.body + "\nThis action is not journaled." : reply.body);
                    } else {
                        report(self, turn, call, false, reply.message);
                    }
                });
            });
    }

    // @p text, cut at a character's edge when it is longer than @p limit, and saying so, so a model or a person
    // does not take a part for the whole.
    [[nodiscard]] static std::string bounded(std::string const& text, std::size_t limit) {
        if (text.size() <= limit) {
            return text;
        }
        std::size_t end = limit;
        while (end > 0 && (static_cast<unsigned char>(text.at(end)) & 0xC0U) == 0x80U) {
            --end;
        }
        return text.substr(0, end) + "\n[cut off: the whole was " + std::to_string(text.size()) + " bytes]";
    }

    // Records what a tool call came to, and moves on to the next one.
    // NOLINTNEXTLINE(misc-no-recursion): each pass takes one pending call, and maxToolCallsPerTurn bounds them.
    static void report(std::shared_ptr<Shared> const& self, TurnPtr const& turn, ToolCall const& call, bool succeeded,
                       std::string const& text, std::optional<std::string> stop = std::nullopt) {
        {
            std::scoped_lock const lock{self->mutex};
            ToolStep step{.name = call.name,
                          .ok = succeeded,
                          .message = bounded(text, kStepMessageBytes),
                          .arguments = bounded(call.argumentsJson, kStepMessageBytes)};
            turn->state.parts.push_back(TurnPart{.kind = "tool", .text = {}, .step = step});
            turn->state.steps.push_back(std::move(step));
        }
        turn->results.push_back(ToolResult{
            .callId = call.id, .text = bounded(text, self->options.limits.maxToolResultBytes), .isError = !succeeded});
        announce(*self, *turn, "tool", call.name,
                 EventExtras{
                     .arguments = bounded(call.argumentsJson, kStepMessageBytes), .ok = succeeded, .newRound = false});
        if (stop) {
            finish(self, turn, TurnStatus::Failed, bounded(*stop, kStepMessageBytes));
            return;
        }
        nextTool(self, turn);
    }
};

namespace {

// A tool name a provider's format accepts, without the `__` a screen's own tools carry.
bool validToolName(std::string const& name) {
    constexpr std::size_t kLongestName = 64;
    return !name.empty() && name.size() <= kLongestName && !name.contains("__") &&
           std::ranges::all_of(name, [](char chr) {
               return (chr >= 'a' && chr <= 'z') || (chr >= 'A' && chr <= 'Z') || (chr >= '0' && chr <= '9') ||
                      chr == '_' || chr == '-';
           });
}

}  // namespace

ChatService::ChatService(ChatOptions options) : _shared{std::make_shared<Shared>(std::move(options))} {
    std::set<std::string, std::less<>> names;
    for (ChatTool const& tool : _shared->options.tools) {
        if (!validToolName(tool.spec.name)) {
            throw std::invalid_argument{"ChatService: `" + tool.spec.name + "` is not a name an extra tool may have"};
        }
        if (!names.insert(tool.spec.name).second) {
            throw std::invalid_argument{"ChatService: two extra tools are named `" + tool.spec.name + "`"};
        }
        if (!tool.handler) {
            throw std::invalid_argument{"ChatService: the extra tool `" + tool.spec.name + "` has no handler"};
        }
    }
    if (_shared->options.provider != nullptr &&
        (_shared->options.worker == nullptr || _shared->options.bundle == nullptr)) {
        throw std::invalid_argument{"ChatService: a provider needs a worker and a bundle"};
    }
    if (_shared->options.transcriber != nullptr) {
        ChatLimits const& limits = _shared->options.limits;
        if (_shared->options.provider == nullptr) {
            throw std::invalid_argument{"ChatService: a transcriber needs a provider"};
        }
        if (limits.maxDictationSeconds == 0 || limits.maxDictationBytes == 0 ||
            limits.maxTranscriptionsPerPrincipal == 0 || limits.transcriptionRetention.count() <= 0) {
            throw std::invalid_argument{"ChatService: a dictation limit of 0 would refuse every dictation"};
        }
        if (limits.maxDictationBytes > kMaxDictationBytes) {
            throw std::invalid_argument{"ChatService: maxDictationBytes is above " +
                                        std::to_string(kMaxDictationBytes)};
        }
    }
}

ChatService::~ChatService() {
    // Stopping a turn builds the event that announces it, which can fail to allocate. A destructor must not throw,
    // and that event is only a hint.
    try {
        Shared::stopWhere(_shared, [](Turn const&) { return true; });
        // Nobody is left to read a transcription, so it is cancelled and announced to no one.
        Shared::stopTranscriptionsWhere(
            _shared, [](Transcription const&) { return true; }, "the chat service stopped", Shared::Tell::No);
        // NOLINTNEXTLINE(bugprone-empty-catch)
    } catch (...) {
    }
}

std::string ChatService::err(wire::Envelope const& request, std::string message) {
    return wire::encode(wire::makeErr(std::move(message), request.callId));
}

bool ChatService::serves(std::string_view kind) const {
    if (_shared->options.provider == nullptr) {
        return false;
    }
    if (kind == kTranscribeKind || kind == kTranscriptionKind) {
        return _shared->options.transcriber != nullptr;
    }
    return kind == kTurnKind || kind == kStateKind || kind == kCancelKind || kind == kEndKind;
}

std::string ChatService::handle(wire::Envelope const& request, session::IAuthorizer const& authorizer) {
    backend::ConnectionFacts facts;
    return handle(request, authorizer, facts);
}

std::string ChatService::handle(wire::Envelope const& request, session::IAuthorizer const& authorizer,
                                backend::ConnectionFacts& facts) {
    if (!authorizer.authorize(request.session, "ui", "chat")) {
        return err(request, std::string{wire::kUnauthorizedMessage});
    }
    if (request.kind == kTurnKind) {
        return Shared::startTurn(_shared, request, facts.cid);
    }
    if (request.kind == kStateKind) {
        return _shared->readState(request);
    }
    if (request.kind == kTranscribeKind) {
        return Shared::startTranscription(_shared, request, facts.cid);
    }
    if (request.kind == kTranscriptionKind) {
        return _shared->readTranscription(request);
    }
    if (request.kind == kCancelKind) {
        auto const principal = request.session.principal;
        auto const cid = facts.cid;
        Shared::stopWhere(_shared,
                          [&](Turn const& turn) { return turn.cid == cid && turn.session.principal == principal; });
        return wire::encode(wire::makeOk(request.callId));
    }
    return Shared::endChat(_shared, request, facts.cid);
}

void ChatService::bound(std::weak_ptr<backend::RemoteServer> const& server) {
    {
        std::scoped_lock const lock{_shared->mutex};
        _shared->server = server;
    }
    if (_shared->options.provider != nullptr) {
        _shared->warnAboutOptOuts();
    }
}

void ChatService::connectionClosed(backend::ConnectionId cid) {
    Shared::stopWhere(_shared, [cid](Turn const& turn) { return turn.cid == cid; });
    // No one can read a closed connection's transcriptions: they are cancelled silently and forgotten.
    Shared::stopTranscriptionsWhere(
        _shared, [cid](Transcription const& entry) { return entry.cid == cid; }, "the connection closed",
        Shared::Tell::No);
    std::scoped_lock const lock{_shared->mutex};
    std::erase_if(_shared->transcriptions, [cid](auto const& held) { return held.second->cid == cid; });
}

}  // namespace morph::ui::chat
