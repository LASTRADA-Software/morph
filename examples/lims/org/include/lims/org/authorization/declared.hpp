// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <morph/core/action_guard.hpp>
#include <morph/core/registry.hpp>
#include <morph/session/session.hpp>
#include <morph/util/rational.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "lims/org/authorization/declared_guard.hpp"
#include "lims/org/core/errors.hpp"
#include "lims/org/core/principal.hpp"
#include "lims/org/core/types.hpp"
#include "lims/org/dto/role_dto.hpp"

/// @file
/// Declared authorization: per action, the role it needs, the scope its payload names, an optional value limit and
/// an optional separation-of-duties pair, declared once beside the action by the module that owns it.
///
/// `DeclaredRuleGuard` is the action guard every declared action names (`using guard = ::lims::DeclaredRuleGuard;`),
/// so the framework judges the rule on the decoded action on every dispatch path, after the validator and before the
/// handler. A handler calls `requireAuthorized` again only just before a write whose scope or amount a row gives, so
/// a change committed since the guard ran is seen; the rule reads on its own connection, never through the handler's
/// transaction.
///
/// `IAuthorizer::authorize` at the server edge is handed the model and action ids, never the payload, so the edge can
/// ask whether a principal holds the role somewhere (`lims::app::declaredRequirements`) and nothing more.

