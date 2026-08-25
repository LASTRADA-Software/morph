// SPDX-License-Identifier: Apache-2.0
//
// The compiled half of the shared `x-rules` corpus (morph#176).
//
// `x-rules` is evaluated twice — here by `morph::forms::allRulesSatisfied`,
// and again in JavaScript by `src/qt/forms/qml/DynamicForm.qml`. Before this
// file the only thing tying the two together was a hand-mirrored pair of test
// files covering three of the sixteen rule kinds: adding a kind to one side
// left the other silently untested, and two kinds (`atLeastOneOf`,
// `mutuallyExclusive`) could be disabled client-side with the whole renderer
// suite still green.
//
// The pin is `src/qt/forms/tests/data/rule_corpus.json`: **one file, two
// readers**. This file drives every row through the compiled evaluator;
// `src/qt/forms/tests/tst_DynamicFormRuleCorpus.qml` drives the same rows
// through a real `DynamicForm`. Neither owns the cases, so the two evaluators
// cannot be pinned to different case lists.
//
// Three assertions here are what make the corpus non-drifting rather than
// merely shared:
//
//   1. every schema in the corpus is the current `schemaJson<A>()`
//      byte-for-byte, so an emitter change cannot leave the renderer being fed
//      a schema no compiled action produces any more;
//   2. every named `detail::RuleKind` appears somewhere in the corpus, so a
//      seventeenth kind cannot be added to the vocabulary without rows;
//   3. every kind carries both verdicts, so a corpus that only ever says
//      "allow" — which would pass against a client that never blocks — is a
//      failure rather than a silent hole.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <fstream>
#include <glaze/glaze.hpp>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "forms_rule_corpus.hpp"

namespace {

using morph::test::rulecorpus::corpusActions;
using morph::test::rulecorpus::Draft;
using morph::test::rulecorpus::findAction;

/// @brief One row of the corpus, as this side needs it.
struct CorpusCase {
    std::string id;
    std::string action;
    Draft state;
    bool ready = false;
    /// @brief Presentation expectations. Only the renderer can assert these
    /// (the compiled side has no visibility API — a `VisibleWhen` node's
    /// `test()` returns `true` unconditionally, by construction), but the
    /// corpus's shape guard below still checks that both are represented.
    std::map<std::string, bool, std::less<>> visible;
    std::map<std::string, bool, std::less<>> readonly;
};

/// @brief The whole corpus, parsed once.
struct Corpus {
    std::map<std::string, std::string, std::less<>> schemas;
    std::vector<CorpusCase> cases;
};

/// @brief Reads and parses the corpus file. Fails loudly rather than
///        returning an empty corpus: zero rows would pass every loop below.
[[nodiscard]] const Corpus& corpus() {
    static const Corpus parsed = [] {
        std::ifstream file{MORPH_FORMS_RULE_CORPUS};
        REQUIRE(file.is_open());
        std::ostringstream buffer;
        buffer << file.rdbuf();
        const std::string text = buffer.str();

        glz::generic_u64 dom{};
        REQUIRE_FALSE(glz::read_json(dom, text));
        // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) — glaze DOM
        REQUIRE(dom.contains("schemas"));
        REQUIRE(dom.contains("cases"));

        Corpus result{};
        for (const auto& [name, schema] : dom["schemas"].get<glz::generic_u64::object_t>()) {
            result.schemas.emplace(name, schema.as<std::string>());
        }
        for (const auto& entry : dom["cases"].get<glz::generic_u64::array_t>()) {
            CorpusCase row{};
            row.id = entry["id"].as<std::string>();
            row.action = entry["action"].as<std::string>();
            row.ready = entry["ready"].as<bool>();
            for (const auto& [field, value] : entry["state"].get<glz::generic_u64::object_t>()) {
                row.state.emplace(field, value.as<std::string>());
            }
            if (entry.contains("visible")) {
                for (const auto& [field, value] : entry["visible"].get<glz::generic_u64::object_t>()) {
                    row.visible.emplace(field, value.as<bool>());
                }
            }
            if (entry.contains("readonly")) {
                for (const auto& [field, value] : entry["readonly"].get<glz::generic_u64::object_t>()) {
                    row.readonly.emplace(field, value.as<bool>());
                }
            }
            result.cases.push_back(std::move(row));
        }
        // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        REQUIRE_FALSE(result.schemas.empty());
        REQUIRE_FALSE(result.cases.empty());
        return result;
    }();
    return parsed;
}

/// @brief Collects every `"kind"` string reachable from a rule/condition node,
///        recursing through `when` / `conditions` / `condition`.
void collectKinds(const glz::generic_u64& node, std::set<std::string>& out) {  // NOLINT(misc-no-recursion)
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) — glaze DOM
    if (!node.is_object()) {
        return;
    }
    if (node.contains("kind")) {
        out.insert(node["kind"].as<std::string>());
    }
    if (node.contains("when")) {
        collectKinds(node["when"], out);
    }
    if (node.contains("condition")) {
        collectKinds(node["condition"], out);
    }
    if (node.contains("conditions")) {
        for (const auto& child : node["conditions"].get<glz::generic_u64::array_t>()) {
            collectKinds(child, out);
        }
    }
    // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
}

