// SPDX-License-Identifier: Apache-2.0
//
// Dictation on the server: a clip accepted at once, transcribed on the worker, and its result read from the state the
// `transcription` event points at. Each refusal names its bound, and each transcription ends exactly once.

// clang-format off
#include <morph/ui/value.hpp>
// clang-format on

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <morph/core/executor.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/session/session.hpp>
#include <morph/ui/chat/config.hpp>
#include <morph/ui/chat/protocol.hpp>
#include <morph/ui/chat/service.hpp>
#include <morph/ui/chat/transcriber.hpp>
#include <morph/ui/service.hpp>
#include <morph/ui/testing/fake_chat_provider.hpp>
#include <morph/ui/testing/fake_transcriber.hpp>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "test_support.hpp"

namespace {

using namespace std::chrono_literals;
namespace ui = morph::ui;
namespace chat = morph::ui::chat;
namespace wire = morph::wire;
using morph::backend::ConnectionId;

constexpr std::string_view kScreen = R"({"vocab":"ui/1","id":"s","view":{"kind":"text","text":"S"}})";

// "RIFF" and four bytes, base64: what the service takes is opaque to it.
constexpr std::string_view kClip = "UklGRgABAgM=";

// The token is the principal; `muted` may not chat.
struct Authorizer final : morph::session::IAuthorizer {
    [[nodiscard]] bool authorize(morph::session::Context const& ctx, std::string_view modelType,
                                 std::string_view actionType) const override {
        return !(ctx.principal == "muted" && modelType == "ui" && actionType == "chat");
    }
    [[nodiscard]] std::optional<std::string> authenticate(morph::session::Context const& ctx) const override {
        return ctx.token.empty() ? std::nullopt : std::optional{ctx.token};
    }
    [[nodiscard]] bool authorizeInstance(morph::session::Context const& /*ctx*/, std::string_view /*modelType*/,
                                         std::string_view /*actionType*/, std::uint64_t /*modelId*/,
                                         std::string_view /*owner*/) const override {
        return true;
    }
};

struct EventWire {
    std::mutex mutex;
    std::vector<std::string> sent;

    morph::backend::ConnectionWriter writer() {
        return {.write =
                    [this](std::string frame) {
                        std::scoped_lock const lock{mutex};
                        sent.push_back(std::move(frame));
                    },
                .backlog = [] { return std::size_t{0}; }};
    }

    // The transcription events sent so far, oldest first.
    [[nodiscard]] std::vector<chat::TranscriptionState> transcriptions() {
        std::scoped_lock const lock{mutex};
        std::vector<chat::TranscriptionState> out;
        for (std::string const& frame : sent) {
            auto const env = wire::decode(frame);
            wire::EventBody body;
            if (glz::read<glz::opts{.error_on_unknown_keys = false}>(body, env.body) ||
                body.topic != chat::kChatTopic) {
                continue;
            }
            auto const event = chat::readTranscriptionEvent(body.payload.str);
            if (event && event->kind == chat::kTranscriptionEventKind) {
                out.push_back(event->state);
            }
        }
        return out;
    }
};

bool waitFor(auto done, std::chrono::milliseconds limit = 5s) {
    auto const deadline = std::chrono::steady_clock::now() + limit;
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        morph::testing::storageOwner().runFor(2ms);
    }
    return true;
}

std::shared_ptr<ui::ServedBundle const> bundleOf() {
    ui::UiRegistry registry{ui::ApplicationInfo{.id = "dictation", .version = "1"}};
    ui::CatalogEntry entry;
    entry.id = "s";
    REQUIRE(registry.addScreen(entry, std::string{kScreen}).has_value());
    auto built = registry.build();
    REQUIRE(built.has_value());
    return *built;
}

