// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file forms_rule_corpus.hpp
/// @brief The compiled half of the shared `x-rules` corpus: one action per
///        rule kind, plus the generic draft→action driver that lets a corpus
///        row be evaluated by `morph::forms::allRulesSatisfied` without a
///        per-row C++ statement.
///
/// `x-rules` has two evaluators — `morph::forms::allRulesSatisfied` here, and
/// a JavaScript reimplementation in `src/qt/forms/qml/DynamicForm.qml` — and
/// nothing structural pinned them to each other (morph#176). A hand-mirrored
/// pair of test files does not fix that: adding a rule kind to one side leaves
/// the other silently untested, which is how `atLeastOneOf` and
/// `mutuallyExclusive` could be disabled client-side with the renderer suite
/// still green.
///
/// The pin is `src/qt/forms/tests/data/rule_corpus.json`: **one file, two
/// readers**. `tests/test_forms_rule_corpus.cpp` drives every row through the
/// compiled evaluator; `src/qt/forms/tests/tst_DynamicFormRuleCorpus.qml`
/// drives the same rows through a real `DynamicForm`. Neither owns the cases.
///
/// This header is also the corpus's generator: `corpusActions()` exposes each
/// fixture's `schemaJson<A>()`, and the C++ test asserts the checked-in file
/// carries that text byte-for-byte, so an emitter change cannot leave a stale
/// schema in the corpus.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <morph/forms/forms.hpp>
#include <morph/util/quantity.hpp>
#include <morph/util/rational.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// The fixtures below are file-scope, not anonymous-namespaced: glaze's
// reflection needs linkage.
// NOLINTBEGIN(misc-use-internal-linkage)
// NOLINTBEGIN(cert-err58-cpp,bugprone-throwing-static-initialization,misc-const-correctness)

/// @brief The corpus's miniature unit system — one unit, two decimals.
enum class RcUnit : std::uint8_t { money };

/// @brief Unit metadata for `RcUnit`.
template <>
struct morph::units::UnitTraits<RcUnit> {
    /// @brief Describes the corpus's single unit.
    /// @param unit The unit to describe (only one exists).
    /// @return Its ascii id, display text and default precision.
    static constexpr morph::units::UnitMeta meta(RcUnit unit) noexcept {
        switch (unit) {
            case RcUnit::money:
                return {.id = "money", .display = "$", .defaultDecimals = 2};
            default:
                return {.id = "?", .display = "?", .defaultDecimals = 2};
        }
    }
};

/// @brief The corpus's quantity type.
using RcMoney = morph::units::Quantity<RcUnit::money>;

// ---------------------------------------------------------------------------
// One fixture per rule kind. Every quantity member is listed in
// `optionalFields` so the static `required` array stays empty: the corpus is
// measuring `x-rules`, and a field that is required by the *schema* would gate
// submission for a reason that has nothing to do with the rule under test.
// ---------------------------------------------------------------------------

/// @brief `requiredWhen` + `engaged`.
struct RcEngaged {
    RcMoney amount;                   ///< The gating field.
    std::optional<std::string> note;  ///< Required once `amount` is engaged.

    /// @brief Both members are optional; `x-rules` alone gates.
    static constexpr std::array optionalFields{std::string_view{"amount"}};
    /// @brief The rule under test.
    static constexpr auto formRules = morph::forms::ruleList(
        morph::forms::requiredWhen(&RcEngaged::note, morph::forms::engaged(&RcEngaged::amount)));
};

/// @brief `requiredWhen` + `notEngaged`.
struct RcNotEngaged {
    RcMoney amount;                   ///< The gating field.
    std::optional<std::string> note;  ///< Required while `amount` is blank.

    /// @brief Both members are optional; `x-rules` alone gates.
    static constexpr std::array optionalFields{std::string_view{"amount"}};
    /// @brief The rule under test.
    static constexpr auto formRules = morph::forms::ruleList(
        morph::forms::requiredWhen(&RcNotEngaged::note, morph::forms::notEngaged(&RcNotEngaged::amount)));
};

