// SPDX-License-Identifier: Apache-2.0
//
// Concept: the whole morph seam, in one file, with nothing else in it.
//
// This is the worked snippet `docs/GETTING-STARTED.md` walks through, kept
// here rather than only in the prose so it is compiled and run by CI — a
// tutorial that drifts from the code it describes is worse than none. If you
// change anything below, update that document's "The whole seam in one file"
// section to match.
//
// It is deliberately the *smallest* thing that still shows all four moving
// parts a first morph app needs:
//
//   1. a model — plain, single-threaded C++ with typed actions;
//   2. registration — BRIDGE_REGISTER_MODEL / BRIDGE_REGISTER_ACTION;
//   3. a call site — BridgeHandler<M>::execute(...) returning Completion<T>,
//      whose callbacks arrive on the executor you chose;
//   4. the payoff — the identical call site against a second backend that
//      serialises through the wire protocol.
//
// `examples/pastebin` (rung 1 of the application ladder) is the same four
// parts at real scale, with persistence, a GUI and a socket transport.
//
// Full design reference: docs/spec/core/bridge.md, docs/spec/core/completion.md,
// docs/spec/core/executor.md, docs/spec/core/registry.md.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <exception>
#include <map>
#include <memory>
#include <morph/core/backend.hpp>
#include <morph/core/bridge.hpp>
#include <morph/core/executor.hpp>
#include <morph/core/registry.hpp>
#include <morph/core/remote.hpp>
#include <morph/forms/forms.hpp>
#include <stdexcept>
#include <string>

// Model/action types need external (file-scope) linkage — see
// journal_and_outbox.cpp's file-scope comment for why. "GettingStarted" is
// this file's unique type-id prefix.

// ── 1. Actions and results are plain structs ───────────────────────────────
//
// No base class, no macro, no serialisation code: Glaze reflects the members,
// so these types travel the wire protocol as-is.

struct GsCreatePaste {
    std::string content;
    std::string syntax;

    // Optional. When present, the framework calls it *before* execute() on
    // both execution paths — local and remote — and rejects the action if it
    // returns false. See docs/spec/core/registry.md, "Validation and logging
    // policy".
    [[nodiscard]] bool validate() const { return !content.empty() && !syntax.empty(); }
};

struct GsCreatePasteResult {
    std::string id;
};

struct GsGetPaste {
    std::string id;
};

struct GsPasteView {
    std::string id;
    std::string content;
    std::string syntax;
};

// ── 2. The model is the application ────────────────────────────────────────
//
// Plain, single-threaded C++. It knows nothing about morph: no executor, no
// mutex, no bridge, no transport. morph runs each model instance on its own
// strand, so two execute() calls on one instance never overlap.

class GsPasteModel {
public:
    GsCreatePasteResult execute(const GsCreatePaste& action) {
        const std::string id = "paste-" + std::to_string(_pastes.size() + 1);
        _pastes[id] = GsPasteView{.id = id, .content = action.content, .syntax = action.syntax};
        return GsCreatePasteResult{.id = id};
    }

    GsPasteView execute(const GsGetPaste& action) {
        const auto found = _pastes.find(action.id);
        if (found == _pastes.end()) {
            // Domain failures are thrown. morph captures the exception and
            // delivers it to .onError() on the caller's executor; over a
            // socket, what() travels back in the error envelope.
            throw std::runtime_error{"no such paste: " + action.id};
        }
        return found->second;
    }

private:
    std::map<std::string, GsPasteView> _pastes;
};

// ── 3. Registration: the string ids the wire protocol uses ─────────────────
//
// Each macro specialises a traits template (ModelTraits<M> / ActionTraits<A>)
// and emits a file-scope initialiser that registers the type before main().
// The ids are strings because a remote peer only ever names a type by string.

BRIDGE_REGISTER_MODEL(GsPasteModel, "GettingStarted_PasteModel")
BRIDGE_REGISTER_ACTION(GsPasteModel, GsCreatePaste, "GettingStarted_CreatePaste")
BRIDGE_REGISTER_ACTION(GsPasteModel, GsGetPaste, "GettingStarted_GetPaste")

namespace {

/// @brief Turns the "UI event loop" until @p done, or until a bounded budget
///        is spent.
///
/// `MainThreadExecutor::runFor()` is a pump, not a flush: it always blocks
/// for its whole timeout. Small slices keep this fast while still failing in
/// bounded time if a completion never settles.
void pumpUntil(morph::exec::MainThreadExecutor& gui, const bool& done) {
    for (int turn = 0; turn < 400 && !done; ++turn) {
        gui.runFor(std::chrono::milliseconds{5});
    }
}

/// @brief The two deployment shapes this file exercises.
enum class Deployment {
    InProcess,    ///< `LocalBackend` — the model runs on a worker pool here.
    OverTheWire,  ///< `SimulatedRemoteBackend` — the same serialise/dispatch
                  ///< path a socket takes, minus the socket.
};

/// @brief Builds the backend for @p deployment.
///
/// @p server is kept alive by the caller for the `OverTheWire` case.
[[nodiscard]] std::unique_ptr<morph::backend::detail::IBackend> makeBackend(
    Deployment deployment, morph::exec::ThreadPoolExecutor& pool,
    const std::shared_ptr<morph::backend::RemoteServer>& server) {
    if (deployment == Deployment::InProcess) {
        return std::make_unique<morph::backend::LocalBackend>(pool);
    }
    return std::make_unique<morph::backend::SimulatedRemoteBackend>(*server);
}

}  // namespace