struct Rig {
    // Declared ahead of the pools, so it outlives the threads that write frames into it.
    EventWire eventWire;
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::ThreadPoolExecutor worker{2};
    std::shared_ptr<chat::testing::FakeChatProvider> provider = std::make_shared<chat::testing::FakeChatProvider>();
    std::shared_ptr<chat::testing::FakeTranscriber> fake = std::make_shared<chat::testing::FakeTranscriber>();
    // The service's clock, in whole seconds, which a test moves by hand.
    std::shared_ptr<std::atomic<std::int64_t>> clock = std::make_shared<std::atomic<std::int64_t>>(0);
    std::shared_ptr<morph::backend::RemoteServer> server;
    std::shared_ptr<chat::ChatService> service;
    std::atomic<std::uint64_t> nextCall{1};

    explicit Rig(chat::ChatLimits limits = {}, std::shared_ptr<chat::ITranscriber> const& other = nullptr,
                 bool withTranscriber = true) {
        auto const bundle = bundleOf();
        chat::ChatOptions options;
        options.provider = provider;
        options.worker = &worker;
        options.bundle = bundle;
        options.limits = limits;
        if (withTranscriber) {
            options.transcriber = other != nullptr ? other : std::shared_ptr<chat::ITranscriber>{fake};
        }
        options.transcriptionLanguage = "en";
        options.now = [clock = clock] {
            return std::chrono::steady_clock::time_point{std::chrono::seconds{clock->load()}};
        };
        service = std::make_shared<chat::ChatService>(std::move(options));
        morph::backend::ServerConfig config;
        config.services.push_back(std::make_shared<ui::UiService>(bundle));
        config.services.push_back(service);
        server =
            std::make_shared<morph::backend::RemoteServer>(pool, std::make_shared<Authorizer>(), std::move(config));
    }

    wire::Envelope call(wire::Envelope env, ConnectionId cid, std::string const& token = "alice") {
        env.callId = nextCall.fetch_add(1);
        env.session.token = token;
        env.session.principal = token;
        auto const replied = std::make_shared<std::promise<std::string>>();
        auto answer = replied->get_future();
        server->handle(wire::encode(env), [replied](std::string const& text) { replied->set_value(text); }, cid);
        REQUIRE(waitFor([&] { return answer.wait_for(0s) == std::future_status::ready; }));
        return wire::decode(answer.get());
    }

    ConnectionId gui(std::string const& token = "alice") {
        auto const cid = server->openConnection();
        server->setConnectionWriter(cid, eventWire.writer());
        wire::Envelope hello;
        hello.kind = std::string{ui::kHelloKind};
        hello.body = R"({"vocabularies":["ui/1","expr/1"],"events":true})";
        auto const reply = call(std::move(hello), cid, token);
        REQUIRE(reply.kind == "ok");
        return cid;
    }

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a test helper; the trailing arguments are rarely given.
    wire::Envelope transcribeRaw(std::string_view audio, ConnectionId cid, std::string const& token = "alice",
                                 std::string mediaType = "audio/wav", std::optional<std::string> language = {}) {
        wire::Envelope env;
        env.kind = std::string{chat::kTranscribeKind};
        env.body = chat::writeMessage(chat::TranscribeRequest{
            .audio = std::string{audio}, .mediaType = std::move(mediaType), .language = std::move(language)});
        return call(std::move(env), cid, token);
    }

    std::uint64_t transcribe(std::string_view audio, ConnectionId cid, std::string const& token = "alice",
                             std::optional<std::string> language = {}) {
        auto const reply = transcribeRaw(audio, cid, token, "audio/wav", std::move(language));
        INFO(reply.message);
        REQUIRE(reply.kind == "ok");
        auto const accepted = chat::readTranscribeAccepted(reply.body);
        REQUIRE(accepted.has_value());
        return accepted->id;
    }

    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a test helper; the token is almost always left out.
    chat::TranscriptionState state(std::uint64_t id, ConnectionId cid, std::string const& token = "alice") {
        wire::Envelope env;
        env.kind = std::string{chat::kTranscriptionKind};
        env.body = chat::writeMessage(chat::TranscriptionQuery{.id = id});
        auto const reply = call(std::move(env), cid, token);
        REQUIRE(reply.kind == "ok");
        auto const read = chat::readTranscriptionState(reply.body);
        REQUIRE(read.has_value());
        return *read;
    }

