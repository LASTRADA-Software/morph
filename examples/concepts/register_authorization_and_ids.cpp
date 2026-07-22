// SPDX-License-Identifier: Apache-2.0
//
// Concept: register-time authorization (morph::session::IAuthorizer::
// authorizeRegister) — gating *who may create* a model instance, as distinct
// from `authorize` (gates *executing an action* on an existing instance) and
// `authorizeInstance` (gates *touching a specific* existing instance).
//
// A custom IAuthorizer subclass overrides authorizeRegister() to deny
// registration for a specific model type; RemoteServer consults it on every
// `register` envelope, before the instance is even constructed. The default
// authorizer (AllowAllAuthorizer) allows every registration, so this is
// entirely opt-in.
//
// One-line note on ids: RemoteServer now mints opaque model ids rather than
// small sequential integers, so a client cannot usefully guess a neighboring
// instance's id from its own.
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

// "RegAuthDemo" is this file's unique type-id prefix. Two distinct (empty)
// model types back the two type ids — ModelTraits<M> is specialised once per
// C++ type, so registering the same type under two different string ids
// would need the lower-level ModelRegistryFactory::registerModel<M>(id) API
// instead of this macro. Both are genuinely registered in the registry (not
// left unknown) so a denied register can only ever fail with "unauthorized",
// never an unrelated "unknown model type" error.

struct RegAuthDemoOpenModel {};
struct RegAuthDemoRestrictedModel {};

BRIDGE_REGISTER_MODEL(RegAuthDemoOpenModel, "RegAuthDemo_Open")
BRIDGE_REGISTER_MODEL(RegAuthDemoRestrictedModel, "RegAuthDemo_Restricted")

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
    // Note: allowed.env.modelId is an opaque id, not a small sequential
    // counter — it is not guessable from a neighboring instance's id.
}
