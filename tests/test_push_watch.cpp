// SPDX-License-Identifier: Apache-2.0

// The server's per-connection watch table and the core `watch`/`unwatch`
// kinds: what a watch may name, how it is stored, and what drops it.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <latch>
#include <map>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/effects.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/records.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/session/session.hpp>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "test_support.hpp"

// Glaze reflects member names through an external-linkage object, so these types cannot sit in an
// anonymous namespace.
namespace push_watch_test {

struct Row {
    std::int64_t id{};
    std::string name;
};

struct RowList {
    std::vector<Row> rows;
};

struct ListRows {
    int limit{};
};

struct SecretRow {
    std::int64_t id{};
};

struct SecretList {
    std::vector<SecretRow> rows;
};

struct ListSecrets {
    int limit{};
};

struct RowModel {
    RowList execute(ListRows const& /*action*/) { return {}; }
};

struct SecretModel {
    SecretList execute(ListSecrets const& /*action*/) { return {}; }
};

struct FanRow {
    std::int64_t id{};
    std::string name;
};

struct FanRowList {
    std::vector<FanRow> rows;
};

struct FanOther {
    std::int64_t id{};
};

struct FanOtherList {
    std::vector<FanOther> rows;
};

/// Edits one row and returns it.
struct EditFanRow {
    std::int64_t id{};
};

/// Edits one row after `delayMs`, so an execute timeout can answer first.
struct SlowEditFanRow {
    std::int64_t id{};
    int delayMs{};
};

/// Declares it edits rows, then fails.
struct FailFanRow {
    std::int64_t id{};
};

/// Reads the other record type; only there so a connection can watch it.
struct ListFanOthers {
    int limit{};
};

/// Changes the instance's own state.
struct TouchFan {
    int by{};
};

struct FanModel {
    int touched = 0;
    FanRowList execute(EditFanRow const& action) { return {.rows = {{.id = action.id, .name = "edited"}}}; }
    FanRowList execute(SlowEditFanRow const& action) {
        std::this_thread::sleep_for(std::chrono::milliseconds{action.delayMs});
        return {.rows = {{.id = action.id, .name = "slow"}}};
    }
    FanRowList execute(FailFanRow const& /*action*/) { throw std::runtime_error("edit failed"); }
    FanOtherList execute(ListFanOthers const& /*action*/) { return {}; }
    int execute(TouchFan const& action) { return touched += action.by; }
};

}  // namespace push_watch_test

using namespace push_watch_test;

BRIDGE_REGISTER_RECORD(Row, "T_WatchRow")
BRIDGE_REGISTER_RECORD(SecretRow, "T_WatchSecret")
BRIDGE_REGISTER_MODEL(RowModel, "T_WatchRowModel")
BRIDGE_REGISTER_ACTION(RowModel, ListRows, "T_WatchListRows")
BRIDGE_REGISTER_MODEL(SecretModel, "T_WatchSecretModel")
BRIDGE_REGISTER_ACTION(SecretModel, ListSecrets, "T_WatchListSecrets")

BRIDGE_REGISTER_RECORD(FanRow, "T_FanRow")
BRIDGE_REGISTER_RECORD(FanOther, "T_FanOther")
BRIDGE_REGISTER_MODEL(FanModel, "T_FanModel")
BRIDGE_REGISTER_ACTION(FanModel, EditFanRow, "T_FanEdit")
BRIDGE_REGISTER_EFFECTS(EditFanRow, morph::effects::updates<FanRow>)
BRIDGE_REGISTER_ACTION(FanModel, SlowEditFanRow, "T_FanSlowEdit")
BRIDGE_REGISTER_EFFECTS(SlowEditFanRow, morph::effects::updates<FanRow>)
BRIDGE_REGISTER_ACTION(FanModel, FailFanRow, "T_FanFail")
BRIDGE_REGISTER_EFFECTS(FailFanRow, morph::effects::updates<FanRow>)
BRIDGE_REGISTER_ACTION(FanModel, ListFanOthers, "T_FanListOthers")
BRIDGE_REGISTER_ACTION(FanModel, TouchFan, "T_FanTouch")
BRIDGE_REGISTER_EFFECTS(TouchFan, morph::effects::changesInstance)

namespace {

using ::morph::backend::ConnectionId;
using ::morph::backend::RemoteServer;

constexpr std::string_view kSecretModel = "T_WatchSecretModel";

/// What one `authorizeInstance` call was asked.
struct InstanceQuestion {
    std::string modelType;
    std::string actionType;
    std::uint64_t modelId{};
    std::string owner;
};

/// Verifies a session by its token: the principal is the token up to a `:`,
/// so `alice` and `alice:scoped` are two tokens of one principal. Refuses every
/// action and instance of the secret model, and every action to a `:scoped`
/// token.
struct SecretGuard final : ::morph::session::IAuthorizer {
    mutable std::atomic<int> authorizeCalls{0};
    mutable std::atomic<int> authenticateCalls{0};
    mutable std::mutex mtx;
    mutable std::vector<InstanceQuestion> instanceQuestions;

