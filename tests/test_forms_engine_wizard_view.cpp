// SPDX-License-Identifier: Apache-2.0
//
// wizardView on RecordingBackend: a drawn wizard (the header, the current step's form, Back and Next and their
// gates, the chrome slots), asserted on mounted widgets. The session's own claims are
// test_forms_engine_wizard.cpp's.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <memory>
#include <morph/forms/engine/form_view.hpp>
#include <morph/forms/engine/overrides.hpp>
#include <morph/forms/engine/wizard.hpp>
#include <morph/forms/engine/wizard_view.hpp>
#include <morph/ui/mount.hpp>
#include <morph/ui/testing/recording_backend.hpp>
#include <morph/ui/view.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "forms_engine_support.hpp"

namespace {

using morph::forms::WizardModel;
using morph::forms::WizardSession;
using morph::forms::WizardViewOptions;
using morph::test::formsengine::Call;
using morph::test::formsengine::Harness;
using morph::ui::testing::RecordingBackend;
namespace ui = morph::ui;

// The fixture wizard and its schemas.
constexpr std::string_view kWizard = R"({"w-title":"Test flow","w-steps":[
  {"action":"WizStepOne","title":"One"},
  {"action":"WizStepTwo","title":"Two","prefill":{"refId":"WizStepOne.id"}}]})";

[[nodiscard]] std::optional<std::string> stepSchema(std::string_view action) {
    if (action == "WizStepOne") {
        return R"({"properties":{"label":{"type":"string","x-order":0}},"required":["label"]})";
    }
    if (action == "WizStepTwo") {
        return R"({"properties":{"refId":{"type":"integer","x-order":0}},"required":["refId"]})";
    }
    return std::nullopt;
}

/// Step one replies with the id it created, past 2^53; step two with a summary.
[[nodiscard]] std::optional<std::string> wizardServer(Call const& call) {
    if (call.action == "WizStepOne") {
        return std::string{R"({"id":9007199254740993})"};
    }
    return std::string{R"({"summary":"ok"})"};
}

/// A mounted wizard: the session first, the mount after it, so the mount goes first.
struct Mounted {
    Harness harness;
    RecordingBackend backend;
    std::unique_ptr<WizardSession> session;
    std::unique_ptr<ui::Mounted> view;

    explicit Mounted(WizardViewOptions const& options = {}) {
        harness.server.replyWith(wizardServer);
        auto model = WizardModel::fromSchema("TestWizard", kWizard);
        REQUIRE(model.has_value());
        session = std::make_unique<WizardSession>(harness.rt, *std::move(model), stepSchema,
                                                  harness.server.submitter(), harness.server.fetcher());
        view = std::make_unique<ui::Mounted>(harness.rt, backend, morph::forms::wizardView(*session, options));
        harness.settle();
    }

    ~Mounted() { view.reset(); }
    Mounted(Mounted const&) = delete;
    Mounted& operator=(Mounted const&) = delete;
    Mounted(Mounted&&) = delete;
    Mounted& operator=(Mounted&&) = delete;

    [[nodiscard]] int widget(std::string_view kind, std::string_view testId) const {
        auto const found = backend.find(kind, "testId", testId);
        INFO("no " << kind << " with testId " << testId << "\n" << backend.dump());
        REQUIRE(found.has_value());
        return *found;
    }

    [[nodiscard]] bool has(std::string_view kind, std::string_view testId) const {
        return backend.find(kind, "testId", testId).has_value();
    }

    [[nodiscard]] std::string prop(std::string_view kind, std::string_view testId, std::string_view name) const {
        return backend.prop(widget(kind, testId), name);
    }

    void click(std::string_view testId) {
        backend.click(widget("Button", testId));
        harness.settle();
    }

    void edit(std::string_view field, std::string text) {
        backend.edit(widget("TextInput", "field_" + std::string{field}), std::move(text));
        harness.settle();
    }

    [[nodiscard]] std::size_t count(std::string_view action) const {
        std::size_t total = 0;
        for (Call const& call : harness.server.submits()) {
            total += call.action == action ? 1U : 0U;
        }
        return total;
    }
};

}  // namespace

TEST_CASE("wizardView: the header names the flow and the step; only the current step's form is mounted",
          "[forms-engine][wizard-view]") {
    Mounted const mounted;
    CHECK(mounted.prop("Text", "wizard_title", "text") == "Test flow  (1 / 2)");
    CHECK(mounted.prop("Steps", "wizard_steps", "steps") == "[One:Active,Two:Pending]");
    CHECK(mounted.has("Column", "wizard_step_0"));
    CHECK_FALSE(mounted.has("Column", "wizard_step_1"));
    CHECK(mounted.has("TextInput", "field_label"));
    CHECK_FALSE(mounted.has("TextInput", "field_refId"));
    CHECK(mounted.prop("Text", "wizard_last", "visible") == "false");
}

