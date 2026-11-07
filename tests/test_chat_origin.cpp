// SPDX-License-Identifier: Apache-2.0
//
// A run's origin: what the server says started an execute, which only the server can set, and which reaches the
// journal entry that execute writes.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <core/async/Task.hpp>
#include <cstdint>
#include <future>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/coroutine.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/model.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/timeout_scheduler.hpp>
#include <morph/core/wire.hpp>
#include <morph/journal/action_log.hpp>
#include <morph/journal/action_log_json.hpp>
#include <morph/session/session.hpp>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "test_support.hpp"

// NOLINTBEGIN(misc-use-internal-linkage): registered types must have external linkage.
namespace chat_origin {

struct Mark {
    std::string text;
};
struct SuspendAndMark {
    std::string text;
};

inline std::mutex& seenMutex() {
    static std::mutex shared;
    return shared;
}
inline std::string& seenAfterResume() {
    static std::string shared;
    return shared;
}

inline morph::async::detail::TimeoutScheduler& scheduler() {
    static morph::async::detail::TimeoutScheduler shared;
    return shared;
}

struct OriginModel {
    std::string execute(Mark const& action) const { return action.text; }

    core::async::Task<std::string> execute(SuspendAndMark action) const {
        co_await morph::async::delay(scheduler(), std::chrono::milliseconds{20});
        auto const* const origin = morph::session::currentOrigin();
        std::scoped_lock const lock{seenMutex()};
        seenAfterResume() = origin == nullptr ? std::string{} : origin->kind;
        co_return action.text;
    }
};

}  // namespace chat_origin
// NOLINTEND(misc-use-internal-linkage)

BRIDGE_REGISTER_MODEL(chat_origin::OriginModel, "Origin_Model")
BRIDGE_REGISTER_ACTION(chat_origin::OriginModel, chat_origin::Mark, "Origin_Mark")
BRIDGE_REGISTER_ACTION(chat_origin::OriginModel, chat_origin::SuspendAndMark, "Origin_Suspend")

namespace {

using namespace std::chrono_literals;
namespace wire = morph::wire;

constexpr std::string_view kTurnText = "container 100, before 299, after 223";

struct Authorizer final : morph::session::IAuthorizer {
    [[nodiscard]] bool authorize(morph::session::Context const& ctx, std::string_view /*modelType*/,
                                 std::string_view /*actionType*/) const override {
        return ctx.principal != "mallory";
    }
    [[nodiscard]] std::optional<std::string> authenticate(morph::session::Context const& ctx) const override {
        return ctx.token.empty() ? std::nullopt : std::optional{ctx.token};
    }
};

std::shared_ptr<morph::journal::InMemoryActionLog> sharedLog() {
    static auto const shared = [] {
        auto log = std::make_shared<morph::journal::InMemoryActionLog>(morph::testing::storageOwner());
        morph::model::detail::defaultRegistry().registerModel<chat_origin::OriginModel>("Origin_Model", [log] {
            auto holder = std::make_unique<morph::model::detail::ModelHolder<chat_origin::OriginModel>>();
            holder->attachActionLog(log, {});
            return holder;
        });
        return log;
    }();
    return shared;
}

struct Rig {
    std::shared_ptr<morph::journal::InMemoryActionLog> log = sharedLog();
    std::size_t before = log->entries().size();
    morph::exec::ThreadPoolExecutor pool{2};
    std::shared_ptr<morph::backend::RemoteServer> server = std::make_shared<morph::backend::RemoteServer>(
        pool, std::make_shared<Authorizer>(), morph::backend::ServerConfig{});
    std::uint64_t instance = 0;

    Rig() {
        auto reply = post(wire::makeRegister("Origin_Model"));
        REQUIRE(reply.kind == "ok");
        instance = reply.modelId;
    }

    static wire::Envelope settle(std::future<std::string>& answer) {
        auto const deadline = std::chrono::steady_clock::now() + 5s;
        while (answer.wait_for(1ms) != std::future_status::ready) {
            morph::testing::storageOwner().runFor(1ms);
            REQUIRE(std::chrono::steady_clock::now() < deadline);
        }
        return wire::decode(answer.get());
    }

    wire::Envelope post(wire::Envelope env, std::string const& token = "alice") {
        static std::atomic<std::uint64_t> nextCall{1};
        env.callId = nextCall.fetch_add(1);
        env.session.token = token;
        env.session.principal = token;
        auto replied = std::make_shared<std::promise<std::string>>();
        auto answer = replied->get_future();
        server->handle(wire::encode(env), [replied](std::string const& text) { replied->set_value(text); });
        return settle(answer);
    }

    [[nodiscard]] wire::Envelope executeOf(std::string action, std::string const& token = "alice") const {
        wire::Envelope env;
        env.kind = "execute";
        env.modelId = instance;
        env.modelType = "Origin_Model";
        env.actionType = std::move(action);
        env.body = R"({"text":"hi"})";
        env.session.token = token;
        env.session.principal = token;
        return env;
    }