    [[nodiscard]] bool authorize(const ::morph::session::Context& ctx, std::string_view modelType,
                                 std::string_view /*actionType*/) const override {
        authorizeCalls.fetch_add(1);
        return modelType != kSecretModel && !ctx.token.ends_with(":scoped");
    }
    [[nodiscard]] std::optional<std::string> authenticate(const ::morph::session::Context& ctx) const override {
        authenticateCalls.fetch_add(1);
        if (ctx.token.empty()) {
            return std::nullopt;
        }
        return ctx.token.substr(0, ctx.token.find(':'));
    }
    [[nodiscard]] bool authorizeInstance(const ::morph::session::Context& /*ctx*/, std::string_view modelType,
                                         std::string_view actionType, std::uint64_t modelId,
                                         std::string_view ownerPrincipal) const override {
        std::scoped_lock const lock{mtx};
        instanceQuestions.push_back(
            {std::string{modelType}, std::string{actionType}, modelId, std::string{ownerPrincipal}});
        return modelType != kSecretModel;
    }
};

/// Overrides only `authorize`, refusing the secret model's actions: its
/// `authorizeInstance` is the allow-all default.
struct AuthorizeOnly final : ::morph::session::IAuthorizer {
    [[nodiscard]] bool authorize(const ::morph::session::Context& /*ctx*/, std::string_view modelType,
                                 std::string_view /*actionType*/) const override {
        return modelType != kSecretModel;
    }
};

struct Rig {
    ::morph::exec::ThreadPoolExecutor pool{2};
    std::shared_ptr<SecretGuard> guard = std::make_shared<SecretGuard>();
    std::shared_ptr<RemoteServer> server;

    explicit Rig(std::shared_ptr<::morph::session::IAuthorizer> authorizer = nullptr)
        : server{std::make_shared<RemoteServer>(pool, authorizer ? std::move(authorizer) : guard)} {}

    /// Sends @p kind with @p body on @p cid, signed as @p principal, and returns the reply.
    ::morph::wire::Envelope send(ConnectionId cid, std::string_view kind, ::morph::wire::WatchBody const& body,
                                 std::string const& principal = "alice") {
        ::morph::wire::Envelope env;
        env.kind = std::string{kind};
        env.callId = 7;
        env.session.token = principal;
        env.body = glz::write_json(body).value_or(std::string{});
        return ::morph::wire::decode(server->handleInline(::morph::wire::encode(env), cid));
    }

    /// Sends a `watch` and returns its decoded `ok` reply.
    ::morph::wire::WatchReply watch(ConnectionId cid, ::morph::wire::WatchBody const& body,
                                    std::string const& principal = "alice") {
        auto const reply = send(cid, ::morph::wire::kWatchKind, body, principal);
        REQUIRE(reply.kind == "ok");
        auto parsed = ::morph::wire::readWatchReply(reply.body);
        REQUIRE(parsed);
        return *parsed;
    }

    /// Runs @p read on the server strand and returns what it returned.
    template <class Fn>
    auto onStrand(Fn read) {
        std::optional<decltype(read(server->watchTable()))> answer;
        std::latch done{1};
        server->strand().post([&] {
            answer.emplace(read(server->watchTable()));
            done.count_down();
        });
        done.wait();
        return std::move(*answer);
    }

    std::vector<ConnectionId> recordWatchers(std::string_view record) {
        return onStrand([record](auto const& table) { return table.watchersOf(record); });
    }

    std::vector<ConnectionId> instanceWatchers(std::string_view modelType, std::string_view primary) {
        return onStrand([modelType, primary](auto const& table) { return table.watchersOf(modelType, primary); });
    }
};

::morph::wire::WatchBody records(std::vector<std::string> names, bool replace = false) {
    return {.records = std::move(names), .instances = {}, .replace = replace};
}

::morph::wire::WatchBody instance(std::string modelType, std::string primary) {
    return {.records = {}, .instances = {{.modelType = std::move(modelType), .primary = std::move(primary)}}};
}

}  // namespace

TEST_CASE("watch_record_admitted", "[push][watch]") {
    Rig rig;
    auto const cid = rig.server->openConnection();

    auto const reply = rig.watch(cid, records({"T_WatchRow"}));
    CHECK(reply.refusedRecords.empty());
    CHECK(reply.refusedInstances.empty());
    CHECK(rig.recordWatchers("T_WatchRow") == std::vector<ConnectionId>{cid});

    // The watch keeps the session it arrived with and the principal verified from it.
    auto const stored = rig.onStrand([cid](auto const& table) -> std::optional<std::pair<std::string, std::string>> {
        auto const* watch = table.watchOf(cid, "T_WatchRow");
        if (watch == nullptr) {
            return std::nullopt;
        }
        return std::pair{watch->principal, watch->context.token};
    });
    REQUIRE(stored);
    CHECK(stored->first == "alice");
    CHECK(stored->second == "alice");

    // A record no registered record has is refused, as is a record no action's result holds... and
    // `unwatch` takes the admitted one away again.
    auto const unknown = rig.watch(cid, records({"T_WatchNoSuchRecord"}));
    CHECK(unknown.refusedRecords == std::vector<std::string>{"T_WatchNoSuchRecord"});
    auto const unwatched = rig.send(cid, ::morph::wire::kUnwatchKind, records({"T_WatchRow"}));
    CHECK(unwatched.kind == "ok");
    CHECK(rig.recordWatchers("T_WatchRow").empty());
}

TEST_CASE("watch_on_unreadable_type_refused", "[push][watch]") {
    Rig rig;
    auto const cid = rig.server->openConnection();

    ::morph::wire::WatchBody body = records({"T_WatchSecret", "T_WatchRow"});
    body.instances = {{.modelType = std::string{kSecretModel}, .primary = "k1"},
                      {.modelType = "T_WatchRowModel", .primary = "k1"}};
    auto const reply = rig.watch(cid, body);

    CHECK(reply.refusedRecords == std::vector<std::string>{"T_WatchSecret"});
    REQUIRE(reply.refusedInstances.size() == 1U);
    CHECK(reply.refusedInstances.front().modelType == kSecretModel);
    CHECK(reply.refusedInstances.front().primary == "k1");
    CHECK(rig.recordWatchers("T_WatchSecret").empty());
    CHECK(rig.instanceWatchers(kSecretModel, "k1").empty());
    CHECK(rig.recordWatchers("T_WatchRow") == std::vector<ConnectionId>{cid});
    CHECK(rig.instanceWatchers("T_WatchRowModel", "k1") == std::vector<ConnectionId>{cid});
}

