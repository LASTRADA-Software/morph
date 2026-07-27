// SPDX-License-Identifier: Apache-2.0
//
// Concept: protocol-version negotiation and connection scoping — two
// independent pieces of RemoteServer's transport-facing surface.
//
//   (a) Protocol version negotiation: a client sends a `"hello"` envelope
//       (morph::wire::makeHello) announcing the protocol version it speaks;
//       the server replies with the inclusive version range it accepts
//       (morph::wire::ProtocolRange), so a mismatched peer can fail fast
//       and legibly instead of misbehaving on ordinary traffic.
//   (b) Connection scoping: RemoteServer::openConnection() mints a scope a
//       transport attributes registrations to; RemoteServer::closeConnection()
//       reclaims every model still registered under that scope in one call —
//       the cleanup a transport runs when a socket disconnects, so a dropped
//       client can never leak its instances.
//
// Full design reference: docs/spec/core/wire.md ("Protocol version
// negotiation"), docs/spec/core/backend.md ("Protocol-version negotiation",
// "Connection scopes").

#include <catch2/catch_test_macros.hpp>
#include <functional>
#include <glaze/glaze.hpp>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <string>

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

}  // namespace

// "ConnDemo" is this file's unique type-id prefix. File-scope, not the
// anonymous namespace above: Glaze's reflection needs external linkage for
// a registered model/action type (see journal_and_outbox.cpp's file-scope
// comment for why), even though nothing outside this file uses these types.

struct ConnDemoAction {
    int x = 0;
};
struct ConnDemoModel {
    int execute(const ConnDemoAction& action) { return action.x * action.x; }
};

BRIDGE_REGISTER_MODEL(ConnDemoModel, "ConnDemo_Model")
BRIDGE_REGISTER_ACTION(ConnDemoModel, ConnDemoAction, "ConnDemo_Action")

// ── (a) Protocol version negotiation ────────────────────────────────────────
//
// Reach for this when client and server may be built from different
// versions of morph (e.g. rolling deployment, an old mobile client still in
// the field): negotiate once per connection, before sending any
// register/execute, so an incompatible peer is rejected with a clear
// "protocol version unsupported" instead of a confusing later failure.

TEST_CASE("protocol negotiation: hello returns the server's supported version range", "[concepts][protocol]") {
    InlineExecutor pool;
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);

    CapturedReply reply;
    server->handle(morph::wire::encode(morph::wire::makeHello()), std::ref(reply));

    REQUIRE(reply.env.kind == "ok");
    morph::wire::ProtocolRange range;
    // glz::read_json returns a falsy error code on success (a truthy value
    // means a parse error), so REQUIRE_FALSE asserts the parse succeeded.
    REQUIRE_FALSE(glz::read_json(range, reply.env.body));
    REQUIRE(range.min == morph::wire::kProtocolVersion);
    REQUIRE(range.max == morph::wire::kProtocolVersion);
}

TEST_CASE("protocol negotiation: a hello outside the server's configured range is rejected", "[concepts][protocol]") {
    InlineExecutor pool;
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    server->setSupportedVersionRange(2, 3);  // this deployment dropped support for version 1

    CapturedReply reply;
    server->handle(morph::wire::encode(morph::wire::makeHello(1)), std::ref(reply));

    REQUIRE(reply.env.kind == "err");
    REQUIRE(reply.env.message == "protocol version unsupported");
}

// ── (b) Connection scoping ──────────────────────────────────────────────────
//
// Reach for this whenever a transport has a concept of "the client
// disconnected": open a scope per accepted connection, register every model
// that connection creates through the scoped handle() overload, and close
// the scope on disconnect — one call then reclaims everything that
// connection ever registered, instead of the server accumulating orphaned
// instances forever.

TEST_CASE("connection scope: closeConnection reclaims every model registered under it", "[concepts][connections]") {
    InlineExecutor pool;
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);

    auto cid = server->openConnection();

    CapturedReply reg;
    server->handle(morph::wire::encode(morph::wire::makeRegister("ConnDemo_Model")), std::ref(reg), cid);
    REQUIRE(reg.env.kind == "ok");
    auto modelId = reg.env.modelId;

    // The instance works normally while its connection is still open.
    morph::wire::Envelope execReq;
    execReq.kind = "execute";
    execReq.modelId = modelId;
    execReq.modelType = "ConnDemo_Model";
    execReq.actionType = "ConnDemo_Action";
    execReq.body = R"({"x":4})";
    CapturedReply before;
    server->handle(morph::wire::encode(execReq), std::ref(before));
    REQUIRE(before.env.kind == "ok");
    REQUIRE(before.env.body == "16");

    // Simulates the transport observing the connection drop.
    server->closeConnection(cid);

    // The instance is gone — exactly as if it had been explicitly deregistered.
    CapturedReply after;
    server->handle(morph::wire::encode(execReq), std::ref(after));
    REQUIRE(after.env.kind == "err");
    REQUIRE(after.env.message == "model not found");
}
