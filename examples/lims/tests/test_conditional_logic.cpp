// SPDX-License-Identifier: Apache-2.0
//
// Conditional form logic parity (README build order §5, review D8).
//
// The claim under test is the forms spec's headline one: "One declaration
// drives three consumers … the client and the server evaluate identically
// from the same serialized form." A client that renders from `x-rules` and a
// server that runs the compiled `A::formRules` must reach the *same* verdict
// for the same field values.
//
// Nothing shipped lets a C++ client check that. `morph::forms` gives the
// server `allRulesSatisfied<A>(action)` over compiled nodes; the reference
// renderer's evaluator is JavaScript inside `src/qt/forms/qml/DynamicForm.qml`
// (and is properly tested, by `tst_DynamicFormRules.qml`). A non-QML client —
// which is what this rung's field devices are — has to write its own, and
// nothing cross-checks the two implementations against each other.
//
// So this file writes the third one: a small, generic, data-driven evaluator
// over the served `x-rules` JSON, and then asserts it agrees with the
// compiled evaluation across an exhaustive matrix of field states. That is
// the parity suite D8 asks for, and the evaluator is deliberately generic —
// it never mentions `dilution` or `value` — so it is testing the *vocabulary*,
// not this one action.

#include <catch2/catch_test_macros.hpp>
#include <glaze/glaze.hpp>
#include <morph/forms/forms.hpp>
#include <morph/util/rational.hpp>
#include <string>
#include <vector>

#include "lims/core/errors.hpp"
#include "lims/models/analysis_catalog_model.hpp"
#include "lims/models/sample_model.hpp"
#include "lims_test_support.hpp"
#include "testkit/db_fixture.hpp"

using lims::test::ScopedPrincipal;
using morph::ladder::testkit::DbFixture;
using morph::math::DecimalPlaces;
using morph::math::Denominator;
using morph::math::Numerator;
using morph::math::Rational;

namespace {

/// @brief An exact rational at @p places decimal places.
/// @param num Numerator.
/// @param den Denominator.
/// @param places Decimal-precision tag.
/// @return The canonical rational.
[[nodiscard]] Rational exact(std::int64_t num, std::int64_t den, std::uint32_t places) {
    return Rational{Numerator{num}, Denominator{den}, DecimalPlaces{places}};
}

// ── A client-side `x-rules` evaluator ──────────────────────────────────────
//
// What a non-QML renderer has to implement. Generic over the vocabulary, not
// over this rung: it knows `engaged`/`notEngaged`/`equals`/`and`/`or`/`not`
// and the membership and comparison kinds, and it knows nothing else.

/// @brief One field's current draft state, as a renderer holds it.
struct DraftField {
    /// @brief Whether the user has put anything in the field.
    bool engaged = false;

    /// @brief The field's value as a string, when it has one. Only `equals`
    ///        against a string literal reads it, which is all this rung's
    ///        served rules use.
    std::string text;
};

/// @brief The whole draft: wire field name → state.
using Draft = std::map<std::string, DraftField>;

/// @brief Whether @p name is engaged in @p draft. An unknown field is treated
///        as unengaged, exactly as a renderer holding no widget for it would.
/// @param draft The current draft.
/// @param name The wire field name.
/// @return `true` when the field carries a value.
[[nodiscard]] bool engagedIn(const Draft& draft, const std::string& name) {
    const auto found = draft.find(name);
    return found != draft.end() && found->second.engaged;
}

/// @brief Evaluates one condition node from a served `x-rules` entry.
///
/// **Never claims an unrecognised `kind` holds.** forms.md ("Renderer
/// fallback" → "'Cannot evaluate' means defer, not block") settles the
/// contract: an unknown kind is a third answer, and the rule it belongs to is
/// handed to the server rather than blocked on. `clientWouldSubmit` below is
/// where the deferring happens; this function only refuses to assert.
///
/// **A known narrowing.** The contract says "cannot evaluate" *propagates*
/// through `and`/`or`/`not`; this evaluator is two-valued, so `notOf(unknown)`
/// would come out `true` here rather than unevaluable. No schema this rung
/// serves nests an unknown kind under a `not`, so nothing in this file
/// reaches it — the shipped renderer (`DynamicForm.qml`) implements the
/// three-valued form, and a client promoted out of this file would have to.
/// @param node The condition node.
/// @param draft The current draft.
/// @return `true` when the condition holds; `false` when it does not, or when
///         this client cannot evaluate it.
[[nodiscard]] bool evaluateCondition(const glz::generic_u64& node, const Draft& draft) {  // NOLINT(misc-no-recursion)
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) — glaze DOM
    if (!node.contains("kind")) {
        return false;
    }
    const auto kind = node["kind"].as<std::string>();