TEST_CASE("instance_watch_stored_before_instance_exists", "[push][watch]") {
    Rig rig;
    auto const cid = rig.server->openConnection();

    // No client has registered or attached `T_WatchRowModel` under "never-made": the topic is kept by
    // its key alone.
    auto const reply = rig.watch(cid, instance("T_WatchRowModel", "never-made"));
    CHECK(reply.refusedInstances.empty());
    CHECK(rig.instanceWatchers("T_WatchRowModel", "never-made") == std::vector<ConnectionId>{cid});
    CHECK(rig.instanceWatchers("T_WatchRowModel", "other").empty());
}

TEST_CASE("replace_supersedes", "[push][watch]") {
    Rig rig;
    auto const cid = rig.server->openConnection();
    auto const other = rig.server->openConnection();

    ::morph::wire::WatchBody first = records({"T_WatchRow"});
    first.instances = {{.modelType = "T_WatchRowModel", .primary = "a"}};
    (void)rig.watch(cid, first);
    (void)rig.watch(other, records({"T_WatchRow"}));

    ::morph::wire::WatchBody full = instance("T_WatchRowModel", "b");
    full.replace = true;
    auto const reply = rig.watch(cid, full);
    CHECK(reply.refusedInstances.empty());

    // The connection's set is exactly the body; another connection's is untouched.
    CHECK(rig.recordWatchers("T_WatchRow") == std::vector<ConnectionId>{other});
    CHECK(rig.instanceWatchers("T_WatchRowModel", "a").empty());
    CHECK(rig.instanceWatchers("T_WatchRowModel", "b") == std::vector<ConnectionId>{cid});

    // Without `replace`, a watch adds.
    (void)rig.watch(cid, records({"T_WatchRow"}));
    CHECK(rig.recordWatchers("T_WatchRow") == std::vector<ConnectionId>{cid, other});
    CHECK(rig.instanceWatchers("T_WatchRowModel", "b") == std::vector<ConnectionId>{cid});
}

TEST_CASE("close_connection_drops_watches", "[push][watch]") {
    Rig rig;
    auto const cid = rig.server->openConnection();
    auto const other = rig.server->openConnection();
    ::morph::wire::WatchBody body = records({"T_WatchRow"});
    body.instances = {{.modelType = "T_WatchRowModel", .primary = "a"}};
    (void)rig.watch(cid, body);
    (void)rig.watch(other, records({"T_WatchRow"}));

    rig.server->closeConnection(cid);

    CHECK(rig.recordWatchers("T_WatchRow") == std::vector<ConnectionId>{other});
    CHECK(rig.instanceWatchers("T_WatchRowModel", "a").empty());

    // A closed connection takes no new watches.
    auto const late = rig.send(cid, ::morph::wire::kWatchKind, records({"T_WatchRow"}));
    CHECK(late.kind == "err");
    CHECK(rig.recordWatchers("T_WatchRow") == std::vector<ConnectionId>{other});
}

TEST_CASE("watch_before_hello_is_stored", "[push][watch]") {
    Rig rig;
    auto const cid = rig.server->openConnection();

    (void)rig.watch(cid, records({"T_WatchRow"}));

    CHECK_FALSE(rig.onStrand([&](auto const& /*table*/) { return rig.server->eventsNegotiated(cid); }));
    CHECK(rig.recordWatchers("T_WatchRow") == std::vector<ConnectionId>{cid});
}

TEST_CASE("write_failure_drops_watches", "[push][watch]") {
    Rig rig;
    auto const cid = rig.server->openConnection();
    rig.server->setConnectionWriter(cid, {.write = [](std::string const&) { throw std::runtime_error("socket gone"); },
                                          .backlog = [] { return std::size_t{0}; }});
    (void)rig.watch(cid, records({"T_WatchRow"}));
    REQUIRE(rig.recordWatchers("T_WatchRow") == std::vector<ConnectionId>{cid});

    rig.server->post(cid, ::morph::wire::encode(::morph::wire::makeEvent(::morph::wire::kChangesTopic, "{}")));

    CHECK(rig.recordWatchers("T_WatchRow").empty());
}

TEST_CASE("unscoped_watch_refused", "[push][watch]") {
    Rig rig;

    auto const reply = rig.send(ConnectionId{0}, ::morph::wire::kWatchKind, records({"T_WatchRow"}));
    CHECK(reply.kind == "err");
    auto const unwatch = rig.send(ConnectionId{0}, ::morph::wire::kUnwatchKind, records({"T_WatchRow"}));
    CHECK(unwatch.kind == "err");
    CHECK(rig.recordWatchers("T_WatchRow").empty());
}

TEST_CASE("record_admission_cached_per_principal", "[push][watch]") {
    Rig rig;
    auto const cid = rig.server->openConnection();

    (void)rig.watch(cid, records({"T_WatchRow"}));
    int const afterFirst = rig.guard->authorizeCalls.load();
    REQUIRE(afterFirst > 0);
    (void)rig.watch(cid, records({"T_WatchRow"}));
    CHECK(rig.guard->authorizeCalls.load() == afterFirst);

    // A caller with no verified principal is judged every time.
    auto const anonymous = rig.server->openConnection();
    (void)rig.watch(anonymous, records({"T_WatchRow"}), "");
    int const afterAnonymous = rig.guard->authorizeCalls.load();
    CHECK(afterAnonymous > afterFirst);
    (void)rig.watch(anonymous, records({"T_WatchRow"}), "");
    CHECK(rig.guard->authorizeCalls.load() > afterAnonymous);

    // Once the principal's watches are gone, so is its cached answer.
    rig.server->closeConnection(cid);
    auto const again = rig.server->openConnection();
    (void)rig.watch(again, records({"T_WatchRow"}));
    CHECK(rig.guard->authorizeCalls.load() > afterAnonymous + 1);
}

