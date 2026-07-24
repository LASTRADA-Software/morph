// SPDX-License-Identifier: Apache-2.0
//
// Concept: register-time authorization (morph::session::IAuthorizer::
// authorizeRegister) — gating *who may create* a model instance, as distinct
// from `authorize` (gates *executing an action* on an existing instance) and
// `authorizeInstance` (gates *touching a specific* existing instance --
// not demonstrated in this file; see docs/spec/session/session.md).
//
// Reach for this when only some callers should be able to create instances
// of a given model type at all (e.g. an admin-only model): a custom
// IAuthorizer subclass overrides authorizeRegister() to deny registration
// for a specific model type; RemoteServer consults it on every `register`
// envelope, before the instance is even constructed. The default authorizer
// (AllowAllAuthorizer) allows every registration, so this is entirely
// opt-in.
//
// Opaque ids: RemoteServer mints opaque model ids (a keyed bijective
// permutation of a monotonic counter, see OpaqueIdGenerator in
// include/morph/core/remote.hpp) rather than small sequential integers, so a
// client cannot usefully guess a neighboring instance's id from its own --
// demonstrated below by registering two instances back to back and checking
// their ids are not adjacent.
//
// Full design reference: docs/spec/session/session.md ("The
// `authorizeRegister` hook — gating registration").

#include <catch2/catch_test_macros.hpp>
#include <functional>
#include <memory>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/session/session.hpp>
#include <string>
#include <string_view>

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

// Denies registration of one specific model type; every other type (and
// every action dispatch, since `authorize` is left at its default) is
// unaffected. A real deployment might instead check `ctx.principal` or a
// per-tenant allowlist here.
struct DenyOneModelTypeAuthorizer : morph::session::IAuthorizer {
    [[nodiscard]] bool authorize(const morph::session::Context&, std::string_view, std::string_view) const override {
        return true;
    }
    [[nodiscard]] bool authorizeRegister(const morph::session::Context&, std::string_view modelType) const override {
        return modelType != "RegAuthDemo_Restricted";
    }
};

}  // namespace

// "RegAuthDemo" is this file's unique type-id prefix. File-scope, not the
// anonymous namespace above: Glaze's reflection needs external linkage for
// a registered model type (see journal_and_outbox.cpp's file-scope comment
// for why). Two distinct (empty) model types back the two type ids —
// ModelTraits<M> is specialised once per C++ type, so registering the same
// type under two different string ids would need the lower-level
// ModelRegistryFactory::registerModel<M>(id) API instead of this macro. Both
// are genuinely registered in the registry (not left unknown) so a denied
// register can only ever fail with "unauthorized", never an unrelated
// "unknown model type" error.

struct RegAuthDemoOpenModel {};
struct RegAuthDemoRestrictedModel {};

BRIDGE_REGISTER_MODEL(RegAuthDemoOpenModel, "RegAuthDemo_Open")
BRIDGE_REGISTER_MODEL(RegAuthDemoRestrictedModel, "RegAuthDemo_Restricted")

// ── Register-time authorization ─────────────────────────────────────────────
//
// Reach for this when only some callers should be able to create instances of
// a given model type at all (an admin-only model, a feature behind a plan
// tier): authorizeRegister() runs before the instance is even constructed, so
// a denied caller never gets as far as touching any state.

TEST_CASE("register authorization: a custom IAuthorizer denies registering a specific model type",
          "[concepts][session][auth]") {
    InlineExecutor pool;
    auto authorizer = std::make_shared<DenyOneModelTypeAuthorizer>();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, authorizer);

    CapturedReply denied;
    server->handle(morph::wire::encode(morph::wire::makeRegister("RegAuthDemo_Restricted")), std::ref(denied));

    REQUIRE(denied.env.kind == "err");
    REQUIRE(denied.env.message == "unauthorized");
}

TEST_CASE("register authorization: the same authorizer still allows an unrestricted model type",
          "[concepts][session][auth]") {
    InlineExecutor pool;
    auto authorizer = std::make_shared<DenyOneModelTypeAuthorizer>();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, authorizer);

    CapturedReply allowed;
    server->handle(morph::wire::encode(morph::wire::makeRegister("RegAuthDemo_Open")), std::ref(allowed));

    REQUIRE(allowed.env.kind == "ok");
}

// ── Opaque ids ───────────────────────────────────────────────────────────────
//
// Reach for this when a client-visible model id must not leak how many
// instances exist or which was created first (an incrementing counter would
// do both): RemoteServer mints ids through a keyed bijective permutation, so
// two instances registered back to back get unrelated-looking ids.

TEST_CASE("register authorization: two back-to-back registrations do not get adjacent (sequential) ids",
          "[concepts][session][ids]") {
    InlineExecutor pool;
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);

    CapturedReply first;
    server->handle(morph::wire::encode(morph::wire::makeRegister("RegAuthDemo_Open")), std::ref(first));
    REQUIRE(first.env.kind == "ok");

    CapturedReply second;
    server->handle(morph::wire::encode(morph::wire::makeRegister("RegAuthDemo_Open")), std::ref(second));
    REQUIRE(second.env.kind == "ok");

    // A small sequential counter would make these adjacent (id2 == id1 + 1);
    // the opaque permutation guarantees they are not, even though the
    // underlying counter driving them is.
    REQUIRE(second.env.modelId != first.env.modelId + 1);
}
