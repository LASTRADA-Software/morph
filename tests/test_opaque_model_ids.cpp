// SPDX-License-Identifier: Apache-2.0
//
// Tests for opaque (non-sequential, unguessable) RemoteServer model ids.
// RemoteServer used to assign ids from a bare sequential counter
// (`_nextId.fetch_add(1) + 1`); this file pins the replacement: a keyed
// 64-bit permutation (`morph::backend::detail::OpaqueIdGenerator`) applied to
// the counter, so ids are still guaranteed unique (the permutation is a
// bijection) but are not sequential or predictable from a previously
// observed id. See docs/spec/core/backend.md and docs/spec/security.md.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <string>
#include <string_view>
#include <unordered_set>

#include "test_support.hpp"

// Model/action types need external linkage for glaze reflection.
struct OpqEchoAction {
    int x = 0;
};
struct OpqEchoModel {
    int execute(const OpqEchoAction& act) { return act.x; }
};

template <>
struct morph::model::ModelTraits<OpqEchoModel> {
    static constexpr std::string_view typeId() { return "OPQ_EchoModel"; }
};
template <>
struct morph::model::ActionTraits<OpqEchoAction> {
    using Result = int;
    static constexpr std::string_view typeId() { return "OPQ_EchoAction"; }
    static std::string toJson(const OpqEchoAction& act) {
        std::string out;
        (void)glz::write_json(act, out);
        return out;
    }
    static OpqEchoAction fromJson(std::string_view json) {
        OpqEchoAction action{};
        (void)glz::read_json(action, json);
        return action;
    }
    static std::string resultToJson(const int& res) {
        std::string out;
        (void)glz::write_json(res, out);
        return out;
    }
    static int resultFromJson(std::string_view json) {
        int result{};
        (void)glz::read_json(result, json);
        return result;
    }
};

namespace {

struct OpqEnv {
    morph::model::detail::ActionDispatcher dispatcher;
    morph::model::detail::ModelRegistryFactory registry;
    OpqEnv() {
        registry.registerModel<OpqEchoModel>("OPQ_EchoModel");
        dispatcher.registerAction<OpqEchoModel, OpqEchoAction>("OPQ_EchoModel", "OPQ_EchoAction");
    }
};

uint64_t registerOnce(const std::shared_ptr<morph::backend::RemoteServer>& server) {
    morph::testing::WaitReply reg;
    server->handle(morph::wire::encode(morph::wire::makeRegister("OPQ_EchoModel")), std::ref(reg));
    REQUIRE(reg.await());
    REQUIRE(reg.env.kind == "ok");
    return reg.env.modelId;
}

}  // namespace

// ── Unit tests: the keyed permutation itself ─────────────────────────────────

TEST_CASE("OpaqueIdGenerator is a bijection: 20000 counters produce 20000 distinct ids", "[opaque_id][unit]") {
    morph::backend::detail::OpaqueIdGenerator gen;
    std::unordered_set<uint64_t> seen;
    constexpr uint64_t n = 20000;
    for (uint64_t counter = 1; counter <= n; ++counter) {
        seen.insert(gen.permute(counter));
    }
    REQUIRE(seen.size() == n);
}

