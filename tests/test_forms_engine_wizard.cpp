// SPDX-License-Identifier: Apache-2.0
//
// Wizards, headless: the claims of src/qt/forms/tests/tst_wizardview.qml re-expressed on WizardSession, and the
// rules spec 2 adds for returning to a step and resubmitting a changed one. Back and Next drawn as buttons, and the
// chrome of tst_ViewChrome.qml, wait for the view.

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <memory>
#include <morph/forms/engine/form_session.hpp>
#include <morph/forms/engine/wizard.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "forms_engine_support.hpp"

namespace {

using morph::forms::WizardModel;
using morph::forms::WizardSession;
using morph::test::formsengine::Call;
using morph::test::formsengine::Harness;

// The wizard and schemas of tst_wizardview.qml.
constexpr std::string_view kWizard = R"({"w-title":"Test flow","w-steps":[
  {"action":"WizStepOne","title":"One"},
  {"action":"WizStepTwo","title":"Two","prefill":{"refId":"WizStepOne.id"}}]})";

[[nodiscard]] morph::forms::SchemaLookup schemas(bool explicitSecond = false) {
    return [explicitSecond](std::string_view action) -> std::optional<std::string> {
        if (action == "WizStepOne") {
            return R"({"properties":{"label":{"type":"string","x-order":0}},"required":["label"]})";
        }
        if (action == "WizStepTwo") {
            return explicitSecond
                       ? R"({"properties":{"refId":{"type":"integer","x-order":0},"note":{"type":"string","x-order":1}},)"
                         R"("required":["refId","note"],"x-submitMode":"explicit"})"
                       : R"({"properties":{"refId":{"type":"integer","x-order":0}},"required":["refId"]})";
        }
        return std::nullopt;
    };
}

[[nodiscard]] WizardModel wizardModel(std::string_view json = kWizard) {
    auto model = WizardModel::fromSchema("TestWizard", json);
    INFO((model ? std::string{} : model.error().path + ": " + model.error().message));
    REQUIRE(model.has_value());
    return *std::move(model);
}

/// Step one replies with the id it created, past 2^53 so it must travel exactly; step two with a summary.
[[nodiscard]] std::optional<std::string> wizardServer(Call const& call) {
    if (call.action == "WizStepOne") {
        return std::string{R"({"id":9007199254740993,"label":"x"})"};
    }
    return std::string{R"({"summary":"ok"})"};
}

struct Stack {
    Harness harness;
    std::unique_ptr<WizardSession> wizard;