TEST_CASE("admission_cache_keys_on_the_token", "[push][watch]") {
    Rig rig;
    auto const cid = rig.server->openConnection();
    auto const scoped = rig.server->openConnection();

    // alice's full token is admitted, and the answer cached while she watches.
    CHECK(rig.watch(cid, records({"T_WatchRow"}), "alice").refusedRecords.empty());
    ::morph::wire::WatchBody body = records({"T_WatchRow"});
    body.instances = {{.modelType = "T_WatchRowModel", .primary = "a"}};
    CHECK(rig.watch(cid, body, "alice").refusedInstances.empty());

    // Her scoped token verifies to the same principal but carries other claims: judged on its own.
    auto const reply = rig.watch(scoped, body, "alice:scoped");
    CHECK(reply.refusedRecords == std::vector<std::string>{"T_WatchRow"});
    REQUIRE(reply.refusedInstances.size() == 1U);
    CHECK(rig.recordWatchers("T_WatchRow") == std::vector<ConnectionId>{cid});
}

TEST_CASE("instance_topic_needs_a_runnable_action", "[push][watch]") {
    Rig rig{std::make_shared<AuthorizeOnly>()};
    auto const cid = rig.server->openConnection();

    ::morph::wire::WatchBody body = instance(std::string{kSecretModel}, "k");
    body.instances.push_back({.modelType = "T_WatchRowModel", .primary = "k"});
    auto const reply = rig.watch(cid, body);

    REQUIRE(reply.refusedInstances.size() == 1U);
    CHECK(reply.refusedInstances.front().modelType == kSecretModel);
    CHECK(rig.instanceWatchers(kSecretModel, "k").empty());
    CHECK(rig.instanceWatchers("T_WatchRowModel", "k") == std::vector<ConnectionId>{cid});
}

TEST_CASE("instance_topic_of_unregistered_type_refused", "[push][watch]") {
    Rig rig{std::make_shared<AuthorizeOnly>()};
    auto const cid = rig.server->openConnection();

    auto const reply = rig.watch(cid, instance("T_WatchNoSuchModel", "k"));

    REQUIRE(reply.refusedInstances.size() == 1U);
    CHECK(reply.refusedInstances.front().modelType == "T_WatchNoSuchModel");
    CHECK(rig.instanceWatchers("T_WatchNoSuchModel", "k").empty());
}

TEST_CASE("instance_admission_asks_without_instance_or_owner", "[push][watch]") {
    Rig rig;
    auto const cid = rig.server->openConnection();

    (void)rig.watch(cid, instance("T_WatchRowModel", "k"));

    std::scoped_lock const lock{rig.guard->mtx};
    REQUIRE(rig.guard->instanceQuestions.size() == 1U);
    auto const& asked = rig.guard->instanceQuestions.front();
    CHECK(asked.modelType == "T_WatchRowModel");
    CHECK(asked.actionType.empty());
    CHECK(asked.modelId == 0U);
    CHECK(asked.owner.empty());
}

TEST_CASE("connection_watch_cap", "[push][watch]") {
    Rig rig;
    auto const cid = rig.server->openConnection();

    ::morph::wire::WatchBody full;
    for (std::size_t index = 0; index < ::morph::wire::kMaxWatchTopics; ++index) {
        full.instances.push_back({.modelType = "T_WatchRowModel", .primary = std::to_string(index)});
    }
    CHECK(rig.watch(cid, full).refusedInstances.empty());
    CHECK(rig.onStrand([cid](auto const& table) { return table.topicCount(cid); }) == ::morph::wire::kMaxWatchTopics);

    // At the cap, a new topic is refused; one already watched is not.
    ::morph::wire::WatchBody more = records({"T_WatchRow"});
    more.instances = {{.modelType = "T_WatchRowModel", .primary = "0"}};
    auto const refused = rig.watch(cid, more);
    CHECK(refused.refusedRecords == std::vector<std::string>{"T_WatchRow"});
    CHECK(refused.refusedInstances.empty());
    CHECK(rig.recordWatchers("T_WatchRow").empty());

    // A topic freed by unwatch makes room again.
    CHECK(rig.send(cid, ::morph::wire::kUnwatchKind, instance("T_WatchRowModel", "0")).kind == "ok");
    CHECK(rig.watch(cid, records({"T_WatchRow"})).refusedRecords.empty());
    CHECK(rig.recordWatchers("T_WatchRow") == std::vector<ConnectionId>{cid});
    CHECK(rig.onStrand([cid](auto const& table) { return table.topicCount(cid); }) == ::morph::wire::kMaxWatchTopics);
}

TEST_CASE("duplicates_in_one_body_judged_once", "[push][watch]") {
    Rig rig;
    auto const cid = rig.server->openConnection();

    ::morph::wire::WatchBody body = records({"T_WatchRow", "T_WatchSecret", "T_WatchRow", "T_WatchSecret"});
    body.instances = {{.modelType = std::string{kSecretModel}, .primary = "k"},
                      {.modelType = "T_WatchRowModel", .primary = "k"},
                      {.modelType = std::string{kSecretModel}, .primary = "k"},
                      {.modelType = "T_WatchRowModel", .primary = "k"}};
    auto const reply = rig.watch(cid, body);

    CHECK(reply.refusedRecords == std::vector<std::string>{"T_WatchSecret"});
    REQUIRE(reply.refusedInstances.size() == 1U);
    CHECK(reply.refusedInstances.front().modelType == kSecretModel);
    CHECK(rig.onStrand([cid](auto const& table) { return table.topicCount(cid); }) == 2U);
}

