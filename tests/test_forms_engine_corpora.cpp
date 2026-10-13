// SPDX-License-Identifier: Apache-2.0
//
// Parity with the QML renderer, measured: the shared x-rules corpus and the
// body corpus, both driven through the forms engine.
//
// - tests/data/rule_corpus.json: every row's `ready`, `visible` and `readonly`
//   verdicts, and the engine's `ready` against `allRulesSatisfied` on the same
//   row.
// - tests/data/body_corpus.json: every case's body, which
//   src/qt/forms/tests/tst_BodyCorpus.qml wrote from a real DynamicForm, must
//   be what the engine sends, byte for byte. The few cases where the engine
//   deliberately sends something else are named below with the body it sends,
//   and each still pins the QML body, so a change on either side is reviewed.

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <functional>
#include <map>
#include <morph/forms/engine/field_model.hpp>
#include <morph/forms/engine/form_session.hpp>
#include <morph/render/locale_format.hpp>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "forms_engine_support.hpp"
#include "forms_rule_corpus.hpp"

namespace {

using morph::test::formsengine::Harness;
using morph::test::rulecorpus::Draft;
namespace engine = morph::forms::detail::engine;

[[nodiscard]] engine::Json readJsonFile(char const* path) {
    std::ifstream file{path};
    REQUIRE(file.is_open());
    std::ostringstream buffer;
    buffer << file.rdbuf();
    auto dom = engine::parseJson(buffer.str());
    REQUIRE(dom.has_value());
    return *std::move(dom);
}

[[nodiscard]] std::string textAt(engine::Json const& node, std::string_view key) {
    auto const* const text = engine::stringAt(node, key);
    REQUIRE(text != nullptr);
    return *text;
}

// ──── The rule corpus ────────────────────────────────────────────────────────

struct RuleRow {
    std::string id;
    std::string action;
    Draft state;
    bool ready = false;
    std::map<std::string, bool, std::less<>> visible;
    std::map<std::string, bool, std::less<>> readonly;
};

struct RuleCorpus {
    std::map<std::string, std::string, std::less<>> schemas;
    std::vector<RuleRow> rows;
};

[[nodiscard]] RuleCorpus const& ruleCorpus() {
    static RuleCorpus const parsed = [] {
        engine::Json const dom = readJsonFile(MORPH_FORMS_RULE_CORPUS);
        RuleCorpus corpus{};
        for (auto const& [name, schema] : engine::member(dom, "schemas")->get_object()) {
            corpus.schemas.emplace(name, schema.get<std::string>());
        }
        for (auto const& entry : engine::member(dom, "cases")->get_array()) {
            RuleRow row{};
            row.id = textAt(entry, "id");
            row.action = textAt(entry, "action");
            row.ready = engine::member(entry, "ready")->get<bool>();
            for (auto const& [field, text] : engine::member(entry, "state")->get_object()) {
                row.state.emplace(field, text.get<std::string>());
            }
            for (std::string_view const key : {"visible", "readonly"}) {
                if (auto const* const flags = engine::member(entry, key)) {
                    for (auto const& [field, flag] : flags->get_object()) {
                        (key == "visible" ? row.visible : row.readonly).emplace(field, flag.get<bool>());
                    }
                }
            }
            corpus.rows.push_back(std::move(row));
        }
        return corpus;
    }();
    return parsed;
}

struct RuleVerdict {
    bool ready = false;
    std::map<std::string, bool, std::less<>> visible;
    std::map<std::string, bool, std::less<>> readonly;
};

[[nodiscard]] RuleVerdict runRuleRow(RuleRow const& row) {
    Harness harness;
    auto const form = harness.session("Test_" + row.action, ruleCorpus().schemas.at(row.action));
    for (auto const& [name, text] : row.state) {
        form->field(name).text().set(text);
    }
    harness.settle();
    RuleVerdict verdict;
    verdict.ready = form->ready();
    for (auto const& [name, ignored] : row.visible) {
        verdict.visible.emplace(name, form->field(name).visible().get());
    }
    for (auto const& [name, ignored] : row.readonly) {
        verdict.readonly.emplace(name, form->field(name).readonly().get());
    }
    return verdict;
}

// ──── The body corpus ────────────────────────────────────────────────────────

struct BodyStep {
    std::string field;
    std::string text;
    std::optional<std::size_t> unit;
};

struct BodyCase {
    std::string id;
    std::string schema;
    std::string locale;
    int offset = 0;
    std::optional<std::string> prefill;
    std::vector<BodyStep> steps;
    std::optional<std::string> body;
};

[[nodiscard]] std::vector<BodyCase> const& bodyCorpus() {
    static std::vector<BodyCase> const parsed = [] {
        engine::Json const dom = readJsonFile(MORPH_FORMS_BODY_CORPUS);
        std::vector<BodyCase> cases;
        for (auto const& entry : engine::member(dom, "cases")->get_array()) {
            BodyCase row{};
            row.id = textAt(entry, "id");
            row.schema = textAt(entry, "schema");
            row.locale = textAt(entry, "locale");
            row.offset = static_cast<int>(engine::member(entry, "offset")->as<std::int64_t>());
            if (auto const* const prefill = engine::stringAt(entry, "prefill")) {
                row.prefill = *prefill;
            }
            for (auto const& step : engine::member(entry, "steps")->get_array()) {
                BodyStep parsedStep{.field = textAt(step, "field"), .text = textAt(step, "text"), .unit = {}};
                if (auto const* const unit = engine::member(step, "unit")) {
                    parsedStep.unit = static_cast<std::size_t>(unit->as<std::int64_t>());
                }
                row.steps.push_back(std::move(parsedStep));
            }
            engine::Json const* const body = engine::member(entry, "body");
            REQUIRE(body != nullptr);
            if (auto const* const text = body->get_if<std::string>()) {
                row.body = *text;
            }
            cases.push_back(std::move(row));
        }
        return cases;
    }();
    return parsed;
}

/// The locales the body corpus names, as the QML renderer reads them from Qt.
[[nodiscard]] morph::render::NumericLocale numericLocale(std::string_view name) {
    if (name == "de_DE") {
        return morph::render::NumericLocale{.decimalSeparator = ",", .groupSeparator = "."};
    }
    REQUIRE(name == "C");
    return morph::render::NumericLocale{};
}

[[nodiscard]] std::optional<std::string> engineBody(BodyCase const& row) {
    Harness harness;
    morph::forms::FormSessionOptions options{};
    options.locale = numericLocale(row.locale);
    options.displayOffsetMinutes = row.offset;
    auto const form = harness.session("BodyCorpus", row.schema, std::move(options));
    if (row.prefill.has_value()) {
        form->prefill(*row.prefill);
        harness.settle();
    }
    for (BodyStep const& step : row.steps) {
        if (step.unit.has_value()) {
            form->field(step.field).unit().set(*step.unit);
        }
        harness.type(*form, step.field, step.text);
    }
    return form->body();
}

/// Where the engine deliberately sends a different body than the QML renderer, and why.
struct Difference {
    std::string_view id;
    std::optional<std::string_view> qml;
    std::optional<std::string_view> engineSends;
};

constexpr std::array<Difference, 1> kDifferences{{
    // Nested objects and collections are fields of the engine; the QML renderer leaves them unrepresentable unless
    // a slot claims them, so its form never becomes ready.
    {.id = "prefill/nested",
     .qml = std::nullopt,
     .engineSends = R"({"name":"n","address":{"street":"Main","zip":12345},)"
                    R"("lines":[{"sku":"A","qty":2,"price":{"num":150,"den":100,"dp":2}},{"sku":"B","qty":1}]})"},
}};

[[nodiscard]] Difference const* differenceFor(std::string_view id) {
    for (Difference const& entry : kDifferences) {
        if (entry.id == id) {
            return &entry;
        }
    }
    return nullptr;
}

[[nodiscard]] std::optional<std::string> owned(std::optional<std::string_view> text) {
    return text.has_value() ? std::optional<std::string>{std::string{*text}} : std::nullopt;
}

}  // namespace