    const auto namedFields = [&node]() -> std::vector<std::string> {
        std::vector<std::string> names;
        if (node.contains("fields")) {
            for (const auto& field : node["fields"].get<glz::generic_u64::array_t>()) {
                names.push_back(field.as<std::string>());
            }
        }
        return names;
    };

    if (kind == "engaged" || kind == "notEngaged") {
        const auto names = namedFields();
        if (names.empty()) {
            return false;
        }
        const bool isEngaged = engagedIn(draft, names.front());
        return kind == "engaged" ? isEngaged : !isEngaged;
    }
    if (kind == "equals") {
        const auto names = namedFields();
        if (names.empty() || !node.contains("value")) {
            return false;
        }
        // Not vacuous: an unengaged field cannot equal anything.
        if (!engagedIn(draft, names.front())) {
            return false;
        }
        return draft.at(names.front()).text == node["value"].as<std::string>();
    }
    if (kind == "and" || kind == "or") {
        if (!node.contains("conditions")) {
            return false;
        }
        const auto& children = node["conditions"].get<glz::generic_u64::array_t>();
        bool all = true;
        bool any = false;
        for (const auto& child : children) {
            const bool held = evaluateCondition(child, draft);
            all = all && held;
            any = any || held;
        }
        return kind == "and" ? all : any;
    }
    if (kind == "not") {
        return node.contains("condition") && !evaluateCondition(node["condition"], draft);
    }
    // Unrecognised: cannot evaluate, so do not claim it holds.
    return false;
    // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
}

/// @brief What a client concludes about one served rule.
enum class ClientVerdict : std::uint8_t {
    Satisfied,       ///< The rule holds for this draft.
    Violated,        ///< The rule does not hold; submission should be blocked.
    Presentation,    ///< A `visibleWhen`/`readonlyWhen` rule, which never gates.
    CannotEvaluate,  ///< An unrecognised rule kind; defer to the server.
};

/// @brief Evaluates one top-level served rule against @p draft.
/// @param rule The rule node from `x-rules`.
/// @param draft The current draft.
/// @return The client's verdict.
[[nodiscard]] ClientVerdict evaluateRule(const glz::generic_u64& rule, const Draft& draft) {
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) — glaze DOM
    if (!rule.contains("kind")) {
        return ClientVerdict::CannotEvaluate;
    }
    const auto kind = rule["kind"].as<std::string>();

    if (kind == "visibleWhen" || kind == "readonlyWhen") {
        return ClientVerdict::Presentation;
    }

    std::vector<std::string> names;
    if (rule.contains("fields")) {
        for (const auto& field : rule["fields"].get<glz::generic_u64::array_t>()) {
            names.push_back(field.as<std::string>());
        }
    }

    if (kind == "requiredWhen") {
        if (names.empty() || !rule.contains("when")) {
            return ClientVerdict::CannotEvaluate;
        }
        if (!evaluateCondition(rule["when"], draft)) {
            return ClientVerdict::Satisfied;  // vacuous while the condition does not hold
        }
        return engagedIn(draft, names.front()) ? ClientVerdict::Satisfied : ClientVerdict::Violated;
    }
    if (kind == "exactlyOneOf" || kind == "atLeastOneOf" || kind == "mutuallyExclusive") {
        std::size_t count = 0;
        for (const auto& name : names) {
            count += engagedIn(draft, name) ? 1U : 0U;
        }
        if (kind == "exactlyOneOf") {
            return count == 1 ? ClientVerdict::Satisfied : ClientVerdict::Violated;
        }
        if (kind == "atLeastOneOf") {
            return count >= 1 ? ClientVerdict::Satisfied : ClientVerdict::Violated;
        }
        return count <= 1 ? ClientVerdict::Satisfied : ClientVerdict::Violated;
    }
    if (kind == "and" || kind == "or" || kind == "not") {
        return evaluateCondition(rule, draft) ? ClientVerdict::Satisfied : ClientVerdict::Violated;
    }
    return ClientVerdict::CannotEvaluate;
    // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
}

