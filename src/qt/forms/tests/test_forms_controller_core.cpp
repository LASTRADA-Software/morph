// SPDX-License-Identifier: Apache-2.0
//
// Catch2 (own main, controls QCoreApplication lifetime like
// tests/qt/test_qt_websocket.cpp) coverage of FormsControllerCore<Model>:
// generic submit + generic options-fetch via executeJson, proving the
// shipped core no longer hardcodes one action the way the pre-factoring
// example FormsController did.

#include <QCoreApplication>
#include <QEventLoop>
#include <atomic>
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <morph/core/registry.hpp>
#include <morph/qt/forms/forms_controller_core.hpp>
#include <string>
#include <thread>
#include <vector>

// Deliberately at file scope, NOT inside an anonymous namespace: glaze's
// reflection (glz::detail::get_name, reached via BRIDGE_REGISTER_MODEL/
// BRIDGE_REGISTER_ACTION's ActionTraits wiring) forms an `extern const T
// external;` declaration for each registered type, which requires T to have
// external linkage -- a type defined inside an unnamed namespace does not
// qualify, and fails to compile ("used but not defined in this translation
// unit, and cannot be defined in any other translation unit because its type
// does not have linkage"). This is the same constraint tests/test_quantity_
// forms.cpp's QF-prefix convention exists for; no prefix is needed here since
// this is its own standalone executable (morph_forms_controller_core_tests),
// never linked into the shared morph_tests binary, so there is no ODR risk
// from other translation units reusing these names.

struct EchoAction {
    std::string text;
};

struct WidgetRow {
    std::int64_t id = 0;
    std::string name;
};

struct ListWidgetsResult {
    std::vector<WidgetRow> widgets;
};

struct ListWidgets {};

// An options action that takes an input field — the shape a dependent
// Choice's options action has (docs/spec/forms/choice.md's "Dependent
// (cascading) options"): fetchOptions must forward the caller's body to it,
// not just its name.
struct ListFilteredWidgets {
    std::string category;
};

class PingModel {
public:
    std::string execute(const EchoAction& action) { return "echo: " + action.text; }
    ListWidgetsResult execute(const ListWidgets&) { return ListWidgetsResult{.widgets = {{1, "alpha"}, {2, "beta"}}}; }
    ListWidgetsResult execute(const ListFilteredWidgets& action) {
        if (action.category == "greek") {
            return ListWidgetsResult{.widgets = {{3, "gamma"}}};
        }
        return ListWidgetsResult{};
    }
};

BRIDGE_REGISTER_MODEL(PingModel, "PingModel")
BRIDGE_REGISTER_ACTION(PingModel, EchoAction, "EchoAction")
BRIDGE_REGISTER_ACTION(PingModel, ListWidgets, "ListWidgets")
BRIDGE_REGISTER_ACTION(PingModel, ListFilteredWidgets, "ListFilteredWidgets")