namespace lims {

class OrgModel;

namespace detail {

/// @brief The roles @p principal holds anywhere, read from `OrgModel`.
///
/// A template over the model so that `OrgModel` needs to be complete only where a rule is judged, not where this
/// header is read: `OrgModel`'s own header includes this one's through its authorization declarations.
/// @tparam Org `OrgModel`.
/// @param principal The principal.
/// @return Its roles.
template <typename Org = OrgModel>
[[nodiscard]] std::vector<LimsRole> rolesHeldAnywhere(std::string_view principal) {
    return Org::rolesAnywhere(principal);
}

/// @brief The roles @p principal holds in @p scope, read from `OrgModel`.
/// @tparam Org `OrgModel`.
/// @param principal The principal.
/// @param scope The scope.
/// @return Its roles there.
template <typename Org = OrgModel>
[[nodiscard]] std::vector<LimsRole> rolesHeldIn(std::string_view principal, Scope const& scope) {
    return Org::rolesIn(principal, scope);
}

}  // namespace detail

/// @brief One action's declared authorization; an action with no specialisation declares none.
///
/// A specialisation, beside the action's registration in the module that owns it, provides one of:
/// - `static constexpr LimsRole role` — the role the action needs;
/// - `static constexpr bool anyRole = true` — some role, whichever: the principal must hold at least one role in the
///   scope, and the action does not care which;
///
/// and, each only where the action needs it:
/// - `static Scope scopeOf(Action const&)` — the scope the action's payload names, read from its scope field; without
///   it the role is needed in some scope, any. A role held at a company holds at its sites;
/// - `static constexpr bool scopeInPayload = true` — `scopeOf` returns a scope the payload itself names, so a refusal
///   may say which one. A scope read from a row (every `scopeOf` that looks something up, and every
///   `scopeFromModel`) is not declared so, and a refusal names no scope: it would tell a caller whether the id
///   exists and, when it does, where the record is;
/// - `static constexpr bool scopeFromModel = true` and `template <typename Model> static std::vector<Scope>
///   scopesIn(Model const&, Action const&)` — the payload names no scope but the model knows it (the site of the
///   sample the handler is attached to): the guard judges the rule in every scope the model resolves, read before the
///   handler runs;
/// - `static bool bootstrapOpen()` — the rule is waived while this holds;
/// - `static constexpr bool checkedInHandler = true` — the handler judges the rule itself, because it answers
///   something else first, and the action declares no guard;
/// - `static ::morph::math::Rational amountOf(Action const&)` and
///   `static std::optional<::morph::math::Rational> limitOf(std::string_view principal, Scope const&)` — a value
///   limit: the principal may act on amounts up to their limit in the scope, and on none without one;
/// - `static constexpr std::string_view separatedFrom` (the action id of the pair's first half),
///   `static constexpr std::string_view sameActorRefusal` (the refusal's words) and
///   `static std::optional<std::string> firstPerformerOf(Action const&)` (who performed the first half on the same
///   subject) — a separation-of-duties pair: the principal may not be that performer;
/// - `static constexpr std::array<SeparatedFrom, N> separations` and
///   `static std::array<std::optional<std::string>, N> firstPerformersOf(Action const&)` — several pairs, each
///   with its own refusal, answered in the same order: the principal may not be any of those performers;
/// - `static constexpr bool notOnSelf = true` and `static std::string subjectOf(Action const&)` — the principal the
///   action is done to: the caller may not be that principal, so nobody sets their own limit or grants themselves a
///   role.
/// @tparam Action The action.
template <typename Action>
struct Authorization;

/// @brief Whether @p Action declares a specific role.
/// @tparam Action The action.
template <typename Action>
concept DeclaresRole = requires {
    { Authorization<Action>::role } -> std::convertible_to<LimsRole>;
};

/// @brief Whether @p Action declares that some role, whichever, is enough.
/// @tparam Action The action.
template <typename Action>
concept DeclaresAnyRole = requires { requires Authorization<Action>::anyRole; };

/// @brief Whether @p Action declares a rule.
/// @tparam Action The action.
template <typename Action>
concept DeclaresRule = DeclaresRole<Action> || DeclaresAnyRole<Action>;

/// @brief Whether @p Action's rule leaves the scope to the model, which passes it to `requireAuthorizedIn`.
/// @tparam Action The action.
template <typename Action>
concept DeclaresModelScope = DeclaresRule<Action> && requires { requires Authorization<Action>::scopeFromModel; };

/// @brief The role @p Action declares.
/// @tparam Action A declared action.
/// @return The role; nothing for an action that accepts any role.
template <typename Action>
    requires DeclaresRule<Action>
[[nodiscard]] constexpr std::optional<LimsRole> declaredRole() {
    if constexpr (DeclaresRole<Action>) {
        return Authorization<Action>::role;
    } else {
        return std::nullopt;
    }
}

/// @brief Whether @p Action's rule names the scope its payload holds.
/// @tparam Action The action.
template <typename Action>
concept DeclaresScope = DeclaresRule<Action> && requires(Action const& action) {
    { Authorization<Action>::scopeOf(action) } -> std::convertible_to<Scope>;
};

/// @brief Whether @p Action's rule declares that the scope it names is one the payload spells out, which a refusal may
///        therefore repeat.
/// @tparam Action The action.
template <typename Action>
concept DeclaresScopeInPayload = DeclaresRule<Action> && requires { requires Authorization<Action>::scopeInPayload; };

/// @brief Whether @p Action's rule declares a state check that runs once the principal holds the role where the
///        action's subject is visible to them and before anything about the subject is read for the verdict: the
///        value, the creator and the other facts of a subject the principal has no standing over stay unread.
/// @tparam Action The action.
template <typename Action>
concept DeclaresStateCheck = DeclaresRole<Action> && DeclaresScope<Action> &&
                             requires(Action const& action) { Authorization<Action>::checkState(action); };

/// @brief Whether @p Action's rule declares a value limit. A limit is judged in a scope, so it needs one.
/// @tparam Action The action.
template <typename Action>
concept DeclaresValueLimit =
    DeclaresScope<Action> && requires(Action const& action, std::string_view principal, Scope const& scope) {
        { Authorization<Action>::amountOf(action) } -> std::convertible_to<::morph::math::Rational>;
        {
            Authorization<Action>::limitOf(principal, scope)
        } -> std::convertible_to<std::optional<::morph::math::Rational>>;
    };

/// @brief Whether @p Action is the second half of a declared separation-of-duties pair.
/// @tparam Action The action.
template <typename Action>
concept DeclaresSeparation = DeclaresRule<Action> && requires(Action const& action) {
    { Authorization<Action>::separatedFrom } -> std::convertible_to<std::string_view>;
    { Authorization<Action>::sameActorRefusal } -> std::convertible_to<std::string_view>;
    { Authorization<Action>::firstPerformerOf(action) } -> std::convertible_to<std::optional<std::string>>;
};

/// @brief Whether @p Action refuses a caller who is its own subject.
/// @tparam Action The action.
template <typename Action>
concept DeclaresNotOnSelf = DeclaresRule<Action> && requires(Action const& action) {
    requires Authorization<Action>::notOnSelf;
    { Authorization<Action>::subjectOf(action) } -> std::convertible_to<std::string>;
};

/// @brief A rule needing @p Role at the company the action's `company` field names: the shape most ERP actions have.
/// @tparam Action An action with a `CompanyId company` member.
/// @tparam Role The role.
template <typename Action, LimsRole Role>
struct CompanyRule {
    /// @brief The role.
    static constexpr LimsRole role = Role;
    /// @brief The company is in the payload.
    static constexpr bool scopeInPayload = true;

