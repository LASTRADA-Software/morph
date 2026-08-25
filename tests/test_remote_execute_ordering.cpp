// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <memory>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/session/session.hpp>
#include <string>
#include <thread>

#include "test_support.hpp"

// Regression test for docs/findings/035
// (remote-server-execute-reordering.md): RemoteServer::handle() posts every
// envelope to the shared worker pool before any per-model ordering exists, so
// two `execute` envelopes for the *same* model, sent back-to-back on one
// connection, can reach the model's own strand out of send order the moment
// more than one pool worker is free to race the pre-strand work
// (decode/authorize/authenticate/registry-lookup) ahead of the other.
//
// examples/common/testkit/test_fault_proxy.cpp's `FaultProxy::dropReply` test
// caught this incidentally (it happens to send two calls close together) but
// relies on real OS thread scheduling to hit the race, so it passed on every
// quiet/fast run and only failed, intermittently, under CI load — not a
// reliable reproduction on its own.
//
// This test forces the exact interleaving instead of hoping for it: a real
// ThreadPoolExecutor{2} (so the two calls' pre-strand work can genuinely run
// concurrently, on separate threads, exactly as in production), paired with a
// custom IAuthorizer whose `authorize()` deliberately sleeps for call A's
// invocation only. That guarantees call B's pre-strand work (which never
// sleeps) finishes first on every run, deterministically — call B's own
// pool thread reaches the point where it would call `_strand.post(mid, ...)`
// while call A's thread is still sleeping inside `authorize()`, on every
// single run of this test, not just probabilistically. A
// DeterministicExecutor-based version (single-threaded, step-driven) was
// tried first and does not work for this: it cannot model "B's pool thread
// blocks waiting for A to make progress" without a second real thread to
// make that progress — DeterministicExecutor only runs one task to
// completion at a time, so a fix that makes B legitimately wait for A
// deadlocks it. Real threads are required to exercise the actual blocking
// wait finding 035's fix introduces.

namespace {

// Deliberately at namespace scope, not inside an anonymous namespace: glz's
// reflection (which the model/action registration below relies on to
// serialize these types across the wire) needs external linkage on the type
// -- see glaze/reflection/get_name.hpp's `extern const T external`, and this
// file's own sibling examples/common/testkit/test_fault_proxy.cpp's identical
// note on FaultProbeAdd/FaultProbeCounter. (This anonymous namespace wraps
// only the authorizer and helper functions below, none of which need
// external linkage; EroAddAction/EroCounterModel are defined just outside
// it, further down, for exactly that reason.)

/// @brief Allow-all authorizer whose `authorize()` sleeps once, for the
///        first call it sees carrying `EroAddAction::by == kSlowByValue` —
///        every other call (including a second `by == kSlowByValue` call,
///        should a future edit to this test ever add one) returns
///        immediately. This is what turns "the race might happen" into "the
///        race always happens": call A's pre-strand work is held up right
///        here, in `dispatchExecute`'s own authorize() step, for long enough
///        that call B's identical pre-strand work — running concurrently on
///        the pool's other thread — reliably finishes first and reaches the
///        ticket-wait point before A ever does.
class SlowFirstAuthorizer : public morph::session::IAuthorizer {
public:
    [[nodiscard]] bool authorize(const morph::session::Context&, std::string_view, std::string_view) const override {
        if (!_slowCallTaken.exchange(true)) {
            std::this_thread::sleep_for(std::chrono::milliseconds{200});
        }
        return true;
    }

private:
    mutable std::atomic<bool> _slowCallTaken{false};
};

/// @brief Allow-all authorizer whose `authorize()` sleeps once, for the
///        first call it sees carrying `EroAddAction::by == kSlowByValue` --
///        every call carrying `EroAddAction::by == kRejectByValue` returns
///        `false` immediately, every run, with no sleep at all.
///
/// Drives `dispatchExecute`'s per-model execute-ordering gate (remote.hpp's
/// `_executeGates`) past full drain while an earlier-numbered ticket for the
/// same model is still outstanding: the slow call takes ticket 0 and is held
/// up in `authorize()`; two `kRejectByValue` calls for the *same* model, sent
/// after it, take tickets 1 and 2 and are rejected in `dispatchExecute`
/// before ever reaching `awaitExecuteTurn` (the "unauthorized" branch
/// releases its ticket immediately, without waiting). Both reject calls
/// therefore release and drain the gate (`releaseExecuteTicket`'s
/// `nextToRun == nextTicket` check erases the map entry once ticket 2
/// releases) while ticket 0 has not yet even called `awaitExecuteTurn` --
/// exactly the interleaving `awaitExecuteTurn`'s and `releaseExecuteTicket`'s
/// own "gate already gone" branches exist for.
class SlowFirstThenRejectAuthorizer : public morph::session::IAuthorizer {
public:
    [[nodiscard]] bool authorize(const morph::session::Context&, std::string_view,
                                 std::string_view actionType) const override {
        if (actionType == kRejectMarker) {
            return false;
        }
        if (!_slowCallTaken.exchange(true)) {
            std::this_thread::sleep_for(std::chrono::milliseconds{200});
        }
        return true;
    }