TEST_CASE("replace_with_refused_topics", "[push][watch]") {
    Rig rig;
    auto const cid = rig.server->openConnection();
    (void)rig.watch(cid, records({"T_WatchRow"}));

    ::morph::wire::WatchBody full = records({"T_WatchSecret"}, true);
    full.instances = {{.modelType = "T_WatchRowModel", .primary = "a"}};
    auto const reply = rig.watch(cid, full);

    CHECK(reply.refusedRecords == std::vector<std::string>{"T_WatchSecret"});
    CHECK(rig.recordWatchers("T_WatchRow").empty());
    CHECK(rig.recordWatchers("T_WatchSecret").empty());
    CHECK(rig.instanceWatchers("T_WatchRowModel", "a") == std::vector<ConnectionId>{cid});
    CHECK(rig.onStrand([cid](auto const& table) { return table.topicCount(cid); }) == 1U);
}

TEST_CASE("unwatch_of_unwatched_topic_is_ok", "[push][watch]") {
    Rig rig;
    auto const bare = rig.server->openConnection();
    auto const cid = rig.server->openConnection();
    (void)rig.watch(cid, records({"T_WatchRow"}));

    CHECK(rig.send(bare, ::morph::wire::kUnwatchKind, records({"T_WatchRow"})).kind == "ok");
    CHECK(rig.send(cid, ::morph::wire::kUnwatchKind, instance("T_WatchRowModel", "never")).kind == "ok");
    CHECK(rig.recordWatchers("T_WatchRow") == std::vector<ConnectionId>{cid});
}

// ── The principal baseline: a connection's watches belong to one principal ───

namespace {

/// Sends a `schemas` for the row model on @p cid, signed with @p token: a kind
/// that authorises and is not a watch, so it changes no watch of its own.
::morph::wire::Envelope describe(Rig& rig, ConnectionId cid, std::string const& token) {
    ::morph::wire::Envelope env;
    env.kind = "schemas";
    env.callId = 9;
    env.typeId = "T_WatchRowModel";
    env.session.token = token;
    return ::morph::wire::decode(rig.server->handleInline(::morph::wire::encode(env), cid));
}

/// Sends an `execute` of @p actionType on @p modelId of @p modelType through
/// @p cid, signed with @p token, and waits for the reply.
::morph::wire::Envelope executeOn(Rig& rig, ConnectionId cid, std::string_view modelType, std::string_view actionType,
                                  std::uint64_t modelId, std::string const& token) {
    ::morph::wire::Envelope env;
    env.kind = "execute";
    env.callId = 11;
    env.modelId = modelId;
    env.modelType = std::string{modelType};
    env.actionType = std::string{actionType};
    env.body = R"({"limit":1})";
    env.session.token = token;
    auto waiter = std::make_shared<::morph::testing::WaitReply>();
    rig.server->handle(::morph::wire::encode(env), [waiter](std::string const& msg) { (*waiter)(msg); }, cid);
    REQUIRE(waiter->await());
    return waiter->env;
}

/// Sends a core `hello` on @p cid, signed with @p token, and returns its reply's kind.
std::string hello(Rig& rig, ConnectionId cid, std::string const& token) {
    ::morph::wire::Envelope env = ::morph::wire::makeHello();
    env.session.token = token;
    return ::morph::wire::decode(rig.server->handleInline(::morph::wire::encode(env), cid)).kind;
}

}  // namespace

TEST_CASE("envelope_under_other_principal_drops_watches", "[push][watch]") {
    Rig rig;
    auto const cid = rig.server->openConnection();
    auto const other = rig.server->openConnection();
    ::morph::wire::WatchBody body = records({"T_WatchRow"});
    body.instances = {{.modelType = "T_WatchRowModel", .primary = "a"}};
    (void)rig.watch(cid, body, "alice");
    (void)rig.watch(other, records({"T_WatchRow"}), "alice");

    // Any kind that authorises compares, not only `watch`.
    CHECK(describe(rig, cid, "bob").kind == "ok");
    CHECK(rig.recordWatchers("T_WatchRow") == std::vector<ConnectionId>{other});
    CHECK(rig.instanceWatchers("T_WatchRowModel", "a").empty());

    // bob is the baseline now: his watch stays while he sends, and goes when alice sends again.
    (void)rig.watch(cid, instance("T_WatchRowModel", "b"), "bob");
    CHECK(describe(rig, cid, "bob").kind == "ok");
    CHECK(rig.instanceWatchers("T_WatchRowModel", "b") == std::vector<ConnectionId>{cid});
    (void)rig.watch(cid, records({"T_WatchRow"}), "alice");
    CHECK(rig.instanceWatchers("T_WatchRowModel", "b").empty());
    // A `watch` under the new principal drops the old set before it adds its own.
    CHECK(rig.recordWatchers("T_WatchRow") == std::vector<ConnectionId>{cid, other});
}

TEST_CASE("unauthenticated_envelope_drops_watches", "[push][watch]") {
    Rig rig;
    auto const cid = rig.server->openConnection();
    (void)rig.watch(cid, records({"T_WatchRow"}), "alice");
    REQUIRE(rig.recordWatchers("T_WatchRow") == std::vector<ConnectionId>{cid});

    // A token `authenticate` refuses is stamped with no principal, which is not alice.
    (void)describe(rig, cid, "");

    CHECK(rig.recordWatchers("T_WatchRow").empty());
}

