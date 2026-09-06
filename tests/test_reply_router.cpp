// SPDX-License-Identifier: Apache-2.0

// Direct tests for the callId-multiplexed execute-reply router extracted out of
// SocketBackend (include/morph/core/detail/reply_router.hpp).
//
// The point of the extraction, and of this file, is cost: every case here is a
// pure function of a wire::Envelope or of an in-memory map, so none of them
// needs a ThreadPoolExecutor, a RemoteServer, a SocketServer bound to a real
// ephemeral port, or a Bridge -- which is what tests/net/test_socket_backend.cpp
// has to stand up (~43 lines and, for the timeout case, ~350ms of wall clock)
// before it can assert anything. These run in microseconds and can therefore
// cover arms the transport-level tests never reach at all: an `ok` reply whose
// message field happens to read "timeout", an err reply with an empty message,
// and insertIf's admit-rejection path, which over a real socket needs a
// disconnect raced against an execute to provoke.
//
// Deliberately in tests/ (the always-built core suite) rather than tests/net/,
// even though the extraction came out of SocketBackend: reply_router.hpp lives
// under include/morph/core/detail/ and one of its three consumers --
// SimulatedRemoteBackend in core/remote.hpp -- is compiled in every
// configuration. tests/net/ only builds when MORPH_BUILD_NET=ON (default OFF),
// so registering these there would leave the classifier's only direct tests
// unbuilt in a default checkout while the code under test still shipped.

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <morph/core/detail/reply_router.hpp>
#include <morph/core/wire.hpp>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using ::morph::backend::detail::classifyExecuteReply;
using ::morph::backend::detail::ExecuteReplyKind;
using ::morph::backend::detail::PendingCallTable;

// A stand-in for SocketBackend::PendingExecute: move-only-ish payload carrying
// enough state to prove the table hands back exactly what was put in, and to
// prove it destroys nothing it should have handed over.
struct FakePending {
    int tag = 0;
    std::shared_ptr<int> shared;
};

}  // namespace

// ── classifyExecuteReply ────────────────────────────────────────────────────

TEST_CASE("classifyExecuteReply: an ok reply classifies as Value", "[backend][reply_router]") {
    ::morph::wire::Envelope env;
    env.kind = "ok";
    env.callId = 7;
    env.body = R"({"value":42})";
    CHECK(classifyExecuteReply(env) == ExecuteReplyKind::Value);
}

TEST_CASE("classifyExecuteReply: an err reply carrying the timeout message classifies as Timeout",
          "[backend][reply_router][timeout]") {
    // The unit-level half of test_socket_backend.cpp's "executeTimeout surfaces
    // as backend::TimeoutError" regression case (#447): the server's own
    // LimitPolicy::executeTimeout reply must be distinguishable from an
    // arbitrary `err`, so callers can tell "the server gave up on this specific
    // call" from an application error. That case still exists over the real
    // transport to prove SocketBackend wires this arm to backend::TimeoutError;
    // what it could not cheaply do is vary the envelope, which is all this file
    // does below.
    ::morph::wire::Envelope env;
    env.kind = "err";
    env.callId = 7;
    env.message = std::string{::morph::wire::kExecuteTimeoutMessage};
    CHECK(classifyExecuteReply(env) == ExecuteReplyKind::Timeout);
}

TEST_CASE("classifyExecuteReply: the timeout arm matches the shared wire constant, not a local literal",
          "[backend][reply_router][timeout]") {
    // Guards the drift src/qt/qt_websocket_backend.cpp had actually developed
    // (a hand-typed `env.message == "timeout"`) before this router was
    // extracted. If kExecuteTimeoutMessage's value ever changed, a hand-typed
    // copy would silently stop classifying as Timeout; this asserts the router
    // follows the constant.
    ::morph::wire::Envelope env;
    env.kind = "err";
    env.message = std::string{::morph::wire::kExecuteTimeoutMessage};
    REQUIRE(classifyExecuteReply(env) == ExecuteReplyKind::Timeout);
    CHECK(::morph::wire::kExecuteTimeoutMessage == "timeout");
}