TEST_CASE("wizardView: Next is disabled until the step is ready, and Back until there is a step behind",
          "[forms-engine][wizard-view]") {
    Mounted mounted;
    CHECK(mounted.prop("Button", "wizardNext", "enabled") == "false");
    CHECK(mounted.prop("Button", "wizardBack", "enabled") == "false");
    mounted.edit("label", "x");
    CHECK(mounted.count("WizStepOne") == 1);  // automatic: the step submitted as it became ready
    CHECK(mounted.prop("Button", "wizardNext", "enabled") == "true");
}

TEST_CASE("wizardView: Next moves on with the prefill applied; Back returns with the values kept",
          "[forms-engine][wizard-view]") {
    Mounted mounted;
    mounted.edit("label", "x");
    mounted.click("wizardNext");
    CHECK(mounted.session->current() == 1);
    CHECK(mounted.prop("Text", "wizard_title", "text") == "Test flow  (2 / 2)");
    CHECK(mounted.prop("Steps", "wizard_steps", "steps") == "[One:Done,Two:Active]");
    CHECK(mounted.prop("TextInput", "field_refId", "text") == "9007199254740993");  // exact, from the reply
    CHECK(mounted.prop("Text", "wizard_last", "visible") == "true");
    CHECK(mounted.prop("Button", "wizardNext", "enabled") == "false");
    CHECK(mounted.prop("Button", "wizardBack", "enabled") == "true");
    mounted.click("wizardBack");
    CHECK(mounted.session->current() == 0);
    CHECK(mounted.prop("TextInput", "field_label", "text") == "x");
    mounted.click("wizardNext");  // a done step moves on without submitting again
    CHECK(mounted.count("WizStepOne") == 1);
    CHECK(mounted.session->current() == 1);
}

TEST_CASE("wizardView: the header and nav chrome replace the built-ins with the same gates",
          "[forms-engine][wizard-view][chrome]") {
    WizardViewOptions options{};
    options.chrome.header = [](WizardSession& session) {
        WizardSession* const wizard = &session;
        ui::Text text{.text = [wizard] {
            return wizard->title() + "|" + std::to_string(wizard->current()) + "/" +
                   std::to_string(wizard->stepCount()) + "|" + wizard->stepTitle(wizard->current());
        }};
        text.common.testId = "wizardHeaderChrome";
        return ui::text(std::move(text));
    };
    options.chrome.nav = [](WizardSession& session) {
        WizardSession* const wizard = &session;
        ui::Text gates{.text = [wizard] {
            return std::string{wizard->canBack() ? "back" : "-"} + "," + (wizard->canNext() ? "next" : "-") + "," +
                   (wizard->onLastStep() ? "last" : "-");
        }};
        gates.common.testId = "wizardNavChrome";
        ui::Button back{.label = "<", .onClick = [wizard] { wizard->back(); }};
        back.common.testId = "wizardNavChrome_back";
        // Not gated by `enabled`: the session's `next` is the gate, as the QML chrome's `next()` is.
        ui::Button next{.label = ">", .onClick = [wizard] { wizard->next(); }};
        next.common.testId = "wizardNavChrome_next";
        return ui::row(ui::Row{
            .children = {ui::text(std::move(gates)), ui::button(std::move(back)), ui::button(std::move(next))}});
    };
    Mounted mounted{options};
    CHECK_FALSE(mounted.has("Text", "wizard_title"));
    CHECK_FALSE(mounted.has("Button", "wizardNext"));
    CHECK_FALSE(mounted.has("Button", "wizardBack"));
    CHECK(mounted.prop("Text", "wizardHeaderChrome", "text") == "Test flow|0/2|One");
    CHECK(mounted.prop("Text", "wizardNavChrome", "text") == "-,-,-");
    mounted.click("wizardNavChrome_next");  // nothing happens until the step has replied
    CHECK(mounted.session->current() == 0);
    mounted.edit("label", "x");
    CHECK(mounted.prop("Text", "wizardNavChrome", "text") == "-,next,-");
    mounted.click("wizardNavChrome_next");
    CHECK(mounted.prop("Text", "wizardHeaderChrome", "text") == "Test flow|1/2|Two");
    CHECK(mounted.prop("Text", "wizardNavChrome", "text") == "back,-,last");
    mounted.click("wizardNavChrome_back");
    CHECK(mounted.session->current() == 0);
}

TEST_CASE("wizardView: the form options reach every step's fields", "[forms-engine][wizard-view][chrome]") {
    WizardViewOptions options{};
    auto const custom = [](morph::forms::FieldView const& field) {
        ui::Text text{.text = "custom " + field.state().path()};
        text.common.testId = "override_" + field.state().path();
        return ui::text(std::move(text));
    };
    options.form.overrides.byKind(morph::forms::FieldKind::Integer, custom);
    options.form.overrides.byKind(morph::forms::FieldKind::Text, custom);
    Mounted mounted{options};
    CHECK(mounted.prop("Text", "override_label", "text") == "custom label");
    mounted.session->step(0).field("label").text().set("x");
    mounted.harness.settle();
    mounted.click("wizardNext");
    CHECK(mounted.prop("Text", "override_refId", "text") == "custom refId");
}