TEST_CASE("hello_does_not_reset_baseline", "[push][watch]") {
    Rig rig;
    auto const cid = rig.server->openConnection();

    // A `hello` first, under another principal, sets no baseline...
    REQUIRE(hello(rig, cid, "bob") == "ok");
    (void)rig.watch(cid, records({"T_WatchRow"}), "alice");
    CHECK(describe(rig, cid, "alice").kind == "ok");
    REQUIRE(rig.recordWatchers("T_WatchRow") == std::vector<ConnectionId>{cid});

    // ...and one after the watches, under another principal or none, drops none.
    REQUIRE(hello(rig, cid, "bob") == "ok");
    REQUIRE(hello(rig, cid, "") == "ok");
    CHECK(rig.recordWatchers("T_WatchRow") == std::vector<ConnectionId>{cid});
    CHECK(describe(rig, cid, "alice").kind == "ok");
    CHECK(rig.recordWatchers("T_WatchRow") == std::vector<ConnectionId>{cid});
}

TEST_CASE("same_principal_new_token_keeps_watches", "[push][watch]") {
    Rig rig;
    auto const cid = rig.server->openConnection();
    (void)rig.watch(cid, records({"T_WatchRow"}), "alice");

    // A refreshed token verifies to the same principal: the baseline holds.
    CHECK(describe(rig, cid, "alice:refreshed").kind == "ok");

    CHECK(rig.recordWatchers("T_WatchRow") == std::vector<ConnectionId>{cid});
    auto const token = rig.onStrand([cid](auto const& table) -> std::optional<std::string> {
        auto const* watch = table.watchOf(cid, "T_WatchRow");
        return watch == nullptr ? std::nullopt : std::optional{watch->context.token};
    });
    CHECK(token == std::optional<std::string>{"alice"});
}

TEST_CASE("first_stamp_sets_baseline_even_when_unauthenticated", "[push][watch]") {
    Rig rig;
    auto const cid = rig.server->openConnection();

    // The first stamped envelope carries no valid token: its empty principal is the baseline.
    (void)rig.watch(cid, records({"T_WatchRow"}), "");
    REQUIRE(rig.recordWatchers("T_WatchRow") == std::vector<ConnectionId>{cid});

    CHECK(describe(rig, cid, "alice").kind == "ok");

    CHECK(rig.recordWatchers("T_WatchRow").empty());
}

TEST_CASE("refused_execute_still_drops_watches", "[push][watch]") {
    Rig rig;
    auto const cid = rig.server->openConnection();
    (void)rig.watch(cid, records({"T_WatchRow"}), "alice");
    REQUIRE(rig.recordWatchers("T_WatchRow") == std::vector<ConnectionId>{cid});

    // Signed out, the client sends only executes, each refused at `authorize`.
    auto const reply = executeOn(rig, cid, kSecretModel, "T_WatchListSecrets", 1, "");
    CHECK(reply.kind == "err");
    CHECK(reply.message == "unauthorized");

    CHECK(rig.recordWatchers("T_WatchRow").empty());
}

TEST_CASE("execute_authenticates_once", "[push][watch]") {
    Rig rig;
    auto const cid = rig.server->openConnection();
    auto registerRow = ::morph::wire::makeRegister("T_WatchRowModel");
    registerRow.session.token = "alice";
    auto const registered = ::morph::wire::decode(rig.server->handleInline(::morph::wire::encode(registerRow), cid));
    REQUIRE(registered.kind == "ok");

    int const before = rig.guard->authenticateCalls.load();
    CHECK(executeOn(rig, cid, "T_WatchRowModel", "T_WatchListRows", registered.modelId, "alice").kind == "ok");
    CHECK(rig.guard->authenticateCalls.load() == before + 1);

    CHECK(executeOn(rig, cid, kSecretModel, "T_WatchListSecrets", 1, "alice").kind == "err");
    CHECK(rig.guard->authenticateCalls.load() == before + 2);
}

// ── Fan-out: what a successful execute tells the watching connections ───────

namespace {

constexpr std::string_view kFanModel = "T_FanModel";
constexpr std::string_view kFanHello = "t-fan-hello";

/// Negotiates events for the connection a `t-fan-hello` arrives on, as the UI
/// service's `ui-hello` does.
class FanHello final : public ::morph::backend::IEnvelopeService {
public:
    using IEnvelopeService::handle;

    [[nodiscard]] bool serves(std::string_view kind) const override { return kind == kFanHello; }

    [[nodiscard]] std::string handle(::morph::wire::Envelope const& request,
                                     ::morph::session::IAuthorizer const& /*authorizer*/) override {
        return ::morph::wire::encode(::morph::wire::makeOk(request.callId));
    }

    [[nodiscard]] std::string handle(::morph::wire::Envelope const& request,
                                     ::morph::session::IAuthorizer const& authorizer,
                                     ::morph::backend::ConnectionFacts& facts) override {
        facts.events = true;
        return handle(request, authorizer);
    }
};

/// What the server wrote to one connection's writer: events only, since
/// replies go to the reply callback.
struct EventWire {
    std::mutex mtx;
    std::vector<std::string> sent;

    ::morph::backend::ConnectionWriter writer() {
        return {.write =
                    [this](std::string frame) {
                        std::scoped_lock const lock{mtx};
                        sent.push_back(std::move(frame));
                    },
                .backlog = [] { return std::size_t{0}; }};
    }

    std::vector<std::string> snapshot() {
        std::scoped_lock const lock{mtx};
        return sent;
    }

    /// The `changes` payloads received, decoded.
    std::vector<::morph::wire::ChangesBody> changes() {
        std::vector<::morph::wire::ChangesBody> bodies;
        for (auto const& frame : snapshot()) {
            auto const env = ::morph::wire::decode(frame);
            REQUIRE(env.kind == ::morph::wire::kEventKind);
            ::morph::wire::EventBody event;
            REQUIRE_FALSE(glz::read_json(event, env.body));
            REQUIRE(event.topic == ::morph::wire::kChangesTopic);
            auto body = ::morph::wire::readChangesBody(event.payload.str);
            REQUIRE(body);
            bodies.push_back(std::move(*body));
        }
        return bodies;
    }
};

struct FanRig {
    ::morph::exec::ThreadPoolExecutor pool{2};
    std::shared_ptr<RemoteServer> server;
    std::map<ConnectionId, std::unique_ptr<EventWire>> wires;