/// @brief `equals` against a `bool` literal — divergence (a) of morph#176.
struct RcEqualsBool {
    std::optional<bool> flag;         ///< Compared against the literal `true`.
    std::optional<std::string> note;  ///< Required while `flag` is true.

    /// @brief The rule under test.
    static constexpr auto formRules = morph::forms::ruleList(
        morph::forms::requiredWhen(&RcEqualsBool::note, morph::forms::equals(&RcEqualsBool::flag, true)));
};

/// @brief `equals` against an `int64` literal beyond 2^53 — divergence (b).
struct RcEqualsBigInt {
    std::optional<std::int64_t> id;   ///< Compared against an inexact-as-double literal.
    std::optional<std::string> note;  ///< Required while `id` matches exactly.

    /// @brief The rule under test.
    static constexpr auto formRules = morph::forms::ruleList(morph::forms::requiredWhen(
        &RcEqualsBigInt::note, morph::forms::equals(&RcEqualsBigInt::id, std::int64_t{9007199254740993})));
};

/// @brief `equals` against a string literal.
struct RcEqualsString {
    std::optional<std::string> code;  ///< Compared against `"URGENT"`.
    std::optional<std::string> note;  ///< Required while `code` is `"URGENT"`.

    /// @brief The rule under test.
    static constexpr auto formRules = morph::forms::ruleList(
        morph::forms::requiredWhen(&RcEqualsString::note, morph::forms::equals(&RcEqualsString::code, "URGENT")));
};

/// @brief `greater`, as a top-level rule.
struct RcGreater {
    RcMoney low;   ///< Right-hand operand.
    RcMoney high;  ///< Left-hand operand.

    /// @brief Both members are optional; `x-rules` alone gates.
    static constexpr std::array optionalFields{std::string_view{"low"}, std::string_view{"high"}};
    /// @brief The rule under test.
    static constexpr auto formRules = morph::forms::ruleList(morph::forms::greater(&RcGreater::high, &RcGreater::low));
};

/// @brief `greaterOrEqual`, as a top-level rule.
struct RcGreaterOrEqual {
    RcMoney low;   ///< Right-hand operand.
    RcMoney high;  ///< Left-hand operand.

    /// @brief Both members are optional; `x-rules` alone gates.
    static constexpr std::array optionalFields{std::string_view{"low"}, std::string_view{"high"}};
    /// @brief The rule under test.
    static constexpr auto formRules =
        morph::forms::ruleList(morph::forms::greaterOrEqual(&RcGreaterOrEqual::high, &RcGreaterOrEqual::low));
};

/// @brief `less`, as a top-level rule.
struct RcLess {
    RcMoney low;   ///< Left-hand operand.
    RcMoney high;  ///< Right-hand operand.

    /// @brief Both members are optional; `x-rules` alone gates.
    static constexpr std::array optionalFields{std::string_view{"low"}, std::string_view{"high"}};
    /// @brief The rule under test.
    static constexpr auto formRules = morph::forms::ruleList(morph::forms::less(&RcLess::low, &RcLess::high));
};

/// @brief `lessOrEqual`, as a top-level rule.
struct RcLessOrEqual {
    RcMoney low;   ///< Left-hand operand.
    RcMoney high;  ///< Right-hand operand.

    /// @brief Both members are optional; `x-rules` alone gates.
    static constexpr std::array optionalFields{std::string_view{"low"}, std::string_view{"high"}};
    /// @brief The rule under test.
    static constexpr auto formRules =
        morph::forms::ruleList(morph::forms::lessOrEqual(&RcLessOrEqual::low, &RcLessOrEqual::high));
};

/// @brief `exactlyOneOf`.
struct RcExactlyOneOf {
    std::optional<std::string> email;  ///< First alternative.
    std::optional<std::string> phone;  ///< Second alternative.

    /// @brief The rule under test.
    static constexpr auto formRules =
        morph::forms::ruleList(morph::forms::exactlyOneOf(&RcExactlyOneOf::email, &RcExactlyOneOf::phone));
};

