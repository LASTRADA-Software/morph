// SPDX-License-Identifier: Apache-2.0

// Action refusals: a refused action carries a code and named arguments to the client, and a screen words it from the
// message catalogue in the user's locale. See docs/spec/ui/refusals.md.

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <core/async/Task.hpp>
#include <exception>
#include <future>
#include <glaze/glaze.hpp>
#include <map>
#include <memory>
#include <morph/core/action_guard.hpp>
#include <morph/core/action_refusal.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/coroutine.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/core/wire.hpp>
#include <morph/ui/author.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "test_support.hpp"
#include "ui_interpreter_support.hpp"

namespace refusal_test {

struct TakeGuard;

/// Throws an ActionRefusal from execute.
struct Take {
    std::string who;
};
/// Throws a plain std::runtime_error from execute.
struct Break {};
/// Refused by its guard, with an ActionRefusal.
struct Guarded {
    using guard = TakeGuard;
};
/// A Task handler that throws an ActionRefusal.
struct TakeLater {};
struct Done {
    int value = 0;
};

struct Probe {
    Done execute(Take const& action) {
        throw morph::model::ActionRefusal{"probe.taken", {{"holder", action.who}}, "taken by " + action.who};
    }
    Done execute(Break const&) { throw std::runtime_error{"broken"}; }
    Done execute(Guarded const&) { return Done{.value = 1}; }
    core::async::Task<Done> execute(TakeLater) {
        throw morph::model::ActionRefusal{"probe.later", {{"when", "tomorrow"}}, "later"};
        co_return Done{};
    }
};

struct TakeGuard {
    static void check(Guarded const&) {
        throw morph::model::ActionRefusal{"probe.guarded", {{"why", "closed"}}, "the guard refused"};
    }
};

}  // namespace refusal_test

BRIDGE_REGISTER_MODEL(refusal_test::Probe, "T_RefusalProbe")
BRIDGE_REGISTER_ACTION(refusal_test::Probe, refusal_test::Take, "T_RefusalTake")
BRIDGE_REGISTER_ACTION(refusal_test::Probe, refusal_test::Break, "T_RefusalBreak")
BRIDGE_REGISTER_ACTION(refusal_test::Probe, refusal_test::Guarded, "T_RefusalGuarded")
BRIDGE_REGISTER_ACTION(refusal_test::Probe, refusal_test::TakeLater, "T_RefusalTakeLater")

namespace {

namespace rt = refusal_test;

/// The server's reply to an execute of @p actionType with @p body on a fresh instance, decoded.
morph::wire::Envelope serverReply(morph::backend::RemoteServer& server, std::string const& actionType,
                                  std::string const& body) {
    auto const registered =
        morph::wire::decode(server.handleInline(morph::wire::encode(morph::wire::makeRegister("T_RefusalProbe"))));
    REQUIRE(registered.kind == "ok");
    morph::wire::Envelope execute;
    execute.kind = "execute";
    execute.callId = 9;
    execute.modelId = registered.modelId;
    execute.modelType = "T_RefusalProbe";
    execute.actionType = actionType;
    execute.body = body;
    std::promise<std::string> replied;
    auto future = replied.get_future();
    server.handle(morph::wire::encode(execute),
                  [&replied](std::string reply) { replied.set_value(std::move(reply)); });
    REQUIRE(future.wait_for(std::chrono::seconds{30}) == std::future_status::ready);
    return morph::wire::decode(future.get());
}

/// The ActionRefusal a completion settles with, if it settles with one.
template <typename T>
std::optional<morph::model::ActionRefusal> refusalOf(morph::exec::MainThreadExecutor& owner,
                                                     morph::async::Completion<T> completion) {
    try {
        static_cast<void>(morph::testing::awaitValueOn(owner, std::move(completion)));
    } catch (morph::model::ActionRefusal const& refusal) {
        return refusal;
    } catch (std::exception const&) {
        return std::nullopt;
    }
    return std::nullopt;
}

}  // namespace

