// SPDX-License-Identifier: Apache-2.0
//
// Concept: server-side action validation (morph::model::ActionValidator).
//
// An action struct can expose a `bool validate() const` member; before
// `Model::execute()` runs on the server dispatch path (the one path an
// untrusted remote client can reach directly, bypassing any client-side UI
// gate), the framework calls it automatically and rejects the action with
// `morph::model::ValidationError` if it returns `false`. No registration
// macro, no extra wiring — declaring `validate()` on the action is enough.
//
// Full design reference: docs/spec/core/registry.md ("Validation and logging
// policy"), docs/spec/core/backend.md ("RemoteServer — server-side message
// handler").

#include <catch2/catch_test_macros.hpp>
#include <functional>
#include <memory>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <string>

namespace {

// Runs every posted task synchronously on the caller's thread, so a
// RemoteServer's asynchronous handle() call resolves before it returns —
// keeps this example free of any polling/waiting boilerplate.
struct InlineExecutor : morph::exec::IExecutor {
    void post(std::function<void()> task) override { task(); }
};

// Captures the single reply a RemoteServer::handle() call produces.
struct CapturedReply {
    morph::wire::Envelope env;
    void operator()(const std::string& raw) { env = morph::wire::decode(raw); }
};

}  // namespace

// "ValidationDemo" is this file's unique type-id prefix. File-scope, not the
// anonymous namespace above: Glaze's reflection needs external linkage for
// a registered model/action type (see journal_and_outbox.cpp's file-scope
// comment for why), even though nothing outside this file uses these types.

struct ValidationDemoAction {
    std::string name;

    // Requires a non-empty name before the action is allowed to execute.
    // This is the entire opt-in: no macro, no separate validator type.
    [[nodiscard]] bool validate() const { return !name.empty(); }
};

struct ValidationDemoModel {
    std::string execute(const ValidationDemoAction& action) { return "hello, " + action.name; }
};

BRIDGE_REGISTER_MODEL(ValidationDemoModel, "ValidationDemo_Model")
BRIDGE_REGISTER_ACTION(ValidationDemoModel, ValidationDemoAction, "ValidationDemo_Action")

// ── Server-side validation ───────────────────────────────────────────────────
//
// Reach for this when an action must satisfy a precondition regardless of
// how it was submitted (a well-behaved GUI, a hand-crafted request, a buggy
// client): a `validate()` member is enforced on the server dispatch path
// itself, so a remote client cannot bypass it by skipping client-side checks.

TEST_CASE("server-side validation: RemoteServer rejects an invalid action with ValidationError",
          "[concepts][validation]") {
    InlineExecutor pool;
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);

    CapturedReply regReply;
    server->handle(morph::wire::encode(morph::wire::makeRegister("ValidationDemo_Model")), std::ref(regReply));
    REQUIRE(regReply.env.kind == "ok");
    auto modelId = regReply.env.modelId;

    morph::wire::Envelope invalidCall;
    invalidCall.kind = "execute";
    invalidCall.modelId = modelId;
    invalidCall.modelType = "ValidationDemo_Model";
    invalidCall.actionType = "ValidationDemo_Action";
    invalidCall.body = R"({"name":""})";  // fails validate(): name is empty

    CapturedReply invalidReply;
    server->handle(morph::wire::encode(invalidCall), std::ref(invalidReply));

    REQUIRE(invalidReply.env.kind == "err");
    REQUIRE(invalidReply.env.message == "action failed validation: ValidationDemo_Model/ValidationDemo_Action");
}

TEST_CASE("server-side validation: RemoteServer dispatches a valid action normally", "[concepts][validation]") {
    InlineExecutor pool;
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);

    CapturedReply regReply;
    server->handle(morph::wire::encode(morph::wire::makeRegister("ValidationDemo_Model")), std::ref(regReply));
    REQUIRE(regReply.env.kind == "ok");
    auto modelId = regReply.env.modelId;

    morph::wire::Envelope validCall;
    validCall.kind = "execute";
    validCall.modelId = modelId;
    validCall.modelType = "ValidationDemo_Model";
    validCall.actionType = "ValidationDemo_Action";
    validCall.body = R"({"name":"Ada"})";  // passes validate(): name is non-empty

    CapturedReply validReply;
    server->handle(morph::wire::encode(validCall), std::ref(validReply));

    REQUIRE(validReply.env.kind == "ok");
    REQUIRE(validReply.env.body == R"("hello, Ada")");
}
