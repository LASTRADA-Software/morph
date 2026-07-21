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

class PingModel {
public:
    std::string execute(const EchoAction& action) { return "echo: " + action.text; }
    ListWidgetsResult execute(const ListWidgets&) { return ListWidgetsResult{.widgets = {{1, "alpha"}, {2, "beta"}}}; }
};

BRIDGE_REGISTER_MODEL(PingModel, "PingModel")
BRIDGE_REGISTER_ACTION(PingModel, EchoAction, "EchoAction")
BRIDGE_REGISTER_ACTION(PingModel, ListWidgets, "ListWidgets")

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
        "ListWidgets",
        [&](std::string resultJson) {
            reply = std::move(resultJson);
            done.store(true);
        },
        [&](const std::exception_ptr&) { done.store(true); });

    pumpUntil([&] { return done.load(); });
    CHECK(reply.find("alpha") != std::string::npos);
    CHECK(reply.find("beta") != std::string::npos);
}

int main(int argc, char** argv) {
    QCoreApplication app{argc, argv};
    return Catch::Session().run(argc, argv);
}
