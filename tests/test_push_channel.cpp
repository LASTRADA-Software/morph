// SPDX-License-Identifier: Apache-2.0

// The server-initiated `event` kind: its builder, its routing ahead of reply
// matching, and the local backend's delivery seam.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <functional>
#include <morph/core/backend.hpp>
#include <morph/core/detail/reply_router.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/wire.hpp>
#include <string>
#include <utility>
#include <vector>

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