    /// @brief The company the payload names.
    /// @param action The action.
    /// @return Its company.
    [[nodiscard]] static Scope scopeOf(Action const& action) { return Scope{action.company}; }
};

/// @brief What a declared rule decides.
enum class Verdict : std::uint8_t {
    Allowed,      ///< Every declared condition holds.
    MissingRole,  ///< The principal does not hold the role (any role, for a rule that accepts any) in the scope.
    SameActor,    ///< The principal performed the pair's first half on the same subject.
    OnSelf,       ///< The principal is the subject of an action that may not be done to oneself.
    NoLimit,      ///< The action carries an amount and the principal has no limit in the scope.
    OverLimit,    ///< The amount is above the principal's limit in the scope.
};

/// @brief Everything a rule is judged on, gathered by `requireAuthorized` from the declaration and the payload.
struct AuthorizationFacts {
    /// @brief The authenticated principal.
    std::string principal;
    /// @brief The role the action needs; empty when any role will do.
    std::optional<LimsRole> required = LimsRole::Analyst;
    /// @brief The roles the principal holds in the scope (anywhere, for a rule without one).
    std::vector<LimsRole> held;
    /// @brief Who performed the pair's first half on the same subject; empty without a pair or a performer.
    std::optional<std::string> firstPerformer;
    /// @brief The principal the action is done to; empty for an action that may be done to oneself.
    std::optional<std::string> notOnSelfSubject;
    /// @brief The amount the action acts on; empty without a value limit.
    std::optional<::morph::math::Rational> amount;
    /// @brief The principal's limit in the scope; empty when they have none.
    std::optional<::morph::math::Rational> limit;
};

/// @brief Judges @p facts. Pure: the same facts always give the same verdict.
///
/// The role is the first question, the pair the second, self-application the third, the limit the fourth, so a refusal
/// names the first reason that applies.
/// @param facts The facts.
/// @return The verdict.
[[nodiscard]] Verdict authorizationVerdict(AuthorizationFacts const& facts);

/// @brief The words a refusal uses for a required role.
/// @param role The role; empty for any role.
/// @return "the <role> role", or "any role".
[[nodiscard]] inline std::string roleWords(std::optional<LimsRole> role) {
    return role.has_value() ? "the " + std::string{roleName(*role)} + " role" : std::string{"any role"};
}

/// @brief Throws `Forbidden` unless the authenticated principal holds @p Action's declared role (any role, for a
///        rule that accepts any) in some scope.
///
/// Asked before the rows an action names are looked up, so a caller without the role learns nothing about which ids
/// exist; `requireAuthorized` then judges the scope once the rows are known.
/// @tparam Action A declared action.
/// @throws Forbidden when the principal holds the role in no scope.
template <typename Action>
    requires DeclaresRule<Action>
void requireRoleSomewhere() {
    std::string const principal = ::morph::session::current()->principal;
    auto const held = detail::rolesHeldAnywhere(principal);
    auto const role = declaredRole<Action>();
    if (role.has_value() ? std::ranges::find(held, *role) == held.end() : held.empty()) {
        throw Forbidden{std::string{::morph::model::ActionTraits<Action>::typeId()} + ": '" + principal +
                        "' does not hold " + roleWords(role)};
    }
}

/// @brief One separation-of-duties pair of an action with several: the first half's action id and the refusal.
struct SeparatedFrom {
    /// @brief The first half's registered action id.
    std::string_view action;
    /// @brief What the refusal says after the principal's name, such as "created this invoice and cannot also
    ///        approve it".
    std::string_view refusal;
};

/// @brief Whether @p Action declares several separation-of-duties pairs: `separations`, and `firstPerformersOf`
///        answering who performed each pair's first half on the same subject, in the same order.
/// @tparam Action The action type.
template <typename Action>
concept DeclaresSeparations = DeclaresRule<Action> && requires(Action const& action) {
    { Authorization<Action>::separations.size() } -> std::convertible_to<std::size_t>;
    { Authorization<Action>::firstPerformersOf(action)[0] } -> std::convertible_to<std::optional<std::string>>;
};

/// @brief A rule's facts for one action and principal, with where it was judged and the words a same-actor refusal
///        uses.
struct JudgedFacts {
    /// @brief The facts.
    AuthorizationFacts facts;
    /// @brief The scope, in words: "at company 2" where the payload names it, "where this applies" where a row gave
    ///        it, or "in any scope".
    std::string where;
    /// @brief The refusal's words when the verdict is `SameActor`; empty without a pair.
    std::string sameActorRefusal;
};

namespace detail {

/// @brief Gathers what @p Action's rule judges @p principal on, in @p scope or, when there is none, in any scope.
/// @tparam Action A declared action.
/// @param action The action.
/// @param principal The principal.
/// @param scope The scope the rule is judged in; empty for a rule that needs the role in some scope.
/// @return The facts, where they were judged, and a same-actor refusal's words.
template <typename Action>
[[nodiscard]] JudgedFacts judgedFactsIn(Action const& action, std::string const& principal,
                                        std::optional<Scope> const& scope) {
    using Rule = Authorization<Action>;
    // Only one form is read, so a rule declaring both would have the other ignored without a word.
    static_assert(!(DeclaresSeparation<Action> && DeclaresSeparations<Action>),
                  "an authorization declares one separation of duties pair or `separations`, not both");
    JudgedFacts judged{.facts = AuthorizationFacts{.principal = principal,
                                                   .required = declaredRole<Action>(),
                                                   .held = {},
                                                   .firstPerformer = std::nullopt,
                                                   .notOnSelfSubject = std::nullopt,
                                                   .amount = std::nullopt,
                                                   .limit = std::nullopt},
                       .where = "in any scope",
                       .sameActorRefusal = {}};
    if (scope.has_value()) {
        judged.facts.held = detail::rolesHeldIn(principal, *scope);
        // A scope read from a row is not named: an unknown id and a record in a scope the caller cannot see must be
        // refused in the same words.
        judged.where = DeclaresScopeInPayload<Action> ? "at " + scopeName(*scope) : std::string{"where this applies"};
        if constexpr (DeclaresValueLimit<Action>) {
            judged.facts.amount = Rule::amountOf(action);
            judged.facts.limit = Rule::limitOf(principal, *scope);
        }
    } else {
        judged.facts.held = detail::rolesHeldAnywhere(principal);
    }
    if constexpr (DeclaresSeparation<Action>) {
        judged.facts.firstPerformer = Rule::firstPerformerOf(action);
        judged.sameActorRefusal = std::string{Rule::sameActorRefusal};
    } else if constexpr (DeclaresSeparations<Action>) {
        auto const performers = Rule::firstPerformersOf(action);
        for (std::size_t pair = 0; pair < performers.size(); ++pair) {
            if (performers.at(pair) == principal) {
                judged.facts.firstPerformer = performers.at(pair);
                judged.sameActorRefusal = std::string{Rule::separations.at(pair).refusal};
                break;
            }
        }
    }
    if constexpr (DeclaresNotOnSelf<Action>) {
        judged.facts.notOnSelfSubject = Rule::subjectOf(action);
    }
    return judged;
}

/// @brief Judges @p action against its declaration in @p scope, or in any scope when there is none.
/// @tparam Action A declared action.
/// @param action The action.
/// @param scope The scope the rule is judged in; empty for a rule that needs the role in some scope.
/// @throws Forbidden with the reason, in words, when the verdict is not `Allowed`.
template <typename Action>
void judge(Action const& action, std::optional<Scope> const& scope) {
    std::string const principal = ::morph::session::current()->principal;
    auto const judged = judgedFactsIn(action, principal, scope);
    std::string const actionType{::morph::model::ActionTraits<Action>::typeId()};
    Verdict const verdict = authorizationVerdict(judged.facts);
    if (verdict == Verdict::Allowed) {
        return;
    }
    if (verdict == Verdict::MissingRole) {
        throw Forbidden{actionType + ": '" + principal + "' does not hold " + roleWords(declaredRole<Action>()) + " " +
                        judged.where};
    }
    if (verdict == Verdict::OnSelf) {
        throw Forbidden{actionType + ": '" + principal + "' may not do this to themselves"};
    }
    if (verdict == Verdict::NoLimit) {
        throw Forbidden{actionType + ": '" + principal + "' has no value limit " + judged.where};
    }
    if (verdict == Verdict::OverLimit) {
        throw Forbidden{actionType + ": the amount is above '" + principal + "''s value limit " + judged.where};
    }
    if (!judged.sameActorRefusal.empty()) {
        throw Forbidden{actionType + ": '" + principal + "' " + judged.sameActorRefusal};
    }
    throw Forbidden{actionType + ": refused"};
}

}  // namespace detail

/// @brief Gathers what @p Action's rule judges @p principal on, from the declaration and the payload.
///
/// `requireAuthorized` judges by these facts, and a list that shows what a principal could do now asks for the same
/// ones, so the rule is evaluated in one place.
/// @tparam Action A declared action whose scope the payload names, or that needs none.
/// @param action The action.
/// @param principal The principal.
/// @return The facts, where they were judged, and a same-actor refusal's words.
template <typename Action>
    requires DeclaresRule<Action> && (!DeclaresModelScope<Action>)
[[nodiscard]] JudgedFacts authorizationFactsOf(Action const& action, std::string const& principal) {
    if constexpr (DeclaresScope<Action>) {
        return detail::judgedFactsIn(action, principal, std::optional<Scope>{Authorization<Action>::scopeOf(action)});
    } else {
        return detail::judgedFactsIn(action, principal, std::nullopt);
    }
}

/// @brief Checks @p action against its declaration, as the authenticated principal.
///
/// Called by `DeclaredRuleGuard` on every dispatch path, after the payload is validated and, where the rule needs a
/// row, before that row is looked up for anything but its scope, so a refusal is about permission and never a
/// disguised missing row: an unknown scope holds no role, so it is refused as a missing role. A handler calls it again
/// just before its write.
/// @tparam Action A declared action whose scope the payload names, or that needs none.
/// @param action The action.
/// @throws Forbidden with the reason, in words, when the verdict is not `Allowed`.
template <typename Action>
    requires DeclaresRule<Action> && (!DeclaresModelScope<Action>)
void requireAuthorized(Action const& action) {
    if constexpr (DeclaresStateCheck<Action>) {
        Scope const scope = Authorization<Action>::scopeOf(action);
        std::string const principal = ::morph::session::current()->principal;
        auto const held = detail::rolesHeldIn(principal, scope);
        // The words name neither the scope nor the subject: a subject that does not exist and one in a scope the
        // principal holds nothing in are refused identically.
        if (std::ranges::find(held, Authorization<Action>::role) == held.end()) {
            throw Forbidden{std::string{::morph::model::ActionTraits<Action>::typeId()} + ": '" + principal +
                            "' does not hold " + roleWords(declaredRole<Action>()) + " where this applies"};
        }
        Authorization<Action>::checkState(action);
        detail::judge(action, std::optional<Scope>{scope});
    } else if constexpr (DeclaresScope<Action>) {
        detail::judge(action, std::optional<Scope>{Authorization<Action>::scopeOf(action)});
    } else {
        detail::judge(action, std::nullopt);
    }
}

/// @brief Checks @p action against its declaration in @p scope, as the authenticated principal: for a rule whose scope
///        the model knows and the payload does not carry.
/// @tparam Action A declared action with `scopeFromModel`.
/// @param action The action.
/// @param scope The scope the model resolved; an empty scope when the row it comes from does not exist, which holds
///        no role and is refused as a missing role.
/// @throws Forbidden with the reason, in words, when the verdict is not `Allowed`.
template <typename Action>
    requires DeclaresModelScope<Action>
void requireAuthorizedIn(Action const& action, Scope const& scope) {
    detail::judge(action, std::optional<Scope>{scope});
}

/// @brief Whether @p Action's rule is waived while an exemption the declaration names holds: the first role
///        assignment on a lab with no supervisor.
/// @tparam Action The action.
template <typename Action>
concept DeclaresBootstrap = DeclaresRule<Action> && requires {
    { Authorization<Action>::bootstrapOpen() } -> std::convertible_to<bool>;
};

/// @brief Whether @p Action's rule is checked by its handler, because the handler answers something else first, and
///        so the action declares no guard.
/// @tparam Action The action.
template <typename Action>
concept DeclaresCheckedInHandler =
    DeclaresRule<Action> && requires { requires Authorization<Action>::checkedInHandler; };

/// @brief The guard of every lims action that declares a rule: the framework runs it on the decoded action, on every
///        dispatch path, after the validator and before the handler.
struct DeclaredRuleGuard {
    /// @brief Refuses @p action unless its declared rule allows the current principal, in the scope its payload
    ///        names.
    /// @tparam Action A lims action whose rule names its scope in the payload, or needs none.
    /// @param action The decoded action.
    /// @throws EmptyPrincipalError when no principal is authenticated.
    /// @throws Forbidden with the reason, in words, when the rule refuses.
    template <typename Action>
        requires DeclaresRule<Action> && (!DeclaresModelScope<Action>)
    static void check(Action const& action) {
        static_assert(!DeclaresCheckedInHandler<Action>, "an action whose handler checks its rule declares no guard");
        requirePrincipal();
        if constexpr (DeclaresBootstrap<Action>) {
            if (Authorization<Action>::bootstrapOpen()) {
                return;
            }
        }
        requireRoleSomewhere<Action>();
        requireAuthorized(action);
    }