    static constexpr std::string_view kRejectMarker = "ERO_RejectAction";

private:
    mutable std::atomic<bool> _slowCallTaken{false};
};

}  // namespace

struct EroAddAction {
    int by = 0;
};

// A running total, not a pure function of the action -- mirrors
// FaultProbeCounter in test_fault_proxy.cpp: only an accumulator can
// distinguish "processed out of order" from "processed in order", since the
// wrong order still produces *a* plausible-looking total, just the wrong one.
struct EroCounterModel {
    int value = 0;
    int execute(EroAddAction action) {
        value += action.by;
        return value;
    }
};

template <>
struct morph::model::ModelTraits<EroCounterModel> {
    static constexpr std::string_view typeId() { return "ERO_CounterModel"; }
};
template <>
struct morph::model::ActionTraits<EroAddAction> {
    using Result = int;
    static constexpr std::string_view typeId() { return "ERO_AddAction"; }
    static std::string toJson(const EroAddAction& action) { return "{\"by\":" + std::to_string(action.by) + "}"; }
    static EroAddAction fromJson(std::string_view json) {
        EroAddAction action;
        // Minimal hand-rolled parse -- the fixed shape ({"by":N}) doesn't
        // justify pulling in glaze here; every sibling RemoteServer test in
        // this directory (test_remote_connection_scope.cpp's CsSquareAction,
        // etc.) round-trips through the real ActionDispatcher via glaze
        // instead, but this model only needs `execute()` reached directly
        // from RemoteServer's own decode path, which calls fromJson() itself.
        auto pos = json.find(':');
        if (pos != std::string_view::npos) {
            action.by = std::stoi(std::string{json.substr(pos + 1, json.find('}') - pos - 1)});
        }
        return action;
    }
    static std::string resultToJson(const int& result) { return std::to_string(result); }
    static int resultFromJson(std::string_view json) { return std::stoi(std::string{json}); }
};

namespace {

using morph::testing::WaitReply;

morph::model::detail::ActionDispatcher& eroDispatcher() {
    static morph::model::detail::ActionDispatcher dispatcher = [] {
        morph::model::detail::ActionDispatcher d;
        d.registerAction<EroCounterModel, EroAddAction>("ERO_CounterModel", "ERO_AddAction");
        return d;
    }();
    return dispatcher;
}

morph::model::detail::ModelRegistryFactory& eroRegistry() {
    static morph::model::detail::ModelRegistryFactory registry = [] {
        morph::model::detail::ModelRegistryFactory r;
        r.registerModel<EroCounterModel>("ERO_CounterModel");
        return r;
    }();
    return registry;
}

}  // namespace

TEST_CASE(
    "RemoteServer::handle() preserves send order for two same-model executes "
    "even when the second one's pre-strand work finishes first",
    "[remote][execute-ordering]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto authorizer = std::make_shared<SlowFirstAuthorizer>();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, authorizer, eroDispatcher(), eroRegistry());

    WaitReply regReply;
    server->handle(morph::wire::encode(morph::wire::makeRegister("ERO_CounterModel")), std::ref(regReply));
    REQUIRE(regReply.await());
    REQUIRE(regReply.env.kind == "ok");
    const auto modelId = regReply.env.modelId;
    REQUIRE(modelId != 0U);

    // Two execute envelopes for the SAME model, sent back-to-back on the
    // same (simulated) connection -- call A (by=10) first, call B (by=100)
    // second, exactly like two requests arriving close together. handle()
    // returns immediately in both cases (it only posts to the pool), so
    // these two calls are made in strict program order here, mirroring two
    // messages arriving in that order over one WebSocket connection.
    morph::wire::Envelope reqA;
    reqA.kind = "execute";
    reqA.callId = 1;
    reqA.modelId = modelId;
    reqA.modelType = "ERO_CounterModel";
    reqA.actionType = "ERO_AddAction";
    reqA.body = R"({"by":10})";
    WaitReply replyA;
    server->handle(morph::wire::encode(reqA), std::ref(replyA));

    morph::wire::Envelope reqB = reqA;
    reqB.callId = 2;
    reqB.body = R"({"by":100})";
    WaitReply replyB;
    server->handle(morph::wire::encode(reqB), std::ref(replyB));

    // SlowFirstAuthorizer guarantees B's authorize() call (and everything
    // after it in B's pre-strand work) finishes before A's does -- A is the
    // first call reaching authorize() program-order, so it is the one held
    // up. Without finding 035's fix, this is precisely the interleaving that
    // lets B's execute reach the model's strand before A's, even though the
    // client sent A first.
    REQUIRE(replyA.await(std::chrono::milliseconds{5000}));
    REQUIRE(replyB.await(std::chrono::milliseconds{5000}));
    REQUIRE(replyA.env.kind == "ok");
    REQUIRE(replyB.env.kind == "ok");

    // The load-bearing assertion: A (by=10) must be applied before B
    // (by=100) resolves, because the client sent A first. If B's effect was
    // applied first (the bug), replyA.env.body is "110" and replyB.env.body
    // is "100" -- still internally consistent, still both "ok", but
    // backwards relative to send order. Correct behaviour is A settles at
    // 10, B settles at 110, in THAT order -- matching send order, not
    // whichever pool thread happened to finish its pre-strand work first.
    CHECK(replyA.env.body == "10");
    CHECK(replyB.env.body == "110");
}