    chat::TranscriptionState settled(std::uint64_t id, ConnectionId cid, std::string const& token = "alice") {
        chat::TranscriptionState last;
        REQUIRE(waitFor([&] {
            last = state(id, cid, token);
            return last.status != chat::TranscriptionStatus::Running;
        }));
        return last;
    }

    chat::TurnState probe(ConnectionId cid) {
        wire::Envelope env;
        env.kind = std::string{chat::kStateKind};
        env.body = "{}";
        auto const reply = call(std::move(env), cid);
        REQUIRE(reply.kind == "ok");
        auto const read = chat::readTurnState(reply.body);
        REQUIRE(read.has_value());
        return *read;
    }
};

}  // namespace

// Mutation: apply no default language, or send the event to no one.
TEST_CASE("chat_dictation_is_accepted_at_once_and_read_from_the_state", "[chat][dictation]") {
    Rig rig;
    auto const cid = rig.gui();
    rig.fake->script(std::string{"hello there"});
    auto const id = rig.transcribe(kClip, cid);
    auto const state = rig.settled(id, cid);
    CHECK(state.status == chat::TranscriptionStatus::Done);
    CHECK(state.text == "hello there");
    CHECK_FALSE(state.error.has_value());
    REQUIRE(waitFor([&] { return !rig.eventWire.transcriptions().empty(); }));
    auto const told = rig.eventWire.transcriptions().front();
    CHECK(told.id == id);
    CHECK(told.status == chat::TranscriptionStatus::Done);

    auto const inputs = rig.fake->inputs();
    REQUIRE(inputs.size() == 1);
    CHECK(inputs.front().audio == std::string{"RIFF\x00\x01\x02\x03", 8});
    CHECK(inputs.front().mediaType == "audio/wav");
    CHECK(inputs.front().language == "en");

    rig.fake->script(std::string{"hallo"});
    static_cast<void>(rig.settled(rig.transcribe(kClip, cid, "alice", "de"), cid));
    CHECK(rig.fake->inputs().back().language == "de");
}

// Mutation: answer the transcription kinds before the chat's gate.
TEST_CASE("chat_dictation_is_behind_the_chat_gate", "[chat][dictation]") {
    Rig rig;
    auto const cid = rig.gui("muted");
    auto const reply = rig.transcribeRaw(kClip, cid, "muted");
    CHECK(reply.kind == "err");
    CHECK(reply.message == wire::kUnauthorizedMessage);
    CHECK(rig.fake->calls() == 0);
}

// Mutation: decode before checking the text's size (the oversized garbage then reads "not base64"), or drop the
// decoded size check.
TEST_CASE("chat_dictation_refusals_name_their_bound", "[chat][dictation]") {
    chat::ChatLimits limits;
    limits.maxDictationBytes = 31;
    Rig rig{limits};
    auto const cid = rig.gui();
    std::string const bound = "the recording is larger than 31 bytes";
    std::string tenGroups;
    for (int group = 0; group < 10; ++group) {
        tenGroups += "YWFh";
    }

    auto const garbage = rig.transcribeRaw(std::string(45, '!'), cid);
    CHECK(garbage.kind == "err");
    CHECK(garbage.message == bound);
    auto const decodedOver = rig.transcribeRaw(tenGroups + "YWE=", cid);
    CHECK(decodedOver.message == bound);
    CHECK(rig.transcribeRaw("@@@@", cid).message == "the recording is not base64");
    CHECK(rig.transcribeRaw(kClip, cid, "alice", "audio/ogg").message == "the recording is not audio/wav");
    CHECK(rig.transcribeRaw(kClip, 0).message == "dictation needs a connection");
    CHECK(rig.fake->calls() == 0);

    rig.fake->script(std::string{"ok"});
    auto const atTheBound = rig.transcribe(tenGroups + "YQ==", cid);
    CHECK(rig.settled(atTheBound, cid).status == chat::TranscriptionStatus::Done);
    CHECK(rig.fake->inputs().front().audio.size() == 31);
}