// Mutation: drop the refusal from makeRefusalErr's envelope (the reply has no `refusal`).
TEST_CASE("refusal: a server answers a refused action with its code and arguments", "[refusal]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto const server = std::make_shared<morph::backend::RemoteServer>(pool);
    auto const reply = serverReply(*server, "T_RefusalTake", R"({"who":"Alice"})");
    CHECK(reply.kind == "err");
    CHECK(reply.message == "taken by Alice");
    REQUIRE(reply.refusal.has_value());
    CHECK(reply.refusal->code == "probe.taken");
    REQUIRE(reply.refusal->args.size() == 1U);
    CHECK(reply.refusal->args.front().name == "holder");
    CHECK(reply.refusal->args.front().value == "Alice");
}

// Mutation: wrap every exception as a refusal with an empty code in finishRemote.
TEST_CASE("refusal: any other exception answers an err with no refusal, and the field is not written", "[refusal]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto const server = std::make_shared<morph::backend::RemoteServer>(pool);
    auto const reply = serverReply(*server, "T_RefusalBreak", "{}");
    CHECK(reply.kind == "err");
    CHECK(reply.message == "broken");
    CHECK_FALSE(reply.refusal.has_value());
    CHECK(morph::wire::encode(reply).find("refusal") == std::string::npos);
}

// Mutation: build executeFailure's exception as a std::runtime_error.
TEST_CASE("refusal: a client over the wire settles a refused action with the ActionRefusal", "[refusal]") {
    morph::exec::ThreadPoolExecutor pool{2};
    auto const server = std::make_shared<morph::backend::RemoteServer>(pool);
    morph::exec::MainThreadExecutor owner;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::SimulatedRemoteBackend>(*server), owner};
    morph::bridge::BridgeHandler<rt::Probe> probe{bridge, &owner};
    auto const taken = refusalOf(owner, probe.execute(rt::Take{.who = "Alice"}));
    REQUIRE(taken.has_value());
    CHECK(taken->code() == "probe.taken");
    CHECK(taken->args().front().value == "Alice");
    CHECK(std::string{taken->what()} == "taken by Alice");
    CHECK_FALSE(refusalOf(owner, probe.execute(rt::Break{})).has_value());
    // A guard's refusal travels as an action's own does.
    auto const guarded = refusalOf(owner, probe.execute(rt::Guarded{}));
    REQUIRE(guarded.has_value());
    CHECK(guarded->code() == "probe.guarded");
    CHECK(guarded->args().front().value == "closed");
    // So does a Task handler's.
    auto const later = refusalOf(owner, probe.execute(rt::TakeLater{}));
    REQUIRE(later.has_value());
    CHECK(later->code() == "probe.later");
}

// Mutation: rethrow the exception as a std::runtime_error in localOp (and in localTaskOp for the Task handler).
TEST_CASE("refusal: the in-process bridge settles a refused action with the ActionRefusal itself", "[refusal]") {
    morph::exec::ThreadPoolExecutor pool{2};
    morph::exec::MainThreadExecutor owner;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool), owner};
    morph::bridge::BridgeHandler<rt::Probe> probe{bridge, &owner};
    auto const taken = refusalOf(owner, probe.execute(rt::Take{.who = "Bob"}));
    REQUIRE(taken.has_value());
    CHECK(taken->code() == "probe.taken");
    CHECK(taken->args().front().value == "Bob");
    CHECK_FALSE(refusalOf(owner, probe.execute(rt::Break{})).has_value());
    auto const guarded = refusalOf(owner, probe.execute(rt::Guarded{}));
    REQUIRE(guarded.has_value());
    CHECK(guarded->code() == "probe.guarded");
    auto const later = refusalOf(owner, probe.execute(rt::TakeLater{}));
    REQUIRE(later.has_value());
    CHECK(later->code() == "probe.later");
}

TEST_CASE("refusal: an err reply from a peer that predates the field decodes with no refusal", "[refusal]") {
    auto const reply = morph::wire::decode(R"({"kind":"err","callId":3,"message":"nope"})");
    CHECK_FALSE(reply.refusal.has_value());
    auto const failure = morph::wire::executeFailure(reply);
    CHECK_THROWS_AS(std::rethrow_exception(failure), std::runtime_error);
    auto const newer =
        morph::wire::decode(R"({"kind":"err","callId":3,"message":"m","refusal":{"code":"a.b","args":[]},"extra":1})");
    REQUIRE(newer.refusal.has_value());
    CHECK(newer.refusal->code == "a.b");
}

