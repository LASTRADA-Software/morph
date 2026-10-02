// SPDX-License-Identifier: Apache-2.0
//
// Concept: observability (morph::observe::setMetricSink) and graceful
// shutdown (RemoteServer::beginShutdown()/health()/drainedWithin()).
//
//   (a) Metric sink: install a callback via morph::observe::setMetricSink and
//       watch it fire during an ordinary register/execute cycle — a
//       zero-instrumentation-code way to feed a real metrics backend
//       (Prometheus, StatsD, …) from a host app.
//   (b) Graceful shutdown: beginShutdown() stops a RemoteServer from
//       accepting *new* register/execute calls while letting whatever is
//       already in flight finish; health().ready flips to false immediately,
//       and drainedWithin() lets an orchestrator wait for in-flight work to
//       actually finish before it kills the process.
//
// Full design reference: docs/spec/core/observability.md, docs/spec/core/
// backend.md ("Graceful shutdown (`beginShutdown()` / `drainedWithin()`)").

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/completion.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/observability.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// Runs every posted task synchronously, so RemoteServer::handle() resolves
// before it returns — keeps this example free of polling boilerplate.
struct InlineExecutor : morph::exec::IExecutor {
    void post(std::function<void()> task) override { task(); }
};

// Captures the single reply a RemoteServer::handle() call produces.
struct CapturedReply {
    morph::wire::Envelope env;
    void operator()(const std::string& raw) { env = morph::wire::decode(raw); }
};

/// A `RemoteServer` answers `health()` and `drainedWithin()` on its own strand,
/// through a `Completion`. Over an `InlineExecutor` that strand runs on the
/// calling thread, so the answer is already there once `then` returns.
template <typename T>
T answeredNow(morph::async::Completion<T> completion) {
    std::optional<T> answer;
    completion.then([&answer](const T& value) { answer = value; });
    if (!answer) {
        throw std::logic_error{"the completion did not settle on this thread"};
    }
    return *answer;
}

}  // namespace

// "ObsDemo" is this file's unique type-id prefix. File-scope, not the
// anonymous namespace above: Glaze's reflection needs external linkage for
// a registered model/action type (see journal_and_outbox.cpp's file-scope
// comment for why), even though nothing outside this file uses these types.

struct ObsDemoAction {
    int x = 0;
};
struct ObsDemoModel {
    int execute(const ObsDemoAction& action) { return action.x; }
};

BRIDGE_REGISTER_MODEL(ObsDemoModel, "ObsDemo_Model")
BRIDGE_REGISTER_ACTION(ObsDemoModel, ObsDemoAction, "ObsDemo_Action")

// ── (a) Metric sink ──────────────────────────────────────────────────────────
//
// Reach for this to feed a real observability stack: the sink is a single
// process-wide callback, so wiring it once at startup instruments every
// register/execute/deregister call on every server, with no per-call code
// anywhere else.

TEST_CASE("observability: setMetricSink observes a registerCount event during register", "[concepts][observability]") {
    // ScopedObserveOverride snapshots the current sink and restores it on
    // scope exit, so this test's sink never leaks into another test case.
    morph::observe::ScopedObserveOverride guard;

    std::vector<morph::observe::Metric> observed;
    morph::observe::setMetricSink([&](const morph::observe::MetricEvent& event) { observed.push_back(event.metric); });

    InlineExecutor pool;
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    CapturedReply reply;
    server->handle(morph::wire::encode(morph::wire::makeRegister("ObsDemo_Model")), std::ref(reply));

    REQUIRE(reply.env.kind == "ok");
    bool sawRegisterCount = false;
    for (auto metric : observed) {
        if (metric == morph::observe::Metric::registerCount) {
            sawRegisterCount = true;
        }
    }
    REQUIRE(sawRegisterCount);
}

// ── (b) Graceful shutdown ────────────────────────────────────────────────────
//
// Reach for this when an orchestrator (systemd, Kubernetes, …) is about to
// stop the process: call beginShutdown() first so the load balancer's health
// check starts failing and new requests stop arriving, then use
// drainedWithin() to wait for whatever was already in flight to finish before
// actually exiting — avoiding a request in the middle of an execute() from
// being cut off mid-flight.

TEST_CASE("graceful shutdown: beginShutdown rejects new registers and flips health().ready to false",
          "[concepts][shutdown]") {
    InlineExecutor pool;
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    REQUIRE(answeredNow(server->health(pool)).ready);

    server->beginShutdown();

    REQUIRE_FALSE(answeredNow(server->health(pool)).ready);

    CapturedReply reply;
    server->handle(morph::wire::encode(morph::wire::makeRegister("ObsDemo_Model")), std::ref(reply));
    REQUIRE(reply.env.kind == "err");
    REQUIRE(reply.env.message == "server shutting down");
}

TEST_CASE("graceful shutdown: drainedWithin returns true immediately once nothing is in flight",
          "[concepts][shutdown]") {
    InlineExecutor pool;
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);

    server->beginShutdown();

    // With an InlineExecutor every call already ran to completion by the time
    // handle() returns, so there is nothing left in flight to wait for.
    REQUIRE(answeredNow(server->drainedWithin(std::chrono::milliseconds{0}, pool)));
}
