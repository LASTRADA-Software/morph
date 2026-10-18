// SPDX-License-Identifier: Apache-2.0

// The server-initiated `event` kind: its builder, its routing ahead of reply
// matching, the local backend's delivery seam, and `RemoteServer` writing
// events to a connection and holding them back from a slow reader.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <latch>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/detail/reply_router.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/observability.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "test_support.hpp"

namespace {

using ::morph::backend::detail::PendingCallTable;
using ::morph::backend::detail::routeEvent;

struct InlineExec final : ::morph::exec::IExecutor {
    int posted = 0;
    void post(std::function<void()> task) override {
        ++posted;
        task();
    }
};

}  // namespace

TEST_CASE("make_event_round_trips_through_encode_and_route", "[push][channel]") {
    auto env = ::morph::wire::makeEvent("changes", R"({"a":[1,2]})");
    REQUIRE(env.kind == "event");
    REQUIRE(env.callId == 0);
    auto decoded = ::morph::wire::decode(::morph::wire::encode(env));

    std::vector<::morph::wire::EventBody> got;
    REQUIRE(routeEvent(decoded, [&](::morph::wire::EventBody e) { got.push_back(std::move(e)); }));
    REQUIRE(got.size() == 1);
    CHECK(got[0].topic == "changes");
    CHECK(got[0].payload.str == R"({"a":[1,2]})");
}

TEST_CASE("reply_router_routes_event_before_call_match", "[push][channel]") {
    PendingCallTable<int> table;
    table.insert(0, 7);

    int calls = 0;
    auto const handler = [&](::morph::wire::EventBody) { ++calls; };
    CHECK(routeEvent(::morph::wire::makeEvent("resync", "null"), handler));
    CHECK(calls == 1);
    CHECK(table.size() == 1);  // the pending callId 0 is untouched

    // A reply is not an event: the caller proceeds to reply matching.
    CHECK_FALSE(routeEvent(::morph::wire::makeOk(0), handler));
    CHECK(calls == 1);
}

TEST_CASE("reply_router_drops_malformed_event_and_event_without_handler", "[push][channel]") {
    ::morph::wire::Envelope bad;
    bad.kind = "event";
    bad.body = "not json";
    int calls = 0;
    CHECK(routeEvent(bad, [&](::morph::wire::EventBody) { ++calls; }));
    CHECK(calls == 0);
    CHECK(routeEvent(::morph::wire::makeEvent("changes", "{}"), {}));
}

TEST_CASE("local_backend_deliver_event_reaches_handler_on_exec", "[push][channel]") {
    InlineExec pool;
    InlineExec cb;
    ::morph::backend::LocalBackend backend{pool};

    backend.deliverEvent({"changes", {"{}"}});  // no handler: dropped
    CHECK(cb.posted == 0);

    std::vector<std::string> topics;
    backend.setEventHandler([&](::morph::wire::EventBody e) { topics.push_back(e.topic); }, &cb);
    backend.deliverEvent({"changes", {"{}"}});
    backend.deliverEvent({"resync", {"null"}});
    CHECK(cb.posted == 2);
    CHECK(topics == std::vector<std::string>{"changes", "resync"});

    backend.setEventHandler(nullptr, nullptr);
    backend.deliverEvent({"changes", {"{}"}});
    CHECK(topics.size() == 2);
}

// ── RemoteServer: writing events to a connection ────────────────────────────

struct PcBumpAction {};
struct PcCounterModel {
    int count = 0;
    int execute(const PcBumpAction&) { return ++count; }
};

template <>
struct morph::model::ModelTraits<PcCounterModel> {
    static constexpr std::string_view typeId() { return "PC_CounterModel"; }
};
template <>
struct morph::model::ActionTraits<PcBumpAction> {
    using Result = int;
    static constexpr std::string_view typeId() { return "PC_BumpAction"; }
    static std::string toJson(const PcBumpAction&) { return "{}"; }
    static PcBumpAction fromJson(std::string_view) { return {}; }
    static std::string resultToJson(const int& res) { return std::to_string(res); }
    static int resultFromJson(std::string_view json) { return std::stoi(std::string{json}); }
};

namespace {

struct PcEnv {
    ::morph::model::detail::ActionDispatcher dispatcher;
    ::morph::model::detail::ModelRegistryFactory registry;
};

PcEnv& pcEnv() {
    static PcEnv env = [] {
        PcEnv made;
        made.registry.registerModel<PcCounterModel>("PC_CounterModel");
        made.dispatcher.registerAction<PcCounterModel, PcBumpAction>("PC_CounterModel", "PC_BumpAction");
        return made;
    }();
    return env;
}

/// What a transport put on one connection's wire, replies and events alike,
/// in the order it was handed them.
struct FakeWire {
    std::mutex mtx;
    std::vector<std::string> sent;
    std::atomic<std::size_t> backlog{0};

    void push(std::string frame) {
        std::scoped_lock const lock{mtx};
        sent.push_back(std::move(frame));
    }
    std::vector<std::string> snapshot() {
        std::scoped_lock const lock{mtx};
        return sent;
    }
    ::morph::backend::ConnectionWriter writer() {
        return {.write = [this](std::string frame) { push(std::move(frame)); },
                .backlog = [this] { return backlog.load(); }};
    }
};

/// Runs every task already posted to the server strand before returning.
void flush(::morph::backend::RemoteServer& server) {
    std::latch done{1};
    server.strand().post([&done] { done.count_down(); });
    done.wait();
}

std::string encodedEvent(std::string_view topic, std::string payload) {
    return ::morph::wire::encode(::morph::wire::makeEvent(topic, std::move(payload)));
}

}  // namespace

