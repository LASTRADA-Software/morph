// SPDX-License-Identifier: Apache-2.0
//
// Tests for IAuthorizer::authorizeRegister (docs/spec/security.md, docs/spec/session/session.md):
// an optional hook consulted on every `register` envelope, gating *who may
// create* a model instance. Defaults to allow so existing deployments are
// unaffected.

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/session/session.hpp>
#include <morph/session/session_auth.hpp>
#include <optional>
#include <string>
#include <string_view>

#include "test_support.hpp"

// Model/action types need external linkage for glaze reflection (see
// tests/test_policy_hardening.cpp).
struct RegAuthAction {
    int x = 0;
};
struct RegAuthModel {
    int execute(const RegAuthAction& act) { return act.x + 1; }
};

template <>
struct morph::model::ModelTraits<RegAuthModel> {
    static constexpr std::string_view typeId() { return "RA_Model"; }
};
template <>
struct morph::model::ActionTraits<RegAuthAction> {
    using Result = int;
    static constexpr std::string_view typeId() { return "RA_Action"; }
    static std::string toJson(const RegAuthAction& act) {
        std::string out;
        (void)glz::write_json(act, out);
        return out;
    }
    static RegAuthAction fromJson(std::string_view json) {
        RegAuthAction action{};
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

// Authenticates ctx.principal verbatim (stand-in for a verified identity, as
// in tests/test_policy_hardening.cpp's OwnershipAuthorizer) and denies
// `register` for the model type "RA_Denied" regardless of caller.
struct RegisterGatingAuthorizer : morph::session::IAuthorizer {
    [[nodiscard]] bool authorize(const morph::session::Context&, std::string_view, std::string_view) const override {
        return true;  // type-level execute gate: irrelevant to these tests
    }
    [[nodiscard]] std::optional<std::string> authenticate(const morph::session::Context& ctx) const override {
        if (ctx.principal.empty()) {
            return std::nullopt;
        }
        return ctx.principal;
    }
    [[nodiscard]] bool authorizeRegister(const morph::session::Context&, std::string_view modelType) const override {
        return modelType != "RA_Denied";
    }
};

// Denies registration for an unauthenticated caller (empty verified
// principal) regardless of modelType.
struct AuthenticatedOnlyRegisterAuthorizer : morph::session::IAuthorizer {
    [[nodiscard]] bool authorize(const morph::session::Context&, std::string_view, std::string_view) const override {
        return true;
    }
    [[nodiscard]] std::optional<std::string> authenticate(const morph::session::Context& ctx) const override {
        if (ctx.principal.empty()) {
            return std::nullopt;
        }
        return ctx.principal;
    }
    [[nodiscard]] bool authorizeRegister(const morph::session::Context& ctx, std::string_view) const override {
        return !ctx.principal.empty();  // ctx.principal is already the *verified* identity here
    }
};

struct RegEnv {
    morph::model::detail::ActionDispatcher dispatcher;
    morph::model::detail::ModelRegistryFactory registry;
    RegEnv() {
        // Both type ids are legitimately known to the registry/dispatcher, so
        // a denied register can only ever fail with "unauthorized" — never
        // the unrelated "unknown model type" error.
        registry.registerModel<RegAuthModel>("RA_Model");
        registry.registerModel<RegAuthModel>("RA_Denied");
        dispatcher.registerAction<RegAuthModel, RegAuthAction>("RA_Model", "RA_Action");
        dispatcher.registerAction<RegAuthModel, RegAuthAction>("RA_Denied", "RA_Action");
    }
};

morph::wire::Envelope registerAs(std::string typeId, std::string principal) {
    auto env = morph::wire::makeRegister(std::move(typeId));
    env.session.principal = std::move(principal);
    return env;
}

}  // namespace

TEST_CASE("authorizeRegister denies registration of a disallowed model type; no instance is created",
          "[register][auth]") {
    morph::testing::InlineExecutor pool;
    RegEnv env;
    auto authz = std::make_shared<RegisterGatingAuthorizer>();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, authz, env.dispatcher, env.registry);