/// @brief `atLeastOneOf` — one of the two kinds no renderer test covered.
struct RcAtLeastOneOf {
    std::optional<std::string> email;  ///< First alternative.
    std::optional<std::string> phone;  ///< Second alternative.

    /// @brief The rule under test.
    static constexpr auto formRules =
        morph::forms::ruleList(morph::forms::atLeastOneOf(&RcAtLeastOneOf::email, &RcAtLeastOneOf::phone));
};

/// @brief `mutuallyExclusive` — the other kind no renderer test covered.
struct RcMutuallyExclusive {
    std::optional<std::string> email;  ///< First alternative.
    std::optional<std::string> phone;  ///< Second alternative.

    /// @brief The rule under test.
    static constexpr auto formRules = morph::forms::ruleList(
        morph::forms::mutuallyExclusive(&RcMutuallyExclusive::email, &RcMutuallyExclusive::phone));
};

/// @brief `visibleWhen` — a presentation kind, which never gates.
struct RcVisibleWhen {
    RcMoney amount;                   ///< The gating field.
    std::optional<std::string> note;  ///< Shown only while `amount` is engaged.

    /// @brief Both members are optional; `x-rules` alone gates.
    static constexpr std::array optionalFields{std::string_view{"amount"}};
    /// @brief The rule under test.
    static constexpr auto formRules = morph::forms::ruleList(
        morph::forms::visibleWhen(&RcVisibleWhen::note, morph::forms::engaged(&RcVisibleWhen::amount)));
};

/// @brief `readonlyWhen` — the other presentation kind.
struct RcReadonlyWhen {
    RcMoney amount;                   ///< The gating field.
    std::optional<std::string> note;  ///< Frozen once `amount` is engaged.

    /// @brief Both members are optional; `x-rules` alone gates.
    static constexpr std::array optionalFields{std::string_view{"amount"}};
    /// @brief The rule under test.
    static constexpr auto formRules = morph::forms::ruleList(
        morph::forms::readonlyWhen(&RcReadonlyWhen::note, morph::forms::engaged(&RcReadonlyWhen::amount)));
};

/// @brief `and`, used directly as a top-level rule.
struct RcAnd {
    std::optional<std::string> email;  ///< First conjunct.
    std::optional<std::string> phone;  ///< Second conjunct.

    /// @brief The rule under test.
    static constexpr auto formRules = morph::forms::ruleList(
        morph::forms::andOf(morph::forms::engaged(&RcAnd::email), morph::forms::engaged(&RcAnd::phone)));
};

/// @brief `or`, used directly as a top-level rule.
struct RcOr {
    std::optional<std::string> email;  ///< First disjunct.
    std::optional<std::string> phone;  ///< Second disjunct.

    /// @brief The rule under test.
    static constexpr auto formRules = morph::forms::ruleList(
        morph::forms::orOf(morph::forms::engaged(&RcOr::email), morph::forms::engaged(&RcOr::phone)));
};

/// @brief `not`, used directly as a top-level rule.
struct RcNot {
    std::optional<std::string> email;  ///< The negated field.

    /// @brief The rule under test.
    static constexpr auto formRules =
        morph::forms::ruleList(morph::forms::notOf(morph::forms::engaged(&RcNot::email)));
};

// NOLINTEND(cert-err58-cpp,bugprone-throwing-static-initialization,misc-const-correctness)
// NOLINTEND(misc-use-internal-linkage)