/// @brief Every rule/condition kind named in one schema's `x-rules`.
[[nodiscard]] std::set<std::string> kindsIn(const std::string& schema) {
    std::set<std::string> kinds{};
    glz::generic_u64 dom{};
    if (glz::read_json(dom, schema)) {
        return kinds;
    }
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access) — glaze DOM
    if (!dom.contains("x-rules")) {
        return kinds;
    }
    for (const auto& rule : dom["x-rules"].get<glz::generic_u64::array_t>()) {
        collectKinds(rule, kinds);
    }
    // NOLINTEND(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
    return kinds;
}

/// @brief Every wire name `detail::ruleKindName` gives a non-empty spelling.
///
/// Walked by ordinal rather than listed by hand, so a seventeenth enumerator
/// joins this set the moment it is named — which is what makes the coverage
/// assertion below fail for a kind nobody wrote corpus rows for.
[[nodiscard]] const std::set<std::string>& everyRuleKindName() {
    static const std::set<std::string> names = [] {
        std::set<std::string> collected{};
        constexpr std::uint8_t probeLimit = 64;
        for (std::uint8_t ordinal = 0; ordinal < probeLimit; ++ordinal) {
            const auto name = morph::forms::detail::ruleKindName(static_cast<morph::forms::detail::RuleKind>(ordinal));
            if (!name.empty()) {
                collected.emplace(name);
            }
        }
        return collected;
    }();
    return names;
}

/// @brief The two kinds that never gate submission, so `ready` cannot carry
///        their verdicts.
[[nodiscard]] bool isPresentationKind(std::string_view kind) {
    return kind == "visibleWhen" || kind == "readonlyWhen";
}

}  // namespace

TEST_CASE("every corpus schema is the current schemaJson output, byte for byte", "[forms][rules][corpus]") {
    // Without this the corpus could go on feeding the renderer a schema no
    // compiled action produces any more, and the "two evaluators agree" claim
    // would be about a fossil.
    for (const auto& action : corpusActions()) {
        const auto found = corpus().schemas.find(action.name);
        INFO("corpus is missing a schema for " << action.name);
        REQUIRE(found != corpus().schemas.end());
        INFO("stale corpus schema for " << action.name << "\nregenerate it from:\n" << action.schema());
        CHECK(found->second == action.schema());
    }
}

TEST_CASE("the corpus names no action this build does not have", "[forms][rules][corpus]") {
    for (const auto& [name, schema] : corpus().schemas) {
        INFO("corpus schema " << name << " has no fixture in tests/forms_rule_corpus.hpp");
        CHECK(findAction(name) != nullptr);
    }
}

TEST_CASE("every corpus row's compiled verdict is the one the corpus records", "[forms][rules][corpus]") {
    for (const auto& row : corpus().cases) {
        const auto* action = findAction(row.action);
        INFO("row " << row.id << " names an unknown action " << row.action);
        REQUIRE(action != nullptr);
        INFO("row " << row.id << ": allRulesSatisfied disagrees with the corpus");
        CHECK(action->satisfied(row.state) == row.ready);
    }
}

TEST_CASE("the corpus covers every rule kind the vocabulary names", "[forms][rules][corpus]") {
    std::set<std::string> covered{};
    for (const auto& [name, schema] : corpus().schemas) {
        for (auto& kind : kindsIn(schema)) {
            covered.insert(kind);
        }
    }
    for (const auto& kind : everyRuleKindName()) {
        INFO("rule kind '" << kind << "' appears in no corpus schema — add fixtures and rows for it");
        CHECK(covered.contains(kind));
    }
    // The reverse direction: a corpus schema carrying a kind the vocabulary
    // does not name would mean the emitter and `ruleKindName` had drifted.
    for (const auto& kind : covered) {
        INFO("corpus carries kind '" << kind << "' which detail::ruleKindName does not name");
        CHECK(everyRuleKindName().contains(kind));
    }
}

TEST_CASE("every rule kind carries both verdicts", "[forms][rules][corpus]") {
    // A corpus whose every row said "allow" would pass against a client that
    // never blocks anything, and one whose every row said "block" would pass
    // against a client that blocks everything. Per kind, not just overall:
    // the whole-corpus version is satisfied by a single well-chosen row and
    // says nothing about the other fifteen kinds.
    std::map<std::string, std::pair<int, int>, std::less<>> verdicts{};      // kind -> (allowed, blocked)
    std::map<std::string, std::pair<int, int>, std::less<>> presentation{};  // kind -> (shown/frozen, hidden/free)

    for (const auto& row : corpus().cases) {
        const auto schema = corpus().schemas.find(row.action);
        REQUIRE(schema != corpus().schemas.end());
        for (const auto& kind : kindsIn(schema->second)) {
            if (isPresentationKind(kind)) {
                for (const auto& [field, shown] : row.visible) {
                    (shown ? presentation[kind].first : presentation[kind].second)++;
                }
                for (const auto& [field, frozen] : row.readonly) {
                    (frozen ? presentation[kind].first : presentation[kind].second)++;
                }
                continue;
            }
            (row.ready ? verdicts[kind].first : verdicts[kind].second)++;
        }
    }

    for (const auto& kind : everyRuleKindName()) {
        if (isPresentationKind(kind)) {
            INFO("presentation kind '" << kind << "' needs a row where it acts and one where it does not");
            CHECK(presentation[kind].first > 0);
            CHECK(presentation[kind].second > 0);
            continue;
        }
        INFO("rule kind '" << kind << "' has no corpus row expecting a submittable form");
        CHECK(verdicts[kind].first > 0);
        INFO("rule kind '" << kind << "' has no corpus row expecting a blocked form");
        CHECK(verdicts[kind].second > 0);
    }
}