    morph::testing::WaitReply denied;
    server->handle(morph::wire::encode(registerAs("RA_Denied", "alice")), std::ref(denied));
    REQUIRE(denied.await());
    REQUIRE(denied.env.kind == "err");
    REQUIRE(denied.env.message == "unauthorized");

    // No instance was created: executing against an arbitrary, never-issued id
    // still reports "model not found".
    morph::wire::Envelope probe;
    probe.kind = "execute";
    probe.modelId = 999999;
    probe.modelType = "RA_Denied";
    probe.actionType = "RA_Action";
    probe.body = R"({"x":1})";
    morph::testing::WaitReply probeReply;
    server->handle(morph::wire::encode(probe), std::ref(probeReply));
    REQUIRE(probeReply.await());
    REQUIRE(probeReply.env.kind == "err");
    REQUIRE(probeReply.env.message == "model not found");

    // The allowed type still registers and executes normally.
    morph::testing::WaitReply reg;
    server->handle(morph::wire::encode(registerAs("RA_Model", "alice")), std::ref(reg));
    REQUIRE(reg.await());
    REQUIRE(reg.env.kind == "ok");

    morph::wire::Envelope exec;
    exec.kind = "execute";
    exec.modelId = reg.env.modelId;
    exec.modelType = "RA_Model";
    exec.actionType = "RA_Action";
    exec.body = R"({"x":1})";
    exec.session.principal = "alice";
    morph::testing::WaitReply run;
    server->handle(morph::wire::encode(exec), std::ref(run));
    REQUIRE(run.await());
    REQUIRE(run.env.kind == "ok");
}

TEST_CASE("authorizeRegister can require authentication before allowing register", "[register][auth]") {
    morph::testing::InlineExecutor pool;
    RegEnv env;
    auto authz = std::make_shared<AuthenticatedOnlyRegisterAuthorizer>();
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, authz, env.dispatcher, env.registry);

    // No principal at all: authenticate() returns nullopt, authorizeRegister denies.
    morph::testing::WaitReply denied;
    server->handle(morph::wire::encode(registerAs("RA_Model", "")), std::ref(denied));
    REQUIRE(denied.await());
    REQUIRE(denied.env.kind == "err");
    REQUIRE(denied.env.message == "unauthorized");

    // An authenticated caller is allowed.
    morph::testing::WaitReply ok;
    server->handle(morph::wire::encode(registerAs("RA_Model", "alice")), std::ref(ok));
    REQUIRE(ok.await());
    REQUIRE(ok.env.kind == "ok");
}

TEST_CASE("without an authorizeRegister override, the default authorizer registers any type (backward compatible)",
          "[register][auth]") {
    morph::testing::InlineExecutor pool;
    RegEnv env;
    // Default allow-all authorizer (no authorizeRegister override).
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, env.dispatcher, env.registry);

    morph::testing::WaitReply reg;
    server->handle(morph::wire::encode(registerAs("RA_Denied", "alice")), std::ref(reg));
    REQUIRE(reg.await());
    REQUIRE(reg.env.kind == "ok");
}

TEST_CASE("a plain SigningAuthorizer (no authorizeRegister override) imposes no register restriction",
          "[register][auth]") {
    morph::testing::InlineExecutor pool;
    RegEnv env;
    const std::string secret = "reg-test-secret";
    auto authz = std::make_shared<morph::session::SigningAuthorizer>(secret);
    auto server = std::make_shared<morph::backend::RemoteServer>(pool, authz, env.dispatcher, env.registry);

    // No token attached at all — SigningAuthorizer::authorize() would deny an
    // execute call, but authorizeRegister is a distinct, un-overridden hook
    // that still defaults to allow, so register succeeds unconditionally.
    morph::testing::WaitReply reg;
    server->handle(morph::wire::encode(registerAs("RA_Denied", "alice")), std::ref(reg));
    REQUIRE(reg.await());
    REQUIRE(reg.env.kind == "ok");
}