// Mutations: drop the connection's `busy` check (the second call on a1 is accepted); drop the principal's cap (the
// call on a3 is accepted).
TEST_CASE("chat_dictation_busy_and_the_principal_cap", "[chat][dictation]") {
    chat::ChatLimits limits;
    limits.maxTranscriptionsPerPrincipal = 2;
    Rig rig{limits};
    rig.fake->hold(true);
    auto const a1 = rig.gui();
    auto const a2 = rig.gui();
    auto const a3 = rig.gui();
    auto const b1 = rig.gui("bob");

    auto const first = rig.transcribe(kClip, a1);
    auto const busy = rig.transcribeRaw(kClip, a1);
    CHECK(busy.kind == "err");
    CHECK(busy.message.starts_with("busy"));
    auto const second = rig.transcribe(kClip, a2);
    auto const capped = rig.transcribeRaw(kClip, a3);
    CHECK(capped.kind == "err");
    CHECK(capped.message == "too many dictations are being transcribed");
    static_cast<void>(rig.transcribe(kClip, b1, "bob"));

    rig.fake->release();
    static_cast<void>(rig.settled(first, a1));
    static_cast<void>(rig.settled(second, a2));
    static_cast<void>(rig.transcribe(kClip, a3));
}

// Mutation: leave `dictation` out of the probe's answer, or put it there without a transcriber.
TEST_CASE("chat_dictation_is_offered_by_the_probe_only_with_a_transcriber", "[chat][dictation]") {
    Rig offered;
    auto const offer = offered.probe(offered.gui()).dictation;
    REQUIRE(offer.has_value());
    CHECK(offer->maxSeconds == 60);
    CHECK(offer->maxBytes == std::uint64_t{4} * 1024 * 1024);
    CHECK(offered.service->serves(chat::kTranscribeKind));

    Rig plain{chat::ChatLimits{}, nullptr, false};
    CHECK_FALSE(plain.probe(plain.gui()).dictation.has_value());
    CHECK_FALSE(plain.service->serves(chat::kTranscribeKind));
    CHECK_FALSE(plain.service->serves(chat::kTranscriptionKind));
}

// Mutation: answer a query without comparing the principal.
TEST_CASE("chat_dictation_of_another_principal_reads_as_none", "[chat][dictation]") {
    Rig rig;
    auto const cid = rig.gui();
    rig.fake->script(std::string{"mine"});
    auto const id = rig.transcribe(kClip, cid);
    CHECK(rig.settled(id, cid).text == "mine");
    auto const bob = rig.state(id, rig.gui("bob"), "bob");
    CHECK(bob.status == chat::TranscriptionStatus::Failed);
    CHECK(bob.error == "no such transcription");
    CHECK_FALSE(bob.text.has_value());
    auto const unknown = rig.state(id + 1000, cid);
    CHECK(unknown.error == "no such transcription");
}