TEST_CASE("classifyExecuteReply: an arbitrary err message classifies as Error", "[backend][reply_router]") {
    ::morph::wire::Envelope env;
    env.kind = "err";
    env.message = "echo failed";
    CHECK(classifyExecuteReply(env) == ExecuteReplyKind::Error);
}

TEST_CASE("classifyExecuteReply: an err reply with an empty message still classifies as Error",
          "[backend][reply_router]") {
    // Not Timeout, and not some fourth state: an empty message is just an
    // Error whose text the caller decides how to render (SocketBackend passes
    // it through verbatim; SimulatedRemoteBackend substitutes "malformed
    // reply"). The router deliberately does not make that choice.
    ::morph::wire::Envelope env;
    env.kind = "err";
    env.message = "";
    CHECK(classifyExecuteReply(env) == ExecuteReplyKind::Error);
}

TEST_CASE("classifyExecuteReply: kind is decided before message, so an ok reply reading 'timeout' is a Value",
          "[backend][reply_router][timeout]") {
    // Order-of-checks contract. `message` is unused on an `ok` reply, but
    // nothing on the wire forbids it being set; classifying such a reply as a
    // Timeout would discard a perfectly good result. Reversing the two checks
    // in the router is exactly the mutation this case kills.
    ::morph::wire::Envelope env;
    env.kind = "ok";
    env.body = R"({"value":1})";
    env.message = std::string{::morph::wire::kExecuteTimeoutMessage};
    CHECK(classifyExecuteReply(env) == ExecuteReplyKind::Value);
}

TEST_CASE("classifyExecuteReply: an unrecognised kind with no message is an Error, not a Value",
          "[backend][reply_router]") {
    // Only the exact string "ok" means success -- a near-miss kind must fail
    // the call rather than be deserialized as a result.
    ::morph::wire::Envelope env;
    env.kind = "OK";
    CHECK(classifyExecuteReply(env) == ExecuteReplyKind::Error);
    env.kind = "okay";
    CHECK(classifyExecuteReply(env) == ExecuteReplyKind::Error);
    env.kind = "";
    CHECK(classifyExecuteReply(env) == ExecuteReplyKind::Error);
}

// ── PendingCallTable ────────────────────────────────────────────────────────

TEST_CASE("PendingCallTable: call ids start at 1 and never repeat", "[backend][reply_router][pending_table]") {
    // 0 is reserved on the wire for "synchronous control reply", so an execute
    // must never be issued under it -- dispatchIncomingEnvelope routes callId 0
    // to the parked sendSync caller instead of to the pending table.
    PendingCallTable<FakePending> table;
    std::uint64_t const first = table.nextCallId();
    CHECK(first == 1U);
    CHECK(table.nextCallId() == 2U);
    CHECK(table.nextCallId() == 3U);
}

TEST_CASE("PendingCallTable: insertIf stores the entry when admit approves",
          "[backend][reply_router][pending_table]") {
    PendingCallTable<FakePending> table;
    auto const callId = table.nextCallId();
    CHECK(table.size() == 0U);
    CHECK(table.insertIf(callId, FakePending{.tag = 11, .shared = nullptr}, [] { return true; }));
    CHECK(table.size() == 1U);
}

TEST_CASE("PendingCallTable: insertIf stores nothing when admit rejects", "[backend][reply_router][pending_table]") {
    // The disconnected path: SocketBackend::execute resolves the Completion
    // with DisconnectedError itself when this returns false, so an entry left
    // behind here would be resolved twice.
    PendingCallTable<FakePending> table;
    auto const callId = table.nextCallId();
    CHECK_FALSE(table.insertIf(callId, FakePending{.tag = 11, .shared = nullptr}, [] { return false; }));
    CHECK(table.size() == 0U);
    CHECK_FALSE(table.take(callId).has_value());
}