// ── 4. One call site, two deployments ──────────────────────────────────────
//
// This is morph's headline claim, as an assertion rather than a promise: the
// body below never mentions which backend it is running against.

TEST_CASE("getting started: the same call site works in-process and over the wire", "[concepts][getting-started]") {
    const auto deployment = GENERATE(Deployment::InProcess, Deployment::OverTheWire);

    morph::exec::MainThreadExecutor gui;      // where .then / .onError land
    morph::exec::ThreadPoolExecutor pool{2};  // where models run

    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    morph::bridge::Bridge bridge{makeBackend(deployment, pool, server)};

    // The handler is the typed, GUI-facing handle to one model type. It
    // registers on construction and deregisters on destruction.
    morph::bridge::BridgeHandler<GsPasteModel> pastes{bridge, &gui};

    // ── create ─────────────────────────────────────────────────────────────
    std::string createdId;
    bool created = false;
    pastes.execute(GsCreatePaste{.content = "hello, morph", .syntax = "text"}).then([&](GsCreatePasteResult result) {
        createdId = result.id;
        created = true;
    });

    // The callback has *not* run yet, even in-process: a Completion always
    // resolves through the executor it was issued with, never inline. This is
    // the single most load-bearing fact about the call site, and the reason a
    // captured `this` needs a lifetime guard in real code.
    CHECK_FALSE(created);

    pumpUntil(gui, created);  // ← your UI event loop turning
    REQUIRE(created);
    CHECK(createdId == "paste-1");

    // ── read it back ───────────────────────────────────────────────────────
    GsPasteView view;
    bool loaded = false;
    pastes.execute(GsGetPaste{.id = createdId}).then([&](GsPasteView value) {
        view = std::move(value);
        loaded = true;
    });
    pumpUntil(gui, loaded);
    REQUIRE(loaded);
    CHECK(view.content == "hello, morph");
    CHECK(view.syntax == "text");

    // ── a domain failure ───────────────────────────────────────────────────
    // The model threw; the message survives both deployments unchanged.
    std::string errorText;
    bool failed = false;
    pastes.execute(GsGetPaste{.id = "nope"})
        .then([&](GsPasteView) { FAIL("an unknown id must not succeed"); })
        .onError([&](const std::exception_ptr& err) {
            try {
                std::rethrow_exception(err);
            } catch (const std::exception& ex) {
                errorText = ex.what();
            }
            failed = true;
        });
    pumpUntil(gui, failed);
    REQUIRE(failed);
    CHECK(errorText == "no such paste: nope");
}

// ── 5. validate() is enforced on both paths ────────────────────────────────
//
// Worth asserting rather than assuming: a client-side form gate is UX, and a
// remote client can skip it. The framework's own check cannot be skipped.

TEST_CASE("getting started: an action's validate() gates execution in both deployments",
          "[concepts][getting-started]") {
    const auto deployment = GENERATE(Deployment::InProcess, Deployment::OverTheWire);

    morph::exec::MainThreadExecutor gui;
    morph::exec::ThreadPoolExecutor pool{2};
    auto server = std::make_shared<morph::backend::RemoteServer>(pool);
    morph::bridge::Bridge bridge{makeBackend(deployment, pool, server)};
    morph::bridge::BridgeHandler<GsPasteModel> pastes{bridge, &gui};

    bool rejected = false;
    pastes.execute(GsCreatePaste{.content = "", .syntax = ""})
        .then([&](GsCreatePasteResult) { FAIL("an invalid action must not execute"); })
        .onError([&](const std::exception_ptr&) { rejected = true; });
    pumpUntil(gui, rejected);
    CHECK(rejected);
}

// ── 6. The form schema is derived from the compiled action type ────────────
//
// Nothing declares this schema: it is read off `GsCreatePaste`'s members, so
// it cannot describe a field the action does not have.

TEST_CASE("getting started: schemaJson describes the action type it was generated from",
          "[concepts][getting-started]") {
    const std::string schema = morph::forms::schemaJson<GsCreatePaste>();

    CHECK(schema.find(R"("content")") != std::string::npos);
    CHECK(schema.find(R"("syntax")") != std::string::npos);
    // Every reflected member is required unless it is a std::optional or is
    // named in the action's own `optionalFields` opt-out.
    CHECK(schema.find(R"("required":["content","syntax"])") != std::string::npos);
}