    wire::Envelope marked(wire::Envelope env) {
        static std::atomic<std::uint64_t> nextCall{1'000'000};
        env.callId = nextCall.fetch_add(1);
        auto replied = std::make_shared<std::promise<std::string>>();
        auto answer = replied->get_future();
        server->executeWithOrigin(std::move(env), morph::session::RunOrigin{"assistant", std::string{kTurnText}},
                                  [replied](std::string const& text) { replied->set_value(text); });
        return settle(answer);
    }

    // The entries this rig's calls added. The log's owner is this thread, so it runs the appends.
    std::vector<morph::journal::LogEntry> added(std::size_t expected) {
        auto const deadline = std::chrono::steady_clock::now() + 5s;
        while (log->entries().size() < before + expected && std::chrono::steady_clock::now() < deadline) {
            morph::testing::storageOwner().runFor(5ms);
        }
        morph::testing::storageOwner().runFor(20ms);
        auto const all = log->entries();
        return {all.begin() + static_cast<std::ptrdiff_t>(before), all.end()};
    }
};

}  // namespace

TEST_CASE("executeWithOrigin_marks_the_journal_entry", "[chat][origin]") {
    Rig rig;
    REQUIRE(rig.marked(rig.executeOf("Origin_Mark")).kind == "ok");
    auto const entries = rig.added(1);
    REQUIRE(entries.size() == 1);
    CHECK(entries.front().originKind == "assistant");
    CHECK(entries.front().originText == kTurnText);
    CHECK(entries.front().principal == "alice");
}

TEST_CASE("an_ordinary_execute_has_no_origin", "[chat][origin]") {
    Rig rig;
    REQUIRE(rig.post(rig.executeOf("Origin_Mark")).kind == "ok");
    auto const entries = rig.added(1);
    REQUIRE(entries.size() == 1);
    CHECK(entries.front().originKind.empty());
    CHECK(entries.front().originText.empty());
}

TEST_CASE("a_client_cannot_send_an_origin", "[chat][origin]") {
    Rig rig;
    auto env = rig.executeOf("Origin_Mark");
    env.session.metadata["originKind"] = "assistant";
    env.session.metadata["origin"] = "assistant";
    auto text = wire::encode(env);
    // The same names at the top level of the envelope and inside its session, where a field would be read from.
    text.insert(1, R"("originKind":"assistant","originText":"forged","origin":{"kind":"assistant"},)");
    auto const at = text.find(R"("session":{)");
    REQUIRE(at != std::string::npos);
    text.insert(at + 11, R"("originKind":"assistant","origin":{"kind":"assistant"},)");
    auto replied = std::make_shared<std::promise<std::string>>();
    auto answer = replied->get_future();
    rig.server->handle(std::move(text), [replied](std::string const& reply) { replied->set_value(reply); });
    REQUIRE(Rig::settle(answer).kind == "ok");
    auto const entries = rig.added(1);
    REQUIRE(entries.size() == 1);
    CHECK(entries.front().originKind.empty());
    CHECK(entries.front().originText.empty());
}

TEST_CASE("a_task_handler_keeps_its_origin_across_a_suspension", "[chat][origin]") {
    Rig rig;
    REQUIRE(rig.marked(rig.executeOf("Origin_Suspend")).kind == "ok");
    {
        std::scoped_lock const lock{chat_origin::seenMutex()};
        CHECK(chat_origin::seenAfterResume() == "assistant");
    }
    auto const entries = rig.added(1);
    REQUIRE(entries.size() == 1);
    CHECK(entries.front().originKind == "assistant");
}

TEST_CASE("an_origin_does_not_outlive_its_run", "[chat][origin]") {
    Rig rig;
    REQUIRE(rig.marked(rig.executeOf("Origin_Mark")).kind == "ok");
    REQUIRE(rig.post(rig.executeOf("Origin_Mark")).kind == "ok");
    auto const entries = rig.added(2);
    REQUIRE(entries.size() == 2);
    CHECK(entries.at(0).originKind == "assistant");
    CHECK(entries.at(1).originKind.empty());
}

TEST_CASE("executeWithOrigin_still_authorizes", "[chat][origin]") {
    Rig rig;
    CHECK(rig.marked(rig.executeOf("Origin_Mark", "mallory")).kind == "err");
    CHECK(rig.added(0).empty());
}

TEST_CASE("log_entry_round_trips_its_origin_and_reads_an_older_entry", "[chat][origin]") {
    morph::journal::LogEntry entry;
    entry.actionType = "Origin_Mark";
    entry.originKind = "assistant";
    entry.originText = "line one\nline two";
    auto const decoded = morph::journal::fromJson(morph::journal::toJson(entry));
    CHECK(decoded.originKind == "assistant");
    CHECK(decoded.originText == "line one\nline two");
    auto const older = morph::journal::fromJson(R"({"seq":1,"actionType":"Origin_Mark","payload":"{}"})");
    CHECK(older.originKind.empty());
    CHECK(older.originText.empty());
}
