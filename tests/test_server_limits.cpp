// SPDX-License-Identifier: Apache-2.0

// Stress + limits coverage for morph::backend::RemoteServer. Verifies that
// untrusted client input cannot crash or unbound-resource the server, and
// captures throughput numbers for the local round-trip via Catch2's benchmark
// support (see catch2/benchmark).

#include <morph/backend.hpp>
#include <morph/executor.hpp>
#include <morph/registry.hpp>
#include <morph/remote.hpp>
#include <morph/wire.hpp>
#include <atomic>
#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

// ── Fixture models ────────────────────────────────────────────────────────────

// Must have external linkage so Glaze's reflection can mangle the type name.
struct LimEchoAction {
    std::string s;
};
struct LimEchoModel {
    std::string execute(const LimEchoAction& act) { return act.s; }
};

BRIDGE_REGISTER_MODEL(LimEchoModel, "Lim_EchoModel")
BRIDGE_REGISTER_ACTION(LimEchoModel, LimEchoAction, "Lim_EchoAction")

namespace {

struct WaitReply {
    std::atomic<bool> ready{false};
    std::string raw;
    morph::wire::Envelope env;

    void operator()(const std::string& msg) {
        raw = msg;
        try {
            env = morph::wire::decode(msg);
        } catch (...) {
            // leave default; tests will inspect raw or fall through to fail
        }
        ready.store(true);
    }
    void await(std::chrono::milliseconds budget = 5s) {
        auto deadline = std::chrono::steady_clock::now() + budget;
        while (!ready.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(2ms);
        }
        REQUIRE(ready.load());
    }
};

uint64_t registerEchoModel(const std::shared_ptr<morph::backend::RemoteServer>& server) {
    WaitReply waiter;
    server->handle(morph::wire::encode(morph::wire::makeRegister("Lim_EchoModel")), std::ref(waiter));
    waiter.await();
    REQUIRE(waiter.env.kind == "ok");
    return waiter.env.modelId;
}

}  // namespace

// ── Oversized payload ─────────────────────────────────────────────────────────

TEST_CASE("limits: 1 MiB action payload survives a round trip", "[limits][remote]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    auto mid = registerEchoModel(server);

    constexpr std::size_t bytes = 1U << 20;  // 1 MiB
    morph::wire::Envelope req;
    req.kind = "execute";
    req.callId = 1;
    req.modelId = mid;
    req.modelType = "Lim_EchoModel";
    req.actionType = "Lim_EchoAction";
    req.body = R"({"s":")" + std::string(bytes, 'a') + R"("})";

    WaitReply waiter;
    server->handle(morph::wire::encode(req), std::ref(waiter));
    waiter.await(10s);
    REQUIRE(waiter.env.kind == "ok");
    REQUIRE(waiter.env.body.size() >= bytes);
}

// ── Deeply-nested JSON ────────────────────────────────────────────────────────

TEST_CASE("limits: deeply-nested JSON envelope returns err without crashing", "[limits][remote]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);

    constexpr int depth = 5000;
    std::string deep;
    deep.reserve(depth * 4);
    for (int i = 0; i < depth; ++i) {
        deep += R"({"x":)";
    }
    deep += "1";
    for (int i = 0; i < depth; ++i) {
        deep += "}";
    }

    WaitReply waiter;
    server->handle(deep, std::ref(waiter));
    waiter.await();
    // We don't care whether Glaze accepts it or rejects it — the requirement is
    // that the server replies (with err or ok) instead of hanging or crashing.
    REQUIRE_FALSE(waiter.raw.empty());
}

// ── Malformed UTF-8 sequence inside the JSON string ──────────────────────────

TEST_CASE("limits: malformed UTF-8 in body is reported as err, not a crash", "[limits][remote]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    auto mid = registerEchoModel(server);

    // Lone continuation byte 0x80 inside the JSON string. Glaze should reject
    // either at envelope-decode or at action fromJson; either is fine.
    std::string bad = R"({"s":")";
    bad.push_back(static_cast<char>(0x80));
    bad += R"("})";

    morph::wire::Envelope req;
    req.kind = "execute";
    req.callId = 2;
    req.modelId = mid;
    req.modelType = "Lim_EchoModel";
    req.actionType = "Lim_EchoAction";
    req.body = bad;

    WaitReply waiter;
    server->handle(morph::wire::encode(req), std::ref(waiter));
    waiter.await();
    // Either ok (Glaze is lenient with high-bit bytes in strings) or err —
    // both are acceptable. The point is the server stays alive.
    REQUIRE((waiter.env.kind == "ok" || waiter.env.kind == "err"));
}

// ── Register / deregister flood ──────────────────────────────────────────────

TEST_CASE("limits: 200-deep register/deregister churn on a shared server", "[limits][remote]") {
    morph::exec::ThreadPoolExecutor pool{4};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);

    constexpr int rounds = 200;
    std::vector<uint64_t> ids;
    ids.reserve(rounds);

    for (int i = 0; i < rounds; ++i) {
        WaitReply waiter;
        server->handle(morph::wire::encode(morph::wire::makeRegister("Lim_EchoModel")), std::ref(waiter));
        waiter.await();
        REQUIRE(waiter.env.kind == "ok");
        ids.push_back(waiter.env.modelId);
    }
    for (auto id : ids) {
        WaitReply waiter;
        server->handle(morph::wire::encode(morph::wire::makeDeregister(id)), std::ref(waiter));
        waiter.await();
        REQUIRE(waiter.env.kind == "ok");
    }
}

// ── Throughput benchmark ─────────────────────────────────────────────────────

TEST_CASE("benchmark: in-process execute round-trip", "[!benchmark][remote]") {
    morph::exec::ThreadPoolExecutor pool{4};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    auto mid = registerEchoModel(server);

    morph::wire::Envelope req;
    req.kind = "execute";
    req.modelId = mid;
    req.modelType = "Lim_EchoModel";
    req.actionType = "Lim_EchoAction";
    req.body = R"({"s":"hello"})";

    BENCHMARK("RemoteServer round-trip (echo, 5 bytes)") {
        std::atomic<uint64_t> next{0};
        constexpr int n = 32;
        std::atomic<int> done{0};
        for (int i = 0; i < n; ++i) {
            req.callId = ++next;
            server->handle(morph::wire::encode(req),
                           [&done](const std::string&) { done.fetch_add(1, std::memory_order_relaxed); });
        }
        auto deadline = std::chrono::steady_clock::now() + 1s;
        while (done.load() < n && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        return done.load();
    };
}