namespace morph::test::rulecorpus {

/// @brief One corpus row's field state: wire field name → the text a user
///        would have typed. An absent or empty entry is an unengaged field.
using Draft = std::map<std::string, std::string, std::less<>>;

/// @brief Writes @p draft into @p action, member by member.
///
/// Generic over the action type via glaze reflection, so a corpus row costs no
/// C++ at all — which is the point: a per-row `action.foo = ...` statement
/// would be a second place the case list lives, and the two would drift.
///
/// The text spellings are the renderer's, not C++'s: a quantity is integral
/// decimal text, a boolean is `"true"`/`"false"` (what `DynamicForm`'s
/// CheckBox stores), an integer is digits. A member type outside that set is
/// left default-constructed rather than guessed at.
/// @tparam A     Action type (a reflectable aggregate).
/// @param action The action to populate.
/// @param draft  The field state.
template <typename A>
void applyDraft(A& action, const Draft& draft) {
    forms::detail::forEachNamedMember(action, [&]<std::size_t I>(std::string_view name, auto& member) {
        static_cast<void>(I);
        const auto found = draft.find(name);
        if (found == draft.end() || found->second.empty()) {
            return;  // unengaged
        }
        const std::string& text = found->second;
        using Member = std::remove_reference_t<decltype(member)>;
        if constexpr (units::isQuantity<Member>) {
            member = Member{math::Rational{std::stoll(text), math::DecimalPlaces{Member::declaredDecimals}}};
        } else if constexpr (std::is_same_v<Member, std::optional<bool>>) {
            member = (text == "true");
        } else if constexpr (std::is_same_v<Member, std::optional<std::int64_t>>) {
            member = std::stoll(text);
        } else if constexpr (std::is_same_v<Member, std::optional<std::string>>) {
            member = text;
        }
    });
}

/// @brief One fixture, reachable by the name a corpus row spells.
struct CorpusAction {
    /// @brief The `"action"` key a corpus row carries.
    std::string name;
    /// @brief The action's compiled schema — what the corpus row must carry
    ///        verbatim for the renderer to be fed the real thing.
    std::string (*schema)();
    /// @brief The compiled verdict for one draft: `allRulesSatisfied`.
    bool (*satisfied)(const Draft&);
};

/// @brief Builds the `CorpusAction` entry for one fixture type.
/// @tparam A Action type.
/// @param name The name corpus rows use for it.
/// @return The entry.
template <typename A>
[[nodiscard]] inline CorpusAction entryFor(std::string name) {
    return {.name = std::move(name),
            .schema = +[] { return morph::forms::schemaJson<A>(); },
            .satisfied =
                +[](const Draft& draft) {
                    A action{};
                    applyDraft(action, draft);
                    return morph::forms::allRulesSatisfied(action);
                }};
}

/// @brief Every corpus fixture, in the order the corpus file lists them.
/// @return The fixture table.
[[nodiscard]] inline const std::vector<CorpusAction>& corpusActions() {
    static const std::vector<CorpusAction> actions = {
        entryFor<RcEngaged>("RcEngaged"),
        entryFor<RcNotEngaged>("RcNotEngaged"),
        entryFor<RcEqualsBool>("RcEqualsBool"),
        entryFor<RcEqualsBigInt>("RcEqualsBigInt"),
        entryFor<RcEqualsString>("RcEqualsString"),
        entryFor<RcGreater>("RcGreater"),
        entryFor<RcGreaterOrEqual>("RcGreaterOrEqual"),
        entryFor<RcLess>("RcLess"),
        entryFor<RcLessOrEqual>("RcLessOrEqual"),
        entryFor<RcExactlyOneOf>("RcExactlyOneOf"),
        entryFor<RcAtLeastOneOf>("RcAtLeastOneOf"),
        entryFor<RcMutuallyExclusive>("RcMutuallyExclusive"),
        entryFor<RcVisibleWhen>("RcVisibleWhen"),
        entryFor<RcReadonlyWhen>("RcReadonlyWhen"),
        entryFor<RcAnd>("RcAnd"),
        entryFor<RcOr>("RcOr"),
        entryFor<RcNot>("RcNot"),
    };
    return actions;
}

/// @brief The fixture named @p name, or `nullptr`.
/// @param name The corpus row's `"action"` value.
/// @return The entry, or `nullptr` when the corpus names an unknown fixture.
[[nodiscard]] inline const CorpusAction* findAction(std::string_view name) {
    for (const auto& entry : corpusActions()) {
        if (entry.name == name) {
            return &entry;
        }
    }
    return nullptr;
}

}  // namespace morph::test::rulecorpus
