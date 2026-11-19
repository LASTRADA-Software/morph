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
#include <morph/ui/document.hpp>
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
    // Guarded by Shared::mutex.
    TurnState state;
    std::uint64_t seq = 0;
    std::shared_ptr<IProviderCall> call;
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
    std::map<std::uint64_t, TurnPtr> turns;
    std::map<std::string, std::vector<std::uint64_t>, std::less<>> finished;
    std::atomic<std::uint64_t> nextTurn{1};
    std::atomic<std::uint64_t> nextCall{1};

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
        }
        return wire::encode(wire::makeOk(request.callId, writeMessage(state)));
    }

    // Stops the principal's turn on this connection and drops their conversations. A turn stopped here remembers
    // nothing afterwards, because `remember` checks that it is still running under the same lock.
    [[nodiscard]] static std::string endChat(std::shared_ptr<Shared> const& self, wire::Envelope const& request,
                                             backend::ConnectionId cid) {
        auto const& principal = request.session.principal;
        stopWhere(self, [&](Turn const& turn) { return turn.cid == cid && turn.session.principal == principal; });
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
            if (auto const refused = refuseDraft(*parsed->draft, self->options.limits.maxProposalBytes)) {
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

    // Ends a turn, once, with a status. A cancelled one also cancels its provider call.
    static void finish(std::shared_ptr<Shared> const& self, TurnPtr const& turn, TurnStatus status,
                       std::string error = {}) {
        std::shared_ptr<IProviderCall> call;
        {
            std::scoped_lock const lock{self->mutex};
            if (turn->state.status != TurnStatus::Running) {
                return;
            }
            turn->state.status = status;
            turn->state.error = std::move(error);
            call = std::move(turn->call);
            turn->call = nullptr;
            auto& kept = self->finished[turn->session.principal];
            kept.push_back(turn->id);
            if (kept.size() > kFinishedTurnsKept) {
                self->turns.erase(kept.front());
                kept.erase(kept.begin());
            }
        }
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
        if (!document) {
            throw std::runtime_error{document.error()};
        }
        std::set<std::string, std::less<>> const permitted(catalog.permitted.begin(), catalog.permitted.end());
        turn->tools = screenTools(*document, turn->request.models, permitted, schemas,
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

    [[nodiscard]] static ChatToolContext contextOf(Shared const& self, Turn const& turn, ChatTool const& tool,
                                                   std::shared_ptr<void const> carried) {
        auto const failed = turn.extraFailures.find(tool.spec.name);
        return ChatToolContext{.session = turn.session,
                               .request = turn.request,
                               .bundle = self.options.bundle,
                               .limits = self.options.limits,
                               .failedCalls = failed == turn.extraFailures.end() ? 0U : failed->second,
                               .carried = std::move(carried)};
    }

    // Wraps a completion so it runs once, on the worker, and only while the turn does: a handler or an offer may
    // answer from any thread, twice, or after the turn stopped.
    template <typename Value>
    [[nodiscard]] static std::function<void(Value)> onWorker(std::shared_ptr<Shared> const& self, TurnPtr const& turn,
                                                             std::function<void(Value)> then) {
        auto const fired = std::make_shared<std::atomic<bool>>(false);
        return [self, turn, then = std::move(then), fired](Value value) {
            if (fired->exchange(true)) {
                return;
            }
            auto const held = std::make_shared<Value>(std::move(value));
            step(self, turn, [then, held] { then(std::move(*held)); });
        };
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
        auto const accepted = [self, turn, index, &tool](ChatToolOffer const& offer) {
            if (!offer.system.empty()) {
                turn->system += "\n\n" + offer.system;
            }
            if (offer.offered) {
                turn->extras.push_back(&tool);
                turn->carried.insert_or_assign(tool.spec.name, offer.carried);
            }
            offerExtras(self, turn, index + 1);
        };
        if (!tool.offer) {
            accepted(ChatToolOffer{.offered = true, .system = {}, .carried = nullptr});
            return;
        }
        auto const answered = onWorker<ChatToolOffer>(self, turn, accepted);
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
        auto const done = onWorker<ChatToolOutcome>(self, turn, [self, turn, call](ChatToolOutcome outcome) {
            finishExtra(self, turn, call, std::move(outcome));
        });
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
        if (outcome.isError) {
            ++turn->extraFailures[call.name];
        }
        // A failed call accepted nothing, whatever it carries.
        if (outcome.draft && !outcome.isError) {
            {
                std::scoped_lock const lock{self->mutex};
                turn->state.draft = *outcome.draft;
            }
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
            finish(self, turn, TurnStatus::Failed, std::move(*stop));
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
}

ChatService::~ChatService() {
    // Stopping a turn builds the event that announces it, which can fail to allocate. A destructor must not throw,
    // and that event is only a hint.
    try {
        Shared::stopWhere(_shared, [](Turn const&) { return true; });
        // NOLINTNEXTLINE(bugprone-empty-catch)
    } catch (...) {
    }
}

std::string ChatService::err(wire::Envelope const& request, std::string message) {
    return wire::encode(wire::makeErr(std::move(message), request.callId));
}

bool ChatService::serves(std::string_view kind) const {
    return _shared->options.provider != nullptr &&
           (kind == kTurnKind || kind == kStateKind || kind == kCancelKind || kind == kEndKind);
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
}

}  // namespace morph::ui::chat