/// @brief Whether a client rendering @p schema would let @p draft be submitted.
///
/// An unevaluable rule does **not** block: the contract is that the client
/// defers such a rule to the server, which is the only party that can decide
/// it. Blocking instead would make an old client unable to submit anything at
/// all against a newer server.
/// @param schema The served schema text.
/// @param draft The current draft.
/// @return `true` when no rule this client understands is violated.
[[nodiscard]] bool clientWouldSubmit(const std::string& schema, const Draft& draft) {
    glz::generic_u64 dom{};
    if (glz::read_json(dom, schema)) {
        return false;
    }
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) — glaze DOM
    if (!dom.contains("x-rules")) {
        return true;
    }
    for (const auto& rule : dom["x-rules"].get<glz::generic_u64::array_t>()) {
        if (evaluateRule(rule, draft) == ClientVerdict::Violated) {
            return false;
        }
    }
    // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    return true;
}

/// @brief Whether a renderer would show @p field for @p draft.
/// @param schema The served schema text.
/// @param field The wire field name.
/// @param draft The current draft.
/// @return `false` only when a `visibleWhen` names the field and its condition
///         does not hold.
[[nodiscard]] bool clientWouldShow(const std::string& schema, const std::string& field, const Draft& draft) {
    glz::generic_u64 dom{};
    if (glz::read_json(dom, schema)) {
        return true;
    }
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) — glaze DOM
    if (!dom.contains("x-rules")) {
        return true;
    }
    for (const auto& rule : dom["x-rules"].get<glz::generic_u64::array_t>()) {
        if (!rule.contains("kind") || rule["kind"].as<std::string>() != "visibleWhen") {
            continue;
        }
        const auto& fields = rule["fields"].get<glz::generic_u64::array_t>();
        if (!fields.empty() && fields.front().as<std::string>() == field) {
            return evaluateCondition(rule["when"], draft);
        }
    }
    // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    return true;
}

// ── The matrix ─────────────────────────────────────────────────────────────

/// @brief One point in the state space: what the operator has filled in.
struct Scenario {
    bool valueEngaged = false;
    bool qualifierEngaged = false;
    bool dilutionEngaged = false;
    std::string dilutionText;
    bool factorEngaged = false;
};

/// @brief Builds the compiled action for @p scenario.
/// @param scenario The point in the state space.
/// @return The action a client at that point would submit.
[[nodiscard]] lims::CaptureConcentration compiledFor(const Scenario& scenario) {
    lims::CaptureConcentration action;
    action.analysisVersionId = lims::AnalysisVersionId{1};
    if (scenario.valueEngaged) {
        action.value = lims::Concentration{exact(12, 5, 3)};
    }
    if (scenario.qualifierEngaged) {
        action.qualifier = lims::QualifierChoice{std::string{lims::kQualifierBelowLod}};
    }
    if (scenario.dilutionEngaged) {
        action.dilution = lims::DilutionChoice{scenario.dilutionText};
    }
    if (scenario.factorEngaged) {
        action.dilutionFactor = lims::DilutionFactor{exact(10, 1, 3)};
    }
    return action;
}

/// @brief Builds the served-schema draft for @p scenario.
/// @param scenario The point in the state space.
/// @return The draft a renderer at that point would hold.
[[nodiscard]] Draft draftFor(const Scenario& scenario) {
    return Draft{
        {"value", {.engaged = scenario.valueEngaged, .text = "2.4"}},
        {"qualifier", {.engaged = scenario.qualifierEngaged, .text = std::string{lims::kQualifierBelowLod}}},
        {"dilution", {.engaged = scenario.dilutionEngaged, .text = scenario.dilutionText}},
        {"dilutionFactor", {.engaged = scenario.factorEngaged, .text = "10"}},
    };
}