TEST_CASE("PendingCallTable: the admit predicate runs while the table's lock is held",
          "[backend][reply_router][pending_table]") {
    // The whole reason insertIf takes a predicate instead of the caller doing
    // `if (connected) table.insert(...)`: the check has to be inside the same
    // critical section as the insert, or a drain() can slip between them and
    // leave the entry stranded with nothing to resolve it. Proven here by
    // observing, from inside the predicate, that a drain() cannot have
    // interleaved -- the table is still whatever it was when insertIf was
    // called, and re-entering it from the predicate would deadlock (so this
    // asserts on state captured before the call instead).
    PendingCallTable<FakePending> table;
    auto const first = table.nextCallId();
    REQUIRE(table.insertIf(first, FakePending{.tag = 1, .shared = nullptr}, [] { return true; }));

    bool predicateRan = false;
    auto const second = table.nextCallId();
    REQUIRE(table.insertIf(second, FakePending{.tag = 2, .shared = nullptr}, [&predicateRan] {
        predicateRan = true;
        return true;
    }));
    CHECK(predicateRan);
    CHECK(table.size() == 2U);
}

TEST_CASE("PendingCallTable: take returns the stored entry and erases it", "[backend][reply_router][pending_table]") {
    PendingCallTable<FakePending> table;
    auto const callId = table.nextCallId();
    auto shared = std::make_shared<int>(99);
    REQUIRE(table.insertIf(callId, FakePending{.tag = 42, .shared = shared}, [] { return true; }));

    auto taken = table.take(callId);
    REQUIRE(taken.has_value());
    CHECK(taken->tag == 42);
    REQUIRE(taken->shared != nullptr);
    CHECK(*taken->shared == 99);
    CHECK(table.size() == 0U);
}

TEST_CASE("PendingCallTable: a second take of the same id yields nothing", "[backend][reply_router][pending_table]") {
    // The late/duplicate-reply path SocketBackend drops silently. It must not
    // resolve the same Completion twice.
    PendingCallTable<FakePending> table;
    auto const callId = table.nextCallId();
    REQUIRE(table.insertIf(callId, FakePending{.tag = 42, .shared = nullptr}, [] { return true; }));
    REQUIRE(table.take(callId).has_value());
    CHECK_FALSE(table.take(callId).has_value());
}

TEST_CASE("PendingCallTable: take of an unknown id yields nothing and disturbs nothing",
          "[backend][reply_router][pending_table]") {
    PendingCallTable<FakePending> table;
    auto const callId = table.nextCallId();
    REQUIRE(table.insertIf(callId, FakePending{.tag = 42, .shared = nullptr}, [] { return true; }));
    CHECK_FALSE(table.take(callId + 1000U).has_value());
    CHECK(table.size() == 1U);
    CHECK(table.take(callId).has_value());
}

TEST_CASE("PendingCallTable: take matches by call id, not by insertion order",
          "[backend][reply_router][pending_table]") {
    // callId multiplexing: replies arrive out of order over a real socket, so
    // the table must key on the id rather than pop a queue.
    PendingCallTable<FakePending> table;
    std::uint64_t ids[3] = {0, 0, 0};
    for (int i = 0; i < 3; ++i) {
        ids[i] = table.nextCallId();
        REQUIRE(table.insertIf(ids[i], FakePending{.tag = i + 100, .shared = nullptr}, [] { return true; }));
    }
    auto middle = table.take(ids[1]);
    REQUIRE(middle.has_value());
    CHECK(middle->tag == 101);
    auto last = table.take(ids[2]);
    REQUIRE(last.has_value());
    CHECK(last->tag == 102);
    auto first = table.take(ids[0]);
    REQUIRE(first.has_value());
    CHECK(first->tag == 100);
    CHECK(table.size() == 0U);
}