TEST_CASE(
    "RemoteServer::dispatchExecute: awaitExecuteTurn and releaseExecuteTicket both cope when their "
    "model's execute gate has already fully drained and been erased",
    "[remote][execute-ordering]") {
    // Three same-model executes, same send order every run:
    //   A (ticket 0, EroAddAction by=kSlowByValue) -- held up in authorize()
    //     for 200ms by SlowFirstThenRejectAuthorizer, so it is reliably still
    //     there when B and C's own pre-strand work runs.
    //   B (ticket 1, ERO_RejectAction) -- authorize() returns false
    //     immediately; dispatchExecute's rejectAndRelease releases ticket 1
    //     right away, never calling awaitExecuteTurn at all.
    //   C (ticket 2, ERO_RejectAction) -- same as B, ticket 2.
    // releaseExecuteTicket unconditionally sets nextToRun = ticket + 1, so
    // C's release (nextToRun 2 -> 3) meets nextTicket (3, since all three
    // tickets were already taken by the time C releases) and erases the
    // gate's map entry -- while A (ticket 0) has not yet reached
    // awaitExecuteTurn at all. When A's authorize() finally returns:
    //   - awaitExecuteTurn(mid, 0) finds no map entry -- "gate already gone"
    //     (line ~1467) -- and returns immediately instead of waiting.
    //   - dispatchExecute posts to the strand, runs, and then calls
    //     releaseExecuteTicket(mid, 0), which *also* finds no map entry --
    //     the defensive "should not happen" branch (line ~1489) -- since C's
    //     release already erased it.
    // Both branches are exercised by this one interleaving, in one test.
    morph::exec::ThreadPoolExecutor pool{3};
    auto authorizer = std::make_shared<SlowFirstThenRejectAuthorizer>();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, authorizer, eroDispatcher(), eroRegistry());

    WaitReply regReply;
    server->handle(morph::wire::encode(morph::wire::makeRegister("ERO_CounterModel")), std::ref(regReply));
    REQUIRE(regReply.await());
    REQUIRE(regReply.env.kind == "ok");
    const auto modelId = regReply.env.modelId;
    REQUIRE(modelId != 0U);

    morph::wire::Envelope reqA;
    reqA.kind = "execute";
    reqA.callId = 1;
    reqA.modelId = modelId;
    reqA.modelType = "ERO_CounterModel";
    reqA.actionType = "ERO_AddAction";
    reqA.body = R"({"by":5})";
    WaitReply replyA;
    server->handle(morph::wire::encode(reqA), std::ref(replyA));

    morph::wire::Envelope reqB = reqA;
    reqB.callId = 2;
    reqB.actionType = std::string{SlowFirstThenRejectAuthorizer::kRejectMarker};
    WaitReply replyB;
    server->handle(morph::wire::encode(reqB), std::ref(replyB));

    morph::wire::Envelope reqC = reqB;
    reqC.callId = 3;
    WaitReply replyC;
    server->handle(morph::wire::encode(reqC), std::ref(replyC));

    // B and C settle fast (rejected in authorize(), no wait); A settles after
    // its 200ms sleep. If awaitExecuteTurn/releaseExecuteTicket's "gate
    // already gone" branches did not handle a missing map entry gracefully
    // (e.g. by dereferencing iter->second unconditionally), this would crash
    // or hang instead of completing within the budget below.
    REQUIRE(replyB.await(std::chrono::milliseconds{2000}));
    REQUIRE(replyC.await(std::chrono::milliseconds{2000}));
    REQUIRE(replyA.await(std::chrono::milliseconds{5000}));

    REQUIRE(replyB.env.kind == "err");
    REQUIRE(replyB.env.message == "unauthorized");
    REQUIRE(replyC.env.kind == "err");
    REQUIRE(replyC.env.message == "unauthorized");
    REQUIRE(replyA.env.kind == "ok");
    CHECK(replyA.env.body == "5");
}