/// @brief Every point in the state space this rung's rules can distinguish.
/// @return 2 x 2 x 3 x 2 = 24 scenarios.
[[nodiscard]] std::vector<Scenario> everyScenario() {
    std::vector<Scenario> all;
    for (const bool value : {false, true}) {
        for (const bool qualifier : {false, true}) {
            for (const auto& dilution :
                 std::vector<std::pair<bool, std::string>>{{false, ""},
                                                           {true, std::string{lims::kDilutionNeat}},
                                                           {true, std::string{lims::kDilutionDiluted}}}) {
                for (const bool factor : {false, true}) {
                    all.push_back(Scenario{.valueEngaged = value,
                                           .qualifierEngaged = qualifier,
                                           .dilutionEngaged = dilution.first,
                                           .dilutionText = dilution.second,
                                           .factorEngaged = factor});
                }
            }
        }
    }
    return all;
}

/// @brief Renders @p scenario for a failure message.
/// @param scenario The point in the state space.
/// @return A readable description.
[[nodiscard]] std::string describe(const Scenario& scenario) {
    return std::string{"value="} + (scenario.valueEngaged ? "1" : "0") +
           " qualifier=" + (scenario.qualifierEngaged ? "1" : "0") +
           " dilution=" + (scenario.dilutionEngaged ? scenario.dilutionText : std::string{"<empty>"}) +
           " factor=" + (scenario.factorEngaged ? "1" : "0");
}

}  // namespace

TEST_CASE("Client and server agree on every point in the rule state space", "[lims][forms][parity]") {
    const auto schema = morph::forms::schemaJson<lims::CaptureConcentration>();
    REQUIRE_FALSE(schema.empty());

    const auto scenarios = everyScenario();
    REQUIRE(scenarios.size() == 24);

    // Both sides must disagree with each other nowhere, and the matrix must
    // actually contain both answers — a suite where every point is "allow"
    // would pass against a client that always says yes.
    std::size_t allowed = 0;
    for (const auto& scenario : scenarios) {
        const auto action = compiledFor(scenario);
        const bool server = morph::forms::allRulesSatisfied(action);
        const bool client = clientWouldSubmit(schema, draftFor(scenario));
        INFO(describe(scenario) << " -- server=" << server << " client=" << client);
        CHECK(server == client);
        allowed += server ? 1U : 0U;
    }
    CHECK(allowed > 0);
    CHECK(allowed < scenarios.size());
}

TEST_CASE("The conditional pair behaves as declared: required and visible together", "[lims][forms][parity]") {
    const auto schema = morph::forms::schemaJson<lims::CaptureConcentration>();

    const Scenario neat{.valueEngaged = true, .dilutionEngaged = true, .dilutionText = "neat"};
    const Scenario dilutedNoFactor{.valueEngaged = true, .dilutionEngaged = true, .dilutionText = "diluted"};
    const Scenario dilutedWithFactor{
        .valueEngaged = true, .dilutionEngaged = true, .dilutionText = "diluted", .factorEngaged = true};
    const Scenario unstated{.valueEngaged = true};

    // Hidden while neat or unstated; shown once diluted.
    CHECK_FALSE(clientWouldShow(schema, "dilutionFactor", draftFor(neat)));
    CHECK_FALSE(clientWouldShow(schema, "dilutionFactor", draftFor(unstated)));
    CHECK(clientWouldShow(schema, "dilutionFactor", draftFor(dilutedWithFactor)));

    // Required exactly when shown — because the action declares both rules,
    // not because either implies the other.
    CHECK(clientWouldSubmit(schema, draftFor(neat)));
    CHECK(clientWouldSubmit(schema, draftFor(unstated)));
    CHECK_FALSE(clientWouldSubmit(schema, draftFor(dilutedNoFactor)));
    CHECK(clientWouldSubmit(schema, draftFor(dilutedWithFactor)));

    // The server says the same thing about each.
    CHECK(morph::forms::allRulesSatisfied(compiledFor(neat)));
    CHECK(morph::forms::allRulesSatisfied(compiledFor(unstated)));
    CHECK_FALSE(morph::forms::allRulesSatisfied(compiledFor(dilutedNoFactor)));
    CHECK(morph::forms::allRulesSatisfied(compiledFor(dilutedWithFactor)));
}