TEST_CASE("PendingCallTable: drain hands back every entry and empties the table",
          "[backend][reply_router][pending_table]") {
    // The disconnect sweep. Returning the entries rather than resolving them
    // in place is deliberate: the caller settles them outside the lock,
    // because a completion callback may re-enter the backend.
    PendingCallTable<FakePending> table;
    for (int i = 0; i < 5; ++i) {
        auto const callId = table.nextCallId();
        REQUIRE(table.insertIf(callId, FakePending{.tag = i, .shared = nullptr}, [] { return true; }));
    }
    REQUIRE(table.size() == 5U);

    auto drained = table.drain();
    CHECK(drained.size() == 5U);
    CHECK(table.size() == 0U);

    int sum = 0;
    for (auto& [callId, pending] : drained) {
        (void)callId;
        sum += pending.tag;
    }
    CHECK(sum == 0 + 1 + 2 + 3 + 4);
}

TEST_CASE("PendingCallTable: draining an empty table is a no-op, not an error",
          "[backend][reply_router][pending_table]") {
    // cancelPending runs unconditionally from ~SocketBackend and from every
    // disconnect, so it is routinely called with nothing in flight.
    PendingCallTable<FakePending> table;
    auto drained = table.drain();
    CHECK(drained.empty());
    CHECK(table.size() == 0U);
}

TEST_CASE("PendingCallTable: an entry taken before a drain is not handed out twice",
          "[backend][reply_router][pending_table]") {
    // A reply that lands just before a disconnect must settle once, as a
    // value; the sweep that follows must not also resolve it with
    // DisconnectedError.
    PendingCallTable<FakePending> table;
    auto const settled = table.nextCallId();
    auto const stillPending = table.nextCallId();
    REQUIRE(table.insertIf(settled, FakePending{.tag = 1, .shared = nullptr}, [] { return true; }));
    REQUIRE(table.insertIf(stillPending, FakePending{.tag = 2, .shared = nullptr}, [] { return true; }));

    REQUIRE(table.take(settled).has_value());
    auto drained = table.drain();
    CHECK(drained.size() == 1U);
    CHECK(drained.begin()->first == stillPending);
    CHECK(drained.begin()->second.tag == 2);
}

TEST_CASE("PendingCallTable: ids allocated after a drain still do not collide with live entries",
          "[backend][reply_router][pending_table]") {
    // Reconnect: the sweep empties the map but must not reset the counter, or
    // a reply in flight for a pre-disconnect call could be matched to a
    // post-reconnect one.
    PendingCallTable<FakePending> table;
    auto const before = table.nextCallId();
    REQUIRE(table.insertIf(before, FakePending{.tag = 1, .shared = nullptr}, [] { return true; }));
    auto drained = table.drain();
    CHECK(drained.size() == 1U);

    auto const after = table.nextCallId();
    CHECK(after > before);
}

TEST_CASE("PendingCallTable: concurrent inserts and takes settle every call exactly once",
          "[backend][reply_router][pending_table]") {
    // No socket and no server -- just the table under contention, which is the
    // part of "many concurrent in-flight executes all resolve, matched by
    // callId" that does not need a transport to exercise.
    PendingCallTable<FakePending> table;
    constexpr int kCalls = 500;
    std::atomic<int> taken{0};

    std::vector<std::uint64_t> ids;
    ids.reserve(kCalls);
    // Asserted once, after the loop, rather than once per iteration: 500
    // identical REQUIREs would say nothing extra while inflating the suite's
    // assertion count by 500.
    bool allInserted = true;
    for (int i = 0; i < kCalls; ++i) {
        auto const callId = table.nextCallId();
        ids.push_back(callId);
        if (!table.insertIf(callId, FakePending{.tag = i, .shared = nullptr}, [] { return true; })) {
            allInserted = false;
        }
    }
    REQUIRE(allInserted);
    REQUIRE(table.size() == static_cast<std::size_t>(kCalls));

    // Two threads racing to take the same ids: each id must be handed to
    // exactly one of them.
    auto takeAll = [&] {
        for (auto callId : ids) {
            if (table.take(callId).has_value()) {
                taken.fetch_add(1);
            }
        }
    };
    std::thread threadA{takeAll};
    std::thread threadB{takeAll};
    threadA.join();
    threadB.join();

    CHECK(taken.load() == kCalls);
    CHECK(table.size() == 0U);
}