// Mutation: accept a transcriber with no provider, or a byte bound past the envelope.
TEST_CASE("chat_dictation_misconfiguration_is_refused_at_construction", "[chat][dictation]") {
    morph::exec::ThreadPoolExecutor worker{1};
    auto const bundle = bundleOf();
    auto const optionsWith = [&](chat::ChatLimits limits, bool withProvider) {
        chat::ChatOptions options;
        if (withProvider) {
            options.provider = std::make_shared<chat::testing::FakeChatProvider>();
        }
        options.worker = &worker;
        options.bundle = bundle;
        options.limits = limits;
        options.transcriber = std::make_shared<chat::testing::FakeTranscriber>();
        return options;
    };
    CHECK_THROWS_AS(static_cast<void>(chat::ChatService{optionsWith(chat::ChatLimits{}, false)}),
                    std::invalid_argument);
    chat::ChatLimits past;
    past.maxDictationBytes = chat::kMaxDictationBytes + 1;
    CHECK_THROWS_AS(static_cast<void>(chat::ChatService{optionsWith(past, true)}), std::invalid_argument);
    for (auto const& zeroed : std::vector<std::function<void(chat::ChatLimits&)>>{
             [](chat::ChatLimits& limits) { limits.maxDictationSeconds = 0; },
             [](chat::ChatLimits& limits) { limits.maxDictationBytes = 0; },
             [](chat::ChatLimits& limits) { limits.maxTranscriptionsPerPrincipal = 0; },
             [](chat::ChatLimits& limits) { limits.transcriptionRetention = std::chrono::seconds{0}; }}) {
        chat::ChatLimits zero;
        zeroed(zero);
        CHECK_THROWS_AS(static_cast<void>(chat::ChatService{optionsWith(zero, true)}), std::invalid_argument);
    }
    chat::ChatLimits atTheCap;
    atTheCap.maxDictationBytes = chat::kMaxDictationBytes;
    CHECK_NOTHROW(static_cast<void>(chat::ChatService{optionsWith(atTheCap, true)}));
}

// Mutation: leave the configured language out of `configure`.
TEST_CASE("chat_dictation_language_comes_from_the_configuration", "[chat][dictation]") {
    chat::ChatConfig config;
    config.transcription = chat::TranscriptionConfig{
        .name = "openai-transcriptions", .baseUrl = {}, .model = "whisper-1", .apiKey = "k", .language = "fr"};
    chat::ChatOptions options;
    options.configure(config);
    CHECK(options.transcriptionLanguage == "fr");
    config.transcription.reset();
    options.configure(config);
    CHECK_FALSE(options.transcriptionLanguage.has_value());
}

namespace {

// A transcriber the test answers by hand, whenever it likes, even after the service cancelled the call: a real one
// can deliver while a cancel is on its way. `called` hands the sink over, since a kept sink would keep the service.
struct ManualTranscriber final : chat::ITranscriber {
    struct Call final : chat::ITranscriberCall {
        explicit Call(std::shared_ptr<std::atomic<bool>> flag) : cancelled{std::move(flag)} {}
        void cancel() override { cancelled->store(true); }
        std::shared_ptr<std::atomic<bool>> cancelled;
    };

    [[nodiscard]] std::shared_ptr<chat::ITranscriberCall> transcribe(chat::TranscriptionInput /*input*/,
                                                                     chat::TranscriberSink given) override {
        std::scoped_lock const lock{mutex};
        sink = std::move(given);
        return std::make_shared<Call>(cancelled);
    }

    [[nodiscard]] chat::TranscriberSink called() {
        REQUIRE(waitFor([&] {
            std::scoped_lock const lock{mutex};
            return sink.has_value();
        }));
        std::scoped_lock const lock{mutex};
        return *std::exchange(sink, std::nullopt);
    }

    std::mutex mutex;
    std::optional<chat::TranscriberSink> sink;
    std::shared_ptr<std::atomic<bool>> cancelled = std::make_shared<std::atomic<bool>>(false);
};

std::size_t eventsFor(Rig& rig, std::uint64_t id) {
    std::size_t count = 0;
    for (chat::TranscriptionState const& told : rig.eventWire.transcriptions()) {
        count += told.id == id ? 1U : 0U;
    }
    return count;
}

}  // namespace

// Mutation: end the entry on the deadline without cancelling its call.
TEST_CASE("chat_transcription_deadline_fails_and_cancels", "[chat][dictation]") {
    chat::ChatLimits limits;
    limits.toolCallTimeout = 150ms;
    Rig rig{limits};
    rig.fake->hold(true);
    auto const cid = rig.gui();
    auto const id = rig.transcribe(kClip, cid);
    auto const state = rig.settled(id, cid);
    CHECK(state.status == chat::TranscriptionStatus::Failed);
    CHECK(state.error == "the transcription did not answer in time");
    CHECK(rig.fake->cancelled() == 1);
    REQUIRE(waitFor([&] { return eventsFor(rig, id) == 1; }));
}