// ── morph#453: the bijection above cannot see the high 32 bits ──
//
// Counters 1..20000 all have a zero high half, so the case above passes whether
// or not `permute` uses `counter >> 32` at all -- it cannot distinguish a
// 64-bit permutation from one that silently ignores the top half. That is the
// "would this still pass if the feature did nothing?" failure, and the mutation
// campaign found it: three survivors sit on the Feistel structure
// (`counter >> 32`, `hi ^ mix(...)`, `hi << 32`), none of which the sample can
// reach.
//
// Simulated against the real round function, over 20000 counters that *do*
// exercise the high half: replacing `>>` with `<<` collapses 20000 ids to **1**,
// and replacing the Feistel `^` with `|` collapses them to **1256**. Both are
// catastrophic id collisions -- for opaque model ids, a security-relevant one --
// and both are invisible to the existing sample.
TEST_CASE("OpaqueIdGenerator is a bijection over counters that exercise the high half",
          "[opaque_id][unit][morph453]") {
    morph::backend::detail::OpaqueIdGenerator gen;
    std::unordered_set<uint64_t> seen;
    constexpr uint64_t n = 20000;
    // Stride by 2^32 so every counter has a distinct, non-zero high word; the
    // +7 keeps the low half non-zero too, so neither half is degenerate.
    for (uint64_t i = 0; i < n; ++i) {
        seen.insert(gen.permute((i << 32U) + 7U));
    }
    REQUIRE(seen.size() == n);

    // And mixing both halves: adjacent counters differing only in the high word
    // must not map to the same id.
    REQUIRE(gen.permute(1U) != gen.permute((1ULL << 32U) + 1U));
    REQUIRE(gen.permute(0U) != gen.permute(1ULL << 32U));
}

TEST_CASE("OpaqueIdGenerator output is not sequential", "[opaque_id][unit]") {
    morph::backend::detail::OpaqueIdGenerator gen;
    const uint64_t first = gen.permute(1);
    const uint64_t second = gen.permute(2);
    const uint64_t third = gen.permute(3);
    REQUIRE(second != first + 1);
    REQUIRE(third != second + 1);
    REQUIRE(third != first + 2);
}

TEST_CASE("OpaqueIdGenerator is keyed: independent instances disagree on the same counter", "[opaque_id][unit]") {
    morph::backend::detail::OpaqueIdGenerator genA;
    morph::backend::detail::OpaqueIdGenerator genB;
    // Each instance draws its round keys independently from std::random_device
    // at construction, so two instances agreeing on permute(1) has probability
    // ~1/2^64 — never observed in practice. This is what makes the permutation
    // opaque: without the key, an observed id cannot be inverted to recover the
    // counter or predict the next one.
    REQUIRE(genA.permute(1) != genB.permute(1));
}

// ── Integration tests: RemoteServer::register issues opaque ids ─────────────

TEST_CASE("two successive registers return non-adjacent ids", "[opaque_id][remote]") {
    morph::testing::InlineExecutor pool;
    OpqEnv env;
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, env.dispatcher, env.registry);

    const auto id1 = registerOnce(server);
    const auto id2 = registerOnce(server);
    REQUIRE(id1 != id2);
    REQUIRE(id2 != id1 + 1);
    REQUIRE(id1 != 0U);
    REQUIRE(id2 != 0U);
}

TEST_CASE("a returned id round-trips through execute and deregister", "[opaque_id][remote]") {
    morph::testing::InlineExecutor pool;
    OpqEnv env;
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, env.dispatcher, env.registry);

    const auto mid = registerOnce(server);

    morph::wire::Envelope exec;
    exec.kind = "execute";
    exec.modelId = mid;
    exec.modelType = "OPQ_EchoModel";
    exec.actionType = "OPQ_EchoAction";
    exec.body = R"({"x":7})";
    morph::testing::WaitReply run;
    server->handle(morph::wire::encode(exec), std::ref(run));
    REQUIRE(run.await());
    REQUIRE(run.env.kind == "ok");
    REQUIRE(run.env.body == "7");

    morph::testing::WaitReply dereg;
    server->handle(morph::wire::encode(morph::wire::makeDeregister(mid)), std::ref(dereg));
    REQUIRE(dereg.await());
    REQUIRE(dereg.env.kind == "ok");
}

TEST_CASE("a 2000-round register churn produces zero id collisions", "[opaque_id][remote]") {
    morph::exec::ThreadPoolExecutor pool{4};
    OpqEnv env;
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, env.dispatcher, env.registry);

    constexpr int rounds = 2000;
    std::unordered_set<uint64_t> ids;
    for (int i = 0; i < rounds; ++i) {
        ids.insert(registerOnce(server));
    }
    REQUIRE(ids.size() == static_cast<std::size_t>(rounds));
}