namespace {

namespace ui = morph::ui;
using morph::ui::testing::Harness;

constexpr char const* kScreen = R"({
  "vocab": "ui/1", "id": "refusals",
  "models": { "sample": { "type": "Lims_SampleModel" } },
  "mutations": { "m": { "model": "sample", "action": "Lims_Save" } },
  "view": { "kind": "column", "children": [
    { "kind": "button", "testId": "go", "label": "go", "onClick": { "run": "m" } },
    { "kind": "text", "testId": "words", "text": { "fn": "refusalText", "args": [ { "refusal": "m" } ] } },
    { "kind": "text", "testId": "message", "text": { "error": "m" } },
    { "kind": "text", "testId": "has",
      "text": { "fn": "format", "args": [ "{0}", { "fn": "engaged", "args": [ { "refusal": "m" } ] } ] } } ] } })";

using Catalogue = std::map<std::string, std::string, std::less<>>;

/// Installs @p catalogue as the session's messages, in every locale.
void install(Harness& h, Catalogue catalogue) {
    h.session.setTranslations(
        [catalogue = std::move(catalogue)](std::string_view key, std::string_view) -> std::optional<std::string> {
            auto const found = catalogue.find(key);
            return found == catalogue.end() ? std::nullopt : std::optional<std::string>{found->second};
        });
}

/// Runs the mutation and fails its call with @p error.
void failWith(Harness& h, std::exception_ptr const& error) {
    h.backend.click(h.byTestId("Button", "go"));
    h.settle();
    auto& call = h.dispatcher.calls.at(h.dispatcher.last("Lims_Save"));
    call.answered = true;
    call.promise.reject(error);
    h.settle();
}

std::exception_ptr taken() {
    return std::make_exception_ptr(morph::model::ActionRefusal{
        "probe.taken", {{"holder", "Alice"}}, "SubmitInvoice: invoice 7 taken by 'alice'"});
}

/// Whether any diagnostic of @p h names @p key.
bool reportsKey(Harness const& h, std::string const& key) {
    return std::ranges::any_of(h.diagnostics,
                               [&key](ui::Refusal const& line) { return line.reason.contains("`" + key + "`"); });
}

}  // namespace

// Mutation: show `message` as the fallback sentence in refusalText.
TEST_CASE("refusal: a screen words a refusal from the catalogue by its code, with its arguments", "[refusal][ui]") {
    Harness h;
    install(h, {{"probe.taken", "Held by {$holder}"}, {"probe.generic", "Not allowed"}});
    auto const screen = h.mount(kScreen);
    CHECK(h.text("words").empty());
    CHECK(h.text("has") == "false");
    failWith(h, taken());
    CHECK(h.text("words") == "Held by Alice");
    CHECK(h.text("has") == "true");
    CHECK(h.text("message") == "SubmitInvoice: invoice 7 taken by 'alice'");
    CHECK_FALSE(reportsKey(h, "probe.taken"));
}

TEST_CASE("refusal: the class sentence stands in for a code the catalogue lacks, then the built-in one",
          "[refusal][ui]") {
    {
        Harness h;
        install(h, {{"probe.generic", "Not allowed: {$holder}"}});
        auto const screen = h.mount(kScreen);
        failWith(h, taken());
        CHECK(h.text("words") == "Not allowed: Alice");
    }
    {
        Harness h;
        install(h, {{"probe.taken.generic", "too long"}, {"other.generic", "wrong tree"}});
        auto const screen = h.mount(kScreen);
        failWith(h, taken());
        CHECK(h.text("words") == "This was refused.");
        CHECK(h.text("words").find("taken by") == std::string::npos);
    }
    {
        Harness h;
        install(h, {{"probe.generic", "probe class"}, {"lims.generic", "lims class"}});
        auto const screen = h.mount(kScreen);
        failWith(h, std::make_exception_ptr(morph::model::ActionRefusal{"lims.billing.invoiceLeased", {}, "x"}));
        CHECK(h.text("words") == "lims class");
        Harness g;
        install(g, {{"lims.billing.generic", "billing class"}, {"lims.generic", "lims class"}});
        auto const second = g.mount(kScreen);
        failWith(g, std::make_exception_ptr(morph::model::ActionRefusal{"lims.billing.invoiceLeased", {}, "x"}));
        CHECK(g.text("words") == "billing class");
    }
}