// Mutation: end the entry on `ui-chat-end` without cancelling its call, or stop every connection's.
TEST_CASE("chat_transcription_is_cancelled_by_ending_the_chat", "[chat][dictation]") {
    Rig rig;
    rig.fake->hold(true);
    auto const cid = rig.gui();
    auto const elsewhere = rig.gui();
    auto const id = rig.transcribe(kClip, cid);
    auto const other = rig.transcribe(kClip, elsewhere);
    wire::Envelope end;
    end.kind = std::string{chat::kEndKind};
    auto const ended = rig.call(std::move(end), cid);
    REQUIRE(ended.kind == "ok");
    auto const state = rig.settled(id, cid);
    CHECK(state.status == chat::TranscriptionStatus::Failed);
    CHECK(state.error == "cancelled");
    CHECK(rig.fake->cancelled() == 1);
    CHECK(rig.state(other, elsewhere).status == chat::TranscriptionStatus::Running);
    rig.fake->release();
}

// Mutation: keep a closed connection's entry, or leave its call running.
TEST_CASE("chat_transcription_is_dropped_with_its_connection", "[chat][dictation]") {
    Rig rig;
    rig.fake->hold(true);
    auto const cid = rig.gui();
    auto const id = rig.transcribe(kClip, cid);
    rig.server->closeConnection(cid);
    REQUIRE(waitFor([&] { return rig.fake->cancelled() == 1; }));
    auto const after = rig.state(id, rig.gui());
    CHECK(after.status == chat::TranscriptionStatus::Failed);
    CHECK(after.error == "no such transcription");
    CHECK(eventsFor(rig, id) == 0);
}

// Mutation: let a later answer overwrite an ended entry.
TEST_CASE("chat_transcription_answer_after_the_end_changes_nothing", "[chat][dictation]") {
    chat::ChatLimits limits;
    limits.toolCallTimeout = 150ms;
    auto const manual = std::make_shared<ManualTranscriber>();
    Rig rig{limits, manual};
    auto const cid = rig.gui();
    auto const id = rig.transcribe(kClip, cid);
    auto const sink = manual->called();
    CHECK(rig.settled(id, cid).error == "the transcription did not answer in time");
    REQUIRE(waitFor([&] { return eventsFor(rig, id) == 1; }));
    sink.done(std::string{"too late"});
    auto const after = rig.state(id, cid);
    CHECK(after.status == chat::TranscriptionStatus::Failed);
    CHECK_FALSE(after.text.has_value());
    morph::testing::storageOwner().runFor(20ms);
    CHECK(eventsFor(rig, id) == 1);
}

// Mutation: leave the calls running when the service is destroyed, or announce them (Tell::Yes in the destructor).
// The service is its own, bound to a server that outlives it, which is the only arrangement in which an announcement
// could still reach a connection.
TEST_CASE("chat_transcription_answer_after_the_service_is_gone", "[chat][dictation]") {
    EventWire wire;
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::ThreadPoolExecutor worker{2};
    auto const bundle = bundleOf();
    auto const authorizer = std::make_shared<Authorizer>();
    morph::backend::ServerConfig config;
    config.services.push_back(std::make_shared<ui::UiService>(bundle));
    auto const server = std::make_shared<morph::backend::RemoteServer>(pool, authorizer, std::move(config));
    auto const cid = server->openConnection();
    server->setConnectionWriter(cid, wire.writer());

    auto const manual = std::make_shared<ManualTranscriber>();
    chat::ChatOptions options;
    options.provider = std::make_shared<chat::testing::FakeChatProvider>();
    options.worker = &worker;
    options.bundle = bundle;
    options.transcriber = manual;
    auto service = std::make_shared<chat::ChatService>(std::move(options));
    service->bound(server);
    morph::backend::ConnectionFacts facts;
    facts.cid = cid;
    auto const start = [&] {
        wire::Envelope env;
        env.kind = std::string{chat::kTranscribeKind};
        env.body = chat::writeMessage(chat::TranscribeRequest{.audio = std::string{kClip}, .mediaType = "audio/wav"});
        env.session.token = "alice";
        env.session.principal = "alice";
        return wire::decode(service->handle(env, *authorizer, facts));
    };

    // The control: an answer while the service lives is announced to the connection it came from.
    REQUIRE(start().kind == "ok");
    manual->called().done(std::string{"heard"});
    REQUIRE(waitFor([&] { return wire.transcriptions().size() == 1; }));

    REQUIRE(start().kind == "ok");
    auto const sink = manual->called();
    service.reset();
    CHECK(manual->cancelled->load());
    sink.done(std::string{"nobody listens"});
    morph::testing::storageOwner().runFor(50ms);
    CHECK(wire.transcriptions().size() == 1);
}