TEST_CASE("post_reaches_connection_writer", "[push][channel]") {
    ::morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<::morph::backend::RemoteServer>(pool, pcEnv().dispatcher, pcEnv().registry);
    FakeWire wire;
    auto const cid = server->openConnection();
    server->setConnectionWriter(cid, wire.writer());

    auto const first = encodedEvent("changes", R"({"n":1})");
    auto const second = encodedEvent("changes", R"({"n":2})");
    server->post(cid, first);
    server->post(cid, second);
    flush(*server);

    CHECK(wire.snapshot() == std::vector<std::string>{first, second});
}

TEST_CASE("backlog_holds_events_not_replies", "[push][channel]") {
    ::morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<::morph::backend::RemoteServer>(pool, pcEnv().dispatcher, pcEnv().registry);
    FakeWire wire;
    wire.backlog = std::size_t{2} << 20;
    auto const cid = server->openConnection();
    server->setConnectionWriter(cid, wire.writer());
    auto const reply = [&wire](std::string frame) { wire.push(std::move(frame)); };

    server->post(cid, encodedEvent("changes", "{}"));
    server->handle(::morph::wire::encode(::morph::wire::makeRegister("PC_CounterModel")), reply, cid);
    REQUIRE(::morph::testing::waitUntil([&] { return wire.snapshot().size() == 1; }));
    auto const registered = ::morph::wire::decode(wire.snapshot().front());
    REQUIRE(registered.kind == "ok");

    ::morph::wire::Envelope exec;
    exec.kind = "execute";
    exec.callId = 7;
    exec.modelId = registered.modelId;
    exec.modelType = "PC_CounterModel";
    exec.actionType = "PC_BumpAction";
    exec.body = "{}";
    server->handle(::morph::wire::encode(exec), reply, cid);
    server->post(cid, encodedEvent("changes", "{}"));
    REQUIRE(::morph::testing::waitUntil([&] { return wire.snapshot().size() == 2; }));
    server->post(cid, encodedEvent("changes", "{}"));
    flush(*server);

    auto held = wire.snapshot();
    REQUIRE(held.size() == 2);
    for (const auto& frame : held) {
        CHECK(::morph::wire::decode(frame).kind == "ok");
    }

    // Below the bound but not yet drained: the resync is still owed, so an
    // event now would arrive ahead of it.
    wire.backlog = 0;
    server->post(cid, encodedEvent("changes", "{}"));
    flush(*server);
    REQUIRE(wire.snapshot().size() == 2);

    server->connectionDrained(cid);
    flush(*server);
    auto const drained = wire.snapshot();
    REQUIRE(drained.size() == 3);
    CHECK(drained[2] == encodedEvent("resync", "{}"));

    // The resync is owed once: a second drain sends nothing, and events flow again.
    server->connectionDrained(cid);
    auto const after = encodedEvent("changes", R"({"n":3})");
    server->post(cid, after);
    flush(*server);
    auto const resumed = wire.snapshot();
    REQUIRE(resumed.size() == 4);
    CHECK(resumed[3] == after);
}

TEST_CASE("post_after_close_is_dropped", "[push][channel]") {
    ::morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<::morph::backend::RemoteServer>(pool, pcEnv().dispatcher, pcEnv().registry);
    FakeWire wire;
    auto const cid = server->openConnection();
    server->setConnectionWriter(cid, wire.writer());
    server->closeConnection(cid);
    server->post(cid, encodedEvent("changes", "{}"));
    flush(*server);
    CHECK(wire.snapshot().empty());

    // A writer handed over for a connection already closed is not kept.
    server->setConnectionWriter(cid, wire.writer());
    server->post(cid, encodedEvent("changes", "{}"));
    server->post(0, encodedEvent("changes", "{}"));
    flush(*server);
    CHECK(wire.snapshot().empty());
}

TEST_CASE("post_above_envelope_cap_is_dropped_and_counted", "[push][channel]") {
    ::morph::observe::ScopedObserveOverride const restore;
    std::atomic<int> oversize{0};
    ::morph::observe::setMetricSink([&oversize](const ::morph::observe::MetricEvent& evt) {
        if (evt.metric == ::morph::observe::Metric::eventOversize) {
            oversize.fetch_add(1);
        }
    });
    ::morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<::morph::backend::RemoteServer>(pool, pcEnv().dispatcher, pcEnv().registry);
    FakeWire wire;
    auto const cid = server->openConnection();
    server->setConnectionWriter(cid, wire.writer());

    server->post(cid, std::string(::morph::wire::kMaxEnvelopeBytes + 1, 'x'));
    auto const fits = encodedEvent("changes", "{}");
    server->post(cid, fits);
    flush(*server);

    CHECK(oversize.load() == 1);
    CHECK(wire.snapshot() == std::vector<std::string>{fits});
}

TEST_CASE("throwing_connection_writer_does_not_escape_the_server", "[push][channel]") {
    ::morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<::morph::backend::RemoteServer>(pool, pcEnv().dispatcher, pcEnv().registry);
    std::atomic<int> attempts{0};
    auto const cid = server->openConnection();
    server->setConnectionWriter(cid, {.write =
                                          [&attempts](const std::string&) {
                                              attempts.fetch_add(1);
                                              throw std::runtime_error("socket gone");
                                          },
                                      .backlog = [] { return std::size_t{0}; }});
    server->post(cid, encodedEvent("changes", "{}"));
    server->post(cid, encodedEvent("changes", "{}"));
    flush(*server);
    CHECK(attempts.load() == 2);

    // The strand is still serving.
    ::morph::testing::WaitReply hello;
    server->handle(::morph::wire::encode(::morph::wire::makeHello()), std::ref(hello), cid);
    REQUIRE(hello.await());
    CHECK(hello.env.kind == "ok");
}