TEST_CASE("rule corpus: the engine reads every corpus schema", "[forms-engine][rules][corpus]") {
    REQUIRE_FALSE(ruleCorpus().schemas.empty());
    for (auto const& [name, schema] : ruleCorpus().schemas) {
        INFO(name);
        CHECK(morph::forms::FormModel::fromSchema(name, schema).has_value());
    }
}

TEST_CASE("rule corpus: every row's engine verdicts are the ones the corpus records",
          "[forms-engine][rules][corpus]") {
    REQUIRE(ruleCorpus().rows.size() == 46);
    std::size_t presentation = 0;
    for (auto const& row : ruleCorpus().rows) {
        RuleVerdict const verdict = runRuleRow(row);
        INFO("row " << row.id);
        CHECK(verdict.ready == row.ready);
        CHECK(verdict.visible == row.visible);
        CHECK(verdict.readonly == row.readonly);
        presentation += row.visible.size() + row.readonly.size();
    }
    CHECK(presentation == 4);  // every visibleWhen and readonlyWhen expectation was asserted
}

TEST_CASE("rule corpus: the engine and allRulesSatisfied agree on every row", "[forms-engine][rules][corpus]") {
    for (auto const& row : ruleCorpus().rows) {
        auto const* const typed = morph::test::rulecorpus::findAction(row.action);
        REQUIRE(typed != nullptr);
        INFO("row " << row.id);
        CHECK(runRuleRow(row).ready == typed->satisfied(row.state));
    }
}

TEST_CASE("body corpus: the engine sends the QML renderer's bodies, byte for byte", "[forms-engine][corpus]") {
    REQUIRE(bodyCorpus().size() >= 50);
    std::size_t identical = 0;
    std::size_t ready = 0;
    for (BodyCase const& row : bodyCorpus()) {
        INFO("case " << row.id);
        std::optional<std::string> const sent = engineBody(row);
        ready += sent.has_value() ? 1U : 0U;
        if (Difference const* const difference = differenceFor(row.id)) {
            CHECK(row.body == owned(difference->qml));
            CHECK(sent == owned(difference->engineSends));
            continue;
        }
        CHECK(sent == row.body);
        identical += sent == row.body ? 1U : 0U;
    }
    CHECK(identical + kDifferences.size() == bodyCorpus().size());
    // The corpus carries both verdicts: a form that never became ready would pass the not-ready rows alone.
    CHECK(ready > 0);
    CHECK(ready < bodyCorpus().size());
}

TEST_CASE("body corpus: every named difference is a case of the corpus", "[forms-engine][corpus]") {
    for (Difference const& entry : kDifferences) {
        INFO(entry.id);
        bool found = false;
        for (BodyCase const& row : bodyCorpus()) {
            found = found || row.id == entry.id;
        }
        CHECK(found);
    }
}