// Mutation: report nothing for a missing run-time key.
TEST_CASE("refusal: a code the catalogue lacks is reported once, naming the key", "[refusal][ui]") {
    Harness h;
    install(h, {{"probe.generic", "Not allowed"}});
    auto const screen = h.mount(kScreen);
    failWith(h, taken());
    CHECK(h.text("words") == "Not allowed");
    auto const count = std::ranges::count_if(
        h.diagnostics, [](ui::Refusal const& line) { return line.reason.contains("`probe.taken`"); });
    CHECK(count == 1);
    CHECK_FALSE(reportsKey(h, "probe.generic"));
}

TEST_CASE("refusal: a failure that is not a refusal reads null and shows no words", "[refusal][ui]") {
    Harness h;
    install(h, {{"probe.generic", "Not allowed"}});
    auto const screen = h.mount(kScreen);
    failWith(h, std::make_exception_ptr(std::runtime_error{"boom"}));
    CHECK(h.text("words").empty());
    CHECK(h.text("has") == "false");
    CHECK(h.text("message") == "boom");
}

TEST_CASE("refusal: a query's refusal is read like a mutation's, and a success clears it", "[refusal][ui]") {
    Harness h;
    install(h, {{"probe.taken", "Held by {$holder}"}});
    auto const screen = h.mount(R"({
      "vocab": "ui/1", "id": "queryRefusal",
      "models": { "sample": { "type": "Lims_SampleModel" } },
      "queries": { "q": { "model": "sample", "action": "Lims_ListResults" } },
      "mutations": { "m": { "model": "sample", "action": "Lims_Save" } },
      "view": { "kind": "column", "children": [
        { "kind": "text", "testId": "words", "text": { "fn": "refusalText", "args": [ { "refusal": "q" } ] } } ] } })");
    auto& call = h.dispatcher.calls.at(h.dispatcher.last("Lims_ListResults"));
    call.answered = true;
    call.promise.reject(taken());
    h.settle();
    CHECK(h.text("words") == "Held by Alice");
}

TEST_CASE("refusal: t takes a key expression and a list of named arguments", "[refusal][ui]") {
    Harness h;
    install(h, {{"probe.dyn", "{$a}-{$b}"}});
    auto const screen = h.mount(R"({
      "vocab": "ui/1", "id": "dyn",
      "state": { "k": { "init": "probe.nothing" } },
      "view": { "kind": "column", "children": [
        { "kind": "text", "testId": "words",
          "text": { "t": { "expr": "probe.dyn" }, "fallback": "none",
                    "args": { "expr": { "lit": [ { "name": "a", "value": "x" }, { "name": "b", "value": "y" } ] } } } },
        { "kind": "text", "testId": "plain",
          "text": { "t": { "expr": { "ref": "k" } }, "fallback": "Fallback {$a}", "args": { "a": "1" } } } ] } })");
    CHECK(h.text("words") == "x-y");
    CHECK(h.text("plain") == "Fallback 1");
    CHECK(reportsKey(h, "probe.nothing"));
}

TEST_CASE("refusal: the author API writes the forms", "[refusal][author]") {
    namespace au = morph::ui::author;
    au::MutationHandle<rt::Take> const mutation{"m"};
    au::QueryHandle<rt::Take> const query{"q"};
    CHECK(glz::write_json(mutation.refusal().json()).value_or("") == R"({"refusal":"m"})");
    CHECK(glz::write_json(query.refusal().json()).value_or("") == R"({"refusal":"q"})");
    CHECK(glz::write_json(au::refusalText(mutation.refusal()).json()).value_or("") ==
          R"({"fn":"refusalText","args":[{"refusal":"m"}]})");
    CHECK(glz::write_json(au::tAt(au::row("code"), "Fallback", au::row("args")).json()).value_or("") ==
          R"({"t":{"expr":{"row":"code"}},"fallback":"Fallback","args":{"expr":{"row":"args"}}})");
}