    explicit FanRig(std::chrono::milliseconds executeTimeout = std::chrono::milliseconds{0},
                    std::shared_ptr<::morph::session::IAuthorizer> authorizer = nullptr) {
        ::morph::backend::ServerConfig config;
        config.limits.executeTimeout = executeTimeout;
        config.services.push_back(std::make_shared<FanHello>());
        server = authorizer ? std::make_shared<RemoteServer>(pool, std::move(authorizer), std::move(config))
                            : std::make_shared<RemoteServer>(pool, std::move(config));
    }

    /// Opens a connection with a writer, negotiating events when @p events.
    ConnectionId connect(bool events = true) {
        auto const cid = server->openConnection();
        auto& wire = wires[cid];
        wire = std::make_unique<EventWire>();
        server->setConnectionWriter(cid, wire->writer());
        if (events) {
            ::morph::wire::Envelope hello;
            hello.kind = std::string{kFanHello};
            hello.callId = 1;
            REQUIRE(::morph::wire::decode(server->handleInline(::morph::wire::encode(hello), cid)).kind == "ok");
        }
        return cid;
    }

    EventWire& wire(ConnectionId cid) { return *wires.at(cid); }

    void watch(ConnectionId cid, ::morph::wire::WatchBody const& body, std::string const& token = {}) {
        ::morph::wire::Envelope env;
        env.kind = std::string{::morph::wire::kWatchKind};
        env.callId = 2;
        env.session.token = token;
        env.body = glz::write_json(body).value_or(std::string{});
        auto const reply = ::morph::wire::decode(server->handleInline(::morph::wire::encode(env), cid));
        REQUIRE(reply.kind == "ok");
        auto const parsed = ::morph::wire::readWatchReply(reply.body);
        REQUIRE(parsed);
        REQUIRE(parsed->refusedRecords.empty());
        REQUIRE(parsed->refusedInstances.empty());
    }

    /// Registers a private instance on @p cid, or a keyed shared one when @p primary is set.
    std::uint64_t create(ConnectionId cid, std::string primary = {}, std::string const& token = {}) {
        auto env = primary.empty() ? ::morph::wire::makeRegister(std::string{kFanModel})
                                         : ::morph::wire::makeRegisterShared(std::string{kFanModel}, primary);
        env.session.token = token;
        auto const reply = ::morph::wire::decode(server->handleInline(::morph::wire::encode(env), cid));
        REQUIRE(reply.kind == "ok");
        REQUIRE(reply.modelId != 0U);
        return reply.modelId;
    }

    /// Executes @p actionType with @p body on @p modelId through @p cid and
    /// waits for the reply.
    ::morph::wire::Envelope execute(ConnectionId cid, std::uint64_t modelId, std::string_view actionType,
                                    std::string body, std::string const& token = {}) {
        ::morph::wire::Envelope env;
        env.kind = "execute";
        env.session.token = token;
        env.callId = 9;
        env.modelId = modelId;
        env.modelType = std::string{kFanModel};
        env.actionType = std::string{actionType};
        env.body = std::move(body);
        auto waiter = std::make_shared<::morph::testing::WaitReply>();
        server->handle(::morph::wire::encode(env), [waiter](std::string const& msg) { (*waiter)(msg); }, cid);
        REQUIRE(waiter->await());
        return waiter->env;
    }

    /// Runs every task already posted to the server strand.
    void flush() {
        std::latch done{1};
        server->strand().post([&done] { done.count_down(); });
        done.wait();
    }

    /// The connections watching record type @p record, read on the server strand.
    std::vector<ConnectionId> recordWatchers(std::string_view record) {
        std::vector<ConnectionId> watchers;
        std::latch done{1};
        server->strand().post([&] {
            watchers = server->watchTable().watchersOf(record);
            done.count_down();
        });
        done.wait();
        return watchers;
    }
};

/// The keys of @p change, as raw JSON text.
std::vector<std::string> keysOf(::morph::effects::RecordChange const& change) {
    std::vector<std::string> keys;
    if (change.keys) {
        for (auto const& key : *change.keys) {
            keys.push_back(key.str);
        }
    }
    return keys;
}

}  // namespace

TEST_CASE("two_watchers_both_receive_caller_included", "[push][watch]") {
    FanRig rig;
    auto const caller = rig.connect();
    auto const other = rig.connect();
    rig.watch(caller, records({"T_FanRow"}));
    rig.watch(other, records({"T_FanRow"}));
    auto const mid = rig.create(caller);

    CHECK(rig.execute(caller, mid, "T_FanEdit", R"({"id":5})").kind == "ok");
    rig.flush();

    for (auto const cid : {caller, other}) {
        auto const bodies = rig.wire(cid).changes();
        REQUIRE(bodies.size() == 1U);
        REQUIRE(bodies.front().records.size() == 1U);
        CHECK(bodies.front().records.front().record == "T_FanRow");
        CHECK(keysOf(bodies.front().records.front()) == std::vector<std::string>{"5"});
        CHECK(bodies.front().instances.empty());
    }
}

TEST_CASE("unwatched_type_posts_nothing", "[push][watch]") {
    FanRig rig;
    auto const caller = rig.connect();
    auto const other = rig.connect();
    rig.watch(other, records({"T_FanOther"}));
    auto const mid = rig.create(caller);

    CHECK(rig.execute(caller, mid, "T_FanEdit", R"({"id":5})").kind == "ok");
    // A reader changes nothing, so it posts nothing either.
    CHECK(rig.execute(caller, mid, "T_FanListOthers", R"({"limit":1})").kind == "ok");
    rig.flush();

    CHECK(rig.wire(caller).snapshot().empty());
    CHECK(rig.wire(other).snapshot().empty());
}