    /// @brief Refuses @p action unless its declared rule allows the current principal in every scope @p model
    ///        resolves for it.
    /// @tparam Model The model the action is addressed to.
    /// @tparam Action A lims action whose rule takes its scope from the model.
    /// @param model The instance, as it stands before the handler runs.
    /// @param action The decoded action.
    /// @throws EmptyPrincipalError when no principal is authenticated.
    /// @throws NotFound when the model holds nothing to resolve a scope from.
    /// @throws Forbidden with the reason, in words, when the rule refuses in any of the scopes.
    template <typename Model, typename Action>
        requires DeclaresModelScope<Action>
    static void check(Model const& model, Action const& action) {
        requirePrincipal();
        requireRoleSomewhere<Action>();
        for (Scope const& scope : Authorization<Action>::scopesIn(model, action)) {
            requireAuthorizedIn(action, scope);
        }
    }
};

/// @brief One declared role as the server edge can use it: by action id, with no payload.
struct RoleRequirement {
    /// @brief The action id.
    std::string_view actionType;
    /// @brief The role it needs, somewhere; empty when any role will do.
    std::optional<LimsRole> role = LimsRole::Analyst;
};

/// @brief @p Action's declared role, for the edge's table.
/// @tparam Action A declared action.
/// @return Its action id and role.
template <typename Action>
    requires DeclaresRule<Action>
[[nodiscard]] constexpr RoleRequirement requirementOf() {
    return RoleRequirement{.actionType = ::morph::model::ActionTraits<Action>::typeId(),
                           .role = declaredRole<Action>()};
}

}  // namespace lims