TEST_CASE("`equals` is not vacuous on an unengaged field", "[lims][forms][parity]") {
    const auto schema = morph::forms::schemaJson<lims::CaptureConcentration>();

    // An unstated dilution is not "diluted", so the requiredWhen is vacuously
    // satisfied rather than failing. This is the documented asymmetry with the
    // comparison kinds, which *are* vacuous on an unengaged operand, and it is
    // the behaviour a renderer has to match exactly or the form will demand a
    // field the server does not want.
    const Draft unstated{{"dilution", {.engaged = false, .text = "diluted"}},
                         {"value", {.engaged = true, .text = "2.4"}},
                         {"dilutionFactor", {.engaged = false, .text = ""}}};
    CHECK(clientWouldSubmit(schema, unstated));
    CHECK_FALSE(clientWouldShow(schema, "dilutionFactor", unstated));
}

TEST_CASE("An unrecognised rule kind fails closed, and defers rather than blocks", "[lims][forms][parity][d8]") {
    auto schema = morph::forms::schemaJson<lims::CaptureConcentration>();

    // A rule kind from a future framework version, injected into the served
    // schema exactly as a newer server would send it.
    glz::generic_u64 dom{};
    REQUIRE_FALSE(glz::read_json(dom, schema));
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) — glaze DOM
    auto rules = dom["x-rules"].get<glz::generic_u64::array_t>();
    glz::generic_u64 future{};
    future["kind"] = std::string{"sumWithin"};
    glz::generic_u64::array_t futureFields{};
    futureFields.emplace_back(std::string{"value"});
    future["fields"] = futureFields;
    rules.emplace_back(future);
    dom["x-rules"] = rules;

    glz::generic_u64 futureCondition{};
    futureCondition["kind"] = std::string{"withinTolerance"};
    // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    schema = glz::write_json(dom).value_or(schema);

    const Scenario ok{.valueEngaged = true};
    const Draft draft = draftFor(ok);

    // Fail closed on *evaluation*: the client does not claim the unknown rule
    // holds...
    glz::generic_u64 reread{};
    REQUIRE_FALSE(glz::read_json(reread, schema));
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) — glaze DOM
    const auto& parsed = reread["x-rules"].get<glz::generic_u64::array_t>();
    CHECK(evaluateRule(parsed.back(), draft) == ClientVerdict::CannotEvaluate);
    CHECK(evaluateCondition(futureCondition, draft) == false);
    // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)

    // ...and defers on *enforcement*: it still submits, because only the
    // server can decide a rule the client does not understand. A client that
    // blocked instead would be unable to submit anything at all against a
    // newer server, which is a worse failure than one round trip.
    CHECK(clientWouldSubmit(schema, draft));

    // The rules it *does* understand keep working alongside the unknown one.
    const Scenario broken{.valueEngaged = true, .qualifierEngaged = true};
    CHECK_FALSE(clientWouldSubmit(schema, draftFor(broken)));
}

// ── Clear-on-hide, decided ─────────────────────────────────────────────────

