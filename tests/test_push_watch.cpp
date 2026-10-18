// SPDX-License-Identifier: Apache-2.0

// The server's per-connection watch table and the core `watch`/`unwatch`
// kinds: what a watch may name, how it is stored, and what drops it.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <functional>
#include <latch>
#include <memory>
#include <morph/core/bridge.hpp>
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
#include <utility>
#include <vector>

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

}  // namespace push_watch_test

using namespace push_watch_test;

BRIDGE_REGISTER_RECORD(Row, "T_WatchRow")
BRIDGE_REGISTER_RECORD(SecretRow, "T_WatchSecret")
BRIDGE_REGISTER_MODEL(RowModel, "T_WatchRowModel")
BRIDGE_REGISTER_ACTION(RowModel, ListRows, "T_WatchListRows")
BRIDGE_REGISTER_MODEL(SecretModel, "T_WatchSecretModel")
BRIDGE_REGISTER_ACTION(SecretModel, ListSecrets, "T_WatchListSecrets")

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
    mutable std::mutex mtx;
    mutable std::vector<InstanceQuestion> instanceQuestions;

    [[nodiscard]] bool authorize(const ::morph::session::Context& ctx, std::string_view modelType,
                                 std::string_view /*actionType*/) const override {
        authorizeCalls.fetch_add(1);
        return modelType != kSecretModel && !ctx.token.ends_with(":scoped");
    }
    [[nodiscard]] std::optional<std::string> authenticate(const ::morph::session::Context& ctx) const override {
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