TEST_CASE("unnegotiated_connection_receives_nothing", "[push][watch]") {
    FanRig rig;
    auto const negotiated = rig.connect();
    auto const silent = rig.connect(false);
    rig.watch(negotiated, records({"T_FanRow"}));
    rig.watch(silent, records({"T_FanRow"}));
    auto const mid = rig.create(silent);

    CHECK(rig.execute(silent, mid, "T_FanEdit", R"({"id":3})").kind == "ok");
    rig.flush();

    CHECK(rig.wire(silent).snapshot().empty());
    CHECK(rig.wire(negotiated).changes().size() == 1U);
}

TEST_CASE("private_instance_change_only_to_owner", "[push][watch]") {
    FanRig rig;
    auto const owner = rig.connect();
    auto const stranger = rig.connect();
    auto const mid = rig.create(owner);
    // The stranger watches a keyed topic of the same type; a private instance
    // never matches it.
    rig.watch(stranger, instance(std::string{kFanModel}, "k"));

    // Run by a connection that does not own the instance: the owner is told,
    // the caller is not.
    CHECK(rig.execute(stranger, mid, "T_FanTouch", R"({"by":1})").kind == "ok");
    CHECK(rig.execute(owner, mid, "T_FanTouch", R"({"by":1})").kind == "ok");
    rig.flush();

    CHECK(rig.wire(stranger).snapshot().empty());
    auto const bodies = rig.wire(owner).changes();
    REQUIRE(bodies.size() == 2U);
    for (auto const& body : bodies) {
        CHECK(body.records.empty());
        REQUIRE(body.instances.size() == 1U);
        CHECK(body.instances.front().modelType == kFanModel);
        CHECK(body.instances.front().primary.empty());
        CHECK(body.instances.front().instance == mid);
    }
}

TEST_CASE("keyed_instance_created_later_reaches_watcher", "[push][watch]") {
    FanRig rig;
    auto const watcher = rig.connect();
    auto const caller = rig.connect();
    auto const bystander = rig.connect();
    // Watched before any connection has created the instance.
    rig.watch(watcher, instance(std::string{kFanModel}, "k1"));
    rig.watch(bystander, instance(std::string{kFanModel}, "k2"));

    auto const mid = rig.create(caller, "k1");
    CHECK(rig.execute(caller, mid, "T_FanTouch", R"({"by":2})").kind == "ok");
    rig.flush();

    auto const bodies = rig.wire(watcher).changes();
    REQUIRE(bodies.size() == 1U);
    REQUIRE(bodies.front().instances.size() == 1U);
    CHECK(bodies.front().instances.front().modelType == kFanModel);
    CHECK(bodies.front().instances.front().primary == "k1");
    CHECK(bodies.front().instances.front().instance == mid);
    // The caller does not watch the key, and a keyed instance has no owner to tell.
    CHECK(rig.wire(caller).snapshot().empty());
    CHECK(rig.wire(bystander).snapshot().empty());
}

TEST_CASE("timed_out_call_still_reaches_caller_as_event", "[push][watch]") {
    FanRig rig{std::chrono::milliseconds{10}};
    auto const caller = rig.connect();
    rig.watch(caller, records({"T_FanRow"}));
    auto const mid = rig.create(caller);

    auto const reply = rig.execute(caller, mid, "T_FanSlowEdit", R"({"id":8,"delayMs":50})");
    REQUIRE(reply.kind == "err");
    CHECK(reply.message == "timeout");
    CHECK(rig.wire(caller).snapshot().empty());

    // The handler finishes after the timeout answered; its change still arrives.
    REQUIRE(::morph::testing::waitUntil([&] { return !rig.wire(caller).snapshot().empty(); }));
    auto const bodies = rig.wire(caller).changes();
    REQUIRE(bodies.size() == 1U);
    REQUIRE(bodies.front().records.size() == 1U);
    CHECK(keysOf(bodies.front().records.front()) == std::vector<std::string>{"8"});
}

TEST_CASE("failed_execute_posts_nothing", "[push][watch]") {
    FanRig rig;
    auto const caller = rig.connect();
    auto const other = rig.connect();
    rig.watch(caller, records({"T_FanRow"}));
    rig.watch(other, records({"T_FanRow"}));
    auto const mid = rig.create(caller);

    CHECK(rig.execute(caller, mid, "T_FanFail", R"({"id":5})").kind == "err");
    rig.flush();

    CHECK(rig.wire(caller).snapshot().empty());
    CHECK(rig.wire(other).snapshot().empty());
}

TEST_CASE("execute_under_other_principal_drops_watches_before_its_fanout", "[push][watch]") {
    FanRig rig{std::chrono::milliseconds{0}, std::make_shared<SecretGuard>()};
    auto const shared = rig.connect();
    auto const other = rig.connect();
    rig.watch(shared, records({"T_FanRow"}), "alice");
    rig.watch(other, records({"T_FanRow"}), "alice");
    auto const mid = rig.create(shared, {}, "alice");

    // bob executes on alice's connection: admitted, and its change fans out...
    CHECK(rig.execute(shared, mid, "T_FanEdit", R"({"id":5})", "bob").kind == "ok");
    rig.flush();

    // ...to the connection still watching, but not to the watch bob's execute dropped.
    CHECK(rig.wire(other).changes().size() == 1U);
    CHECK(rig.wire(shared).changes().empty());
    CHECK(rig.recordWatchers("T_FanRow") == std::vector<ConnectionId>{other});
}