namespace {

void pumpUntil(const std::function<bool()>& done, int maxIterations = 300) {
    for (int idx = 0; idx < maxIterations && !done(); ++idx) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

}  // namespace

TEST_CASE("morph::qt::forms::FormsControllerCore submits via generic executeJson", "[forms_controller_core]") {
    morph::qt::forms::FormsControllerCore<PingModel> core{R"({"EchoAction":{}})"};
    CHECK(core.schemasJson() == R"({"EchoAction":{}})");

    std::atomic<bool> done{false};
    std::string reply;
    core.submitIfValid(
        "EchoAction", R"({"text":"hi"})",
        [&](std::string resultJson) {
            reply = std::move(resultJson);
            done.store(true);
        },
        [&](const std::exception_ptr&) { done.store(true); });

    pumpUntil([&] { return done.load(); });
    CHECK(reply == R"("echo: hi")");
}

TEST_CASE("morph::qt::forms::FormsControllerCore fetches options for the NAMED action, not a hardcoded one",
          "[forms_controller_core]") {
    morph::qt::forms::FormsControllerCore<PingModel> core{std::string{}};

    std::atomic<bool> done{false};
    std::string reply;
    core.fetchOptions(
        "ListWidgets", "{}",
        [&](std::string resultJson) {
            reply = std::move(resultJson);
            done.store(true);
        },
        [&](const std::exception_ptr&) { done.store(true); });

    pumpUntil([&] { return done.load(); });
    CHECK(reply.find("alpha") != std::string::npos);
    CHECK(reply.find("beta") != std::string::npos);
}

TEST_CASE("morph::qt::forms::FormsControllerCore forwards fetchOptions' body, not just its action name",
          "[forms_controller_core]") {
    // A dependent Choice's options action needs the parent's current value as
    // its request body (docs/spec/forms/choice.md); fetchOptions must be a
    // true name+body pass-through, not an empty-body-only call.
    morph::qt::forms::FormsControllerCore<PingModel> core{std::string{}};

    std::atomic<bool> done{false};
    std::string reply;
    core.fetchOptions(
        "ListFilteredWidgets", R"({"category":"greek"})",
        [&](std::string resultJson) {
            reply = std::move(resultJson);
            done.store(true);
        },
        [&](const std::exception_ptr&) { done.store(true); });

    pumpUntil([&] { return done.load(); });
    CHECK(reply.find("gamma") != std::string::npos);
    CHECK(reply.find("alpha") == std::string::npos);
}

TEST_CASE("morph::qt::forms::FormsControllerCore composes over a caller-supplied Bridge/executor",
          "[forms_controller_core]") {
    // Issue #57: FormsControllerCore must be usable against a Bridge the
    // caller already owns (e.g. one already switched to a Remote/Socket
    // backend, or shared across multiple presenters) instead of always
    // building and owning its own private, always-local Bridge.
    morph::exec::ThreadPoolExecutor pool{2};
    morph::qt::QtExecutor gui;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};

    morph::qt::forms::FormsControllerCore<PingModel> core{bridge, &gui, R"({"EchoAction":{}})"};
    CHECK(core.schemasJson() == R"({"EchoAction":{}})");

    std::atomic<bool> done{false};
    std::string reply;
    core.submitIfValid(
        "EchoAction", R"({"text":"composed"})",
        [&](std::string resultJson) {
            reply = std::move(resultJson);
            done.store(true);
        },
        [&](const std::exception_ptr&) { done.store(true); });

    pumpUntil([&] { return done.load(); });
    CHECK(reply == R"("echo: composed")");
}

TEST_CASE(
    "morph::qt::forms::FormsControllerCore composed over a caller Bridge dispatches through a later switchBackend",
    "[forms_controller_core]") {
    // The composed-over Bridge is the caller's own -- a later switchBackend()
    // on that same Bridge (e.g. local -> remote/socket) must still be
    // reachable through this core's handler, proving the core never captured
    // a private snapshot of the backend.
    morph::exec::ThreadPoolExecutor pool{2};
    morph::qt::QtExecutor gui;
    morph::bridge::Bridge bridge{std::make_unique<morph::backend::LocalBackend>(pool)};

    morph::qt::forms::FormsControllerCore<PingModel> core{bridge, &gui, std::string{}};

    bridge.switchBackend(std::make_unique<morph::backend::LocalBackend>(pool));

    std::atomic<bool> done{false};
    std::string reply;
    core.submitIfValid(
        "EchoAction", R"({"text":"after-switch"})",
        [&](std::string resultJson) {
            reply = std::move(resultJson);
            done.store(true);
        },
        [&](const std::exception_ptr&) { done.store(true); });

    pumpUntil([&] { return done.load(); });
    CHECK(reply == R"("echo: after-switch")");
}

int main(int argc, char** argv) {
    QCoreApplication app{argc, argv};
    return Catch::Session().run(argc, argv);
}