    explicit Stack(bool explicitSecond = false,
                   morph::test::formsengine::FakeServer::Responder responder = wizardServer) {
        harness.server.replyWith(std::move(responder));
        wizard = std::make_unique<WizardSession>(harness.rt, wizardModel(), schemas(explicitSecond),
                                                 harness.server.submitter(), harness.server.fetcher());
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

TEST_CASE("WizardModel: the w-* document reads into titled steps with their prefill", "[forms-engine][wizard]") {
    WizardModel const model = wizardModel();
    CHECK(model.id() == "TestWizard");
    CHECK(model.title() == "Test flow");
    REQUIRE(model.steps().size() == 2);
    CHECK(model.steps()[1].prefill == std::vector<std::pair<std::string, std::string>>{{"refId", "WizStepOne.id"}});
    CHECK(WizardModel::fromSchema("W", R"({"w-steps":{}})").error().path == "w-steps");
    CHECK(WizardModel::fromSchema("W", R"({"w-steps":[]})").error().path == "w-steps");
    CHECK(WizardModel::fromSchema("W", R"({"w-steps":[{"title":"x"}]})").error().path == "w-steps[0]");
    CHECK(WizardModel::fromSchema("W", R"({"w-steps":[{"action":"A","prefill":{"f":"nodot"}}]})").error().path ==
          "w-steps[0].prefill.f");
}

TEST_CASE("WizardSession: an unknown step schema is refused at construction", "[forms-engine][wizard]") {
    Harness harness;
    CHECK_THROWS_AS(WizardSession(harness.rt, wizardModel(), [](std::string_view) { return std::nullopt; },
                                  harness.server.submitter(), {}),
                    std::invalid_argument);
}

TEST_CASE("WizardSession: Next waits for the step's successful submission; Back always returns",
          "[forms-engine][wizard]") {
    Stack stack;
    CHECK(stack.wizard->current() == 0);
    CHECK_FALSE(stack.wizard->canNext());  // nothing typed, nothing replied
    CHECK_FALSE(stack.wizard->canBack());
    stack.harness.server.replyWith([](Call const&) { return std::optional<std::string>{}; });  // hold the reply
    stack.harness.type(stack.wizard->step(0), "label", "sample");
    REQUIRE(stack.count("WizStepOne") == 1);  // automatic mode submitted
    CHECK_FALSE(stack.wizard->stepDone(0));
    CHECK_FALSE(stack.wizard->canNext());  // the submission has not replied
    stack.wizard->next();
    CHECK(stack.wizard->current() == 0);

    stack.harness.server.resolveSubmit(0, R"({"id":1,"label":"sample"})");
    stack.harness.settle();
    CHECK(stack.wizard->stepDone(0));
    CHECK(stack.wizard->canNext());
    stack.wizard->next();
    stack.harness.settle();
    CHECK(stack.wizard->current() == 1);
    CHECK(stack.wizard->onLastStep());
    CHECK_FALSE(stack.wizard->canNext());
    CHECK(stack.wizard->canBack());
    stack.wizard->back();
    CHECK(stack.wizard->current() == 0);
    CHECK(stack.wizard->step(0).field("label").text().peek() == "sample");  // kept
}

TEST_CASE("WizardSession: a failed submission leaves the step not done", "[forms-engine][wizard]") {
    Stack stack{false, [](Call const&) { return std::optional<std::string>{}; }};
    stack.harness.type(stack.wizard->step(0), "label", "sample");
    stack.harness.server.rejectSubmit(0, "refused");
    stack.harness.settle();
    CHECK_FALSE(stack.wizard->stepDone(0));
    CHECK(stack.wizard->step(0).lastError() != nullptr);
    CHECK(stack.wizard->canNext());  // ready again: Next resubmits
    stack.wizard->next();
    stack.harness.settle();
    REQUIRE(stack.count("WizStepOne") == 2);
    CHECK(stack.wizard->current() == 0);  // waits for the resubmission
    stack.harness.server.resolveSubmit(1, R"({"id":1})");
    stack.harness.settle();
    CHECK(stack.wizard->current() == 1);  // and moves on once it succeeded
}

TEST_CASE("WizardSession: entering a step applies its prefill as JSON values, the reply winning",
          "[forms-engine][wizard]") {
    Stack stack;
    stack.harness.type(stack.wizard->step(0), "label", "sample");
    CHECK(stack.wizard->resolved("WizStepOne.label") == R"("x")");  // the reply's label beat the draft's
    CHECK(stack.wizard->resolved("WizStepOne.id") == "9007199254740993");
    stack.wizard->next();
    stack.harness.settle();
    REQUIRE(stack.wizard->current() == 1);
    CHECK(stack.wizard->step(1).field("refId").text().peek() == "9007199254740993");
    // Prefilled ready in automatic mode: entering submitted it once, with the exact id.
    REQUIRE(stack.count("WizStepTwo") == 1);
    CHECK(stack.harness.server.lastBody() == R"({"refId":9007199254740993})");
}

TEST_CASE("WizardSession: returning to a done step submits nothing; Next resubmits only a changed body",
          "[forms-engine][wizard]") {
    Stack stack;
    stack.harness.type(stack.wizard->step(0), "label", "sample");
    stack.wizard->next();
    stack.harness.settle();
    REQUIRE(stack.wizard->current() == 1);
    std::size_t const firstSteps = stack.count("WizStepOne");
    std::size_t const secondSteps = stack.count("WizStepTwo");

    stack.wizard->back();
    stack.harness.settle();
    stack.wizard->next();  // step one is done with this body: no resubmission
    stack.harness.settle();
    CHECK(stack.wizard->current() == 1);
    CHECK(stack.count("WizStepOne") == firstSteps);
    CHECK(stack.count("WizStepTwo") == secondSteps);  // step two is done too: entering it submits nothing

    stack.wizard->back();
    stack.harness.type(stack.wizard->step(0), "label", "changed");  // automatic mode resubmits the changed body
    CHECK(stack.count("WizStepOne") == firstSteps + 1);
    CHECK(stack.wizard->stepDone(0));
}

TEST_CASE("WizardSession: in explicit mode Next submits the ready step and moves on after it succeeds",
          "[forms-engine][wizard]") {
    Stack stack{true};
    stack.harness.type(stack.wizard->step(0), "label", "sample");
    stack.wizard->next();
    stack.harness.settle();
    REQUIRE(stack.wizard->current() == 1);
    morph::forms::FormSession& second = stack.wizard->step(1);
    CHECK(second.field("refId").text().peek() == "9007199254740993");
    CHECK(stack.count("WizStepTwo") == 0);  // explicit: entering submits nothing
    stack.harness.type(second, "note", "n");
    CHECK(stack.count("WizStepTwo") == 0);
    CHECK_FALSE(stack.wizard->canNext());  // the last step has no Next
    second.submit();
    stack.harness.settle();
    CHECK(stack.count("WizStepTwo") == 1);
    CHECK(stack.wizard->stepDone(1));
    // Done means "this body succeeded": an edit makes the step not done until the new body does.
    stack.harness.type(second, "note", "changed");
    CHECK_FALSE(stack.wizard->stepDone(1));
    second.submit();
    stack.harness.settle();
    CHECK(stack.wizard->stepDone(1));
}

TEST_CASE("WizardSession: titles resolve through the translations", "[forms-engine][wizard]") {
    Harness harness;
    morph::forms::FormSessionOptions options{};
    options.translations = [](std::string_view key, std::string_view) -> std::optional<std::string> {
        if (key == "TestWizard.title") {
            return "Ablauf";
        }
        if (key == "TestWizard.step.1.title") {
            return "Zwei";
        }
        return std::nullopt;
    };
    WizardSession const wizard{harness.rt, wizardModel(), schemas(), harness.server.submitter(), {}, options};
    CHECK(wizard.title() == "Ablauf");
    CHECK(wizard.stepTitle(0) == "One");
    CHECK(wizard.stepTitle(1) == "Zwei");
}

TEST_CASE("WizardModel: a malformed wizard document names where", "[forms-engine][wizard]") {
    auto const where = [](std::string_view json) {
        auto const model = WizardModel::fromSchema("W", json);
        return model.has_value() ? std::string{"(read)"} : model.error().path;
    };
    CHECK(where("not json").empty());
    CHECK(where("[]").empty());
    CHECK(where(R"({"w-steps":{}})") == "w-steps");
    CHECK(where(R"({"w-steps":[{"action":"A","prefill":[]}]})") == "w-steps[0].prefill");
    CHECK(where(R"({"w-steps":[{"action":"A","prefill":{"x":5}}]})") == "w-steps[0].prefill.x");
    CHECK(where(R"({"w-steps":[{"action":"A","prefill":{"x":"nodot"}}]})") == "w-steps[0].prefill.x");
}

TEST_CASE("WizardSession: construction refuses a missing submitter or a step schema that does not read",
          "[forms-engine][wizard]") {
    Harness harness;
    CHECK_THROWS_AS(WizardSession(harness.rt, wizardModel(), schemas(), {}, {}), std::invalid_argument);
    auto const broken = [](std::string_view) -> std::optional<std::string> { return "not a schema"; };
    CHECK_THROWS_AS(WizardSession(harness.rt, wizardModel(), broken, harness.server.submitter(), {}),
                    std::invalid_argument);
    CHECK_THROWS_AS(WizardSession(harness.rt, wizardModel(), {}, harness.server.submitter(), {}),
                    std::invalid_argument);
}

TEST_CASE(
    "WizardSession: Back on the first step stays; an unknown resolved value is absent; a prefill with nothing "
    "resolved leaves the field blank",
    "[forms-engine][wizard]") {
    Stack stack{false, [](Call const& call) -> std::optional<std::string> {
                    if (call.action == "WizStepOne") {
                        return std::string{"[1]"};  // not an object: nothing resolved
                    }
                    return std::string{"{}"};
                }};
    stack.wizard->back();
    CHECK(stack.wizard->current() == 0);
    CHECK_FALSE(stack.wizard->resolved("WizStepOne.id").has_value());
    stack.harness.type(stack.wizard->step(0), "label", "x");
    stack.wizard->next();
    stack.harness.settle();
    CHECK(stack.wizard->current() == 1);
    CHECK(stack.wizard->step(1).field("refId").text().get().empty());
}

TEST_CASE("WizardSession: a failed submission from Next stays on the step", "[forms-engine][wizard]") {
    Stack stack{true, [](Call const& call) -> std::optional<std::string> {
                    if (call.action == "WizStepOne") {
                        return std::string{R"({"id":1})"};
                    }
                    return std::nullopt;  // step two's submissions wait
                }};
    stack.harness.type(stack.wizard->step(0), "label", "x");
    stack.wizard->next();
    stack.harness.settle();
    REQUIRE(stack.wizard->current() == 1);
    stack.harness.type(stack.wizard->step(1), "note", "n");
    stack.wizard->back();  // leave the step while nothing is in flight
    stack.wizard->next();
    stack.harness.settle();
    REQUIRE(stack.wizard->current() == 1);
    stack.wizard->step(1).submit();
    stack.harness.settle();
    stack.harness.server.rejectSubmit(stack.harness.server.submits().size() - 1, "refused");
    stack.harness.settle();
    CHECK_FALSE(stack.wizard->stepDone(1));
    CHECK(stack.wizard->current() == 1);
}