// Mutation: never forget a finished entry, or forget it at once.
TEST_CASE("chat_transcription_is_forgotten_after_its_retention", "[chat][dictation]") {
    chat::ChatLimits limits;
    limits.transcriptionRetention = std::chrono::seconds{60};
    Rig rig{limits};
    auto const cid = rig.gui();
    rig.fake->script(std::string{"kept a while"});
    auto const id = rig.transcribe(kClip, cid);
    CHECK(rig.settled(id, cid).text == "kept a while");
    rig.clock->fetch_add(59);
    CHECK(rig.state(id, cid).text == "kept a while");
    rig.clock->fetch_add(2);
    CHECK(rig.state(id, cid).error == "no such transcription");
}

// Mutation: add the transcript to the conversation.
TEST_CASE("chat_transcription_leaves_the_conversation_alone", "[chat][dictation]") {
    Rig rig;
    auto const cid = rig.gui();
    rig.fake->script(std::string{"heard words"});
    static_cast<void>(rig.settled(rig.transcribe(kClip, cid), cid));
    rig.provider->script(chat::Response{.text = "ok", .calls = {}, .opaque = {}});
    chat::TurnRequest turn;
    turn.screenId = "s";
    turn.text = "typed words";
    wire::Envelope start;
    start.kind = std::string{chat::kTurnKind};
    start.body = chat::writeMessage(turn);
    auto const accepted = chat::readTurnAccepted(rig.call(std::move(start), cid).body);
    REQUIRE(accepted.has_value());
    REQUIRE(waitFor([&] { return rig.provider->calls() == 1; }));
    auto const requests = rig.provider->requests();
    REQUIRE(requests.front().messages.size() == 1);
    CHECK(requests.front().messages.front().text == "typed words");
    CHECK_FALSE(requests.front().system.contains("heard words"));
}

namespace {

// Throws something that is not a `std::exception`.
struct ThrowingTranscriber final : chat::ITranscriber {
    [[nodiscard]] std::shared_ptr<chat::ITranscriberCall> transcribe(chat::TranscriptionInput /*input*/,
                                                                     chat::TranscriberSink /*sink*/) override {
        // NOLINTNEXTLINE(hicpp-exception-baseclass,cert-err60-cpp,bugprone-std-exception-baseclass): the point is a type no `std::exception` catch sees.
        throw 42;
    }
};

}  // namespace

// Mutation: remove the catch-all, so the entry stays running and the worker thread dies on the throw.
TEST_CASE("chat_transcription_a_transcriber_throwing_anything_fails_the_entry", "[chat][dictation]") {
    Rig rig{chat::ChatLimits{}, std::make_shared<ThrowingTranscriber>()};
    auto const cid = rig.gui();
    auto const id = rig.transcribe(kClip, cid);
    CHECK(rig.settled(id, cid).status == chat::TranscriptionStatus::Failed);
    CHECK(waitFor([&] { return eventsFor(rig, id) == 1; }));
}