TEST_CASE("A hidden field's draft value still travels, and the server acts on it", "[lims][forms][parity][d8]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    lims::AnalysisCatalogModel catalog;
    lims::SampleModel model;

    const auto nitrate =
        catalog.execute(lims::DefineAnalysis{.name = "Nitrate", .canonicalUnit = "mg_per_L", .decimalPlaces = 3});
    const auto client = model.execute(lims::RegisterClient{.name = "Waterworks Ltd"});
    model.execute(lims::RegisterSample{.clientId = client.clientId, .reference = "WW-1"});
    model.execute(lims::ReceiveSample{});
    model.execute(lims::StartWork{});

    // The operator selected `diluted`, typed a factor, then changed their mind
    // back to `neat`. `visibleWhen` hides the factor but never clears it —
    // that is the framework's documented behaviour, and this rung does not
    // fight it: it makes the server ignore a factor that no longer applies.
    const auto stored =
        model.execute(lims::CaptureConcentration{.analysisVersionId = nitrate.versionId,
                                                 .value = lims::Concentration{exact(12, 5, 3)},
                                                 .dilution = lims::DilutionChoice{std::string{lims::kDilutionNeat}},
                                                 .dilutionFactor = lims::DilutionFactor{exact(10, 1, 3)}});

    // 2.4 mg/L, not 24: the stale factor travelled and was ignored, because
    // the preparation says neat. Clearing on hide client-side would have made
    // the same outcome depend on the renderer doing it, which is not something
    // the server can verify.
    REQUIRE(stored.value.hasValue());
    CHECK((*stored.value).numerator == 12);
    CHECK((*stored.value).denominator == 5);
}

TEST_CASE("A declared dilution is applied exactly", "[lims][forms][dilution]") {
    DbFixture fixture;
    const ScopedPrincipal alice{"alice"};
    lims::AnalysisCatalogModel catalog;
    lims::SampleModel model;

    const auto nitrate =
        catalog.execute(lims::DefineAnalysis{.name = "Nitrate", .canonicalUnit = "mg_per_L", .decimalPlaces = 3});
    const auto client = model.execute(lims::RegisterClient{.name = "Waterworks Ltd"});
    model.execute(lims::RegisterSample{.clientId = client.clientId, .reference = "WW-1"});
    model.execute(lims::ReceiveSample{});
    model.execute(lims::StartWork{});

    // 2.4 mg/L measured on a 1:10 dilution reports as 24 mg/L, exactly.
    const auto stored =
        model.execute(lims::CaptureConcentration{.analysisVersionId = nitrate.versionId,
                                                 .value = lims::Concentration{exact(12, 5, 3)},
                                                 .dilution = lims::DilutionChoice{std::string{lims::kDilutionDiluted}},
                                                 .dilutionFactor = lims::DilutionFactor{exact(10, 1, 3)}});
    REQUIRE(stored.value.hasValue());
    CHECK((*stored.value).numerator == 24);
    CHECK((*stored.value).denominator == 1);

    // A factor of zero or less is not a dilution. `requiredWhen` can insist the
    // field is filled in; it cannot insist the number means anything, so the
    // model checks that itself.
    CHECK_THROWS_AS(
        model.execute(lims::CaptureConcentration{.analysisVersionId = nitrate.versionId,
                                                 .value = lims::Concentration{exact(12, 5, 3)},
                                                 .dilution = lims::DilutionChoice{std::string{lims::kDilutionDiluted}},
                                                 .dilutionFactor = lims::DilutionFactor{exact(0, 1, 3)}}),
        lims::ValidationError);

    // And an unknown preparation code is refused rather than assumed neat —
    // the same fail-closed rule the qualifier codes follow.
    CHECK_THROWS_AS(model.execute(lims::CaptureConcentration{.analysisVersionId = nitrate.versionId,
                                                             .value = lims::Concentration{exact(12, 5, 3)},
                                                             .dilution = lims::DilutionChoice{std::string{"halved"}}}),
                    lims::ValidationError);
}

TEST_CASE("The dilution picklist is served and matches the codes the model accepts", "[lims][forms][dilution]") {
    DbFixture fixture;
    lims::SampleModel model;

    const auto served = model.execute(lims::ListDilutionModes{});
    REQUIRE(served.modes.size() == 2);
    CHECK(served.modes[0].id == lims::kDilutionNeat);
    CHECK(served.modes[1].id == lims::kDilutionDiluted);

    const auto schema = morph::forms::schemaJson<lims::CaptureConcentration>();
    CHECK(schema.find(R"("x-optionsAction":"ListDilutionModes")") != std::string::npos);
}
